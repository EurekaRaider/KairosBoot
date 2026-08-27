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
    // libusb 1.0.30 defines this as the WinUSB backend's exact DEVINST. A zero
    // value is never accepted because no other Windows property is a proven
    // substitute for this bridge.
    unsigned long libusb_session_data{};
    // PnP generation anchor captured from that DEVINST before the remaining
    // libusb snapshot fields. Both native validation snapshots must resolve the
    // session back to this exact instance ID; a recycled DEVINST is rejected.
    std::string device_instance_id_utf8;
    std::uint16_t vendor_id{};
    std::uint16_t product_id{};
    std::uint8_t bus_number{};
    std::uint8_t device_address{};
    std::vector<std::uint8_t> port_numbers;
    std::optional<std::string> serial_utf8;
    WindowsUsbInterfaceFingerprint interface_fingerprint;
};

// Raw, read-only Configuration Manager snapshot seam. Production populates it
// from one exact DEVINST and its parent chain; tests can inject it on any host.
// The chain is ordered leaf-to-root and is deliberately truncated at the first
// host-controller device-interface node.
struct WindowsUsbNativeNodeSnapshot final {
    unsigned long system_node{};
    std::optional<unsigned long> parent_system_node;
    std::string device_instance_id_utf8;
    bool present{};
    bool exposes_usb_hub_interface{};
    bool exposes_usb_host_controller_interface{};
    std::vector<std::string> hardware_ids_utf8;
    std::vector<std::string> location_paths_utf8;

    [[nodiscard]] bool operator==(
        const WindowsUsbNativeNodeSnapshot&) const = default;
};

struct WindowsUsbNativeSnapshot final {
    unsigned long requested_system_node{};
    std::vector<WindowsUsbNativeNodeSnapshot> chain_leaf_to_root;

    [[nodiscard]] bool operator==(const WindowsUsbNativeSnapshot&) const =
        default;
};

struct WindowsUsbLocationPath final {
    std::string controller_prefix_utf8;
    std::uint32_t root_hub_index{};
    std::vector<std::uint8_t> port_numbers;
    std::optional<std::uint8_t> interface_number;

    [[nodiscard]] bool operator==(const WindowsUsbLocationPath&) const = default;
};

// Backend-owned immutable record produced from one present-device snapshot.
// Bus/address/serial/interface data remain copied from the same libusb
// snapshot; PnP properties are not treated as equivalent sources for them.
struct WindowsUsbTopologyNode final {
    unsigned long libusb_session_data{};
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
    // Included in the double-snapshot equality check so an otherwise hidden
    // composite/proxy devnode replacement cannot pass the TOCTOU gate.
    std::vector<unsigned long> validation_chain_system_nodes;
    std::vector<std::string> validation_chain_instance_ids_utf8;

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
    unsigned long libusb_session_data{};
    std::string device_instance_id_utf8;
    std::string message;

    [[nodiscard]] bool operator==(const WindowsUsbTopologyError&) const = default;
};

struct WindowsUsbTopologyDeviceCandidates final {
    unsigned long libusb_session_data{};
    WindowsUsbNativeSnapshot native_snapshot;
    std::vector<WindowsUsbTopologyNode> interface_candidates;

    [[nodiscard]] bool operator==(
        const WindowsUsbTopologyDeviceCandidates&) const = default;
};

using WindowsUsbNativeSnapshotResult =
    std::expected<WindowsUsbNativeSnapshot, WindowsUsbTopologyError>;
using WindowsUsbTopologyDeviceCandidatesResult =
    std::expected<WindowsUsbTopologyDeviceCandidates,
                  WindowsUsbTopologyError>;
using WindowsUsbTopologyResult =
    std::expected<WindowsUsbTopology, WindowsUsbTopologyError>;

class IWindowsUsbTopologyBackend {
public:
    virtual ~IWindowsUsbTopologyBackend() = default;

    [[nodiscard]] virtual std::expected<std::vector<WindowsUsbTopologyNode>,
                                        WindowsUsbTopologyError>
    read_candidates(const WindowsUsbTopologyQuery& query,
                    std::chrono::steady_clock::time_point deadline,
                    std::stop_token stop_token) const = 0;

    // One result per distinct session, in first-query order. A device-scoped
    // error covers every interface query for that session.
    [[nodiscard]] virtual std::expected<
        std::vector<WindowsUsbTopologyDeviceCandidatesResult>,
        WindowsUsbTopologyError>
    read_candidate_batch(
        std::span<const WindowsUsbTopologyQuery> queries,
        std::chrono::steady_clock::time_point deadline,
        std::stop_token stop_token) const = 0;
};

