// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace kairosboot::transport {

struct UsbDeviceInfo;

inline constexpr std::size_t kMaximumUsbTopologyDepth = 16U;

// Immutable identity copied from one libusb enumeration result. The device
// address is intentionally transient; bus + hub/port chain is the physical
// reconnect key for the lifetime of the host USB topology.
struct LinuxUsbTopologyQuery final {
    std::uint16_t vendor_id{};
    std::uint16_t product_id{};
    std::uint8_t bus_number{};
    std::uint8_t device_address{};
    std::vector<std::uint8_t> port_numbers;
    std::optional<std::string> serial_utf8;
    // USB descriptor text, not Fastboot getvar:product.
    std::optional<std::string> product_utf8;
};

// Topology is deliberately device-scoped. Interface/configuration/alternate
// identity stays on UsbDeviceInfo and is revalidated by open_bulk_out(); it
// must never be folded into, or deduplicated by, this physical-device key.
// Internal cross-layer topology value. physical_port_path is accepted by the
// existing DeviceSelection/Fleet model, root_controller_id is accepted by the
// weighted fleet scheduler, and bus_number + hub_port_chain maps directly to
// the reconnect coordinator's UsbPhysicalPortPath.
struct LinuxUsbTopology final {
    std::string physical_port_path;
    std::string root_controller_id;
    std::vector<std::uint8_t> hub_port_chain;
    std::uint16_t vendor_id{};
    std::uint16_t product_id{};
    std::uint8_t bus_number{};
    std::uint8_t device_address{};
    std::optional<std::string> serial_utf8;
    // USB descriptor text, not Fastboot getvar:product.
    std::optional<std::string> product_utf8;
    std::string sysfs_device_path;

    [[nodiscard]] bool operator==(const LinuxUsbTopology&) const = default;
};

enum class LinuxUsbTopologyErrorKind : std::uint8_t {
    InvalidArgument,
    UnsupportedPlatform,
    NotFound,
    PermissionDenied,
    UnsafePath,
    IoError,
    MalformedSysfs,
    IdentityChanged,
    IdentityMismatch,
    AmbiguousMapping,
    TopologyTooDeep,
    ResourceExhausted,
};

enum class LinuxUsbTopologyStage : std::uint8_t {
    Validation,
    RootOpen,
    Lookup,
    SymlinkResolution,
    AttributeRead,
    Correlation,
};

struct LinuxUsbTopologyError final {
    LinuxUsbTopologyErrorKind kind{LinuxUsbTopologyErrorKind::InvalidArgument};
    LinuxUsbTopologyStage stage{LinuxUsbTopologyStage::Validation};
    int native_code{};
    std::string path;
    std::string message;

    [[nodiscard]] bool operator==(const LinuxUsbTopologyError&) const = default;
};

// A reader-owned, internally verified snapshot. Keeping this seam injectable
// makes correlation, duplicate, and re-enumeration tests independent of the
// host's real /sys tree.
struct LinuxUsbSysfsNode final {
    std::string entry_name;
    std::string root_relative_path;
    std::string root_controller_id;
    std::uint16_t vendor_id{};
    std::uint16_t product_id{};
    std::uint8_t bus_number{};
    std::uint8_t device_address{};
    std::vector<std::uint8_t> port_numbers;
    std::optional<std::string> serial_utf8;
    // USB descriptor text read from sysfs, when present.
    std::optional<std::string> product_utf8;
};

class ILinuxUsbSysfsReader {
public:
    virtual ~ILinuxUsbSysfsReader() = default;

    [[nodiscard]] virtual std::expected<std::vector<LinuxUsbSysfsNode>,
                                        LinuxUsbTopologyError>
    read_candidates(const LinuxUsbTopologyQuery& query) const = 0;
};

enum class LinuxUsbSysfsReadCheckpoint : std::uint8_t {
    DeviceOpened,
    SnapshotRead,
};

using LinuxUsbSysfsCheckpointHook =
    void (*)(LinuxUsbSysfsReadCheckpoint, void*) noexcept;

// Production reader. Every path below sysfs_root is opened component-by-
// component with openat + O_NOFOLLOW. USB bus entries are never followed;
// their readlinkat target is normalized beneath the trusted root and then
// opened without following any target-path symlink.
class OpenatLinuxUsbSysfsReader final : public ILinuxUsbSysfsReader {
public:
    explicit OpenatLinuxUsbSysfsReader(
        std::string sysfs_root = "/sys",
        LinuxUsbSysfsCheckpointHook checkpoint_hook = nullptr,
        void* checkpoint_context = nullptr);

    [[nodiscard]] std::expected<std::vector<LinuxUsbSysfsNode>,
                                LinuxUsbTopologyError>
    read_candidates(const LinuxUsbTopologyQuery& query) const override;

private:
    std::string sysfs_root_;
#if !defined(_WIN32)
    LinuxUsbSysfsCheckpointHook checkpoint_hook_{};
    void* checkpoint_context_{};
#endif
};

class LinuxUsbTopologyDiscovery final {
public:
    explicit LinuxUsbTopologyDiscovery(const ILinuxUsbSysfsReader& reader) noexcept;

    [[nodiscard]] std::expected<LinuxUsbTopology, LinuxUsbTopologyError>
    discover(const LinuxUsbTopologyQuery& query) const;

private:
    const ILinuxUsbSysfsReader& reader_;
};

[[nodiscard]] LinuxUsbTopologyQuery make_linux_usb_topology_query(
    const UsbDeviceInfo& device,
    // Optional USB descriptor text; do not pass Fastboot getvar:product.
    std::optional<std::string_view> product_utf8 = std::nullopt);

[[nodiscard]] std::expected<std::string, LinuxUsbTopologyError>
canonical_linux_usb_port_path(std::uint8_t bus_number,
                              const std::vector<std::uint8_t>& port_numbers);

}  // namespace kairosboot::transport
