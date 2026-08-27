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
using kairosboot::transport::SetupApiWindowsUsbTopologyBackend;
using kairosboot::transport::UsbDeviceInfo;
using kairosboot::transport::WindowsUsbInterfaceFingerprint;
using kairosboot::transport::WindowsUsbNativeErrorDomain;
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

[[nodiscard]] WindowsUsbInterfaceFingerprint fingerprint() {
    return WindowsUsbInterfaceFingerprint{
        .interface_number = 0U,
        .interface_class = 0xFFU,
        .interface_subclass = 0x42U,
        .interface_protocol = 0x03U,
    };
}

[[nodiscard]] WindowsUsbTopologyQuery query(
    const std::uint8_t bus = 1U,
    const std::uint8_t address = 9U,
    std::vector<std::uint8_t> ports = {2U, 3U}) {
    return WindowsUsbTopologyQuery{
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
    const std::uint32_t root_hub_index = 0U) {
    std::string result = "PCIROOT(0)#PCI(1400)#USBROOT(" +
        std::to_string(root_hub_index) + ")";
    for (const auto port : ports) {
        result += "#USB(" + std::to_string(port) + ")";
    }
    return result;
}

[[nodiscard]] WindowsUsbTopologyNode node(
    const WindowsUsbTopologyQuery& wanted,
    std::string device_id = "USB\\VID_18D1&PID_4EE0&MI_00\\SERIAL-01") {
    return WindowsUsbTopologyNode{
        .device_instance_id_utf8 = std::move(device_id),
        .root_controller_instance_id_utf8 =
            "PCI\\VEN_8086&DEV_7AE0\\CONTROLLER-01",
        .hub_instance_ids_utf8 = {
            "USB\\ROOT_HUB30\\ROOT-01",
            "USB\\VID_2109&PID_2817\\HUB-01",
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
    };
}

using BackendResult = std::expected<std::vector<WindowsUsbTopologyNode>,
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

[[nodiscard]] FakeBackend stable_backend(
    std::vector<WindowsUsbTopologyNode> nodes) {
    FakeBackend backend;
    backend.results = {nodes, std::move(nodes)};
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

void stable_snapshot_maps_all_consumer_identity() {
    const auto wanted = query();
    auto backend = stable_backend({node(wanted)});
    WindowsUsbTopologyDiscovery discovery(backend);

    const auto result = discovery.discover(wanted);
    CHECK(result.has_value());
    CHECK(backend.calls == 2U);
    CHECK(result->physical_port_path == "usb:1-2.3");
    CHECK(result->root_controller_id ==
          "windows-pnp:PCI\\VEN_8086&DEV_7AE0\\CONTROLLER-01");
    CHECK(result->hub_port_chain == std::vector<std::uint8_t>({2U, 3U}));
    CHECK(result->vendor_id == wanted.vendor_id);
    CHECK(result->product_id == wanted.product_id);
    CHECK(result->device_address == wanted.device_address);
    CHECK(result->serial_utf8 == wanted.serial_utf8);
    CHECK(result->interface_fingerprint == wanted.interface_fingerprint);
    CHECK(result->hub_instance_ids_utf8.size() == 2U);
}

void backend_order_does_not_create_a_false_race() {
    auto wanted = query();
    auto wrong = node(wanted, "USB\\VID_18D1&PID_4EE0&MI_00\\OTHER-PATH");
    wrong.port_numbers = {7U};
    wrong.location_path_utf8 = location_path(wrong.port_numbers);
    auto correct = node(wanted);

    FakeBackend backend;
    backend.results = {
        std::vector{wrong, correct},
        std::vector{correct, wrong},
    };
    WindowsUsbTopologyDiscovery discovery(backend);
    const auto result = discovery.discover(wanted);
    CHECK(result.has_value());
    CHECK(result->device_instance_id_utf8 == correct.device_instance_id_utf8);
}

void changed_snapshot_fails_closed_before_correlation() {
    const auto wanted = query();
    auto before = node(wanted);
    auto after = before;
    after.root_controller_instance_id_utf8 =
        "PCI\\VEN_8086&DEV_7AE0\\CONTROLLER-02";
    FakeBackend backend;
    backend.results = {std::vector{before}, std::vector{after}};
    WindowsUsbTopologyDiscovery discovery(backend);

    const auto result = discovery.discover(wanted);
    CHECK(!result.has_value());
    CHECK(result.error().kind == WindowsUsbTopologyErrorKind::IdentityChanged);
    CHECK(result.error().stage == WindowsUsbTopologyStage::StabilityCheck);
}

void ambiguity_and_duplicate_serial_fail_closed() {
    const auto wanted = query();
    auto first = node(wanted);
    auto second = node(
        wanted, "USB\\VID_18D1&PID_4EE0&MI_00\\DUPLICATE-SERIAL-NODE");
    second.serial_utf8 = first.serial_utf8;
    auto backend = stable_backend({first, second});
    WindowsUsbTopologyDiscovery discovery(backend);

    const auto result = discovery.discover(wanted);
    CHECK(!result.has_value());
    CHECK(result.error().kind == WindowsUsbTopologyErrorKind::AmbiguousMapping);
    CHECK(result.error().stage == WindowsUsbTopologyStage::Correlation);
    CHECK(!result.error().device_instance_id_utf8.empty());
}

void duplicate_serial_on_another_port_never_crosses_devices() {
    const auto wanted = query();
    auto wrong = node(
        wanted, "USB\\VID_18D1&PID_4EE0&MI_00\\SAME-SERIAL-OTHER-PORT");
    wrong.port_numbers = {8U, 4U};
    wrong.location_path_utf8 = location_path(wrong.port_numbers);
    auto correct = node(wanted);
    wrong.serial_utf8 = correct.serial_utf8;
    auto backend = stable_backend({wrong, correct});
    WindowsUsbTopologyDiscovery discovery(backend);

    const auto result = discovery.discover(wanted);
    CHECK(result.has_value());
    CHECK(result->physical_port_path == "usb:1-2.3");
    CHECK(result->device_instance_id_utf8 == correct.device_instance_id_utf8);
}

void serial_and_complete_interface_fingerprint_are_secondary_guards() {
    const auto wanted = query();

    auto missing_serial = node(wanted);
    missing_serial.serial_utf8.reset();
    auto serial_backend = stable_backend({missing_serial});
    WindowsUsbTopologyDiscovery serial_discovery(serial_backend);
    const auto serial_result = serial_discovery.discover(wanted);
    CHECK(!serial_result.has_value());
    CHECK(serial_result.error().kind ==
          WindowsUsbTopologyErrorKind::IdentityMismatch);

    auto changed_interface = node(wanted);
    changed_interface.interface_fingerprint.interface_protocol = 0x02U;
    auto interface_backend = stable_backend({changed_interface});
    WindowsUsbTopologyDiscovery interface_discovery(interface_backend);
    const auto interface_result = interface_discovery.discover(wanted);
    CHECK(!interface_result.has_value());
    CHECK(interface_result.error().kind ==
          WindowsUsbTopologyErrorKind::IdentityMismatch);
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

void backend_error_metadata_is_preserved_verbatim() {
    const auto wanted = query();
    const WindowsUsbTopologyError expected{
        .kind = WindowsUsbTopologyErrorKind::PermissionDenied,
        .stage = WindowsUsbTopologyStage::PropertyRead,
        .native_domain = WindowsUsbNativeErrorDomain::ConfigurationManager,
        .native_code = 0x33U,
        .device_instance_id_utf8 = "USB\\VID_18D1&PID_4EE0\\DENIED",
        .message = "property access denied",
    };
    FakeBackend backend;
    backend.results = {std::unexpected(expected)};
    WindowsUsbTopologyDiscovery discovery(backend);

    const auto result = discovery.discover(wanted);
    CHECK(!result.has_value());
    CHECK(result.error() == expected);
    CHECK(backend.calls == 1U);
}

void malformed_and_invalid_inputs_never_publish_topology() {
    auto wanted = query();
    FakeBackend invalid_backend;
    invalid_backend.results = {std::vector<WindowsUsbTopologyNode>{}};
    WindowsUsbTopologyDiscovery invalid_discovery(invalid_backend);

    wanted.port_numbers = {2U, 0U};
    const auto invalid = invalid_discovery.discover(wanted);
    CHECK(!invalid.has_value());
    CHECK(invalid.error().kind == WindowsUsbTopologyErrorKind::InvalidArgument);
    CHECK(invalid_backend.calls == 0U);

    wanted = query();
    wanted.port_numbers.assign(17U, 1U);
    const auto deep = invalid_discovery.discover(wanted);
    CHECK(!deep.has_value());
    CHECK(deep.error().kind == WindowsUsbTopologyErrorKind::TopologyTooDeep);
    CHECK(invalid_backend.calls == 0U);

    wanted = query();
    auto malformed_node = node(wanted);
    malformed_node.root_hub_index = 2U;
    auto malformed_backend = stable_backend({malformed_node});
    WindowsUsbTopologyDiscovery malformed_discovery(malformed_backend);
    const auto malformed = malformed_discovery.discover(wanted);
    CHECK(!malformed.has_value());
    CHECK(malformed.error().kind ==
          WindowsUsbTopologyErrorKind::MalformedSnapshot);
    CHECK(malformed.error().device_instance_id_utf8 ==
          malformed_node.device_instance_id_utf8);

    auto duplicate_hub = node(wanted);
    duplicate_hub.hub_instance_ids_utf8.push_back(
        "usb\\root_hub30\\root-01");
    auto duplicate_hub_backend = stable_backend({duplicate_hub});
    WindowsUsbTopologyDiscovery duplicate_hub_discovery(duplicate_hub_backend);
    const auto duplicate = duplicate_hub_discovery.discover(wanted);
    CHECK(!duplicate.has_value());
    CHECK(duplicate.error().kind ==
          WindowsUsbTopologyErrorKind::MalformedSnapshot);
}

void location_parser_is_strict_and_bounded() {
    const auto parsed = parse_windows_usb_location_path(
        "PCIROOT(0)#PCI(1400)#USBROOT(1)#USB(2)#USB(11)");
    CHECK(parsed.has_value());
    CHECK(parsed->controller_prefix_utf8 == "PCIROOT(0)#PCI(1400)");
    CHECK(parsed->root_hub_index == 1U);
    CHECK(parsed->port_numbers == std::vector<std::uint8_t>({2U, 11U}));

    const auto zero_port = parse_windows_usb_location_path(
        "PCIROOT(0)#PCI(1400)#USBROOT(0)#USB(0)");
    CHECK(!zero_port.has_value());
    CHECK(zero_port.error().kind ==
          WindowsUsbTopologyErrorKind::MalformedSnapshot);

    const auto trailing_component = parse_windows_usb_location_path(
        "PCIROOT(0)#USBROOT(0)#USB(1)#PCI(0)");
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

void libusb_snapshot_adapter_preserves_the_interface_fingerprint() {
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

    const auto adapted = make_windows_usb_topology_query(device);
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

void production_backend_is_explicitly_platform_gated() {
#if !defined(_WIN32)
    SetupApiWindowsUsbTopologyBackend backend;
    const auto result = backend.read_candidates(
        query(), std::chrono::steady_clock::time_point::max(), {});
    CHECK(!result.has_value());
    CHECK(result.error().kind ==
          WindowsUsbTopologyErrorKind::UnsupportedPlatform);
#endif
}

}  // namespace

int main() {
    const std::vector<std::pair<std::string_view, void (*)()>> tests{
        {"stable mapping", stable_snapshot_maps_all_consumer_identity},
        {"order-independent snapshot", backend_order_does_not_create_a_false_race},
        {"TOCTOU", changed_snapshot_fails_closed_before_correlation},
        {"ambiguous duplicate serial", ambiguity_and_duplicate_serial_fail_closed},
        {"duplicate serial isolation", duplicate_serial_on_another_port_never_crosses_devices},
        {"secondary identity guards", serial_and_complete_interface_fingerprint_are_secondary_guards},
        {"cancellation", cancellation_before_and_between_snapshots_is_deterministic},
        {"timeout", timeout_before_and_between_snapshots_is_deterministic},
        {"native error metadata", backend_error_metadata_is_preserved_verbatim},
        {"invalid and malformed", malformed_and_invalid_inputs_never_publish_topology},
        {"location parser", location_parser_is_strict_and_bounded},
        {"libusb adapter", libusb_snapshot_adapter_preserves_the_interface_fingerprint},
        {"physical path", physical_path_format_rejects_unsafe_identity},
        {"platform gate", production_backend_is_explicitly_platform_gated},
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
