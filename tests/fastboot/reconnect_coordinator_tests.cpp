// SPDX-License-Identifier: MIT
#include "src/fastboot/reconnect_coordinator.hpp"

#include <chrono>
#include <cstddef>
#include <expected>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using kairosboot::fastboot::FastbootUsbMode;
using kairosboot::fastboot::IReconnectDiscovery;
using kairosboot::fastboot::IReconnectSessionOpener;
using kairosboot::fastboot::IReconnectWaiter;
using kairosboot::fastboot::OpenedReconnectSession;
using kairosboot::fastboot::ReconnectCandidate;
using kairosboot::fastboot::ReconnectCoordinator;
using kairosboot::fastboot::ReconnectDeviceIdentity;
using kairosboot::fastboot::ReconnectDiscoveryError;
using kairosboot::fastboot::ReconnectErrorCode;
using kairosboot::fastboot::ReconnectObservation;
using kairosboot::fastboot::ReconnectOpenError;
using kairosboot::fastboot::ReconnectOptions;
using kairosboot::fastboot::ReconnectStage;
using kairosboot::fastboot::ReconnectTarget;
using kairosboot::fastboot::ReconnectTimePoint;
using kairosboot::fastboot::ReconnectUsbFingerprint;
using kairosboot::fastboot::ReconnectWaitResult;
using kairosboot::fastboot::ReconnectWaitStatus;
using kairosboot::fastboot::UsbPhysicalPortPath;
using kairosboot::protocol::FastbootSession;
using kairosboot::protocol::ITransportSession;
using kairosboot::protocol::TransferCertainty;
using kairosboot::protocol::TransferResult;
using kairosboot::protocol::TransportStatus;

class CheckFailure final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

#define CHECK(condition)                                                                  \
    do {                                                                                  \
        if (!(condition)) {                                                               \
            throw CheckFailure(std::string("check failed: ") + #condition + " at line " + \
                               std::to_string(__LINE__));                                 \
        }                                                                                 \
    } while (false)

[[nodiscard]] UsbPhysicalPortPath port(
    const std::uint8_t bus,
    std::vector<std::uint8_t> ports) {
    return UsbPhysicalPortPath{
        .bus_number = bus,
        .ports = std::move(ports),
    };
}

[[nodiscard]] ReconnectUsbFingerprint fingerprint(
    const std::uint16_t vendor_id = 0x18D1,
    const std::uint16_t product_id = 0x4EE0,
    const std::uint8_t interface_number = 0) {
    return ReconnectUsbFingerprint{
        .vendor_id = vendor_id,
        .product_id = product_id,
        .interface_number = interface_number,
        .interface_class = 0xFF,
        .interface_subclass = 0x42,
        .interface_protocol = 0x03,
    };
}

[[nodiscard]] ReconnectCandidate candidate(
    UsbPhysicalPortPath physical_port,
    std::optional<std::string> serial = std::string{"SERIAL-A"},
    const ReconnectUsbFingerprint usb_fingerprint = fingerprint()) {
    return ReconnectCandidate{
        .physical_port = std::move(physical_port),
        .serial = std::move(serial),
        .usb_fingerprint = usb_fingerprint,
    };
}

[[nodiscard]] ReconnectDeviceIdentity identity(
    UsbPhysicalPortPath physical_port,
    std::optional<std::string> serial = std::string{"SERIAL-A"},
    std::string product = "product_a",
    const FastbootUsbMode mode = FastbootUsbMode::Fastbootd,
    const ReconnectUsbFingerprint usb_fingerprint = fingerprint()) {
    return ReconnectDeviceIdentity{
        .physical_port = std::move(physical_port),
        .serial = std::move(serial),
        .usb_fingerprint = usb_fingerprint,
        .product = std::move(product),
        .mode = mode,
    };
}

[[nodiscard]] ReconnectDeviceIdentity identity(
    const ReconnectCandidate& passive,
    std::string product = "product_a",
    const FastbootUsbMode mode = FastbootUsbMode::Fastbootd) {
    return identity(
        passive.physical_port,
        passive.serial,
        std::move(product),
        mode,
        passive.usb_fingerprint);
}

[[nodiscard]] ReconnectTarget target(
    const TransferCertainty certainty = TransferCertainty::FullyTransferred) {
    return ReconnectTarget{
        .physical_port = port(1, {2, 3}),
        .serial = std::string{"SERIAL-A"},
        .usb_fingerprint = fingerprint(),
        .product = "product_a",
        .previous_mode = FastbootUsbMode::Bootloader,
        .required_mode = FastbootUsbMode::Fastbootd,
        .preceding_operation_certainty = certainty,
    };
}

struct CloseState final {
    std::size_t close_calls{};
};

class NoopTransport final : public ITransportSession {
public:
    explicit NoopTransport(std::shared_ptr<CloseState> close_state)
        : close_state_(std::move(close_state)) {}

    [[nodiscard]] TransferResult write(
        std::span<const std::byte>,
        std::chrono::milliseconds) override {
        return unavailable();
    }

    [[nodiscard]] TransferResult read(
        std::span<std::byte>,
        std::chrono::milliseconds) override {
        return unavailable();
    }

    [[nodiscard]] TransferResult read_data(
        std::span<std::byte>,
        std::chrono::milliseconds) override {
        return unavailable();
    }

    void request_cancel() noexcept override {}

    void close() noexcept override {
        if (!closed_) {
            closed_ = true;
            ++close_state_->close_calls;
        }
    }

private:
    [[nodiscard]] static TransferResult unavailable() {
        return TransferResult{
            .status = TransportStatus::IoError,
            .transferred = 0,
            .certainty = TransferCertainty::NotTransferred,
            .truncated = false,
            .detail = "test transport has no protocol script",
            .native_code = 0,
        };
    }

    std::shared_ptr<CloseState> close_state_;
    bool closed_{};
};

