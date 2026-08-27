// SPDX-License-Identifier: MIT
#include "src/transport/windows_usb_topology.hpp"

#include "src/transport/libusb_runtime.hpp"

#include <chrono>
#include <exception>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using kairosboot::transport::IWindowsUsbTopologyBackend;
using kairosboot::transport::IWindowsUsbTopologyNativeBackend;
using kairosboot::transport::SetupApiWindowsUsbNativeBackend;
using kairosboot::transport::SetupApiWindowsUsbTopologyBackend;
using kairosboot::transport::UsbDeviceInfo;
using kairosboot::transport::WindowsUsbInterfaceFingerprint;
using kairosboot::transport::WindowsUsbNativeErrorDomain;
using kairosboot::transport::WindowsUsbNativeNodeSnapshot;
using kairosboot::transport::WindowsUsbNativeSnapshot;
using kairosboot::transport::WindowsUsbTopologyDiscovery;
using kairosboot::transport::WindowsUsbTopologyError;
using kairosboot::transport::WindowsUsbTopologyErrorKind;
using kairosboot::transport::WindowsUsbTopologyNode;
using kairosboot::transport::WindowsUsbTopologyQuery;
using kairosboot::transport::WindowsUsbTopologyStage;
using kairosboot::transport::canonical_windows_usb_port_path;
using kairosboot::transport::make_windows_usb_topology_query;
using kairosboot::transport::parse_windows_usb_location_path;

