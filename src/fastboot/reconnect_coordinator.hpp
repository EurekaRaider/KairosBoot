// SPDX-License-Identifier: MIT
#pragma once

#include "src/protocol/fastboot_protocol.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace kairosboot::fastboot {

struct UsbPhysicalPortPath final {
    std::uint8_t bus_number{};
    std::vector<std::uint8_t> ports;

    [[nodiscard]] bool operator==(const UsbPhysicalPortPath&) const = default;
};

enum class FastbootUsbMode : std::uint8_t {
    Bootloader,
    Fastbootd,
};

// Discovery identity for one Fastboot USB interface. The physical port is the
// reconnect key. serial/product are only secondary identity checks and are
// never used to select a device on a different port.
struct ReconnectDeviceIdentity final {
    UsbPhysicalPortPath physical_port;
    std::string serial;
    std::string product;
    FastbootUsbMode mode{FastbootUsbMode::Bootloader};
};

struct ReconnectTarget final {
    UsbPhysicalPortPath physical_port;
    std::string serial;
    std::string product;
    FastbootUsbMode previous_mode{FastbootUsbMode::Bootloader};
    FastbootUsbMode required_mode{FastbootUsbMode::Fastbootd};
    // The coordinator will not hand a new session to the caller when the
    // preceding protocol operation has an uncertain outbound outcome.
    protocol::TransferCertainty preceding_operation_certainty{
        protocol::TransferCertainty::FullyTransferred};
};

struct ReconnectDiscoveryError final {
    std::string message;
    int native_code{};
    bool retryable{};
};

struct ReconnectOpenError final {
    std::string message;
    int native_code{};
    bool retryable{};
    // Certainty for bytes emitted while opening/probing the replacement
    // session. Only NotTransferred failures may be retried.
    protocol::TransferCertainty outbound_certainty{
        protocol::TransferCertainty::NotTransferred};
};

struct OpenedReconnectSession final {
    // The opener must obtain this identity after it exclusively opens the
    // candidate. The coordinator compares it again to close scan/open races.
    ReconnectDeviceIdentity verified_identity;
    std::unique_ptr<protocol::FastbootSession> session;
};

class IReconnectDiscovery {
public:
    virtual ~IReconnectDiscovery() = default;

    [[nodiscard]] virtual std::expected<std::vector<ReconnectDeviceIdentity>,
                                        ReconnectDiscoveryError>
    discover(std::stop_token cancellation) = 0;
};

class IReconnectSessionOpener {
public:
    virtual ~IReconnectSessionOpener() = default;

    [[nodiscard]] virtual std::expected<OpenedReconnectSession,
                                        ReconnectOpenError>
    open(const ReconnectDeviceIdentity& candidate,
         std::stop_token cancellation) = 0;
};

enum class ReconnectWaitStatus : std::uint8_t {
    Elapsed,
    Cancelled,
    Failed,
};

struct ReconnectWaitResult final {
    ReconnectWaitStatus status{ReconnectWaitStatus::Elapsed};
    std::string message;
    int native_code{};
};

class IReconnectWaiter {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    virtual ~IReconnectWaiter() = default;

    [[nodiscard]] virtual TimePoint now() const noexcept = 0;
    [[nodiscard]] virtual ReconnectWaitResult wait_for(
        std::chrono::milliseconds duration,
        std::stop_token cancellation) = 0;
};

// Interruptible steady-clock waiter for production callers. Tests can inject a
// manual clock to make deadline and backoff behavior deterministic.
class SteadyReconnectWaiter final : public IReconnectWaiter {
public:
    [[nodiscard]] TimePoint now() const noexcept override;
    [[nodiscard]] ReconnectWaitResult wait_for(
        std::chrono::milliseconds duration,
        std::stop_token cancellation) override;
};

struct ReconnectOptions final {
    std::chrono::milliseconds initial_backoff{50};
    std::chrono::milliseconds maximum_backoff{1'000};
    std::size_t maximum_discovered_devices{4'096};
};

enum class ReconnectErrorCode : std::uint8_t {
    InvalidArgument,
    UnsafePreviousOutcome,
    Cancelled,
    DeadlineExceeded,
    DiscoveryFailed,
    DiscoveryContractViolation,
    AmbiguousPhysicalPort,
    PortOccupiedByDifferentDevice,
    ProductMismatch,
    OpenFailed,
    OpenOutcomeUncertain,
    OpenContractViolation,
    DeviceChangedDuringOpen,
    WaitFailed,
    ClockContractViolation,
};

enum class ReconnectStage : std::uint8_t {
    Validation,
    Discovery,
    Selection,
    Backoff,
    Opening,
    Verification,
};

enum class ReconnectObservation : std::uint8_t {
    None,
    DeviceAbsent,
    PreviousModePresent,
    RequiredModePresent,
};

struct ReconnectError final {
    ReconnectErrorCode code{ReconnectErrorCode::InvalidArgument};
    ReconnectStage stage{ReconnectStage::Validation};
    std::string message;
    std::size_t discovery_attempts{};
    std::size_t open_attempts{};
    ReconnectObservation last_observation{ReconnectObservation::None};
    std::optional<ReconnectDeviceIdentity> observed_identity;
    std::optional<ReconnectDiscoveryError> discovery_error;
    std::optional<ReconnectOpenError> open_error;
    int native_code{};
    protocol::TransferCertainty preceding_operation_certainty{
        protocol::TransferCertainty::NotTransferred};
    // Discovery/selection never emits protocol bytes. Opening failures copy
    // the opener's certainty so callers can map NotSent/PartialOrUnknown
    // without inferring a safe resume point.
    protocol::TransferCertainty reconnect_outbound_certainty{
        protocol::TransferCertainty::NotTransferred};
};

struct ReconnectedSession final {
    ReconnectDeviceIdentity identity;
    std::unique_ptr<protocol::FastbootSession> session;
    std::size_t discovery_attempts{};
    std::size_t open_attempts{};
};

// Coordinates one USB re-enumeration. It does not own a public operation and
// does not execute Fastboot commands. Selection always starts at the exact
// physical port, then validates serial/product/mode both before and after the
// injected opener obtains an exclusive session.
class ReconnectCoordinator final {
public:
    ReconnectCoordinator(
        IReconnectDiscovery& discovery,
        IReconnectSessionOpener& opener,
        IReconnectWaiter& waiter) noexcept;

    [[nodiscard]] std::expected<ReconnectedSession, ReconnectError> reconnect(
        const ReconnectTarget& target,
        IReconnectWaiter::TimePoint deadline,
        ReconnectOptions options = {},
        std::stop_token cancellation = {});

private:
    IReconnectDiscovery& discovery_;
    IReconnectSessionOpener& opener_;
    IReconnectWaiter& waiter_;
};

}  // namespace kairosboot::fastboot