using DiscoveryResult =
    std::expected<std::vector<ReconnectCandidate>, ReconnectDiscoveryError>;

class QueueDiscovery final : public IReconnectDiscovery {
public:
    std::vector<DiscoveryResult> steps;
    std::function<void(std::size_t, std::stop_token)> before_return;
    std::vector<ReconnectTimePoint> deadlines;
    std::size_t calls{};

    [[nodiscard]] DiscoveryResult discover(
        const ReconnectTimePoint deadline,
        const std::stop_token cancellation) override {
        ++calls;
        deadlines.push_back(deadline);
        if (before_return) {
            before_return(calls, cancellation);
        }
        if (calls <= steps.size()) {
            return steps[calls - 1U];
        }
        return std::vector<ReconnectCandidate>{};
    }
};

struct OpenAction final {
    std::optional<ReconnectOpenError> error{};
    std::optional<ReconnectDeviceIdentity> verified_identity{};
    bool null_session{};
    TransferCertainty outbound_certainty{TransferCertainty::FullyTransferred};
    bool close_session_before_return{};
    std::shared_ptr<CloseState> close_state{std::make_shared<CloseState>()};
};

class QueueOpener final : public IReconnectSessionOpener {
public:
    std::vector<OpenAction> actions;
    std::function<void(std::size_t, std::stop_token)> before_return;
    std::vector<ReconnectCandidate> candidates;
    std::vector<ReconnectTimePoint> deadlines;

    [[nodiscard]] std::expected<OpenedReconnectSession, ReconnectOpenError>
    open(const ReconnectCandidate& candidate,
         const ReconnectTimePoint deadline,
         const std::stop_token cancellation) override {
        candidates.push_back(candidate);
        deadlines.push_back(deadline);
        const auto call = candidates.size();
        if (before_return) {
            before_return(call, cancellation);
        }

        OpenAction action;
        if (call <= actions.size()) {
            action = actions[call - 1U];
        }
        if (action.error.has_value()) {
            return std::unexpected(*action.error);
        }
        auto verified = action.verified_identity.value_or(identity(candidate));
        std::unique_ptr<FastbootSession> session;
        if (!action.null_session) {
            session = std::make_unique<FastbootSession>(
                std::make_unique<NoopTransport>(action.close_state));
            if (action.close_session_before_return) {
                session->close();
            }
        }
        return OpenedReconnectSession{
            .verified_identity = std::move(verified),
            .session = std::move(session),
            .outbound_certainty = action.outbound_certainty,
        };
    }
};

class ManualWaiter final : public IReconnectWaiter {
public:
    mutable TimePoint current{};
    mutable std::size_t now_calls{};
    std::optional<std::size_t> advance_on_now_call;
    std::chrono::milliseconds advance_on_trigger{};
    std::optional<std::size_t> cancel_on_now_call;
    std::stop_source* cancel_on_now_source{};
    std::vector<std::chrono::milliseconds> waits;
    ReconnectWaitStatus forced_status{ReconnectWaitStatus::Elapsed};
    std::string forced_message;
    int forced_native_code{};
    std::stop_source* cancel_source{};
    bool advance_clock{true};

    [[nodiscard]] TimePoint now() const noexcept override {
        ++now_calls;
        if (advance_on_now_call == now_calls) {
            current += advance_on_trigger;
        }
        if (cancel_on_now_call == now_calls && cancel_on_now_source != nullptr) {
            cancel_on_now_source->request_stop();
        }
        return current;
    }

    [[nodiscard]] ReconnectWaitResult wait_for(
        const std::chrono::milliseconds duration,
        const std::stop_token) override {
        waits.push_back(duration);
        if (cancel_source != nullptr) {
            cancel_source->request_stop();
        }
        if (advance_clock) {
            current += duration;
        }
        return ReconnectWaitResult{
            .status = cancel_source != nullptr
                ? ReconnectWaitStatus::Cancelled
                : forced_status,
            .message = forced_message,
            .native_code = forced_native_code,
        };
    }
};

[[nodiscard]] ReconnectOptions options() {
    return ReconnectOptions{
        .initial_backoff = 10ms,
        .maximum_backoff = 40ms,
        .maximum_discovered_devices = 32,
    };
}

void mode_transition_tolerates_transient_enumeration_jitter() {
    const auto wanted = target();
    QueueDiscovery discovery;
    discovery.steps = {
        std::vector<ReconnectCandidate>{},
        std::vector<ReconnectCandidate>{candidate(wanted.physical_port)},
        std::vector<ReconnectCandidate>{},
        std::vector<ReconnectCandidate>{candidate(wanted.physical_port)},
    };
    QueueOpener opener;
    opener.actions = {
        OpenAction{
            .verified_identity = identity(
                wanted.physical_port,
                wanted.serial,
                wanted.product,
                FastbootUsbMode::Bootloader),
        },
        OpenAction{},
    };
    ManualWaiter waiter;
    ReconnectCoordinator coordinator(discovery, opener, waiter);

    auto result = coordinator.reconnect(
        wanted, waiter.now() + 1s, options());
    CHECK(result.has_value());
    CHECK(result->identity.physical_port == wanted.physical_port);
    CHECK(result->identity.mode == FastbootUsbMode::Fastbootd);
    CHECK(result->discovery_attempts == 4);
    CHECK(result->open_attempts == 2);
    CHECK(waiter.waits ==
          std::vector<std::chrono::milliseconds>({10ms, 20ms, 40ms}));
}

