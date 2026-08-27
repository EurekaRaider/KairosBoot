// SPDX-License-Identifier: MIT
#pragma once

#include "src/fastboot/reconnect_coordinator.hpp"
#include "src/fleet/job_plan.hpp"
#include "src/transport/libusb_runtime.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace kairosboot::fleet {

using DevicePreflightClock = std::chrono::steady_clock;
using DevicePreflightTimePoint = DevicePreflightClock::time_point;

// Complete passive USB identity that is stable across the enumeration/open
// boundary. Product and Fastboot mode deliberately do not belong here.
struct DevicePreflightUsbFingerprint final {
    std::uint16_t vendor_id{};
    std::uint16_t product_id{};
    std::uint8_t configuration_value{};
    std::uint8_t interface_number{};
    std::uint8_t alternate_setting{};
    std::uint8_t interface_class{};
    std::uint8_t interface_subclass{};
    std::uint8_t interface_protocol{};
    std::uint8_t bulk_out_endpoint{};
    std::uint16_t bulk_out_max_packet_size{};
    std::uint8_t bulk_in_endpoint{};
    std::uint16_t bulk_in_max_packet_size{};

    [[nodiscard]] bool operator==(
        const DevicePreflightUsbFingerprint&) const = default;
};

struct DevicePreflightUsbIdentity final {
    std::string physical_port_path;
    std::string root_controller_id;
    std::vector<std::uint8_t> hub_port_chain;
    std::uint8_t bus_number{};
    std::optional<std::string> serial;
    DevicePreflightUsbFingerprint usb_fingerprint;

    [[nodiscard]] bool operator==(
        const DevicePreflightUsbIdentity&) const = default;
};

enum class DevicePreflightOpenErrorCode : std::uint8_t {
    Cancelled,
    DeadlineExceeded,
    NotFound,
    Busy,
    PermissionDenied,
    DriverUnavailable,
    TransportFailure,
    ResourceExhausted,
    UnexpectedFailure,
};

struct DevicePreflightOpenError final {
    DevicePreflightOpenErrorCode code{
        DevicePreflightOpenErrorCode::UnexpectedFailure};
    std::string message;
    int native_code{};
    protocol::TransferCertainty outbound_certainty{
        protocol::TransferCertainty::NotTransferred};
};

struct OpenedDevicePreflightSession final {
    // This identity must be captured only after the candidate has been opened
    // exclusively. Preflight compares it with the enumeration snapshot before
    // issuing any protocol probe.
    DevicePreflightUsbIdentity verified_usb_identity;
    std::unique_ptr<protocol::FastbootSession> session;
};

class IDevicePreflightSessionOpener {
public:
    virtual ~IDevicePreflightSessionOpener() = default;

    // Implementations must exclusively open this exact enumeration snapshot,
    // re-read its passive identity, and bound native work by the same absolute
    // deadline. No destructive Fastboot command is permitted here.
    [[nodiscard]] virtual std::expected<OpenedDevicePreflightSession,
                                        DevicePreflightOpenError>
    open(const transport::UsbDeviceInfo& device,
         DevicePreflightTimePoint deadline,
         std::stop_token cancellation) = 0;
};

struct DevicePreflightProbeResult final {
    std::string product;
    fastboot::FastbootUsbMode mode{fastboot::FastbootUsbMode::Bootloader};
    // A custom probe must explicitly attest that both values came from the
    // live, exclusive Fastboot session. The built-in probe sets these only
    // after getvar:product and getvar:is-userspace complete on the wire.
    bool product_query_completed{};
    bool mode_query_completed{};
};

enum class DevicePreflightProbeErrorCode : std::uint8_t {
    Cancelled,
    DeadlineExceeded,
    ProtocolFailure,
    DeviceRejected,
    InvalidResponse,
    ResourceExhausted,
    UnexpectedFailure,
};

struct DevicePreflightProbeError final {
    DevicePreflightProbeErrorCode code{
        DevicePreflightProbeErrorCode::UnexpectedFailure};
    std::string message;
    int native_code{};
    protocol::TransferCertainty outbound_certainty{
        protocol::TransferCertainty::NotTransferred};
};

class IDevicePreflightProbe {
public:
    virtual ~IDevicePreflightProbe() = default;

    // Implementations may issue only non-destructive identity queries. They
    // must query product and mode through this exact exclusive session and
    // honour both deadline and cancellation.
    [[nodiscard]] virtual std::expected<DevicePreflightProbeResult,
                                        DevicePreflightProbeError>
    probe(protocol::FastbootSession& session,
          DevicePreflightTimePoint deadline,
          std::stop_token cancellation) = 0;
};

// Production protocol probe. USB descriptor product text is never accepted as
// input and therefore cannot substitute for Fastboot getvar:product.
class FastbootDevicePreflightProbe final : public IDevicePreflightProbe {
public:
    [[nodiscard]] std::expected<DevicePreflightProbeResult,
                                DevicePreflightProbeError>
    probe(protocol::FastbootSession& session,
          DevicePreflightTimePoint deadline,
          std::stop_token cancellation) override;
};

enum class DevicePreflightOutcomeCode : std::uint8_t {
    Ready,
    ProductMismatch,
};

struct DevicePreflightOutcome final {
    DevicePreflightOutcomeCode code{DevicePreflightOutcomeCode::Ready};
    std::size_t target_index{};
    std::string target_name;
    DevicePreflightUsbIdentity usb_identity;
    std::string expected_product;
    std::string observed_product;
    fastboot::FastbootUsbMode observed_mode{
        fastboot::FastbootUsbMode::Bootloader};
};

