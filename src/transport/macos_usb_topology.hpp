// SPDX-License-Identifier: MIT
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace kairosboot::transport {

struct UsbDeviceInfo;

using MacUsbTopologyClock = std::chrono::steady_clock;
using MacUsbTopologyTimePoint = MacUsbTopologyClock::time_point;

// USB 2.0 permits at most seven tiers. libusb builds Darwin port paths from
// each device's UInt8 PortNum and parent sessionID links; locationID supplies
// only the zero-based bus number through its top byte.
inline constexpr std::size_t kMaximumMacUsbTopologyDepth = 7U;

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

// Immutable identity copied from one libusb enumeration result. Darwin
// sessionID is the primary IORegistry correlation key; transient address and
// bus + port_numbers remain mandatory full-snapshot verification fields.
struct MacUsbTopologyQuery final {
    std::uint16_t vendor_id{};
    std::uint16_t product_id{};
    std::uint8_t bus_number{};
    std::uint8_t device_address{};
    std::uint64_t session_id{};
    std::vector<std::uint8_t> port_numbers;
    MacUsbInterfaceFingerprint interface_fingerprint;
    std::optional<std::string> serial_utf8;
    // USB descriptor text, not Fastboot getvar:product.
    std::optional<std::string> product_utf8;
};

// Every entry represents one libusb device generation and carries all of its
// selected interfaces in deterministic enumeration order.
struct MacUsbTopologyDeviceQuery final {
    std::vector<MacUsbTopologyQuery> interfaces;
};

struct MacUsbRegistryInterface final {
    std::uint64_t registry_entry_id{};
    MacUsbInterfaceFingerprint fingerprint;
    std::string registry_path;

    [[nodiscard]] bool operator==(const MacUsbRegistryInterface&) const = default;
};

enum class MacUsbRegistryEntryKind : std::uint8_t {
    UsbDevice,
    UsbRootHub,
    HostController,
    LegacyHostController,
    Other,
};

// Nearest-to-farthest IOService-plane ancestry. Keeping the classification in
// the immutable snapshot makes Intel IOUSBController and modern
// IOUSBHostController layouts testable without depending on host hardware.
struct MacUsbRegistryAncestor final {
    std::uint64_t registry_entry_id{};
    MacUsbRegistryEntryKind kind{MacUsbRegistryEntryKind::Other};
    std::string registry_path;

    [[nodiscard]] bool operator==(const MacUsbRegistryAncestor&) const = default;
};

// One immutable, device-scoped IORegistry observation. Production obtains two
// platform-wide observations for the requested batch; discovery publishes a
// device only when that device's complete snapshots are identical.
struct MacUsbRegistryNode final {
    std::uint64_t registry_entry_id{};
    std::uint64_t session_id{};
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
    std::vector<MacUsbRegistryAncestor> service_ancestry;
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

using MacUsbTopologyDeviceResult =
    std::expected<std::vector<MacUsbTopology>, MacUsbTopologyError>;

// Injectable, read-only IORegistry seam. Implementations must honor the same
// absolute deadline and stop token for every enumeration. Returning a snapshot
// never authorizes opening a device or mutating any registry/system state.
class IMacUsbRegistryBackend {
public:
    virtual ~IMacUsbRegistryBackend() = default;

    [[nodiscard]] virtual std::expected<std::vector<MacUsbRegistryNode>,
                                        MacUsbTopologyError>
    snapshot(std::span<const MacUsbTopologyQuery> device_queries,
             MacUsbTopologyTimePoint deadline,
             std::stop_token cancellation) const = 0;
};

// Lower-level injectable seam used by the production IOKit backend. A source
// represents one native enumeration pass; a successful pass with no iterator
// is represented by an empty vector.
class IMacUsbRegistrySnapshotSource {
public:
    virtual ~IMacUsbRegistrySnapshotSource() = default;

    [[nodiscard]] virtual std::expected<std::vector<MacUsbRegistryNode>,
                                        MacUsbTopologyError>
    snapshot(std::span<const MacUsbTopologyQuery> device_queries,
             MacUsbTopologyTimePoint deadline,
             std::stop_token cancellation) const = 0;
};

class IokitMacUsbRegistryBackend final : public IMacUsbRegistryBackend {
public:
    IokitMacUsbRegistryBackend() = default;
    explicit IokitMacUsbRegistryBackend(
        const IMacUsbRegistrySnapshotSource& source) noexcept;
    IokitMacUsbRegistryBackend(IMacUsbRegistrySnapshotSource&&) = delete;

    [[nodiscard]] std::expected<std::vector<MacUsbRegistryNode>,
                                MacUsbTopologyError>
    snapshot(std::span<const MacUsbTopologyQuery> device_queries,
             MacUsbTopologyTimePoint deadline,
             std::stop_token cancellation) const override;

private:
    const IMacUsbRegistrySnapshotSource* source_{};
};

class MacUsbTopologyDiscovery final {
public:
    explicit MacUsbTopologyDiscovery(
        const IMacUsbRegistryBackend& backend) noexcept;

    [[nodiscard]] std::expected<MacUsbTopology, MacUsbTopologyError>
    discover(const MacUsbTopologyQuery& query,
             MacUsbTopologyTimePoint deadline,
             std::stop_token cancellation = {}) const;

    // Resolve every matching interface of one libusb device from the same two
    // immutable IORegistry observations. The queries must carry identical
    // device identity and distinct complete interface fingerprints.
    [[nodiscard]] std::expected<std::vector<MacUsbTopology>,
                                MacUsbTopologyError>
    discover_device(std::span<const MacUsbTopologyQuery> queries,
                    MacUsbTopologyTimePoint deadline,
                    std::stop_token cancellation = {}) const;

    // Resolve every selected device from exactly two shared platform snapshot
    // passes. Per-device identity failures are returned in the corresponding
    // result; a batch-wide timeout, cancellation, or native-pass failure is the
    // outer error and never publishes a partial result vector.
    [[nodiscard]] std::expected<std::vector<MacUsbTopologyDeviceResult>,
                                MacUsbTopologyError>
    discover_devices(std::span<const MacUsbTopologyDeviceQuery> devices,
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

}  // namespace kairosboot::transport