void duplicate_serials_are_disambiguated_only_by_physical_port() {
    const auto wanted = target();
    QueueDiscovery discovery;
    discovery.steps = {std::vector<ReconnectCandidate>{
        candidate(port(1, {2, 4}), wanted.serial),
        candidate(wanted.physical_port, wanted.serial),
    }};
    QueueOpener opener;
    ManualWaiter waiter;
    ReconnectCoordinator coordinator(discovery, opener, waiter);

    auto result = coordinator.reconnect(
        wanted, waiter.now() + 1s, options());
    CHECK(result.has_value());
    CHECK(opener.candidates.size() == 1);
    CHECK(opener.candidates.front().physical_port == wanted.physical_port);
}

void duplicate_entries_at_one_port_fail_closed() {
    const auto wanted = target();
    QueueDiscovery discovery;
    discovery.steps = {std::vector<ReconnectCandidate>{
        candidate(wanted.physical_port),
        candidate(wanted.physical_port),
    }};
    QueueOpener opener;
    ManualWaiter waiter;
    ReconnectCoordinator coordinator(discovery, opener, waiter);

    const auto result = coordinator.reconnect(
        wanted, waiter.now() + 1s, options());
    CHECK(!result.has_value());
    CHECK(result.error().code == ReconnectErrorCode::AmbiguousPhysicalPort);
    CHECK(result.error().stage == ReconnectStage::Selection);
    CHECK(opener.candidates.empty());
}

void occupied_port_never_follows_the_expected_serial_elsewhere() {
    const auto wanted = target();
    QueueDiscovery discovery;
    discovery.steps = {std::vector<ReconnectCandidate>{
        candidate(wanted.physical_port, std::string{"OTHER-SERIAL"}),
        candidate(port(1, {4}), wanted.serial),
    }};
    QueueOpener opener;
    ManualWaiter waiter;
    ReconnectCoordinator coordinator(discovery, opener, waiter);

    const auto result = coordinator.reconnect(
        wanted, waiter.now() + 1s, options());
    CHECK(!result.has_value());
    CHECK(result.error().code ==
          ReconnectErrorCode::PortOccupiedByDifferentDevice);
    CHECK(result.error().observed_candidate->serial ==
          std::optional<std::string>{"OTHER-SERIAL"});
    CHECK(opener.candidates.empty());
}

void wrong_product_is_rejected_after_exclusive_open() {
    const auto wanted = target();
    QueueDiscovery discovery;
    discovery.steps = {std::vector<ReconnectCandidate>{candidate(
        wanted.physical_port, wanted.serial)}};
    QueueOpener opener;
    auto close_state = std::make_shared<CloseState>();
    opener.actions = {OpenAction{
        .verified_identity = identity(
            wanted.physical_port, wanted.serial, "product_b"),
        .close_state = close_state,
    }};
    ManualWaiter waiter;
    ReconnectCoordinator coordinator(discovery, opener, waiter);

    const auto result = coordinator.reconnect(
        wanted, waiter.now() + 1s, options());
    CHECK(!result.has_value());
    CHECK(result.error().code == ReconnectErrorCode::ProductMismatch);
    CHECK(result.error().reconnect_outbound_certainty ==
          TransferCertainty::FullyTransferred);
    CHECK(opener.candidates.size() == 1);
    CHECK(close_state->close_calls == 1);
}

void identity_change_during_open_closes_the_cross_wired_session() {
    const auto wanted = target();
    QueueDiscovery discovery;
    discovery.steps = {std::vector<ReconnectCandidate>{candidate(
        wanted.physical_port)}};
    auto close_state = std::make_shared<CloseState>();
    QueueOpener opener;
    opener.actions = {OpenAction{
        .error = std::nullopt,
        .verified_identity = identity(port(1, {9})),
        .null_session = false,
        .close_state = close_state,
    }};
    ManualWaiter waiter;
    ReconnectCoordinator coordinator(discovery, opener, waiter);

    const auto result = coordinator.reconnect(
        wanted, waiter.now() + 1s, options());
    CHECK(!result.has_value());
    CHECK(result.error().code == ReconnectErrorCode::DeviceChangedDuringOpen);
    CHECK(result.error().stage == ReconnectStage::Verification);
    CHECK(result.error().observed_identity->physical_port == port(1, {9}));
    CHECK(close_state->close_calls == 1);
}

void disappearing_device_expires_at_the_exact_deadline() {
    const auto wanted = target();
    QueueDiscovery discovery;
    QueueOpener opener;
    ManualWaiter waiter;
    ReconnectCoordinator coordinator(discovery, opener, waiter);

    const auto result = coordinator.reconnect(
        wanted, waiter.now() + 25ms, options());
    CHECK(!result.has_value());
    CHECK(result.error().code == ReconnectErrorCode::DeadlineExceeded);
    CHECK(result.error().last_observation ==
          ReconnectObservation::DeviceAbsent);
    CHECK(result.error().discovery_attempts == 2);
    CHECK(waiter.waits ==
          std::vector<std::chrono::milliseconds>({10ms, 15ms}));
    CHECK(waiter.now() == IReconnectWaiter::TimePoint{} + 25ms);
}

void not_transferred_open_race_retries_from_discovery() {
    const auto wanted = target();
    const auto passive = candidate(wanted.physical_port);
    QueueDiscovery discovery;
    discovery.steps = {
        std::vector<ReconnectCandidate>{passive},
        std::vector<ReconnectCandidate>{},
        std::vector<ReconnectCandidate>{passive},
    };
    QueueOpener opener;
    opener.actions = {
        OpenAction{
            .error = ReconnectOpenError{
                .message = "device vanished before exclusive open",
                .native_code = -4,
                .retryable = true,
                .outbound_certainty = TransferCertainty::NotTransferred,
            },
            .verified_identity = std::nullopt,
            .null_session = false,
            .close_state = std::make_shared<CloseState>(),
        },
        OpenAction{},
    };
    ManualWaiter waiter;
    ReconnectCoordinator coordinator(discovery, opener, waiter);

    auto result = coordinator.reconnect(
        wanted, waiter.now() + 1s, options());
    CHECK(result.has_value());
    CHECK(result->discovery_attempts == 3);
    CHECK(result->open_attempts == 2);
    CHECK(waiter.waits ==
          std::vector<std::chrono::milliseconds>({10ms, 20ms}));
}

