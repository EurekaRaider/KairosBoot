// SPDX-License-Identifier: MIT
#pragma once

#include "src/fastboot/reconnect_coordinator.hpp"
#include "src/transport/libusb_runtime.hpp"

#include <chrono>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <variant>
#include <vector>

namespace kairosboot::fastboot {

using DevicePreflightClock = std::chrono::steady_clock;
using DevicePreflightTimePoint = DevicePreflightClock::time_point;

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
    std::uint8_t device_address{};
    std::uint64_t backend_session_id{};
    std::optional<std::string> serial;
    DevicePreflightUsbFingerprint usb_fingerprint;
    std::variant<transport::LinuxUsbTopology,
                 transport::WindowsUsbTopology,
                 transport::MacUsbTopology>
        platform_attestation;

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
    DevicePreflightUsbIdentity verified_usb_identity;
    std::unique_ptr<protocol::FastbootSession> session;
};

class IDevicePreflightSessionOpener {
public:
    virtual ~IDevicePreflightSessionOpener() = default;

    [[nodiscard]] virtual std::expected<OpenedDevicePreflightSession,
                                        DevicePreflightOpenError>
    open(const transport::UsbDeviceInfo& device,
         DevicePreflightTimePoint deadline,
         std::stop_token cancellation) = 0;
};

[[nodiscard]] std::expected<std::unique_ptr<IDevicePreflightSessionOpener>,
                            DevicePreflightOpenError>
make_libusb_device_preflight_session_opener(
    std::shared_ptr<transport::LibusbRuntime> runtime,
    std::shared_ptr<transport::BufferBudget> buffer_budget,
    transport::TransferRingConfig data_ring,
    protocol::SessionOptions session_options = {}) noexcept;

struct DevicePreflightProbeResult final {
    std::string product;
    FastbootUsbMode mode{FastbootUsbMode::Bootloader};
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

    [[nodiscard]] virtual std::expected<DevicePreflightProbeResult,
                                        DevicePreflightProbeError>
    probe(protocol::FastbootSession& session,
          DevicePreflightTimePoint deadline,
          std::stop_token cancellation) = 0;
};

class FastbootDevicePreflightProbe final : public IDevicePreflightProbe {
public:
    [[nodiscard]] std::expected<DevicePreflightProbeResult,
                                DevicePreflightProbeError>
    probe(protocol::FastbootSession& session,
          DevicePreflightTimePoint deadline,
          std::stop_token cancellation) override;
};

}  // namespace kairosboot::fastboot
