// SPDX-License-Identifier: MIT
#pragma once

#include "src/protocol/fastboot_protocol.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace kairosboot::fastboot {

using ReconnectClock = std::chrono::steady_clock;
using ReconnectTimePoint = ReconnectClock::time_point;

struct UsbPhysicalPortPath final {
    std::uint8_t bus_number{};
    std::vector<std::uint8_t> ports;

    [[nodiscard]] bool operator==(const UsbPhysicalPortPath&) const = default;
};

enum class FastbootUsbMode : std::uint8_t {
    Bootloader,
    Fastbootd,
};

struct ReconnectUsbFingerprint final {
    std::uint16_t vendor_id{};
    std::uint16_t product_id{};
    std::uint8_t interface_number{};
    std::uint8_t interface_class{};
    std::uint8_t interface_subclass{};
    std::uint8_t interface_protocol{};

    [[nodiscard]] bool operator==(const ReconnectUsbFingerprint&) const = default;
};

// Opaque, implementation-owned authorization to open one passive discovery
// result. Production discovery may attach a one-shot capability; coordinator
// fakes and non-USB implementations may leave it empty.
class ReconnectCandidateOpenCapability {
public:
    virtual ~ReconnectCandidateOpenCapability() = default;

protected:
    ReconnectCandidateOpenCapability() = default;
};

// Passive discovery result for one Fastboot USB interface. Discovery may use
// only USB topology/descriptors and must never emit Fastboot protocol bytes.
struct ReconnectCandidate final {
    UsbPhysicalPortPath physical_port;
    std::optional<std::string> serial;
    ReconnectUsbFingerprint usb_fingerprint;
    std::shared_ptr<const ReconnectCandidateOpenCapability> open_capability;
};

// Identity verified only after exclusively opening the candidate. Product and
// mode are protocol identities and therefore never belong to passive discovery.
struct ReconnectDeviceIdentity final {
    UsbPhysicalPortPath physical_port;
    std::optional<std::string> serial;
    ReconnectUsbFingerprint usb_fingerprint;
    std::string product;
    FastbootUsbMode mode{FastbootUsbMode::Bootloader};
};

enum class ReconnectUsbFingerprintPolicy : std::uint8_t {
    Exact,
    // A fully transferred bootloader/fastbootd transition may legitimately
    // re-enumerate with a different VID/PID/interface. Passive selection then
    // uses the unique physical port; the changed fingerprint is accepted only
    // after live serial/product/mode verification identifies required_mode.
    AllowChangeWithLiveIdentity,
};

struct ReconnectTarget final {
    UsbPhysicalPortPath physical_port;
    std::optional<std::string> serial;
    ReconnectUsbFingerprint usb_fingerprint;
    std::string product;
    FastbootUsbMode previous_mode{FastbootUsbMode::Bootloader};
    FastbootUsbMode required_mode{FastbootUsbMode::Fastbootd};
    ReconnectUsbFingerprintPolicy usb_fingerprint_policy{
        ReconnectUsbFingerprintPolicy::Exact};
    // The coordinator will not hand a new session to the caller when the
    // preceding protocol operation has an uncertain outbound outcome.
    protocol::TransferCertainty preceding_operation_certainty{
        protocol::TransferCertainty::FullyTransferred};
};

// Shared identity predicates used by the coordinator and higher-level actors.
// A missing verified_mode represents passive selection. In transition mode it
// may admit a changed descriptor only so exclusive open can perform live
// verification; a previous-mode live identity still requires the exact old
// fingerprint.
[[nodiscard]] bool reconnect_usb_fingerprint_allowed(
    const ReconnectTarget& target,
    const ReconnectUsbFingerprint& observed,
    std::optional<FastbootUsbMode> verified_mode = std::nullopt) noexcept;

[[nodiscard]] bool reconnect_identity_matches_target(
    const ReconnectTarget& target,
    const ReconnectDeviceIdentity& identity) noexcept;

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
        protocol::TransferCertainty::PartialOrUnknown};
};