void uncertain_open_is_never_retried() {
    const auto wanted = target();
    QueueDiscovery discovery;
    discovery.steps = {std::vector<ReconnectCandidate>{candidate(
        wanted.physical_port)}};
    QueueOpener opener;
    opener.actions = {OpenAction{
        .error = ReconnectOpenError{
            .message = "probe response was truncated",
            .native_code = -1,
            .retryable = true,
            .outbound_certainty = TransferCertainty::PartialOrUnknown,
        },
        .verified_identity = std::nullopt,
        .null_session = false,
        .close_state = std::make_shared<CloseState>(),
    }};
    ManualWaiter waiter;
    ReconnectCoordinator coordinator(discovery, opener, waiter);

    const auto result = coordinator.reconnect(
        wanted, waiter.now() + 1s, options());
    CHECK(!result.has_value());
    CHECK(result.error().code == ReconnectErrorCode::OpenOutcomeUncertain);
    CHECK(result.error().open_attempts == 1);
    CHECK(result.error().reconnect_outbound_certainty ==
          TransferCertainty::PartialOrUnknown);
    CHECK(result.error().open_error->message == "probe response was truncated");
    CHECK(waiter.waits.empty());
}

void uncertain_preceding_operation_never_starts_discovery() {
    const auto wanted = target(TransferCertainty::PartialOrUnknown);
    QueueDiscovery discovery;
    QueueOpener opener;
    ManualWaiter waiter;
    ReconnectCoordinator coordinator(discovery, opener, waiter);

    const auto result = coordinator.reconnect(
        wanted, waiter.now() + 1s, options());
    CHECK(!result.has_value());
    CHECK(result.error().code == ReconnectErrorCode::UnsafePreviousOutcome);
    CHECK(result.error().preceding_operation_certainty ==
          TransferCertainty::PartialOrUnknown);
    CHECK(result.error().reconnect_outbound_certainty ==
          TransferCertainty::NotTransferred);
    CHECK(discovery.calls == 0);
}

void cancellation_is_observed_before_and_during_discovery() {
    {
        const auto wanted = target();
        QueueDiscovery discovery;
        QueueOpener opener;
        ManualWaiter waiter;
        ReconnectCoordinator coordinator(discovery, opener, waiter);
        std::stop_source stop;
        stop.request_stop();
        const auto result = coordinator.reconnect(
            wanted, waiter.now() + 1s, options(), stop.get_token());
        CHECK(!result.has_value());
        CHECK(result.error().code == ReconnectErrorCode::Cancelled);
        CHECK(discovery.calls == 0);
    }
    {
        const auto wanted = target();
        QueueDiscovery discovery;
        QueueOpener opener;
        ManualWaiter waiter;
        ReconnectCoordinator coordinator(discovery, opener, waiter);
        std::stop_source stop;
        discovery.before_return = [&](const std::size_t, const std::stop_token) {
            stop.request_stop();
        };
        const auto result = coordinator.reconnect(
            wanted, waiter.now() + 1s, options(), stop.get_token());
        CHECK(!result.has_value());
        CHECK(result.error().code == ReconnectErrorCode::Cancelled);
        CHECK(result.error().stage == ReconnectStage::Discovery);
        CHECK(discovery.calls == 1);
        CHECK(opener.candidates.empty());
    }
}

void cancellation_interrupts_backoff() {
    const auto wanted = target();
    QueueDiscovery discovery;
    QueueOpener opener;
    ManualWaiter waiter;
    std::stop_source stop;
    waiter.cancel_source = &stop;
    ReconnectCoordinator coordinator(discovery, opener, waiter);

    const auto result = coordinator.reconnect(
        wanted, waiter.now() + 1s, options(), stop.get_token());
    CHECK(!result.has_value());
    CHECK(result.error().code == ReconnectErrorCode::Cancelled);
    CHECK(result.error().stage == ReconnectStage::Backoff);
    CHECK(result.error().discovery_attempts == 1);
    CHECK(waiter.waits == std::vector<std::chrono::milliseconds>({10ms}));
}

void cancellation_during_open_closes_the_unpublished_session() {
    const auto wanted = target();
    QueueDiscovery discovery;
    discovery.steps = {std::vector<ReconnectCandidate>{candidate(
        wanted.physical_port)}};
    auto close_state = std::make_shared<CloseState>();
    QueueOpener opener;
    opener.actions = {OpenAction{
        .error = std::nullopt,
        .verified_identity = std::nullopt,
        .null_session = false,
        .close_state = close_state,
    }};
    std::stop_source stop;
    opener.before_return = [&](const std::size_t, const std::stop_token) {
        stop.request_stop();
    };
    ManualWaiter waiter;
    ReconnectCoordinator coordinator(discovery, opener, waiter);

    const auto result = coordinator.reconnect(
        wanted, waiter.now() + 1s, options(), stop.get_token());
    CHECK(!result.has_value());
    CHECK(result.error().code == ReconnectErrorCode::Cancelled);
    CHECK(result.error().stage == ReconnectStage::Opening);
    CHECK(close_state->close_calls == 1);
}

void previous_mode_that_never_changes_times_out_without_opening() {
    const auto wanted = target();
    const auto passive = candidate(wanted.physical_port, wanted.serial);
    const auto bootloader = identity(
        passive, wanted.product, FastbootUsbMode::Bootloader);
    QueueDiscovery discovery;
    discovery.steps = {
        std::vector<ReconnectCandidate>{passive},
        std::vector<ReconnectCandidate>{passive},
    };
    QueueOpener opener;
    opener.actions = {
        OpenAction{.verified_identity = bootloader},
        OpenAction{.verified_identity = bootloader},
    };
    ManualWaiter waiter;
    ReconnectCoordinator coordinator(discovery, opener, waiter);

    const auto result = coordinator.reconnect(
        wanted, waiter.now() + 25ms, options());
    CHECK(!result.has_value());
    CHECK(result.error().code == ReconnectErrorCode::DeadlineExceeded);
    CHECK(result.error().last_observation ==
          ReconnectObservation::PreviousModePresent);
    CHECK(opener.candidates.size() == 2);
}