#define CHECK(condition)                                                         \
    do {                                                                         \
        if (!(condition)) {                                                       \
            throw std::runtime_error(                                             \
                std::string("check failed at line ") + std::to_string(__LINE__) + \
                ": " #condition);                                                \
        }                                                                        \
    } while (false)

constexpr unsigned long kSession = 0x101UL;

[[nodiscard]] WindowsUsbInterfaceFingerprint fingerprint() {
    return WindowsUsbInterfaceFingerprint{
        .interface_number = 0U,
        .interface_class = 0xFFU,
        .interface_subclass = 0x42U,
        .interface_protocol = 0x03U,
    };
}

[[nodiscard]] WindowsUsbTopologyQuery query(
    const unsigned long session = kSession,
    const std::uint8_t bus = 1U,
    const std::uint8_t address = 9U,
    std::vector<std::uint8_t> ports = {2U, 3U}) {
    return WindowsUsbTopologyQuery{
        .libusb_session_data = session,
        .vendor_id = 0x18D1U,
        .product_id = 0x4EE0U,
        .bus_number = bus,
        .device_address = address,
        .port_numbers = std::move(ports),
        .serial_utf8 = std::string{"SERIAL-01"},
        .interface_fingerprint = fingerprint(),
    };
}

[[nodiscard]] std::string location_path(
    const std::vector<std::uint8_t>& ports,
    const std::uint32_t root_hub_index = 0U,
    const bool with_interface = false) {
    std::string result = "PCIROOT(0)#PCI(1400)#USBROOT(" +
        std::to_string(root_hub_index) + ")";
    for (const auto port : ports) {
        result += "#USB(" + std::to_string(port) + ")";
    }
    if (with_interface) {
        result += "#USBMI(0)";
    }
    return result;
}

[[nodiscard]] WindowsUsbNativeSnapshot native_snapshot(
    const WindowsUsbTopologyQuery& wanted) {
    return WindowsUsbNativeSnapshot{
        .requested_system_node = wanted.libusb_session_data,
        .chain_leaf_to_root = {
            WindowsUsbNativeNodeSnapshot{
                .system_node = wanted.libusb_session_data,
                .parent_system_node = 0x201UL,
                .device_instance_id_utf8 =
                    "USB\\VID_18D1&PID_4EE0&MI_00\\SERIAL-01",
                .present = true,
                .hardware_ids_utf8 = {
                    "USB\\VID_18D1&PID_4EE0&REV_0100&MI_00",
                    "USB\\VID_18D1&PID_4EE0&MI_00",
                },
                .location_paths_utf8 = {
                    location_path(wanted.port_numbers, 0U, true),
                },
            },
            WindowsUsbNativeNodeSnapshot{
                .system_node = 0x201UL,
                .parent_system_node = 0x301UL,
                .device_instance_id_utf8 =
                    "USB\\VID_2109&PID_2817\\EXTERNAL-HUB",
                .present = true,
                .exposes_usb_hub_interface = true,
                .exposes_usb_host_controller_interface = false,
                .hardware_ids_utf8 = {},
                .location_paths_utf8 = {},
            },
            WindowsUsbNativeNodeSnapshot{
                .system_node = 0x301UL,
                .parent_system_node = 0x401UL,
                .device_instance_id_utf8 = "USB\\ROOT_HUB30\\ROOT-01",
                .present = true,
                .exposes_usb_hub_interface = true,
                .exposes_usb_host_controller_interface = false,
                .hardware_ids_utf8 = {},
                .location_paths_utf8 = {},
            },
            WindowsUsbNativeNodeSnapshot{
                .system_node = 0x401UL,
                .parent_system_node = std::nullopt,
                .device_instance_id_utf8 =
                    "PCI\\VEN_8086&DEV_7AE0\\CONTROLLER-01",
                .present = true,
                .exposes_usb_hub_interface = false,
                .exposes_usb_host_controller_interface = true,
                .hardware_ids_utf8 = {},
                .location_paths_utf8 = {},
            },
        },
    };
}

[[nodiscard]] WindowsUsbTopologyNode node(
    const WindowsUsbTopologyQuery& wanted,
    const unsigned long session = kSession,
    std::string device_id =
        "USB\\VID_18D1&PID_4EE0&MI_00\\SERIAL-01") {
    return WindowsUsbTopologyNode{
        .libusb_session_data = session,
        .device_instance_id_utf8 = std::move(device_id),
        .root_controller_instance_id_utf8 =
            "PCI\\VEN_8086&DEV_7AE0\\CONTROLLER-01",
        .hub_instance_ids_utf8 = {
            "USB\\ROOT_HUB30\\ROOT-01",
            "USB\\VID_2109&PID_2817\\EXTERNAL-HUB",
        },
        .location_path_utf8 = location_path(wanted.port_numbers),
        .root_hub_index = 0U,
        .vendor_id = wanted.vendor_id,
        .product_id = wanted.product_id,
        .bus_number = wanted.bus_number,
        .device_address = wanted.device_address,
        .port_numbers = wanted.port_numbers,
        .serial_utf8 = wanted.serial_utf8,
        .interface_fingerprint = wanted.interface_fingerprint,
        .validation_chain_system_nodes = {
            session,
            0x201UL,
            0x301UL,
            0x401UL,
        },
        .validation_chain_instance_ids_utf8 = {
            "USB\\VID_18D1&PID_4EE0&MI_00\\SERIAL-01",
            "USB\\VID_2109&PID_2817\\EXTERNAL-HUB",
            "USB\\ROOT_HUB30\\ROOT-01",
            "PCI\\VEN_8086&DEV_7AE0\\CONTROLLER-01",
        },
    };
}

using BackendResult = std::expected<std::vector<WindowsUsbTopologyNode>,
                                    WindowsUsbTopologyError>;
using NativeResult = std::expected<WindowsUsbNativeSnapshot,
                                   WindowsUsbTopologyError>;

class FakeBackend final : public IWindowsUsbTopologyBackend {
public:
    std::vector<BackendResult> results;
    mutable std::size_t calls{};
    std::stop_source* request_stop_after_first{};

    [[nodiscard]] std::expected<std::vector<WindowsUsbTopologyNode>,
                                WindowsUsbTopologyError>
    read_candidates(const WindowsUsbTopologyQuery&,
                    std::chrono::steady_clock::time_point,
                    std::stop_token) const override {
        if (results.empty()) {
            throw std::runtime_error("fake backend has no result");
        }
        const auto index = calls < results.size() ? calls : results.size() - 1U;
        ++calls;
        if (calls == 1U && request_stop_after_first != nullptr) {
            (void)request_stop_after_first->request_stop();
        }
        return results[index];
    }
};

class FakeNativeBackend final : public IWindowsUsbTopologyNativeBackend {
public:
    std::vector<NativeResult> results;
    mutable std::size_t calls{};
    mutable std::vector<unsigned long> requested_sessions;

    [[nodiscard]] std::expected<WindowsUsbNativeSnapshot,
                                WindowsUsbTopologyError>
    read_snapshot(const unsigned long session,
                  std::chrono::steady_clock::time_point,
                  std::stop_token) const override {
        if (results.empty()) {
            throw std::runtime_error("fake native backend has no result");
        }
        requested_sessions.push_back(session);
        const auto index = calls < results.size() ? calls : results.size() - 1U;
        ++calls;
        return results[index];
    }
};

[[nodiscard]] FakeBackend stable_backend(
    std::vector<WindowsUsbTopologyNode> nodes) {
    FakeBackend backend;
    backend.results = {nodes, std::move(nodes)};
    return backend;
}

[[nodiscard]] FakeNativeBackend stable_native_backend(
    WindowsUsbNativeSnapshot snapshot) {
    FakeNativeBackend backend;
    backend.results = {snapshot, std::move(snapshot)};
    return backend;
}

struct FakeClock final {
    std::vector<std::chrono::steady_clock::time_point> values;
    std::size_t index{};

    [[nodiscard]] static std::chrono::steady_clock::time_point now(
        void* opaque) noexcept {
        auto& clock = *static_cast<FakeClock*>(opaque);
        if (clock.values.empty()) {
            return std::chrono::steady_clock::time_point{};
        }
        const auto selected = clock.index < clock.values.size()
            ? clock.index
            : clock.values.size() - 1U;
        ++clock.index;
        return clock.values[selected];
    }
};

void exact_libusb_session_maps_one_native_parent_chain() {
    const auto wanted = query();
    auto native = stable_native_backend(native_snapshot(wanted));
    SetupApiWindowsUsbTopologyBackend backend(native);
    WindowsUsbTopologyDiscovery discovery(backend);

    const auto result = discovery.discover(wanted);
    CHECK(result.has_value());
    CHECK(native.calls == 2U);
    CHECK(native.requested_sessions ==
          std::vector<unsigned long>({kSession, kSession}));
    CHECK(result->physical_port_path == "usb:1-2.3");
    CHECK(result->root_controller_id ==
          "windows-pnp:PCI\\VEN_8086&DEV_7AE0\\CONTROLLER-01");
    CHECK(result->hub_port_chain == std::vector<std::uint8_t>({2U, 3U}));
    CHECK(result->vendor_id == wanted.vendor_id);
    CHECK(result->product_id == wanted.product_id);
    CHECK(result->bus_number == wanted.bus_number);
    CHECK(result->device_address == wanted.device_address);
    CHECK(result->serial_utf8 == wanted.serial_utf8);
    CHECK(result->interface_fingerprint == wanted.interface_fingerprint);
    CHECK(result->hub_instance_ids_utf8 ==
          std::vector<std::string>({
              "USB\\ROOT_HUB30\\ROOT-01",
              "USB\\VID_2109&PID_2817\\EXTERNAL-HUB",
          }));
    CHECK(result->location_path_utf8 == location_path(wanted.port_numbers));
}

void pnp_never_reinterprets_bus_address_or_interface_fingerprint() {
    auto wanted = query(kSession, 37U, 241U);
    wanted.interface_fingerprint = WindowsUsbInterfaceFingerprint{
        .interface_number = 0U,
        .interface_class = 0xFFU,
        .interface_subclass = 0x7AU,
        .interface_protocol = 0xB1U,
    };
    auto snapshot = native_snapshot(wanted);
    // The raw seam deliberately has no PnP bus/address or CompatibleIds fields.
    // Only the exact session, hardware VID/PID, location, and parent roles are
    // available to the mapper.
    auto native = stable_native_backend(std::move(snapshot));
    SetupApiWindowsUsbTopologyBackend backend(native);
    WindowsUsbTopologyDiscovery discovery(backend);

    const auto result = discovery.discover(wanted);
    CHECK(result.has_value());
    CHECK(result->bus_number == 37U);
    CHECK(result->device_address == 241U);
    CHECK(result->interface_fingerprint == wanted.interface_fingerprint);
}

void zero_session_is_rejected_before_native_access() {
    auto wanted = query(0UL);
    auto native = stable_native_backend(native_snapshot(query()));
    SetupApiWindowsUsbTopologyBackend backend(native);
    WindowsUsbTopologyDiscovery discovery(backend);

    const auto result = discovery.discover(wanted);
    CHECK(!result.has_value());
    CHECK(result.error().kind == WindowsUsbTopologyErrorKind::InvalidArgument);
    CHECK(result.error().stage == WindowsUsbTopologyStage::Validation);
    CHECK(native.calls == 0U);
}

void stale_session_and_broken_parent_relation_fail_closed() {
    const auto wanted = query();

    auto wrong_requested = native_snapshot(wanted);
    wrong_requested.requested_system_node = 0x999UL;
    FakeNativeBackend wrong_native;
    wrong_native.results = {wrong_requested};
    SetupApiWindowsUsbTopologyBackend wrong_backend(wrong_native);
    const auto wrong = wrong_backend.read_candidates(
        wanted, std::chrono::steady_clock::time_point::max(), {});
    CHECK(!wrong.has_value());
    CHECK(wrong.error().kind == WindowsUsbTopologyErrorKind::IdentityMismatch);
    CHECK(wrong.error().libusb_session_data == kSession);

    auto absent = native_snapshot(wanted);
    absent.chain_leaf_to_root.front().present = false;
    FakeNativeBackend absent_native;
    absent_native.results = {absent};
    SetupApiWindowsUsbTopologyBackend absent_backend(absent_native);
    const auto missing = absent_backend.read_candidates(
        wanted, std::chrono::steady_clock::time_point::max(), {});
    CHECK(!missing.has_value());
    CHECK(missing.error().kind == WindowsUsbTopologyErrorKind::IdentityChanged);

    auto broken_parent = native_snapshot(wanted);
    broken_parent.chain_leaf_to_root.front().parent_system_node = 0x777UL;
    FakeNativeBackend parent_native;
    parent_native.results = {broken_parent};
    SetupApiWindowsUsbTopologyBackend parent_backend(parent_native);
    const auto broken = parent_backend.read_candidates(
        wanted, std::chrono::steady_clock::time_point::max(), {});
    CHECK(!broken.has_value());
    CHECK(broken.error().kind == WindowsUsbTopologyErrorKind::IdentityChanged);
    CHECK(broken.error().stage == WindowsUsbTopologyStage::ParentTraversal);
}

void hardware_location_and_interface_mismatch_fail_closed() {
    const auto wanted = query();

    auto wrong_vid = native_snapshot(wanted);
    wrong_vid.chain_leaf_to_root.front().hardware_ids_utf8 = {
        "USB\\VID_18D2&PID_4EE0&MI_00",
    };
    FakeNativeBackend vid_native;
    vid_native.results = {wrong_vid};
    SetupApiWindowsUsbTopologyBackend vid_backend(vid_native);
    const auto vid = vid_backend.read_candidates(
        wanted, std::chrono::steady_clock::time_point::max(), {});
    CHECK(!vid.has_value());
    CHECK(vid.error().kind == WindowsUsbTopologyErrorKind::IdentityMismatch);

    auto wrong_port = native_snapshot(wanted);
    wrong_port.chain_leaf_to_root.front().location_paths_utf8 = {
        location_path({2U, 4U}, 0U, true),
    };
    FakeNativeBackend port_native;
    port_native.results = {wrong_port};
    SetupApiWindowsUsbTopologyBackend port_backend(port_native);
    const auto port = port_backend.read_candidates(
        wanted, std::chrono::steady_clock::time_point::max(), {});
    CHECK(!port.has_value());
    CHECK(port.error().kind == WindowsUsbTopologyErrorKind::IdentityMismatch);

    auto wrong_interface = native_snapshot(wanted);
    wrong_interface.chain_leaf_to_root.front().location_paths_utf8 = {
        location_path(wanted.port_numbers) + "#USBMI(1)",
    };
    FakeNativeBackend interface_native;
    interface_native.results = {wrong_interface};
    SetupApiWindowsUsbTopologyBackend interface_backend(interface_native);
    const auto interface_result = interface_backend.read_candidates(
        wanted, std::chrono::steady_clock::time_point::max(), {});
    CHECK(!interface_result.has_value());
    CHECK(interface_result.error().kind ==
          WindowsUsbTopologyErrorKind::IdentityMismatch);
}

void conflicting_location_and_duplicate_native_identity_are_ambiguous() {
    const auto wanted = query();

    auto conflicting = native_snapshot(wanted);
    conflicting.chain_leaf_to_root.front().location_paths_utf8.push_back(
        "ACPI(_SB_)#PCI(1400)#USBROOT(0)#USB(2)#USB(3)#USBMI(0)");
    FakeNativeBackend location_native;
    location_native.results = {conflicting};
    SetupApiWindowsUsbTopologyBackend location_backend(location_native);
    const auto location = location_backend.read_candidates(
        wanted, std::chrono::steady_clock::time_point::max(), {});
    CHECK(!location.has_value());
    CHECK(location.error().kind == WindowsUsbTopologyErrorKind::AmbiguousMapping);

    auto duplicate = native_snapshot(wanted);
    duplicate.chain_leaf_to_root[1U].system_node = wanted.libusb_session_data;
    duplicate.chain_leaf_to_root[0U].parent_system_node =
        wanted.libusb_session_data;
    FakeNativeBackend duplicate_native;
    duplicate_native.results = {duplicate};
    SetupApiWindowsUsbTopologyBackend duplicate_backend(duplicate_native);
    const auto duplicate_result = duplicate_backend.read_candidates(
        wanted, std::chrono::steady_clock::time_point::max(), {});
    CHECK(!duplicate_result.has_value());
    CHECK(duplicate_result.error().kind ==
          WindowsUsbTopologyErrorKind::AmbiguousMapping);
}

void hidden_parent_change_is_caught_by_double_snapshot() {
    const auto wanted = query();
    auto before = native_snapshot(wanted);
    before.chain_leaf_to_root.insert(
        before.chain_leaf_to_root.begin() + 1,
        WindowsUsbNativeNodeSnapshot{
            .system_node = 0x181UL,
            .parent_system_node = 0x201UL,
            .device_instance_id_utf8 = "USB\\COMPOSITE\\PROXY-A",
            .present = true,
            .exposes_usb_hub_interface = false,
            .exposes_usb_host_controller_interface = false,
            .hardware_ids_utf8 = {},
            .location_paths_utf8 = {},
        });
    before.chain_leaf_to_root.front().parent_system_node = 0x181UL;

    auto after = before;
    after.chain_leaf_to_root[1U].system_node = 0x182UL;
    after.chain_leaf_to_root[1U].device_instance_id_utf8 =
        "USB\\COMPOSITE\\PROXY-B";
    after.chain_leaf_to_root.front().parent_system_node = 0x182UL;

    FakeNativeBackend native;
    native.results = {before, after};
    SetupApiWindowsUsbTopologyBackend backend(native);
    WindowsUsbTopologyDiscovery discovery(backend);
    const auto result = discovery.discover(wanted);
    CHECK(!result.has_value());
    CHECK(result.error().kind == WindowsUsbTopologyErrorKind::IdentityChanged);
    CHECK(result.error().stage == WindowsUsbTopologyStage::StabilityCheck);
    CHECK(native.calls == 2U);
}

void duplicate_serial_on_other_session_cannot_shadow_exact_devinst() {
    const auto wanted = query();
    auto wrong = node(
        wanted,
        0x909UL,
        "USB\\VID_18D1&PID_4EE0&MI_00\\SAME-SERIAL-OTHER-DEVICE");
    wrong.validation_chain_instance_ids_utf8.front() =
        wrong.device_instance_id_utf8;
    auto correct = node(wanted);
    wrong.serial_utf8 = correct.serial_utf8;
    auto backend = stable_backend({wrong, correct});
    WindowsUsbTopologyDiscovery discovery(backend);

    const auto result = discovery.discover(wanted);
    CHECK(result.has_value());
    CHECK(result->device_instance_id_utf8 == correct.device_instance_id_utf8);
}

void duplicate_candidates_for_exact_session_fail_closed() {
    const auto wanted = query();
    auto first = node(wanted);
    auto second = node(
        wanted,
        kSession,
        "USB\\VID_18D1&PID_4EE0&MI_00\\SECOND-NODE");
    second.validation_chain_instance_ids_utf8.front() =
        second.device_instance_id_utf8;
    auto backend = stable_backend({first, second});
    WindowsUsbTopologyDiscovery discovery(backend);

    const auto result = discovery.discover(wanted);
    CHECK(!result.has_value());
    CHECK(result.error().kind == WindowsUsbTopologyErrorKind::AmbiguousMapping);
    CHECK(result.error().stage == WindowsUsbTopologyStage::Correlation);
}

void cancellation_before_and_between_snapshots_is_deterministic() {
    const auto wanted = query();
    auto before_backend = stable_backend({node(wanted)});
    std::stop_source before_stop;
    (void)before_stop.request_stop();
    WindowsUsbTopologyDiscovery before_discovery(before_backend);
    const auto before = before_discovery.discover(
        wanted,
        std::chrono::steady_clock::time_point::max(),
        before_stop.get_token());
    CHECK(!before.has_value());
    CHECK(before.error().kind == WindowsUsbTopologyErrorKind::Cancelled);
    CHECK(before.error().libusb_session_data == kSession);
    CHECK(before_backend.calls == 0U);

    auto between_backend = stable_backend({node(wanted)});
    std::stop_source between_stop;
    between_backend.request_stop_after_first = &between_stop;
    WindowsUsbTopologyDiscovery between_discovery(between_backend);
    const auto between = between_discovery.discover(
        wanted,
        std::chrono::steady_clock::time_point::max(),
        between_stop.get_token());
    CHECK(!between.has_value());
    CHECK(between.error().kind == WindowsUsbTopologyErrorKind::Cancelled);
    CHECK(between.error().stage == WindowsUsbTopologyStage::StabilityCheck);
    CHECK(between_backend.calls == 1U);
}

void timeout_before_and_between_snapshots_is_deterministic() {
    const auto wanted = query();
    const auto epoch = std::chrono::steady_clock::time_point{};
    const auto deadline = epoch + 10ms;

    auto before_backend = stable_backend({node(wanted)});
    FakeClock before_clock{.values = {deadline}};
    WindowsUsbTopologyDiscovery before_discovery(
        before_backend, FakeClock::now, &before_clock);
    const auto before = before_discovery.discover(wanted, deadline);
    CHECK(!before.has_value());
    CHECK(before.error().kind == WindowsUsbTopologyErrorKind::TimedOut);
    CHECK(before_backend.calls == 0U);

    auto between_backend = stable_backend({node(wanted)});
    FakeClock between_clock{.values = {epoch, deadline}};
    WindowsUsbTopologyDiscovery between_discovery(
        between_backend, FakeClock::now, &between_clock);
    const auto between = between_discovery.discover(wanted, deadline);
    CHECK(!between.has_value());
    CHECK(between.error().kind == WindowsUsbTopologyErrorKind::TimedOut);
    CHECK(between.error().stage == WindowsUsbTopologyStage::StabilityCheck);
    CHECK(between_backend.calls == 1U);
}

void native_error_metadata_is_preserved_verbatim() {
    const auto wanted = query();
    const WindowsUsbTopologyError expected{
        .kind = WindowsUsbTopologyErrorKind::PermissionDenied,
        .stage = WindowsUsbTopologyStage::PropertyRead,
        .native_domain = WindowsUsbNativeErrorDomain::ConfigurationManager,
        .native_code = 0x33U,
        .libusb_session_data = kSession,
        .device_instance_id_utf8 = "USB\\VID_18D1&PID_4EE0\\DENIED",
        .message = "property access denied",
    };
    FakeNativeBackend native;
    native.results = {std::unexpected(expected)};
    SetupApiWindowsUsbTopologyBackend backend(native);

    const auto result = backend.read_candidates(
        wanted, std::chrono::steady_clock::time_point::max(), {});
    CHECK(!result.has_value());
    CHECK(result.error() == expected);
    CHECK(native.calls == 1U);
}

void malformed_hub_depth_and_roles_never_publish_topology() {
    const auto wanted = query();

    auto missing_hub = native_snapshot(wanted);
    missing_hub.chain_leaf_to_root[1U].exposes_usb_hub_interface = false;
    FakeNativeBackend hub_native;
    hub_native.results = {missing_hub};
    SetupApiWindowsUsbTopologyBackend hub_backend(hub_native);
    const auto hub = hub_backend.read_candidates(
        wanted, std::chrono::steady_clock::time_point::max(), {});
    CHECK(!hub.has_value());
    CHECK(hub.error().kind == WindowsUsbTopologyErrorKind::IdentityMismatch);

    auto dual_role = native_snapshot(wanted);
    dual_role.chain_leaf_to_root[1U].exposes_usb_host_controller_interface =
        true;
    FakeNativeBackend role_native;
    role_native.results = {dual_role};
    SetupApiWindowsUsbTopologyBackend role_backend(role_native);
    const auto role = role_backend.read_candidates(
        wanted, std::chrono::steady_clock::time_point::max(), {});
    CHECK(!role.has_value());
    CHECK(role.error().kind == WindowsUsbTopologyErrorKind::MalformedSnapshot);
}

void location_parser_accepts_composite_suffix_and_remains_strict() {
    const auto parsed = parse_windows_usb_location_path(
        "PCIROOT(0)#PCI(1400)#USBROOT(1)#USB(2)#USB(11)#USBMI(3)");
    CHECK(parsed.has_value());
    CHECK(parsed->controller_prefix_utf8 == "PCIROOT(0)#PCI(1400)");
    CHECK(parsed->root_hub_index == 1U);
    CHECK(parsed->port_numbers == std::vector<std::uint8_t>({2U, 11U}));
    CHECK(parsed->interface_number == std::optional<std::uint8_t>{3U});

    const auto zero_port = parse_windows_usb_location_path(
        "PCIROOT(0)#PCI(1400)#USBROOT(0)#USB(0)");
    CHECK(!zero_port.has_value());
    CHECK(zero_port.error().kind ==
          WindowsUsbTopologyErrorKind::MalformedSnapshot);

    const auto trailing_component = parse_windows_usb_location_path(
        "PCIROOT(0)#USBROOT(0)#USB(1)#USBMI(0)#PCI(0)");
    CHECK(!trailing_component.has_value());
    CHECK(trailing_component.error().kind ==
          WindowsUsbTopologyErrorKind::MalformedSnapshot);

    std::string deep = "PCIROOT(0)#USBROOT(0)";
    for (std::size_t index = 0U; index < 17U; ++index) {
        deep += "#USB(1)";
    }
    const auto overdeep = parse_windows_usb_location_path(deep);
    CHECK(!overdeep.has_value());
    CHECK(overdeep.error().kind == WindowsUsbTopologyErrorKind::TopologyTooDeep);
}

void libusb_snapshot_adapter_requires_explicit_session_data() {
    UsbDeviceInfo device{};
    device.vendor_id = 0x18D1U;
    device.product_id = 0x4EE0U;
    device.bus_number = 4U;
    device.device_address = 17U;
    device.port_path = {7U, 2U};
    device.serial_utf8 = "SERIAL-ADAPTER";
    device.interface_number = 3U;
    device.interface_class = 0xFFU;
    device.interface_subclass = 0x42U;
    device.interface_protocol = 0x03U;

    const auto adapted = make_windows_usb_topology_query(device, 0xA55UL);
    CHECK(adapted.libusb_session_data == 0xA55UL);
    CHECK(adapted.vendor_id == device.vendor_id);
    CHECK(adapted.product_id == device.product_id);
    CHECK(adapted.bus_number == device.bus_number);
    CHECK(adapted.device_address == device.device_address);
    CHECK(adapted.port_numbers == device.port_path);
    CHECK(adapted.serial_utf8 ==
          std::optional<std::string>{"SERIAL-ADAPTER"});
    CHECK(adapted.interface_fingerprint.interface_number == 3U);
    CHECK(adapted.interface_fingerprint.interface_class == 0xFFU);
    CHECK(adapted.interface_fingerprint.interface_subclass == 0x42U);
    CHECK(adapted.interface_fingerprint.interface_protocol == 0x03U);
}

void physical_path_format_rejects_unsafe_identity() {
    const auto path = canonical_windows_usb_port_path(3U, {1U, 12U});
    CHECK(path.has_value());
    CHECK(*path == "usb:3-1.12");

    const auto zero_bus = canonical_windows_usb_port_path(0U, {1U});
    CHECK(!zero_bus.has_value());
    CHECK(zero_bus.error().kind == WindowsUsbTopologyErrorKind::InvalidArgument);
}

void production_native_backend_is_explicitly_platform_gated() {
#if !defined(_WIN32)
    SetupApiWindowsUsbNativeBackend backend;
    const auto result = backend.read_snapshot(
        kSession, std::chrono::steady_clock::time_point::max(), {});
    CHECK(!result.has_value());
    CHECK(result.error().kind ==
          WindowsUsbTopologyErrorKind::UnsupportedPlatform);
    CHECK(result.error().libusb_session_data == kSession);
#endif
}

}  // namespace

