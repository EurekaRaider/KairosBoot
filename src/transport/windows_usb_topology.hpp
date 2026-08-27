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

inline constexpr std::size_t kMaximumWindowsUsbTopologyDepth = 16U;

struct WindowsUsbInterfaceFingerprint final {
    std::uint8_t interface_number{};
    std::uint8_t interface_class{};
    std::uint8_t interface_subclass{};
    std::uint8_t interface_protocol{};

    [[nodiscard]] bool operator==(
        const WindowsUsbInterfaceFingerprint&) const = default;
};

// Immutable identity copied from one libusb enumeration result. The address is
// intentionally treated as snapshot data, while controller + hub/port chain is
// the physical reconnect identity.
struct WindowsUsbTopologyQuery final {
    std::uint16_t vendor_id{};
    std::uint16_t product_id{};
    std::uint8_t bus_number{};
    std::uint8_t device_address{};
    std::vector<std::uint8_t> port_numbers;
    std::optional<std::string> serial_utf8;
    WindowsUsbInterfaceFingerprint interface_fingerprint;
};

struct WindowsUsbLocationPath final {
    std::string controller_prefix_utf8;
    std::uint32_t root_hub_index{};
    std::vector<std::uint8_t> port_numbers;

    [[nodiscard]] bool operator==(const WindowsUsbLocationPath&) const = default;
};

// Backend-owned immutable record produced from one present-device snapshot.
// The production backend only emits records whose VID/PID/interface descriptor
// compatible IDs matched the query. Discovery still revalidates every field.
struct WindowsUsbTopologyNode final {
    std::string device_instance_id_utf8;
    std::string root_controller_instance_id_utf8;
    std::vector<std::string> hub_instance_ids_utf8;
    std::string location_path_utf8;
    std::uint32_t root_hub_index{};
    std::uint16_t vendor_id{};
    std::uint16_t product_id{};
    std::uint8_t bus_number{};
    std::uint8_t device_address{};
    std::vector<std::uint8_t> port_numbers;
    std::optional<std::string> serial_utf8;
    WindowsUsbInterfaceFingerprint interface_fingerprint;

    [[nodiscard]] bool operator==(const WindowsUsbTopologyNode&) const = default;
};

struct WindowsUsbTopology final {
    std::string physical_port_path;
    std::string root_controller_id;
    std::vector<std::uint8_t> hub_port_chain;
    std::uint16_t vendor_id{};
    std::uint16_t product_id{};
    std::uint8_t bus_number{};
    std::uint8_t device_address{};
    std::optional<std::string> serial_utf8;
    WindowsUsbInterfaceFingerprint interface_fingerprint;
    std::string device_instance_id_utf8;
    std::vector<std::string> hub_instance_ids_utf8;
    std::string location_path_utf8;

    [[nodiscard]] bool operator==(const WindowsUsbTopology&) const = default;
};

enum class WindowsUsbTopologyErrorKind : std::uint8_t {
    InvalidArgument,
    UnsupportedPlatform,
    Cancelled,
    TimedOut,
    NotFound,
    PermissionDenied,
    NativeError,
    MalformedSnapshot,
    IdentityChanged,
    IdentityMismatch,
    AmbiguousMapping,
    TopologyTooDeep,
    ResourceExhausted,
};

enum class WindowsUsbTopologyStage : std::uint8_t {
    Validation,
    Enumeration,
    PropertyRead,
    ParentTraversal,
    Correlation,
    StabilityCheck,
};

enum class WindowsUsbNativeErrorDomain : std::uint8_t {
    None,
    Win32,
    ConfigurationManager,
};

struct WindowsUsbTopologyError final {
    WindowsUsbTopologyErrorKind kind{WindowsUsbTopologyErrorKind::InvalidArgument};
    WindowsUsbTopologyStage stage{WindowsUsbTopologyStage::Validation};
    WindowsUsbNativeErrorDomain native_domain{WindowsUsbNativeErrorDomain::None};
    std::uint32_t native_code{};
    std::string device_instance_id_utf8;
    std::string message;

    [[nodiscard]] bool operator==(const WindowsUsbTopologyError&) const = default;
};

class IWindowsUsbTopologyBackend {
public:
    virtual ~IWindowsUsbTopologyBackend() = default;

    [[nodiscard]] virtual std::expected<std::vector<WindowsUsbTopologyNode>,
                                        WindowsUsbTopologyError>
    read_candidates(const WindowsUsbTopologyQuery& query,
                    std::chrono::steady_clock::time_point deadline,
                    std::stop_token stop_token) const = 0;
};

// Read-only production adapter. On Windows it enumerates present USB devnodes
// with SetupAPI and resolves the parent chain with Configuration Manager. It
// never installs, updates, or replaces a device driver.
class SetupApiWindowsUsbTopologyBackend final
    : public IWindowsUsbTopologyBackend {
public:
    [[nodiscard]] std::expected<std::vector<WindowsUsbTopologyNode>,
                                WindowsUsbTopologyError>
    read_candidates(const WindowsUsbTopologyQuery& query,
                    std::chrono::steady_clock::time_point deadline,
                    std::stop_token stop_token) const override;
};

using WindowsUsbTopologyNow = std::chrono::steady_clock::time_point (*)(
    void*) noexcept;

class WindowsUsbTopologyDiscovery final {
public:
    explicit WindowsUsbTopologyDiscovery(
        const IWindowsUsbTopologyBackend& backend,
        WindowsUsbTopologyNow now = nullptr,
        void* now_context = nullptr) noexcept;

    [[nodiscard]] std::expected<WindowsUsbTopology, WindowsUsbTopologyError>
    discover(
        const WindowsUsbTopologyQuery& query,
        std::chrono::steady_clock::time_point deadline =
            std::chrono::steady_clock::time_point::max(),
        std::stop_token stop_token = {}) const;

private:
    const IWindowsUsbTopologyBackend& backend_;
    WindowsUsbTopologyNow now_{};
    void* now_context_{};
};

[[nodiscard]] WindowsUsbTopologyQuery make_windows_usb_topology_query(
    const UsbDeviceInfo& device);

[[nodiscard]] std::expected<WindowsUsbLocationPath, WindowsUsbTopologyError>
parse_windows_usb_location_path(std::string_view location_path_utf8);

[[nodiscard]] std::expected<std::string, WindowsUsbTopologyError>
canonical_windows_usb_port_path(
    std::uint8_t bus_number,
    const std::vector<std::uint8_t>& port_numbers);

}  // namespace kairosboot::transport
