// SPDX-License-Identifier: MIT
#include "reconnect_coordinator.hpp"

#include <algorithm>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <string_view>
#include <utility>

namespace kairosboot::fastboot {
namespace {

struct AttemptState final {
    std::size_t discovery_attempts{};
    std::size_t open_attempts{};
    ReconnectObservation last_observation{ReconnectObservation::None};
    std::optional<ReconnectDeviceIdentity> observed_identity;
    std::optional<ReconnectDiscoveryError> discovery_error;
    std::optional<ReconnectOpenError> open_error;
};

[[nodiscard]] bool valid_physical_port(
    const UsbPhysicalPortPath& port) noexcept {
    if (port.bus_number == 0 || port.ports.empty() || port.ports.size() > 7) {
        return false;
    }
    return std::ranges::all_of(port.ports, [](const std::uint8_t value) {
        return value != 0;
    });
}

[[nodiscard]] bool valid_identity_text(const std::string_view text) noexcept {
    if (text.empty()) {
        return false;
    }
    return std::ranges::none_of(text, [](const unsigned char value) {
        return value < 0x20U || value == 0x7FU;
    });
}

[[nodiscard]] bool valid_certainty(
    const protocol::TransferCertainty certainty) noexcept {
    switch (certainty) {
        case protocol::TransferCertainty::NotTransferred:
        case protocol::TransferCertainty::FullyTransferred:
        case protocol::TransferCertainty::PartialOrUnknown:
            return true;
    }
    return false;
}

[[nodiscard]] bool valid_mode(const FastbootUsbMode mode) noexcept {
    switch (mode) {
        case FastbootUsbMode::Bootloader:
        case FastbootUsbMode::Fastbootd:
            return true;
    }
    return false;
}

[[nodiscard]] ReconnectError make_error(
    const ReconnectErrorCode code,
    const ReconnectStage stage,
    std::string message,
    const ReconnectTarget& target,
    const AttemptState& state,
    const int native_code = 0,
    const protocol::TransferCertainty reconnect_certainty =
        protocol::TransferCertainty::NotTransferred) {
    ReconnectError error;
    error.code = code;
    error.stage = stage;
    error.message = std::move(message);
    error.discovery_attempts = state.discovery_attempts;
    error.open_attempts = state.open_attempts;
    error.last_observation = state.last_observation;
    error.observed_identity = state.observed_identity;
    error.discovery_error = state.discovery_error;
    error.open_error = state.open_error;
    error.native_code = native_code;
    error.preceding_operation_certainty =
        target.preceding_operation_certainty;
    error.reconnect_outbound_certainty = reconnect_certainty;
    return error;
}

[[nodiscard]] std::chrono::milliseconds remaining_time(
    const IReconnectWaiter::TimePoint now,
    const IReconnectWaiter::TimePoint deadline) noexcept {
    if (deadline == IReconnectWaiter::TimePoint::max()) {
        return std::chrono::milliseconds::max();
    }
    if (now >= deadline) {
        return std::chrono::milliseconds::zero();
    }
    return std::chrono::ceil<std::chrono::milliseconds>(deadline - now);
}

[[nodiscard]] std::chrono::milliseconds next_backoff(
    const std::chrono::milliseconds current,
    const std::chrono::milliseconds maximum) noexcept {
    if (current >= maximum) {
        return maximum;
    }
    const auto current_count = current.count();
    const auto maximum_count = maximum.count();
    if (current_count > maximum_count - current_count) {
        return maximum;
    }
    return std::min(maximum, current + current);
}

[[nodiscard]] bool same_required_identity(
    const ReconnectDeviceIdentity& observed,
    const ReconnectTarget& target) noexcept {
    return observed.physical_port == target.physical_port &&
        observed.serial == target.serial && observed.product == target.product &&
        observed.mode == target.required_mode;
}

[[nodiscard]] std::string deadline_message(
    const ReconnectObservation observation) {
    switch (observation) {
        case ReconnectObservation::None:
            return "Fastboot reconnect deadline expired before a usable discovery result";
        case ReconnectObservation::DeviceAbsent:
            return "Fastboot reconnect deadline expired while the physical port was absent";
        case ReconnectObservation::PreviousModePresent:
            return "Fastboot reconnect deadline expired before the device changed mode";
        case ReconnectObservation::RequiredModePresent:
            return "Fastboot reconnect deadline expired while opening the verified device";
    }
    return "Fastboot reconnect deadline expired";
}

}  // namespace

IReconnectWaiter::TimePoint SteadyReconnectWaiter::now() const noexcept {
    return Clock::now();
}

ReconnectWaitResult SteadyReconnectWaiter::wait_for(
    const std::chrono::milliseconds duration,
    const std::stop_token cancellation) {
    if (cancellation.stop_requested()) {
        return ReconnectWaitResult{
            .status = ReconnectWaitStatus::Cancelled,
            .message = "Fastboot reconnect wait was cancelled",
            .native_code = 0,
        };
    }
    if (duration <= std::chrono::milliseconds::zero()) {
        return ReconnectWaitResult{
            .status = ReconnectWaitStatus::Elapsed,
            .message = {},
            .native_code = 0,
        };
    }

    std::mutex mutex;
    std::condition_variable changed;
    bool cancelled = false;
    std::stop_callback cancel_wait(cancellation, [&] {
        {
            const std::lock_guard lock(mutex);
            cancelled = true;
        }
        changed.notify_all();
    });
    std::unique_lock lock(mutex);
    static_cast<void>(changed.wait_for(lock, duration, [&] {
        return cancelled;
    }));
    return ReconnectWaitResult{
        .status = cancelled || cancellation.stop_requested()
            ? ReconnectWaitStatus::Cancelled
            : ReconnectWaitStatus::Elapsed,
        .message = cancelled || cancellation.stop_requested()
            ? "Fastboot reconnect wait was cancelled"
            : std::string{},
        .native_code = 0,
    };
}

ReconnectCoordinator::ReconnectCoordinator(
    IReconnectDiscovery& discovery,
    IReconnectSessionOpener& opener,
    IReconnectWaiter& waiter) noexcept
    : discovery_(discovery), opener_(opener), waiter_(waiter) {}

std::expected<ReconnectedSession, ReconnectError>
ReconnectCoordinator::reconnect(
    const ReconnectTarget& target,
    const IReconnectWaiter::TimePoint deadline,
    const ReconnectOptions options,
    const std::stop_token cancellation) {
    AttemptState state;
    if (!valid_physical_port(target.physical_port)) {
        return std::unexpected(make_error(
            ReconnectErrorCode::InvalidArgument,
            ReconnectStage::Validation,
            "Fastboot reconnect requires a non-empty physical USB port path",
            target,
            state));
    }
    if (!valid_identity_text(target.serial) ||
        !valid_identity_text(target.product)) {
        return std::unexpected(make_error(
            ReconnectErrorCode::InvalidArgument,
            ReconnectStage::Validation,
            "Fastboot reconnect requires non-empty serial and product identity",
            target,
            state));
    }
    if (!valid_mode(target.previous_mode) ||
        !valid_mode(target.required_mode) ||
        !valid_certainty(target.preceding_operation_certainty)) {
        return std::unexpected(make_error(
            ReconnectErrorCode::InvalidArgument,
            ReconnectStage::Validation,
            "Fastboot reconnect target contains an invalid enum value",
            target,
            state));
    }
    if (options.initial_backoff <= std::chrono::milliseconds::zero() ||
        options.maximum_backoff < options.initial_backoff ||
        options.maximum_discovered_devices == 0) {
        return std::unexpected(make_error(
            ReconnectErrorCode::InvalidArgument,
            ReconnectStage::Validation,
            "Fastboot reconnect backoff and discovery limits are invalid",
            target,
            state));
    }
    if (target.preceding_operation_certainty ==
        protocol::TransferCertainty::PartialOrUnknown) {
        return std::unexpected(make_error(
            ReconnectErrorCode::UnsafePreviousOutcome,
            ReconnectStage::Validation,
            "Fastboot reconnect refused an uncertain preceding operation; no offset or protocol state will be guessed",
            target,
            state));
    }

    auto backoff = options.initial_backoff;
    auto last_time = waiter_.now();
    for (;;) {
        if (cancellation.stop_requested()) {
            return std::unexpected(make_error(
                ReconnectErrorCode::Cancelled,
                ReconnectStage::Discovery,
                "Fastboot reconnect was cancelled before discovery",
                target,
                state));
        }

        const auto now = waiter_.now();
        if (now < last_time) {
            return std::unexpected(make_error(
                ReconnectErrorCode::ClockContractViolation,
                ReconnectStage::Backoff,
                "Fastboot reconnect clock moved backwards",
                target,
                state));
        }
        last_time = now;
        if (now >= deadline) {
            const auto native_code = state.open_error.has_value()
                ? state.open_error->native_code
                : state.discovery_error.has_value()
                    ? state.discovery_error->native_code
                    : 0;
            const auto certainty = state.open_error.has_value()
                ? state.open_error->outbound_certainty
                : protocol::TransferCertainty::NotTransferred;
            return std::unexpected(make_error(
                ReconnectErrorCode::DeadlineExceeded,
                ReconnectStage::Backoff,
                deadline_message(state.last_observation),
                target,
                state,
                native_code,
                certainty));
        }

        ++state.discovery_attempts;
        auto discovered = discovery_.discover(cancellation);
        if (cancellation.stop_requested()) {
            return std::unexpected(make_error(
                ReconnectErrorCode::Cancelled,
                ReconnectStage::Discovery,
                "Fastboot reconnect was cancelled during discovery",
                target,
                state));
        }

        bool retry = false;
        if (!discovered.has_value()) {
            state.discovery_error = discovered.error();
            if (!discovered.error().retryable) {
                return std::unexpected(make_error(
                    ReconnectErrorCode::DiscoveryFailed,
                    ReconnectStage::Discovery,
                    "Fastboot reconnect discovery failed: " +
                        discovered.error().message,
                    target,
                    state,
                    discovered.error().native_code));
            }
            retry = true;
        } else {
            state.discovery_error.reset();
            if (discovered->size() > options.maximum_discovered_devices) {
                return std::unexpected(make_error(
                    ReconnectErrorCode::DiscoveryContractViolation,
                    ReconnectStage::Discovery,
                    "Fastboot reconnect discovery exceeded its configured device limit",
                    target,
                    state));
            }

            const ReconnectDeviceIdentity* candidate = nullptr;
            for (const auto& device : *discovered) {
                if (device.physical_port != target.physical_port) {
                    continue;
                }
                if (candidate != nullptr) {
                    state.observed_identity = device;
                    return std::unexpected(make_error(
                        ReconnectErrorCode::AmbiguousPhysicalPort,
                        ReconnectStage::Selection,
                        "Fastboot reconnect discovery returned multiple devices at the same physical port",
                        target,
                        state));
                }
                candidate = &device;
            }

            if (candidate == nullptr) {
                state.last_observation = ReconnectObservation::DeviceAbsent;
                state.observed_identity.reset();
                retry = true;
            } else {
                state.observed_identity = *candidate;
                if (!valid_physical_port(candidate->physical_port) ||
                    !valid_identity_text(candidate->serial) ||
                    !valid_identity_text(candidate->product) ||
                    !valid_mode(candidate->mode)) {
                    return std::unexpected(make_error(
                        ReconnectErrorCode::DiscoveryContractViolation,
                        ReconnectStage::Discovery,
                        "Fastboot reconnect discovery returned an invalid identity",
                        target,
                        state));
                }
                if (candidate->serial != target.serial) {
                    return std::unexpected(make_error(
                        ReconnectErrorCode::PortOccupiedByDifferentDevice,
                        ReconnectStage::Selection,
                        "Fastboot reconnect physical port is occupied by a different serial",
                        target,
                        state));
                }
                if (candidate->product != target.product) {
                    return std::unexpected(make_error(
                        ReconnectErrorCode::ProductMismatch,
                        ReconnectStage::Selection,
                        "Fastboot reconnect product does not match the original device",
                        target,
                        state));
                }
                if (candidate->mode == target.previous_mode &&
                    target.previous_mode != target.required_mode) {
                    state.last_observation =
                        ReconnectObservation::PreviousModePresent;
                    retry = true;
                } else if (candidate->mode != target.required_mode) {
                    return std::unexpected(make_error(
                        ReconnectErrorCode::DiscoveryContractViolation,
                        ReconnectStage::Selection,
                        "Fastboot reconnect discovery returned an unexpected mode",
                        target,
                        state));
                } else {
                    state.last_observation =
                        ReconnectObservation::RequiredModePresent;
                    ++state.open_attempts;
                    auto opened = opener_.open(*candidate, cancellation);
                    if (cancellation.stop_requested()) {
                        return std::unexpected(make_error(
                            ReconnectErrorCode::Cancelled,
                            ReconnectStage::Opening,
                            "Fastboot reconnect was cancelled while opening the replacement session",
                            target,
                            state));
                    }
                    if (!opened.has_value()) {
                        state.open_error = opened.error();
                        const auto certainty = opened.error().outbound_certainty;
                        if (!valid_certainty(certainty)) {
                            return std::unexpected(make_error(
                                ReconnectErrorCode::OpenContractViolation,
                                ReconnectStage::Opening,
                                "Fastboot reconnect opener returned an invalid transfer certainty",
                                target,
                                state,
                                opened.error().native_code));
                        }
                        if (certainty !=
                            protocol::TransferCertainty::NotTransferred) {
                            return std::unexpected(make_error(
                                ReconnectErrorCode::OpenOutcomeUncertain,
                                ReconnectStage::Opening,
                                "Fastboot reconnect opener emitted protocol bytes; the result will not be retried or resumed",
                                target,
                                state,
                                opened.error().native_code,
                                certainty));
                        }
                        if (!opened.error().retryable) {
                            return std::unexpected(make_error(
                                ReconnectErrorCode::OpenFailed,
                                ReconnectStage::Opening,
                                "Fastboot reconnect could not open the replacement session: " +
                                    opened.error().message,
                                target,
                                state,
                                opened.error().native_code));
                        }
                        retry = true;
                    } else {
                        state.open_error.reset();
                        if (opened->session == nullptr) {
                            state.observed_identity =
                                opened->verified_identity;
                            return std::unexpected(make_error(
                                ReconnectErrorCode::OpenContractViolation,
                                ReconnectStage::Opening,
                                "Fastboot reconnect opener returned a null session",
                                target,
                                state));
                        }
                        if (!same_required_identity(
                                opened->verified_identity, target)) {
                            state.observed_identity =
                                opened->verified_identity;
                            return std::unexpected(make_error(
                                ReconnectErrorCode::DeviceChangedDuringOpen,
                                ReconnectStage::Verification,
                                "Fastboot device identity changed between discovery and exclusive open",
                                target,
                                state));
                        }
                        return ReconnectedSession{
                            .identity = std::move(opened->verified_identity),
                            .session = std::move(opened->session),
                            .discovery_attempts = state.discovery_attempts,
                            .open_attempts = state.open_attempts,
                        };
                    }
                }
            }
        }

        if (!retry) {
            return std::unexpected(make_error(
                ReconnectErrorCode::DiscoveryContractViolation,
                ReconnectStage::Discovery,
                "Fastboot reconnect reached an invalid retry state",
                target,
                state));
        }

        if (cancellation.stop_requested()) {
            return std::unexpected(make_error(
                ReconnectErrorCode::Cancelled,
                ReconnectStage::Backoff,
                "Fastboot reconnect was cancelled before backoff",
                target,
                state));
        }
        const auto before_wait = waiter_.now();
        if (before_wait < last_time) {
            return std::unexpected(make_error(
                ReconnectErrorCode::ClockContractViolation,
                ReconnectStage::Backoff,
                "Fastboot reconnect clock moved backwards before backoff",
                target,
                state));
        }
        last_time = before_wait;
        const auto remaining = remaining_time(before_wait, deadline);
        if (remaining <= std::chrono::milliseconds::zero()) {
            continue;
        }
        const auto delay = std::min(backoff, remaining);
        const auto wait_result = waiter_.wait_for(delay, cancellation);
        if (wait_result.status == ReconnectWaitStatus::Cancelled ||
            cancellation.stop_requested()) {
            return std::unexpected(make_error(
                ReconnectErrorCode::Cancelled,
                ReconnectStage::Backoff,
                wait_result.message.empty()
                    ? "Fastboot reconnect backoff was cancelled"
                    : wait_result.message,
                target,
                state,
                wait_result.native_code));
        }
        if (wait_result.status == ReconnectWaitStatus::Failed) {
            return std::unexpected(make_error(
                ReconnectErrorCode::WaitFailed,
                ReconnectStage::Backoff,
                wait_result.message.empty()
                    ? "Fastboot reconnect backoff wait failed"
                    : wait_result.message,
                target,
                state,
                wait_result.native_code));
        }
        if (wait_result.status != ReconnectWaitStatus::Elapsed) {
            return std::unexpected(make_error(
                ReconnectErrorCode::WaitFailed,
                ReconnectStage::Backoff,
                "Fastboot reconnect waiter returned an invalid status",
                target,
                state,
                wait_result.native_code));
        }
        const auto after_wait = waiter_.now();
        if (after_wait <= before_wait) {
            return std::unexpected(make_error(
                ReconnectErrorCode::ClockContractViolation,
                ReconnectStage::Backoff,
                "Fastboot reconnect waiter elapsed without advancing its clock",
                target,
                state));
        }
        last_time = after_wait;
        backoff = next_backoff(backoff, options.maximum_backoff);
    }
}

}  // namespace kairosboot::fastboot