void permanent_discovery_and_open_failures_are_structured() {
    {
        const auto wanted = target();
        QueueDiscovery discovery;
        discovery.steps = {std::unexpected(ReconnectDiscoveryError{
            .message = "platform enumeration failed",
            .native_code = 91,
            .retryable = false,
        })};
        QueueOpener opener;
        ManualWaiter waiter;
        ReconnectCoordinator coordinator(discovery, opener, waiter);
        const auto result = coordinator.reconnect(
            wanted, waiter.now() + 1s, options());
        CHECK(!result.has_value());
        CHECK(result.error().code == ReconnectErrorCode::DiscoveryFailed);
        CHECK(result.error().native_code == 91);
        CHECK(result.error().discovery_error->message ==
              "platform enumeration failed");
    }
    {
        const auto wanted = target();
        QueueDiscovery discovery;
        discovery.steps = {std::vector<ReconnectCandidate>{candidate(
            wanted.physical_port)}};
        QueueOpener opener;
        opener.actions = {OpenAction{
            .error = ReconnectOpenError{
                .message = "interface claim denied",
                .native_code = 5,
                .retryable = false,
                .outbound_certainty = TransferCertainty::NotTransferred,
            },
            .verified_identity = std::nullopt,
            .null_session = false,
            .close_state = std::make_shared<CloseState>(),
        }};
        ManualWaiter waiter;
        ReconnectCoordinator coordinator(discovery, opener, waiter);
        const auto result = coordinator.reconnect(
            wanted, waiter.now() + 1s, options());
        CHECK(!result.has_value());
        CHECK(result.error().code == ReconnectErrorCode::OpenFailed);
        CHECK(result.error().native_code == 5);
        CHECK(result.error().reconnect_outbound_certainty ==
              TransferCertainty::NotTransferred);
    }
}

void null_or_wrong_product_open_results_fail_closed() {
    {
        const auto wanted = target();
        QueueDiscovery discovery;
        discovery.steps = {std::vector<ReconnectCandidate>{candidate(
            wanted.physical_port)}};
        QueueOpener opener;
        opener.actions = {OpenAction{
            .error = std::nullopt,
            .verified_identity = std::nullopt,
            .null_session = true,
            .close_state = std::make_shared<CloseState>(),
        }};
        ManualWaiter waiter;
        ReconnectCoordinator coordinator(discovery, opener, waiter);
        const auto result = coordinator.reconnect(
            wanted, waiter.now() + 1s, options());
        CHECK(!result.has_value());
        CHECK(result.error().code == ReconnectErrorCode::OpenContractViolation);
    }
    {
        const auto wanted = target();
        QueueDiscovery discovery;
        discovery.steps = {std::vector<ReconnectCandidate>{candidate(
            wanted.physical_port)}};
        QueueOpener opener;
        opener.actions = {OpenAction{
            .error = std::nullopt,
            .verified_identity = identity(
                wanted.physical_port, wanted.serial, "product_b"),
            .null_session = false,
            .close_state = std::make_shared<CloseState>(),
        }};
        ManualWaiter waiter;
        ReconnectCoordinator coordinator(discovery, opener, waiter);
        const auto result = coordinator.reconnect(
            wanted, waiter.now() + 1s, options());
        CHECK(!result.has_value());
        CHECK(result.error().code == ReconnectErrorCode::ProductMismatch);
        CHECK(result.error().observed_identity->product == "product_b");
    }
}

void waiter_and_clock_contract_failures_cannot_spin() {
    {
        const auto wanted = target();
        QueueDiscovery discovery;
        QueueOpener opener;
        ManualWaiter waiter;
        waiter.forced_status = ReconnectWaitStatus::Failed;
        waiter.forced_message = "manual wait backend failed";
        waiter.forced_native_code = 77;
        ReconnectCoordinator coordinator(discovery, opener, waiter);
        const auto result = coordinator.reconnect(
            wanted, waiter.now() + 1s, options());
        CHECK(!result.has_value());
        CHECK(result.error().code == ReconnectErrorCode::WaitFailed);
        CHECK(result.error().native_code == 77);
    }
    {
        const auto wanted = target();
        QueueDiscovery discovery;
        QueueOpener opener;
        ManualWaiter waiter;
        waiter.advance_clock = false;
        ReconnectCoordinator coordinator(discovery, opener, waiter);
        const auto result = coordinator.reconnect(
            wanted, waiter.now() + 1s, options());
        CHECK(!result.has_value());
        CHECK(result.error().code ==
              ReconnectErrorCode::ClockContractViolation);
        CHECK(discovery.calls == 1);
    }
}

void invalid_identity_and_backoff_never_touch_discovery() {
    {
        auto wanted = target();
        wanted.serial = std::string{};
        QueueDiscovery discovery;
        QueueOpener opener;
        ManualWaiter waiter;
        ReconnectCoordinator coordinator(discovery, opener, waiter);
        const auto result = coordinator.reconnect(
            wanted, waiter.now() + 1s, options());
        CHECK(!result.has_value());
        CHECK(result.error().code == ReconnectErrorCode::InvalidArgument);
        CHECK(discovery.calls == 0);
    }
    {
        const auto wanted = target();
        QueueDiscovery discovery;
        QueueOpener opener;
        ManualWaiter waiter;
        ReconnectCoordinator coordinator(discovery, opener, waiter);
        auto invalid_options = options();
        invalid_options.initial_backoff = 0ms;
        const auto result = coordinator.reconnect(
            wanted, waiter.now() + 1s, invalid_options);
        CHECK(!result.has_value());
        CHECK(result.error().code == ReconnectErrorCode::InvalidArgument);
        CHECK(discovery.calls == 0);
    }
}

