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
    std::optional<ReconnectCandidate> observed_candidate;
    std::optional<ReconnectDeviceIdentity> observed_identity;
    std::optional<ReconnectDiscoveryError> discovery_error;
    std::optional<ReconnectOpenError> open_error;
};

[[nodiscard]] bool valid_physical_port(
    const UsbPhysicalPortPath& port) noexcept {
    // libusb's Darwin backend derives a zero-based bus number from locationID,
    // so bus 0 is a valid physical identity on supported macOS hosts.
    if (port.ports.empty() || port.ports.size() > 7) {
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

[[nodiscard]] bool valid_optional_identity_text(
    const std::optional<std::string>& text) noexcept {
    return !text.has_value() || valid_identity_text(*text);
}

[[nodiscard]] bool valid_usb_fingerprint(
    const ReconnectUsbFingerprint& fingerprint) noexcept {
    return fingerprint.vendor_id != 0 && fingerprint.interface_class == 0xFFU &&
        fingerprint.interface_subclass == 0x42U &&
        fingerprint.interface_protocol == 0x03U;
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

[[nodiscard]] bool valid_fingerprint_policy(
    const ReconnectUsbFingerprintPolicy policy) noexcept {
    switch (policy) {
        case ReconnectUsbFingerprintPolicy::Exact:
        case ReconnectUsbFingerprintPolicy::AllowChangeWithLiveIdentity:
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
    error.observed_candidate = state.observed_candidate;
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

void clear_dependency_failures(AttemptState& state) noexcept {
    state.discovery_error.reset();
    state.open_error.reset();
}

[[nodiscard]] std::string deadline_message(
    const ReconnectObservation observation) {
    switch (observation) {
        case ReconnectObservation::None:
            return "Fastboot reconnect deadline expired before a usable discovery result";
        case ReconnectObservation::DiscoveryUnavailable:
            return "Fastboot reconnect deadline expired while passive USB discovery was unavailable";
        case ReconnectObservation::DeviceAbsent:
            return "Fastboot reconnect deadline expired while the physical port was absent";
        case ReconnectObservation::CandidatePresent:
            return "Fastboot reconnect deadline expired while opening the passive USB candidate";
        case ReconnectObservation::PreviousModePresent:
            return "Fastboot reconnect deadline expired before the device changed mode";
        case ReconnectObservation::RequiredModePresent:
            return "Fastboot reconnect deadline expired while opening the verified device";
    }
    return "Fastboot reconnect deadline expired";
}

}  // namespace

bool reconnect_usb_fingerprint_allowed(
    const ReconnectTarget& target,
    const ReconnectUsbFingerprint& observed,
    const std::optional<FastbootUsbMode> verified_mode) noexcept {
    if (target.usb_fingerprint_policy ==
        ReconnectUsbFingerprintPolicy::Exact) {
        return observed == target.usb_fingerprint;
    }
    if (target.usb_fingerprint_policy !=
            ReconnectUsbFingerprintPolicy::AllowChangeWithLiveIdentity ||
        target.previous_mode == target.required_mode ||
        target.preceding_operation_certainty !=
            protocol::TransferCertainty::FullyTransferred) {
        return false;
    }
    if (!verified_mode.has_value()) {
        return true;
    }
    if (*verified_mode == target.previous_mode) {
        return observed == target.usb_fingerprint;
    }
    return *verified_mode == target.required_mode;
}

bool reconnect_identity_matches_target(
    const ReconnectTarget& target,
    const ReconnectDeviceIdentity& identity) noexcept {
    return identity.physical_port == target.physical_port &&
        (!target.serial.has_value() || identity.serial == target.serial) &&
        identity.product == target.product &&
        (identity.mode == target.previous_mode ||
         identity.mode == target.required_mode) &&
        reconnect_usb_fingerprint_allowed(
            target, identity.usb_fingerprint, identity.mode);
}

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

std::expected<void, ReconnectError> validate_reconnect_request(
    const ReconnectTarget& target,
    const ReconnectOptions options) {
    const AttemptState state;
    if (!valid_physical_port(target.physical_port)) {
        return std::unexpected(make_error(
            ReconnectErrorCode::InvalidArgument,
            ReconnectStage::Validation,
            "Fastboot reconnect requires a non-empty physical USB port path",
            target,
            state));
    }
    if (!valid_optional_identity_text(target.serial) ||
        !valid_identity_text(target.product) ||
        !valid_usb_fingerprint(target.usb_fingerprint)) {
        return std::unexpected(make_error(
            ReconnectErrorCode::InvalidArgument,
            ReconnectStage::Validation,
            "Fastboot reconnect requires a valid optional serial, USB fingerprint, and product identity",
            target,
            state));
    }
    if (!valid_mode(target.previous_mode) ||
        !valid_mode(target.required_mode) ||
        !valid_fingerprint_policy(target.usb_fingerprint_policy) ||
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
        options.maximum_discovered_devices == 0 ||
        options.maximum_discovery_attempts == 0 ||
        options.maximum_open_attempts == 0) {
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
    if (target.usb_fingerprint_policy ==
            ReconnectUsbFingerprintPolicy::AllowChangeWithLiveIdentity &&
        (target.previous_mode == target.required_mode ||
         target.preceding_operation_certainty !=
             protocol::TransferCertainty::FullyTransferred)) {
        return std::unexpected(make_error(
            ReconnectErrorCode::InvalidArgument,
            ReconnectStage::Validation,
            "Fastboot reconnect may change USB fingerprint only for a fully transferred mode transition",
            target,
            state));
    }
    return {};
}

std::expected<ReconnectedSession, ReconnectError>
ReconnectCoordinator::reconnect(
    const ReconnectTarget& target,
    const IReconnectWaiter::TimePoint deadline,
    const ReconnectOptions options,
    const std::stop_token cancellation) {
    AttemptState state;
    if (auto valid = validate_reconnect_request(target, options); !valid) {
        return std::unexpected(std::move(valid.error()));
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

        if (state.discovery_attempts >= options.maximum_discovery_attempts) {
            clear_dependency_failures(state);
            return std::unexpected(make_error(
                ReconnectErrorCode::AttemptLimitExceeded,
                ReconnectStage::Discovery,
                "Fastboot reconnect reached its discovery attempt limit",
                target,
                state));
        }
        ++state.discovery_attempts;
        clear_dependency_failures(state);
        state.last_observation = ReconnectObservation::None;
        state.observed_candidate.reset();
        state.observed_identity.reset();
        auto discovered = discovery_.discover(deadline, cancellation);
        if (!discovered.has_value()) {
            state.discovery_error = discovered.error();
            state.last_observation = ReconnectObservation::DiscoveryUnavailable;
            state.observed_candidate.reset();
            state.observed_identity.reset();
        }

        const auto after_discovery = waiter_.now();
        if (after_discovery < last_time) {
            return std::unexpected(make_error(
                ReconnectErrorCode::ClockContractViolation,
                ReconnectStage::Discovery,
                "Fastboot reconnect clock moved backwards during discovery",
                target,
                state,
                state.discovery_error.has_value()
                    ? state.discovery_error->native_code
                    : 0));
        }
        last_time = after_discovery;
        if (cancellation.stop_requested()) {
            return std::unexpected(make_error(
                ReconnectErrorCode::Cancelled,
                ReconnectStage::Discovery,
                "Fastboot reconnect was cancelled during discovery",
                target,
                state,
                state.discovery_error.has_value()
                    ? state.discovery_error->native_code
                    : 0));
        }
        if (after_discovery >= deadline) {
            return std::unexpected(make_error(
                ReconnectErrorCode::DeadlineExceeded,
                ReconnectStage::Discovery,
                "Fastboot reconnect deadline expired during discovery",
                target,
                state,
                state.discovery_error.has_value()
                    ? state.discovery_error->native_code
                    : 0));
        }

        bool retry = false;
        auto retry_certainty = protocol::TransferCertainty::NotTransferred;
        if (!discovered.has_value()) {
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
            if (discovered->size() > options.maximum_discovered_devices) {
                return std::unexpected(make_error(
                    ReconnectErrorCode::DiscoveryContractViolation,
                    ReconnectStage::Discovery,
                    "Fastboot reconnect discovery exceeded its configured device limit",
                    target,
                    state));
            }

            const ReconnectCandidate* candidate = nullptr;
            for (const auto& device : *discovered) {
                if (device.physical_port != target.physical_port) {
                    continue;
                }
                if (candidate != nullptr) {
                    state.observed_candidate = device;
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
                state.observed_candidate.reset();
                state.observed_identity.reset();
                retry = true;
            } else {
                state.last_observation = ReconnectObservation::CandidatePresent;
                state.observed_candidate = *candidate;
                state.observed_identity.reset();
                if (!valid_physical_port(candidate->physical_port) ||
                    !valid_optional_identity_text(candidate->serial) ||
                    !valid_usb_fingerprint(candidate->usb_fingerprint)) {
                    return std::unexpected(make_error(
                        ReconnectErrorCode::DiscoveryContractViolation,
                        ReconnectStage::Discovery,
                        "Fastboot reconnect discovery returned an invalid passive USB candidate",
                        target,
                        state));
                }
                if (!reconnect_usb_fingerprint_allowed(
                        target, candidate->usb_fingerprint)) {
                    return std::unexpected(make_error(
                        ReconnectErrorCode::UsbFingerprintMismatch,
                        ReconnectStage::Selection,
                        "Fastboot reconnect physical port has a different USB fingerprint",
                        target,
                        state));
                }
                if (target.serial.has_value() && candidate->serial.has_value() &&
                    candidate->serial != target.serial) {
                    return std::unexpected(make_error(
                        ReconnectErrorCode::PortOccupiedByDifferentDevice,
                        ReconnectStage::Selection,
                        "Fastboot reconnect physical port is occupied by a different serial",
                        target,
                        state));
                }

                if (state.open_attempts >= options.maximum_open_attempts) {
                    clear_dependency_failures(state);
                    return std::unexpected(make_error(
                        ReconnectErrorCode::AttemptLimitExceeded,
                        ReconnectStage::Opening,
                        "Fastboot reconnect reached its open attempt limit",
                        target,
                        state));
                }

                const auto before_open = waiter_.now();
                if (before_open < last_time) {
                    return std::unexpected(make_error(
                        ReconnectErrorCode::ClockContractViolation,
                        ReconnectStage::Opening,
                        "Fastboot reconnect clock moved backwards before open",
                        target,
                        state));
                }
                last_time = before_open;
                if (cancellation.stop_requested()) {
                    return std::unexpected(make_error(
                        ReconnectErrorCode::Cancelled,
                        ReconnectStage::Opening,
                        "Fastboot reconnect was cancelled before open",
                        target,
                        state));
                }
                if (before_open >= deadline) {
                    return std::unexpected(make_error(
                        ReconnectErrorCode::DeadlineExceeded,
                        ReconnectStage::Opening,
                        "Fastboot reconnect deadline expired before open",
                        target,
                        state));
                }

                ++state.open_attempts;
                auto opened = opener_.open(*candidate, deadline, cancellation);
                const auto certainty = opened.has_value()
                    ? opened->outbound_certainty
                    : opened.error().outbound_certainty;
                const auto native_code = opened.has_value()
                    ? 0
                    : opened.error().native_code;
                state.discovery_error.reset();
                if (!opened.has_value()) {
                    state.open_error = opened.error();
                } else {
                    state.open_error.reset();
                }
                if (!valid_certainty(certainty)) {
                    return std::unexpected(make_error(
                        ReconnectErrorCode::OpenContractViolation,
                        ReconnectStage::Opening,
                        "Fastboot reconnect opener returned an invalid aggregate transfer certainty",
                        target,
                        state,
                        native_code));
                }
                retry_certainty = certainty;

                const auto after_open = waiter_.now();
                if (after_open < last_time) {
                    return std::unexpected(make_error(
                        ReconnectErrorCode::ClockContractViolation,
                        ReconnectStage::Opening,
                        "Fastboot reconnect clock moved backwards during open",
                        target,
                        state,
                        native_code,
                        certainty));
                }
                last_time = after_open;
                if (cancellation.stop_requested()) {
                    return std::unexpected(make_error(
                        ReconnectErrorCode::Cancelled,
                        ReconnectStage::Opening,
                        "Fastboot reconnect was cancelled while opening the replacement session",
                        target,
                        state,
                        native_code,
                        certainty));
                }
                if (after_open >= deadline) {
                    return std::unexpected(make_error(
                        ReconnectErrorCode::DeadlineExceeded,
                        ReconnectStage::Opening,
                        "Fastboot reconnect deadline expired during open",
                        target,
                        state,
                        native_code,
                        certainty));
                }

                if (!opened.has_value()) {
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
                    if (certainty ==
                        protocol::TransferCertainty::PartialOrUnknown) {
                        return std::unexpected(make_error(
                            ReconnectErrorCode::OpenOutcomeUncertain,
                            ReconnectStage::Opening,
                            "Fastboot reconnect opener reported a successful session with an uncertain aggregate transfer outcome",
                            target,
                            state,
                            0,
                            certainty));
                    }
                    if (opened->session == nullptr) {
                        state.observed_identity = opened->verified_identity;
                        return std::unexpected(make_error(
                            ReconnectErrorCode::OpenContractViolation,
                            ReconnectStage::Opening,
                            "Fastboot reconnect opener returned a null session",
                            target,
                            state,
                            0,
                            certainty));
                    }
                    const auto& verified = opened->verified_identity;
                    state.observed_identity = verified;
                    if (!valid_physical_port(verified.physical_port) ||
                        !valid_optional_identity_text(verified.serial) ||
                        !valid_usb_fingerprint(verified.usb_fingerprint) ||
                        !valid_identity_text(verified.product) ||
                        !valid_mode(verified.mode)) {
                        return std::unexpected(make_error(
                            ReconnectErrorCode::OpenContractViolation,
                            ReconnectStage::Verification,
                            "Fastboot reconnect opener returned an invalid verified identity",
                            target,
                            state,
                            0,
                            certainty));
                    }
                    if (verified.physical_port != target.physical_port ||
                        verified.usb_fingerprint !=
                            candidate->usb_fingerprint ||
                        (target.serial.has_value() &&
                         verified.serial != target.serial) ||
                        (candidate->serial.has_value() &&
                         verified.serial != candidate->serial)) {
                        return std::unexpected(make_error(
                            ReconnectErrorCode::DeviceChangedDuringOpen,
                            ReconnectStage::Verification,
                            "Fastboot device identity changed between discovery and exclusive open",
                            target,
                            state,
                            0,
                            certainty));
                    }
                    if (verified.product != target.product) {
                        return std::unexpected(make_error(
                            ReconnectErrorCode::ProductMismatch,
                            ReconnectStage::Verification,
                            "Fastboot reconnect product does not match the original device",
                            target,
                            state,
                            0,
                            certainty));
                    }
                    if (!reconnect_usb_fingerprint_allowed(
                            target,
                            verified.usb_fingerprint,
                            verified.mode)) {
                        return std::unexpected(make_error(
                            ReconnectErrorCode::UsbFingerprintMismatch,
                            ReconnectStage::Verification,
                            "Fastboot reconnect live mode does not authorize the changed USB fingerprint",
                            target,
                            state,
                            0,
                            certainty));
                    }
                    if (verified.mode == target.previous_mode &&
                        target.previous_mode != target.required_mode) {
                        state.last_observation =
                            ReconnectObservation::PreviousModePresent;
                        retry = true;
                    } else if (verified.mode != target.required_mode) {
                        return std::unexpected(make_error(
                            ReconnectErrorCode::OpenContractViolation,
                            ReconnectStage::Verification,
                            "Fastboot reconnect opener returned an unexpected verified mode",
                            target,
                            state,
                            0,
                            certainty));
                    } else if (!reconnect_identity_matches_target(
                                   target, verified)) {
                        return std::unexpected(make_error(
                            ReconnectErrorCode::OpenContractViolation,
                            ReconnectStage::Verification,
                            "Fastboot reconnect verified identity does not match the reconnect target",
                            target,
                            state,
                            0,
                            certainty));
                    } else if (opened->session->state() !=
                               protocol::SessionState::Ready) {
                        return std::unexpected(make_error(
                            ReconnectErrorCode::OpenContractViolation,
                            ReconnectStage::Verification,
                            "Fastboot reconnect opener returned a session that is not ready",
                            target,
                            state,
                            0,
                            certainty));
                    } else {
                        state.last_observation =
                            ReconnectObservation::RequiredModePresent;
                        const auto before_publish = waiter_.now();
                        if (before_publish < last_time) {
                            return std::unexpected(make_error(
                                ReconnectErrorCode::ClockContractViolation,
                                ReconnectStage::Verification,
                                "Fastboot reconnect clock moved backwards before publishing the session",
                                target,
                                state,
                                0,
                                certainty));
                        }
                        last_time = before_publish;
                        if (cancellation.stop_requested()) {
                            return std::unexpected(make_error(
                                ReconnectErrorCode::Cancelled,
                                ReconnectStage::Verification,
                                "Fastboot reconnect was cancelled before publishing the session",
                                target,
                                state,
                                0,
                                certainty));
                        }
                        if (before_publish >= deadline) {
                            return std::unexpected(make_error(
                                ReconnectErrorCode::DeadlineExceeded,
                                ReconnectStage::Verification,
                                "Fastboot reconnect deadline expired before publishing the session",
                                target,
                                state,
                                0,
                                certainty));
                        }
                        return ReconnectedSession{
                            .identity = std::move(opened->verified_identity),
                            .session = std::move(opened->session),
                            .outbound_certainty = certainty,
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
                state,
                0,
                retry_certainty));
        }
        const auto before_wait = waiter_.now();
        if (before_wait < last_time) {
            return std::unexpected(make_error(
                ReconnectErrorCode::ClockContractViolation,
                ReconnectStage::Backoff,
                "Fastboot reconnect clock moved backwards before backoff",
                target,
                state,
                0,
                retry_certainty));
        }
        last_time = before_wait;
        const auto remaining = remaining_time(before_wait, deadline);
        if (remaining <= std::chrono::milliseconds::zero()) {
            return std::unexpected(make_error(
                ReconnectErrorCode::DeadlineExceeded,
                ReconnectStage::Backoff,
                deadline_message(state.last_observation),
                target,
                state,
                0,
                retry_certainty));
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
                wait_result.native_code,
                retry_certainty));
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
                wait_result.native_code,
                retry_certainty));
        }
        if (wait_result.status != ReconnectWaitStatus::Elapsed) {
            return std::unexpected(make_error(
                ReconnectErrorCode::WaitFailed,
                ReconnectStage::Backoff,
                "Fastboot reconnect waiter returned an invalid status",
                target,
                state,
                wait_result.native_code,
                retry_certainty));
        }
        const auto after_wait = waiter_.now();
        if (after_wait <= before_wait) {
            return std::unexpected(make_error(
                ReconnectErrorCode::ClockContractViolation,
                ReconnectStage::Backoff,
                "Fastboot reconnect waiter elapsed without advancing its clock",
                target,
                state,
                0,
                retry_certainty));
        }
        last_time = after_wait;
        if (after_wait >= deadline) {
            return std::unexpected(make_error(
                ReconnectErrorCode::DeadlineExceeded,
                ReconnectStage::Backoff,
                deadline_message(state.last_observation),
                target,
                state,
                0,
                retry_certainty));
        }
        backoff = next_backoff(backoff, options.maximum_backoff);
    }
}

}  // namespace kairosboot::fastboot