int main() {
    const std::vector<std::pair<std::string_view, void (*)()>> tests{
        {"exact session mapping", exact_libusb_session_maps_one_native_parent_chain},
        {"libusb-owned metadata", pnp_never_reinterprets_bus_address_or_interface_fingerprint},
        {"zero session", zero_session_is_rejected_before_native_access},
        {"stale session", stale_session_and_broken_parent_relation_fail_closed},
        {"identity mismatch", hardware_location_and_interface_mismatch_fail_closed},
        {"native ambiguity", conflicting_location_and_duplicate_native_identity_are_ambiguous},
        {"TOCTOU hidden parent", hidden_parent_change_is_caught_by_double_snapshot},
        {"duplicate serial isolation", duplicate_serial_on_other_session_cannot_shadow_exact_devinst},
        {"duplicate exact session", duplicate_candidates_for_exact_session_fail_closed},
        {"cancellation", cancellation_before_and_between_snapshots_is_deterministic},
        {"timeout", timeout_before_and_between_snapshots_is_deterministic},
        {"native error metadata", native_error_metadata_is_preserved_verbatim},
        {"malformed roles", malformed_hub_depth_and_roles_never_publish_topology},
        {"location parser", location_parser_accepts_composite_suffix_and_remains_strict},
        {"libusb adapter", libusb_snapshot_adapter_requires_explicit_session_data},
        {"physical path", physical_path_format_rejects_unsafe_identity},
        {"platform gate", production_native_backend_is_explicitly_platform_gated},
    };

    int failures = 0;
    for (const auto& [name, test] : tests) {
        try {
            test();
            std::cout << "PASS: " << name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "FAIL: " << name << ": " << error.what() << '\n';
        }
    }
    return failures == 0 ? 0 : 1;
}