void same_mode_reconnect_is_supported_when_identity_is_exact() {
    auto wanted = target();
    wanted.previous_mode = FastbootUsbMode::Fastbootd;
    wanted.required_mode = FastbootUsbMode::Fastbootd;
    QueueDiscovery discovery;
    discovery.steps = {std::vector<ReconnectCandidate>{candidate(
        wanted.physical_port)}};
    QueueOpener opener;
    ManualWaiter waiter;
    ReconnectCoordinator coordinator(discovery, opener, waiter);

    auto result = coordinator.reconnect(
        wanted, waiter.now() + 1s, options());
    CHECK(result.has_value());
    CHECK(result->discovery_attempts == 1);
    CHECK(waiter.waits.empty());
}

void fastbootd_to_bootloader_transition_is_symmetric() {
    auto wanted = target();
    wanted.previous_mode = FastbootUsbMode::Fastbootd;
    wanted.required_mode = FastbootUsbMode::Bootloader;
    const auto passive = candidate(wanted.physical_port, wanted.serial);
    QueueDiscovery discovery;
    discovery.steps = {
        std::vector<ReconnectCandidate>{passive},
        std::vector<ReconnectCandidate>{passive},
    };
    QueueOpener opener;
    opener.actions = {
        OpenAction{
            .verified_identity = identity(
                passive, wanted.product, FastbootUsbMode::Fastbootd),
        },
        OpenAction{
            .verified_identity = identity(
                passive, wanted.product, FastbootUsbMode::Bootloader),
        },
    };
    ManualWaiter waiter;
    ReconnectCoordinator coordinator(discovery, opener, waiter);

    auto result = coordinator.reconnect(
        wanted, waiter.now() + 1s, options());
    CHECK(result.has_value());
    CHECK(result->identity.mode == FastbootUsbMode::Bootloader);
    CHECK(result->discovery_attempts == 2);
    CHECK(waiter.waits == std::vector<std::chrono::milliseconds>({10ms}));
}

void absolute_deadline_is_forwarded_and_checked_after_dependencies() {
    {
        const auto wanted = target();
        QueueDiscovery discovery;
        discovery.steps = {std::vector<ReconnectCandidate>{candidate(
            wanted.physical_port)}};
        QueueOpener opener;
        ManualWaiter waiter;
        const auto deadline = waiter.current + 5ms;
        discovery.before_return = [&](const std::size_t, const std::stop_token) {
            waiter.current = deadline;
        };
        ReconnectCoordinator coordinator(discovery, opener, waiter);

        const auto result = coordinator.reconnect(wanted, deadline, options());
        CHECK(!result.has_value());
        CHECK(result.error().code == ReconnectErrorCode::DeadlineExceeded);
        CHECK(result.error().stage == ReconnectStage::Discovery);
        CHECK(discovery.deadlines == std::vector<ReconnectTimePoint>{deadline});
        CHECK(opener.candidates.empty());
    }
    {
        const auto wanted = target();
        QueueDiscovery discovery;
        discovery.steps = {std::vector<ReconnectCandidate>{candidate(
            wanted.physical_port)}};
        auto close_state = std::make_shared<CloseState>();
        QueueOpener opener;
        opener.actions = {OpenAction{.close_state = close_state}};
        ManualWaiter waiter;
        const auto deadline = waiter.current + 5ms;
        opener.before_return = [&](const std::size_t, const std::stop_token) {
            waiter.current = deadline;
        };
        ReconnectCoordinator coordinator(discovery, opener, waiter);

        const auto result = coordinator.reconnect(wanted, deadline, options());
        CHECK(!result.has_value());
        CHECK(result.error().code == ReconnectErrorCode::DeadlineExceeded);
        CHECK(result.error().stage == ReconnectStage::Opening);
        CHECK(result.error().reconnect_outbound_certainty ==
              TransferCertainty::FullyTransferred);
        CHECK(discovery.deadlines == std::vector<ReconnectTimePoint>{deadline});
        CHECK(opener.deadlines == std::vector<ReconnectTimePoint>{deadline});
        CHECK(close_state->close_calls == 1);
    }
}

void deadline_and_cancellation_are_rechecked_before_session_publish() {
    {
        const auto wanted = target();
        QueueDiscovery discovery;
        discovery.steps = {std::vector<ReconnectCandidate>{candidate(
            wanted.physical_port)}};
        auto close_state = std::make_shared<CloseState>();
        QueueOpener opener;
        opener.actions = {OpenAction{.close_state = close_state}};
        ManualWaiter waiter;
        const auto deadline = waiter.current + 5ms;
        waiter.advance_on_now_call = 6;
        waiter.advance_on_trigger = 5ms;
        ReconnectCoordinator coordinator(discovery, opener, waiter);

        const auto result = coordinator.reconnect(wanted, deadline, options());
        CHECK(!result.has_value());
        CHECK(result.error().code == ReconnectErrorCode::DeadlineExceeded);
        CHECK(result.error().stage == ReconnectStage::Verification);
        CHECK(result.error().reconnect_outbound_certainty ==
              TransferCertainty::FullyTransferred);
        CHECK(close_state->close_calls == 1);
    }
    {
        const auto wanted = target();
        QueueDiscovery discovery;
        discovery.steps = {std::vector<ReconnectCandidate>{candidate(
            wanted.physical_port)}};
        auto close_state = std::make_shared<CloseState>();
        QueueOpener opener;
        opener.actions = {OpenAction{.close_state = close_state}};
        ManualWaiter waiter;
        std::stop_source stop;
        waiter.cancel_on_now_call = 6;
        waiter.cancel_on_now_source = &stop;
        ReconnectCoordinator coordinator(discovery, opener, waiter);

        const auto result = coordinator.reconnect(
            wanted, waiter.current + 1s, options(), stop.get_token());
        CHECK(!result.has_value());
        CHECK(result.error().code == ReconnectErrorCode::Cancelled);
        CHECK(result.error().stage == ReconnectStage::Verification);
        CHECK(result.error().reconnect_outbound_certainty ==
              TransferCertainty::FullyTransferred);
        CHECK(close_state->close_calls == 1);
    }
}

