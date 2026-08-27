// SPDX-License-Identifier: MIT
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace kairosboot::transport {

struct UsbDeviceInfo;

using MacUsbTopologyClock = std::chrono::steady_clock;
using MacUsbTopologyTimePoint = MacUsbTopologyClock::time_point;

// Darwin's 32-bit IOKit locationID stores at most six four-bit hub ports.
// libusb 1.0.30 derives its macOS bus number from locationID >> 24, so bus
// zero is valid on this platform even though every port remains non-zero.
inline constexpr std::size_t kMaximumMacUsbTopologyDepth = 6U;

struct MacUsbInterfaceFingerprint final {
    std::uint8_t configuration_value{};
    std::uint8_t interface_number{};
    std::uint8_t alternate_setting{};
    std::uint8_t interface_class{};
    std::uint8_t interface_subclass{};
    std::uint8_t interface_protocol{};

    [[nodiscard]] bool operator==(
        const MacUsbInterfaceFingerprint&) const = default;
};

// Immutable identity copied from one libusb enumeration result. The address
// is intentionally transient, while bus + port_numbers is the physical key.
struct MacUsbTopologyQuery final {
    std::uint16_t vendor_id{};
    std::uint16_t product_id{};
    std::uint8_t bus_number{};
    std::uint8_t device_address{};
    std::vector<std::uint8_t> port_numbers;
    MacUsbInterfaceFingerprint interface_fingerprint;
    std::optional<std::string> serial_utf8;
    // USB descriptor text, not Fastboot getvar:product.
    std::optional<std::string> product_utf8;
};

struct MacUsbRegistryInterface final {
    std::uint64_t registry_entry_id{};
    MacUsbInterfaceFingerprint fingerprint;
    std::string registry_path;

    [[nodiscard]] bool operator==(const MacUsbRegistryInterface&) const = default;
};

// One immutable, device-scoped IORegistry observation. Production obtains two
// independently enumerated observations; discovery publishes topology only
// when the complete query-scoped snapshots are identical.
struct MacUsbRegistryNode final {
    std::uint64_t registry_entry_id{};
    std::uint64_t session_id{};
    std::uint64_t root_controller_registry_entry_id{};
    std::uint32_t location_id{};
    std::uint16_t vendor_id{};
    std::uint16_t product_id{};
    std::uint8_t bus_number{};
    std::uint8_t device_address{};
    std::vector<std::uint8_t> port_numbers;
    std::optional<std::string> serial_utf8;
    // USB descriptor text, not Fastboot getvar:product.
    std::optional<std::string> product_utf8;
    std::string registry_path;
    std::string root_controller_registry_path;
    std::vector<MacUsbRegistryInterface> interfaces;

    [[nodiscard]] bool operator==(const MacUsbRegistryNode&) const = default;
};

struct MacUsbTopology final {
    std::string physical_port_path;
    std::string root_controller_id;
    std::vector<std::uint8_t> hub_port_chain;
    std::uint64_t registry_entry_id{};
    std::uint64_t session_id{};
    std::uint64_t interface_registry_entry_id{};
    std::uint32_t location_id{};
    std::uint16_t vendor_id{};
    std::uint16_t product_id{};
    std::uint8_t bus_number{};
    std::uint8_t device_address{};
    MacUsbInterfaceFingerprint interface_fingerprint;
    std::optional<std::string> serial_utf8;
    std::optional<std::string> product_utf8;
    std::string registry_path;
    std::string interface_registry_path;
    std::string root_controller_registry_path;

    [[nodiscard]] bool operator==(const MacUsbTopology&) const = default;
};

enum class MacUsbTopologyErrorKind : std::uint8_t {
    InvalidArgument,
    UnsupportedPlatform,
    Cancelled,
    Timeout,
    NotFound,
    PermissionDenied,
    IoError,
    MalformedRegistry,
    IdentityChanged,
    IdentityMismatch,
    AmbiguousMapping,
    TopologyTooDeep,
    ResourceExhausted,
};

enum class MacUsbTopologyStage : std::uint8_t {
    Validation,
    DeviceEnumeration,
    DeviceSnapshot,
    InterfaceEnumeration,
    Hierarchy,
    FinalValidation,
    Correlation,
};

struct MacUsbTopologyError final {
    MacUsbTopologyErrorKind kind{MacUsbTopologyErrorKind::InvalidArgument};
    MacUsbTopologyStage stage{MacUsbTopologyStage::Validation};
    std::int32_t native_code{};
    std::string registry_path;
    std::string message;

    [[nodiscard]] bool operator==(const MacUsbTopologyError&) const = default;
};

// Injectable, read-only IORegistry seam. Implementations must honor the same
// absolute deadline and stop token for every enumeration. Returning a snapshot
// never authorizes opening a device or mutating any registry/system state.
class IMacUsbRegistryBackend {
public:
    virtual ~IMacUsbRegistryBackend() = default;

    [[nodiscard]] virtual std::expected<std::vector<MacUsbRegistryNode>,
                                        MacUsbTopologyError>
    snapshot(const MacUsbTopologyQuery& query,
             MacUsbTopologyTimePoint deadline,
             std::stop_token cancellation) const = 0;
};

class IokitMacUsbRegistryBackend final : public IMacUsbRegistryBackend {
public:
    [[nodiscard]] std::expected<std::vector<MacUsbRegistryNode>,
                                MacUsbTopologyError>
    snapshot(const MacUsbTopologyQuery& query,
             MacUsbTopologyTimePoint deadline,
             std::stop_token cancellation) const override;
};

class MacUsbTopologyDiscovery final {
public:
    explicit MacUsbTopologyDiscovery(
        const IMacUsbRegistryBackend& backend) noexcept;

    [[nodiscard]] std::expected<MacUsbTopology, MacUsbTopologyError>
    discover(const MacUsbTopologyQuery& query,
             MacUsbTopologyTimePoint deadline,
             std::stop_token cancellation = {}) const;

private:
    const IMacUsbRegistryBackend& backend_;
};

[[nodiscard]] MacUsbTopologyQuery make_macos_usb_topology_query(
    const UsbDeviceInfo& device,
    // Optional USB descriptor text; do not pass Fastboot getvar:product.
    std::optional<std::string_view> product_utf8 = std::nullopt);

[[nodiscard]] std::expected<std::string, MacUsbTopologyError>
canonical_macos_usb_port_path(
    std::uint8_t bus_number,
    const std::vector<std::uint8_t>& port_numbers);

struct MacUsbDecodedLocation final {
    std::uint8_t bus_number{};
    std::vector<std::uint8_t> port_numbers;

    [[nodiscard]] bool operator==(const MacUsbDecodedLocation&) const = default;
};

[[nodiscard]] std::expected<MacUsbDecodedLocation, MacUsbTopologyError>
decode_macos_usb_location_id(std::uint32_t location_id);

}  // namespace kairosboot::transport