class IWindowsUsbTopologyNativeBackend {
public:
    virtual ~IWindowsUsbTopologyNativeBackend() = default;

    [[nodiscard]] virtual std::expected<WindowsUsbNativeSnapshot,
                                        WindowsUsbTopologyError>
    read_snapshot(unsigned long libusb_session_data,
                  std::chrono::steady_clock::time_point deadline,
                  std::stop_token stop_token) const = 0;

    // One immutable result per requested session, in input order. Production
    // performs one platform-global enumeration pass for the whole span.
    [[nodiscard]] virtual std::expected<
        std::vector<WindowsUsbNativeSnapshotResult>,
        WindowsUsbTopologyError>
    read_snapshots(
        std::span<const unsigned long> libusb_session_data,
        std::chrono::steady_clock::time_point deadline,
        std::stop_token stop_token) const;
};

// Uses only read-only SetupAPI/Configuration Manager queries. It never opens a
// USB transfer handle and never installs, updates, or replaces a driver.
class SetupApiWindowsUsbNativeBackend final
    : public IWindowsUsbTopologyNativeBackend {
public:
    [[nodiscard]] std::expected<WindowsUsbNativeSnapshot,
                                WindowsUsbTopologyError>
    read_snapshot(unsigned long libusb_session_data,
                  std::chrono::steady_clock::time_point deadline,
                  std::stop_token stop_token) const override;

    [[nodiscard]] std::expected<
        std::vector<WindowsUsbNativeSnapshotResult>,
        WindowsUsbTopologyError>
    read_snapshots(
        std::span<const unsigned long> libusb_session_data,
        std::chrono::steady_clock::time_point deadline,
        std::stop_token stop_token) const override;
};

// Read-only production adapter. On Windows it enumerates present USB devnodes
// with SetupAPI and resolves the parent chain with Configuration Manager. It
// never installs, updates, or replaces a device driver.
class SetupApiWindowsUsbTopologyBackend final
    : public IWindowsUsbTopologyBackend {
public:
    SetupApiWindowsUsbTopologyBackend() noexcept = default;

    // The injected backend must outlive this adapter.
    explicit SetupApiWindowsUsbTopologyBackend(
        const IWindowsUsbTopologyNativeBackend& native_backend) noexcept;

    [[nodiscard]] std::expected<std::vector<WindowsUsbTopologyNode>,
                                WindowsUsbTopologyError>
    read_candidates(const WindowsUsbTopologyQuery& query,
                    std::chrono::steady_clock::time_point deadline,
                    std::stop_token stop_token) const override;

    [[nodiscard]] std::expected<
        std::vector<WindowsUsbTopologyDeviceCandidatesResult>,
        WindowsUsbTopologyError>
    read_candidate_batch(
        std::span<const WindowsUsbTopologyQuery> queries,
        std::chrono::steady_clock::time_point deadline,
        std::stop_token stop_token) const override;

private:
    const IWindowsUsbTopologyNativeBackend* native_backend_{};
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

    // Output is exactly aligned with queries. Cancellation, timeout, malformed
    // batch shape, or another batch-global failure returns unexpected and no
    // per-interface result is published.
    [[nodiscard]] std::expected<std::vector<WindowsUsbTopologyResult>,
                                WindowsUsbTopologyError>
    discover_batch(
        std::span<const WindowsUsbTopologyQuery> queries,
        std::chrono::steady_clock::time_point deadline =
            std::chrono::steady_clock::time_point::max(),
        std::stop_token stop_token = {}) const;

private:
    const IWindowsUsbTopologyBackend& backend_;
    WindowsUsbTopologyNow now_{};
    void* now_context_{};
};

[[nodiscard]] WindowsUsbTopologyQuery make_windows_usb_topology_query(
    const UsbDeviceInfo& device,
    unsigned long libusb_session_data,
    std::string_view device_instance_id_utf8);

[[nodiscard]] std::expected<std::string, WindowsUsbTopologyError>
read_windows_usb_session_instance_id(
    unsigned long libusb_session_data,
    std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::time_point::max(),
    std::stop_token stop_token = {});

[[nodiscard]] std::expected<WindowsUsbLocationPath, WindowsUsbTopologyError>
parse_windows_usb_location_path(std::string_view location_path_utf8);

[[nodiscard]] std::expected<std::string, WindowsUsbTopologyError>
canonical_windows_usb_port_path(
    std::uint8_t bus_number,
    const std::vector<std::uint8_t>& port_numbers);

}  // namespace kairosboot::transport