void cancellation_preserves_uncertain_open_error() {
    const auto wanted = target();
    QueueDiscovery discovery;
    discovery.steps = {std::vector<ReconnectCandidate>{candidate(
        wanted.physical_port)}};
    QueueOpener opener;
    opener.actions = {OpenAction{
        .error = ReconnectOpenError{
            .message = "probe transfer became uncertain",
            .native_code = 73,
            .retryable = true,
            .outbound_certainty = TransferCertainty::PartialOrUnknown,
        },
    }};
    std::stop_source stop;
    opener.before_return = [&](const std::size_t, const std::stop_token) {
        stop.request_stop();
    };
    ManualWaiter waiter;
    ReconnectCoordinator coordinator(discovery, opener, waiter);

    const auto result = coordinator.reconnect(
        wanted, waiter.current + 1s, options(), stop.get_token());
    CHECK(!result.has_value());
    CHECK(result.error().code == ReconnectErrorCode::Cancelled);
    CHECK(result.error().stage == ReconnectStage::Opening);
    CHECK(result.error().native_code == 73);
    CHECK(result.error().open_error.has_value());
    CHECK(result.error().open_error->message ==
          "probe transfer became uncertain");
    CHECK(result.error().reconnect_outbound_certainty ==
          TransferCertainty::PartialOrUnknown);
    CHECK(waiter.waits.empty());
}

void missing_serial_uses_path_fingerprint_and_post_open_identity() {
    {
        auto wanted = target();
        wanted.serial.reset();
        const auto passive = candidate(
            wanted.physical_port, std::nullopt, wanted.usb_fingerprint);
        QueueDiscovery discovery;
        discovery.steps = {std::vector<ReconnectCandidate>{passive}};
        QueueOpener opener;
        ManualWaiter waiter;
        ReconnectCoordinator coordinator(discovery, opener, waiter);

        auto result = coordinator.reconnect(
            wanted, waiter.current + 1s, options());
        CHECK(result.has_value());
        CHECK(!result->identity.serial.has_value());
        CHECK(result->identity.product == wanted.product);
    }
    {
        const auto wanted = target();
        const auto passive = candidate(
            wanted.physical_port, std::nullopt, wanted.usb_fingerprint);
        QueueDiscovery discovery;
        discovery.steps = {std::vector<ReconnectCandidate>{passive}};
        QueueOpener opener;
        opener.actions = {OpenAction{
            .verified_identity = identity(
                wanted.physical_port,
                wanted.serial,
                wanted.product,
                wanted.required_mode,
                wanted.usb_fingerprint),
        }};
        ManualWaiter waiter;
        ReconnectCoordinator coordinator(discovery, opener, waiter);

        auto result = coordinator.reconnect(
            wanted, waiter.current + 1s, options());
        CHECK(result.has_value());
        CHECK(result->identity.serial == wanted.serial);
    }
}

void usb_fingerprint_is_checked_before_and_after_open() {
    {
        const auto wanted = target();
        QueueDiscovery discovery;
        discovery.steps = {std::vector<ReconnectCandidate>{candidate(
            wanted.physical_port,
            wanted.serial,
            fingerprint(0x18D1, 0x4EE1))}};
        QueueOpener opener;
        ManualWaiter waiter;
        ReconnectCoordinator coordinator(discovery, opener, waiter);

        const auto result = coordinator.reconnect(
            wanted, waiter.current + 1s, options());
        CHECK(!result.has_value());
        CHECK(result.error().code ==
              ReconnectErrorCode::UsbFingerprintMismatch);
        CHECK(opener.candidates.empty());
    }
    {
        const auto wanted = target();
        const auto passive = candidate(wanted.physical_port);
        QueueDiscovery discovery;
        discovery.steps = {std::vector<ReconnectCandidate>{passive}};
        auto close_state = std::make_shared<CloseState>();
        QueueOpener opener;
        opener.actions = {OpenAction{
            .verified_identity = identity(
                wanted.physical_port,
                wanted.serial,
                wanted.product,
                wanted.required_mode,
                fingerprint(0x18D1, 0x4EE1)),
            .close_state = close_state,
        }};
        ManualWaiter waiter;
        ReconnectCoordinator coordinator(discovery, opener, waiter);

        const auto result = coordinator.reconnect(
            wanted, waiter.current + 1s, options());
        CHECK(!result.has_value());
        CHECK(result.error().code ==
              ReconnectErrorCode::DeviceChangedDuringOpen);
        CHECK(close_state->close_calls == 1);
    }
}

void only_ready_sessions_are_published() {
    const auto wanted = target();
    QueueDiscovery discovery;
    discovery.steps = {std::vector<ReconnectCandidate>{candidate(
        wanted.physical_port)}};
    auto close_state = std::make_shared<CloseState>();
    QueueOpener opener;
    opener.actions = {OpenAction{
        .close_session_before_return = true,
        .close_state = close_state,
    }};
    ManualWaiter waiter;
    ReconnectCoordinator coordinator(discovery, opener, waiter);

    const auto result = coordinator.reconnect(
        wanted, waiter.current + 1s, options());
    CHECK(!result.has_value());
    CHECK(result.error().code == ReconnectErrorCode::OpenContractViolation);
    CHECK(result.error().stage == ReconnectStage::Verification);
    CHECK(close_state->close_calls == 1);
}