enum class DevicePreflightErrorKind : std::uint8_t {
    InvalidArgument,
    Cancelled,
    DeadlineExceeded,
    SnapshotLimitExceeded,
    UnreliableTopology,
    DuplicatePhysicalPath,
    DuplicateSerial,
    MissingSelectorDevice,
    DeviceMatchesMultipleTargets,
    MissingTargetDevice,
    OpenFailed,
    OpenContractViolation,
    DeviceChangedDuringOpen,
    ProbeFailed,
    ProbeContractViolation,
    ProductMismatch,
    ResourceExhausted,
    UnexpectedFailure,
};

enum class DevicePreflightStage : std::uint8_t {
    Validation,
    Snapshot,
    Selection,
    Opening,
    LiveIdentity,
    ProductBarrier,
};

struct DevicePreflightError final {
    DevicePreflightErrorKind kind{DevicePreflightErrorKind::UnexpectedFailure};
    DevicePreflightStage stage{DevicePreflightStage::Validation};
    std::string message;
    std::optional<std::size_t> target_index;
    std::optional<std::size_t> snapshot_index;
    int native_code{};
    protocol::TransferCertainty outbound_certainty{
        protocol::TransferCertainty::NotTransferred};
    // Populated for ProductMismatch. Ready entries prove that those devices
    // passed live identity validation, but no destructive gate is published
    // when any entry mismatches.
    std::vector<DevicePreflightOutcome> outcomes;
};

class PreparedDeviceSession final {
public:
    PreparedDeviceSession(const PreparedDeviceSession&) = delete;
    PreparedDeviceSession& operator=(const PreparedDeviceSession&) = delete;
    PreparedDeviceSession(PreparedDeviceSession&&) noexcept = default;
    PreparedDeviceSession& operator=(PreparedDeviceSession&&) noexcept = default;
    ~PreparedDeviceSession() = default;

    [[nodiscard]] std::size_t target_index() const noexcept;
    [[nodiscard]] std::string_view target_name() const noexcept;
    [[nodiscard]] std::string_view expected_product() const noexcept;
    [[nodiscard]] std::string_view observed_product() const noexcept;
    [[nodiscard]] fastboot::FastbootUsbMode observed_mode() const noexcept;
    [[nodiscard]] const DevicePreflightUsbIdentity& usb_identity() const noexcept;
    [[nodiscard]] protocol::FastbootSession& session() noexcept;

private:
    PreparedDeviceSession(std::size_t target_index,
                          std::string target_name,
                          std::string expected_product,
                          std::string observed_product,
                          fastboot::FastbootUsbMode observed_mode,
                          DevicePreflightUsbIdentity usb_identity,
                          std::unique_ptr<protocol::FastbootSession> session) noexcept;

    std::size_t target_index_{};
    std::string target_name_;
    std::string expected_product_;
    std::string observed_product_;
    fastboot::FastbootUsbMode observed_mode_{
        fastboot::FastbootUsbMode::Bootloader};
    DevicePreflightUsbIdentity usb_identity_;
    std::unique_ptr<protocol::FastbootSession> session_;

    friend std::expected<class PreparedDeviceBatch, DevicePreflightError>
    preflight_fleet_devices(const JobPlan&,
                            std::span<const transport::UsbDeviceInfo>,
                            IDevicePreflightSessionOpener&,
                            IDevicePreflightProbe&,
                            DevicePreflightTimePoint,
                            std::stop_token) noexcept;
};

// This move-only batch is the destructive gate. It can be constructed only
// after every selected device passes snapshot uniqueness, exclusive-open
// identity, live mode, and live product validation.
class PreparedDeviceBatch final {
public:
    PreparedDeviceBatch(const PreparedDeviceBatch&) = delete;
    PreparedDeviceBatch& operator=(const PreparedDeviceBatch&) = delete;
    PreparedDeviceBatch(PreparedDeviceBatch&&) noexcept = default;
    PreparedDeviceBatch& operator=(PreparedDeviceBatch&&) noexcept = default;
    ~PreparedDeviceBatch() = default;

    [[nodiscard]] std::span<const PreparedDeviceSession> devices() const noexcept;
    [[nodiscard]] std::span<PreparedDeviceSession> devices() noexcept;

private:
    explicit PreparedDeviceBatch(
        std::vector<PreparedDeviceSession> devices) noexcept;

    std::vector<PreparedDeviceSession> devices_;

    friend std::expected<PreparedDeviceBatch, DevicePreflightError>
    preflight_fleet_devices(const JobPlan&,
                            std::span<const transport::UsbDeviceInfo>,
                            IDevicePreflightSessionOpener&,
                            IDevicePreflightProbe&,
                            DevicePreflightTimePoint,
                            std::stop_token) noexcept;
};

// Unmatched devices are intentionally ignored. Every selector value must
// resolve to exactly one physical device, every target must resolve to at least
// one device, and a physical device may belong to only one target.
[[nodiscard]] std::expected<PreparedDeviceBatch, DevicePreflightError>
preflight_fleet_devices(
    const JobPlan& plan,
    std::span<const transport::UsbDeviceInfo> snapshot,
    IDevicePreflightSessionOpener& opener,
    IDevicePreflightProbe& probe,
    DevicePreflightTimePoint deadline,
    std::stop_token cancellation = {}) noexcept;

}  // namespace kairosboot::fleet