struct OpenedReconnectSession final {
    // The opener must obtain this identity after it exclusively opens the
    // candidate. The coordinator compares it again to close scan/open races.
    ReconnectDeviceIdentity verified_identity;
    std::unique_ptr<protocol::FastbootSession> session;
    // Aggregate certainty for every protocol byte emitted while opening and
    // verifying this session. It remains authoritative if cancellation or the
    // absolute deadline wins immediately after open() returns.
    protocol::TransferCertainty outbound_certainty{
        protocol::TransferCertainty::PartialOrUnknown};
};

class IReconnectDiscovery {
public:
    virtual ~IReconnectDiscovery() = default;

    // Implementations must bound every native enumeration call by deadline.
    // They may inspect only passive USB metadata and must never emit protocol
    // bytes, so cancellation and timeout always remain NotTransferred.
    [[nodiscard]] virtual std::expected<std::vector<ReconnectCandidate>,
                                        ReconnectDiscoveryError>
    discover(ReconnectTimePoint deadline, std::stop_token cancellation) = 0;
};

class IReconnectSessionOpener {
public:
    virtual ~IReconnectSessionOpener() = default;

    // Implementations exclusively open one passive candidate and may issue only
    // non-destructive identity probes. Every native/protocol call is bounded by
    // the same absolute deadline. Error and success results report aggregate
    // outbound certainty for the complete open/probe attempt.
    [[nodiscard]] virtual std::expected<OpenedReconnectSession,
                                        ReconnectOpenError>
    open(const ReconnectCandidate& candidate,
         ReconnectTimePoint deadline,
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
    using Clock = ReconnectClock;
    using TimePoint = ReconnectTimePoint;

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
    std::size_t maximum_discovery_attempts{
        std::numeric_limits<std::size_t>::max()};
    std::size_t maximum_open_attempts{
        std::numeric_limits<std::size_t>::max()};
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
    UsbFingerprintMismatch,
    ProductMismatch,
    OpenFailed,
    OpenOutcomeUncertain,
    OpenContractViolation,
    DeviceChangedDuringOpen,
    WaitFailed,
    ClockContractViolation,
    AttemptLimitExceeded,
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
    DiscoveryUnavailable,
    DeviceAbsent,
    CandidatePresent,
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
    std::optional<ReconnectCandidate> observed_candidate;
    std::optional<ReconnectDeviceIdentity> observed_identity;
    std::optional<ReconnectDiscoveryError> discovery_error;
    std::optional<ReconnectOpenError> open_error;
    int native_code{};
    protocol::TransferCertainty preceding_operation_certainty{
        protocol::TransferCertainty::NotTransferred};
    // Discovery/selection never emits protocol bytes. Errors after an open()
    // result preserve that attempt's aggregate certainty so callers can map
    // NotSent/PartialOrUnknown without inferring a safe resume point.
    protocol::TransferCertainty reconnect_outbound_certainty{
        protocol::TransferCertainty::NotTransferred};
};

struct ReconnectedSession final {
    ReconnectDeviceIdentity identity;
    std::unique_ptr<protocol::FastbootSession> session;
    // Aggregate certainty for the successful open/identity probe. This stays
    // authoritative if cancellation or expiry wins before a higher-level actor
    // publishes the replacement session.
    protocol::TransferCertainty outbound_certainty{
        protocol::TransferCertainty::PartialOrUnknown};
    std::size_t discovery_attempts{};
    std::size_t open_attempts{};
};

// Performs the coordinator's complete side-effect-free request validation.
// Transition actors call this before emitting a reboot command so an invalid
// physical identity or retry policy can never be discovered only after the
// original session has been retired.
[[nodiscard]] std::expected<void, ReconnectError>
validate_reconnect_request(
    const ReconnectTarget& target,
    ReconnectOptions options = {});

// Coordinates one USB re-enumeration. It does not own a public operation and
// does not execute Fastboot commands. Selection always starts at the exact
// physical port, passive USB fingerprint, and any available serial. Product
// and mode are validated only after the opener obtains an exclusive session.
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