void configured_attempt_limits_fail_before_counters_can_wrap() {
    {
        const auto wanted = target();
        QueueDiscovery discovery;
        QueueOpener opener;
        ManualWaiter waiter;
        ReconnectCoordinator coordinator(discovery, opener, waiter);
        auto limited = options();
        limited.maximum_discovery_attempts = 1;

        const auto result = coordinator.reconnect(
            wanted, waiter.current + 1s, limited);
        CHECK(!result.has_value());
        CHECK(result.error().code == ReconnectErrorCode::AttemptLimitExceeded);
        CHECK(result.error().stage == ReconnectStage::Discovery);
        CHECK(result.error().discovery_attempts == 1);
        CHECK(discovery.calls == 1);
    }
    {
        const auto wanted = target();
        const auto passive = candidate(wanted.physical_port);
        QueueDiscovery discovery;
        discovery.steps = {
            std::vector<ReconnectCandidate>{passive},
            std::vector<ReconnectCandidate>{passive},
        };
        QueueOpener opener;
        opener.actions = {OpenAction{
            .verified_identity = identity(
                passive, wanted.product, FastbootUsbMode::Bootloader),
        }};
        ManualWaiter waiter;
        ReconnectCoordinator coordinator(discovery, opener, waiter);
        auto limited = options();
        limited.maximum_open_attempts = 1;

        const auto result = coordinator.reconnect(
            wanted, waiter.current + 1s, limited);
        CHECK(!result.has_value());
        CHECK(result.error().code == ReconnectErrorCode::AttemptLimitExceeded);
        CHECK(result.error().stage == ReconnectStage::Opening);
        CHECK(result.error().open_attempts == 1);
        CHECK(opener.candidates.size() == 1);
    }
}

void latest_dependency_failure_replaces_prior_open_failure() {
    const auto wanted = target();
    QueueDiscovery discovery;
    discovery.steps = {
        std::vector<ReconnectCandidate>{candidate(wanted.physical_port)},
        std::unexpected(ReconnectDiscoveryError{
            .message = "later discovery failure",
            .native_code = 22,
            .retryable = false,
        }),
    };
    QueueOpener opener;
    opener.actions = {OpenAction{
        .error = ReconnectOpenError{
            .message = "earlier open race",
            .native_code = 11,
            .retryable = true,
            .outbound_certainty = TransferCertainty::NotTransferred,
        },
    }};
    ManualWaiter waiter;
    ReconnectCoordinator coordinator(discovery, opener, waiter);

    const auto result = coordinator.reconnect(
        wanted, waiter.current + 1s, options());
    CHECK(!result.has_value());
    CHECK(result.error().code == ReconnectErrorCode::DiscoveryFailed);
    CHECK(result.error().native_code == 22);
    CHECK(result.error().last_observation ==
          ReconnectObservation::DiscoveryUnavailable);
    CHECK(result.error().discovery_error.has_value());
    CHECK(!result.error().open_error.has_value());
}

}  // namespace

int main() {
    const std::vector<std::pair<const char*, std::function<void()>>> tests{
        {"mode transition jitter", mode_transition_tolerates_transient_enumeration_jitter},
        {"duplicate serial by port", duplicate_serials_are_disambiguated_only_by_physical_port},
        {"ambiguous physical port", duplicate_entries_at_one_port_fail_closed},
        {"occupied physical port", occupied_port_never_follows_the_expected_serial_elsewhere},
        {"wrong product", wrong_product_is_rejected_after_exclusive_open},
        {"cross-wire during open", identity_change_during_open_closes_the_cross_wired_session},
        {"disappearing device deadline", disappearing_device_expires_at_the_exact_deadline},
        {"open race retry", not_transferred_open_race_retries_from_discovery},
        {"uncertain open", uncertain_open_is_never_retried},
        {"uncertain preceding operation", uncertain_preceding_operation_never_starts_discovery},
        {"discovery cancellation", cancellation_is_observed_before_and_during_discovery},
        {"backoff cancellation", cancellation_interrupts_backoff},
        {"open cancellation", cancellation_during_open_closes_the_unpublished_session},
        {"mode transition timeout", previous_mode_that_never_changes_times_out_without_opening},
        {"structured dependency failures", permanent_discovery_and_open_failures_are_structured},
        {"opener contract", null_or_wrong_product_open_results_fail_closed},
        {"waiter clock contract", waiter_and_clock_contract_failures_cannot_spin},
        {"argument validation", invalid_identity_and_backoff_never_touch_discovery},
        {"same mode reconnect", same_mode_reconnect_is_supported_when_identity_is_exact},
        {"reverse mode reconnect", fastbootd_to_bootloader_transition_is_symmetric},
        {"dependency absolute deadline", absolute_deadline_is_forwarded_and_checked_after_dependencies},
        {"pre-publish cancellation and deadline", deadline_and_cancellation_are_rechecked_before_session_publish},
        {"cancelled uncertain open", cancellation_preserves_uncertain_open_error},
        {"missing serial reconnect", missing_serial_uses_path_fingerprint_and_post_open_identity},
        {"USB fingerprint verification", usb_fingerprint_is_checked_before_and_after_open},
        {"ready session contract", only_ready_sessions_are_published},
        {"attempt limits", configured_attempt_limits_fail_before_counters_can_wrap},
        {"latest dependency failure", latest_dependency_failure_replaces_prior_open_failure},
    };

    std::size_t failures = 0;
    for (const auto& [name, test] : tests) {
        try {
            test();
            std::cout << "[PASS] " << name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "[FAIL] " << name << ": " << error.what() << '\n';
        }
    }
    return failures == 0 ? 0 : 1;
}
