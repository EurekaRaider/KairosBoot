// SPDX-License-Identifier: MIT
#include "src/transport/macos_usb_topology.hpp"

#include "src/transport/libusb_runtime.hpp"

#include <chrono>
#include <cstdint>
#include <exception>
#include <expected>
#include <functional>
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
using kairosboot::transport::IMacUsbRegistryBackend;
using kairosboot::transport::MacUsbDecodedLocation;
using kairosboot::transport::MacUsbInterfaceFingerprint;
using kairosboot::transport::MacUsbRegistryInterface;
using kairosboot::transport::MacUsbRegistryNode;
using kairosboot::transport::MacUsbTopology;
using kairosboot::transport::MacUsbTopologyClock;
using kairosboot::transport::MacUsbTopologyDiscovery;
using kairosboot::transport::MacUsbTopologyError;
using kairosboot::transport::MacUsbTopologyErrorKind;
using kairosboot::transport::MacUsbTopologyQuery;
using kairosboot::transport::MacUsbTopologyStage;
using kairosboot::transport::MacUsbTopologyTimePoint;
using kairosboot::transport::UsbDeviceInfo;
using kairosboot::transport::canonical_macos_usb_port_path;
using kairosboot::transport::decode_macos_usb_location_id;
using kairosboot::transport::make_macos_usb_topology_query;

#define CHECK(condition)                                                         \
    do {                                                                         \
        if (!(condition)) {                                                       \
            throw std::runtime_error(                                             \
                std::string("check failed at line ") + std::to_string(__LINE__) + \
                ": " #condition);                                                \
        }                                                                        \
    } while (false)

[[nodiscard]] MacUsbInterfaceFingerprint fingerprint(
    const std::uint8_t interface_number = 0U,
    const std::uint8_t interface_protocol = 0x03U) {
    return MacUsbInterfaceFingerprint{
        .configuration_value = 1U,
        .interface_number = interface_number,
        .alternate_setting = 0U,
        .interface_class = 0xFFU,
        .interface_subclass = 0x42U,
        .interface_protocol = interface_protocol,
    };
}

[[nodiscard]] std::uint32_t location_id(
    const std::uint8_t bus,
    const std::vector<std::uint8_t>& ports) {
    std::uint32_t value = static_cast<std::uint32_t>(bus) << 24U;
    constexpr unsigned int shifts[]{20U, 16U, 12U, 8U, 4U, 0U};
    CHECK(ports.size() <= std::size(shifts));
    for (std::size_t index = 0U; index < ports.size(); ++index) {
        CHECK(ports[index] > 0U && ports[index] <= 0x0FU);
        value |= static_cast<std::uint32_t>(ports[index]) << shifts[index];
    }
    return value;
}

[[nodiscard]] MacUsbTopologyQuery query(
    const std::uint8_t bus = 0U,
    const std::uint8_t address = 5U,
    std::vector<std::uint8_t> ports = {2U, 3U}) {
    return MacUsbTopologyQuery{
        .vendor_id = 0x18D1U,
        .product_id = 0x4EE0U,
        .bus_number = bus,
        .device_address = address,
        .port_numbers = std::move(ports),
        .interface_fingerprint = fingerprint(),
        .serial_utf8 = std::string{"SERIAL-01"},
        .product_utf8 = std::string{"Kairos device"},
    };
}

[[nodiscard]] MacUsbRegistryNode node(
    const MacUsbTopologyQuery& wanted,
    const std::uint64_t entry_id = 0x100U,
    const std::uint64_t interface_entry_id = 0x200U,
    const std::uint64_t session_id = 0x300U,
    const std::uint64_t controller_entry_id = 0xA0U) {
    return MacUsbRegistryNode{
        .registry_entry_id = entry_id,
        .session_id = session_id,
        .root_controller_registry_entry_id = controller_entry_id,
        .location_id = location_id(wanted.bus_number, wanted.port_numbers),
        .vendor_id = wanted.vendor_id,
        .product_id = wanted.product_id,
        .bus_number = wanted.bus_number,
        .device_address = wanted.device_address,
        .port_numbers = wanted.port_numbers,
        .serial_utf8 = wanted.serial_utf8,
        .product_utf8 = wanted.product_utf8,
        .registry_path = "IOUSB:/controller/device-" + std::to_string(entry_id),
        .root_controller_registry_path =
            "IOUSB:/controller-" + std::to_string(controller_entry_id),
        .interfaces = {MacUsbRegistryInterface{
            .registry_entry_id = interface_entry_id,
            .fingerprint = wanted.interface_fingerprint,
            .registry_path =
                "IOService:/device/interface-" +
                std::to_string(interface_entry_id),
        }},
    };
}

class ScriptedBackend final : public IMacUsbRegistryBackend {
public:
    using Result = std::expected<std::vector<MacUsbRegistryNode>,
                                 MacUsbTopologyError>;

    std::vector<Result> results;
    std::function<void(std::size_t)> after_call;
    mutable std::size_t calls{};

    [[nodiscard]] Result snapshot(
        const MacUsbTopologyQuery&,
        const MacUsbTopologyTimePoint,
        const std::stop_token) const override {
        const auto index = calls++;
        if (index >= results.size()) {
            throw std::runtime_error("unexpected registry snapshot call");
        }
        auto result = results[index];
        if (after_call) {
            after_call(calls);
        }
        return result;
    }
};

[[nodiscard]] ScriptedBackend::Result snapshot_result(
    std::vector<MacUsbRegistryNode> nodes) {
    return ScriptedBackend::Result{std::move(nodes)};
}

[[nodiscard]] MacUsbTopology discover_stable(
    const MacUsbTopologyQuery& wanted,
    std::vector<MacUsbRegistryNode> snapshot) {
    ScriptedBackend backend;
    backend.results = {snapshot_result(snapshot), snapshot_result(std::move(snapshot))};
    MacUsbTopologyDiscovery discovery(backend);
    auto result = discovery.discover(
        wanted, MacUsbTopologyClock::now() + 1h);
    CHECK(result.has_value());
    CHECK(backend.calls == 2U);
    return *result;
}

void bus_zero_and_complete_fingerprint_map_to_consumers() {
    const auto wanted = query();
    const auto topology = discover_stable(wanted, {node(wanted)});

    CHECK(topology.physical_port_path == "usb:0-2.3");
    CHECK(topology.root_controller_id ==
          "macos-iokit:00000000000000a0");
    CHECK(topology.hub_port_chain == wanted.port_numbers);
    CHECK(topology.registry_entry_id == 0x100U);
    CHECK(topology.interface_registry_entry_id == 0x200U);
    CHECK(topology.location_id == 0x00230000U);
    CHECK(topology.bus_number == 0U);
    CHECK(topology.device_address == wanted.device_address);
    CHECK(topology.interface_fingerprint == wanted.interface_fingerprint);
    CHECK(topology.serial_utf8 == wanted.serial_utf8);
    CHECK(topology.product_utf8 == wanted.product_utf8);
}

void location_id_and_canonical_path_validation_are_strict() {
    const auto decoded = decode_macos_usb_location_id(0xAB123000U);
    CHECK(decoded.has_value());
    CHECK((*decoded == MacUsbDecodedLocation{
                          .bus_number = 0xABU,
                          .port_numbers = {1U, 2U, 3U},
                      }));

    const auto hole = decode_macos_usb_location_id(0xAB102000U);
    CHECK(!hole.has_value());
    CHECK(hole.error().kind == MacUsbTopologyErrorKind::MalformedRegistry);

    const auto no_port = decode_macos_usb_location_id(0xAB000000U);
    CHECK(!no_port.has_value());
    CHECK(no_port.error().kind == MacUsbTopologyErrorKind::MalformedRegistry);

    const auto path = canonical_macos_usb_port_path(0U, {15U, 1U});
    CHECK((path ==
           std::expected<std::string, MacUsbTopologyError>{"usb:0-15.1"}));
    const auto invalid_port = canonical_macos_usb_port_path(0U, {16U});
    CHECK(!invalid_port.has_value());
    CHECK(invalid_port.error().kind ==
          MacUsbTopologyErrorKind::InvalidArgument);
}

void libusb_snapshot_adapter_preserves_the_interface_fingerprint() {
    UsbDeviceInfo device{};
    device.vendor_id = 0x18D1U;
    device.product_id = 0x4EE0U;
    device.bus_number = 0U;
    device.device_address = 9U;
    device.configuration_value = 2U;
    device.port_path = {15U, 4U};
    device.serial_utf8 = "SERIAL-ADAPTER";
    device.interface_number = 7U;
    device.alternate_setting = 1U;
    device.interface_class = 0xFFU;
    device.interface_subclass = 0x42U;
    device.interface_protocol = 0x03U;

    const auto adapted = make_macos_usb_topology_query(
        device, "Product descriptor");
    CHECK(adapted.vendor_id == device.vendor_id);
    CHECK(adapted.product_id == device.product_id);
    CHECK(adapted.bus_number == 0U);
    CHECK(adapted.device_address == device.device_address);
    CHECK(adapted.port_numbers == device.port_path);
    CHECK(adapted.serial_utf8 ==
          std::optional<std::string>{device.serial_utf8});
    CHECK(adapted.product_utf8 ==
          std::optional<std::string>{"Product descriptor"});
    CHECK((adapted.interface_fingerprint == MacUsbInterfaceFingerprint{
              .configuration_value = 2U,
              .interface_number = 7U,
              .alternate_setting = 1U,
              .interface_class = 0xFFU,
              .interface_subclass = 0x42U,
              .interface_protocol = 0x03U,
          }));
}

void cancellation_before_and_between_snapshots_never_publishes_topology() {
    const auto wanted = query();
    std::stop_source already_cancelled;
    already_cancelled.request_stop();
    ScriptedBackend untouched;
    MacUsbTopologyDiscovery early(untouched);
    const auto cancelled = early.discover(
        wanted,
        MacUsbTopologyClock::now() + 1h,
        already_cancelled.get_token());
    CHECK(!cancelled.has_value());
    CHECK(cancelled.error().kind == MacUsbTopologyErrorKind::Cancelled);
    CHECK(cancelled.error().stage == MacUsbTopologyStage::Validation);
    CHECK(untouched.calls == 0U);

    std::stop_source between_source;
    ScriptedBackend between;
    between.results = {snapshot_result({node(wanted)}),
                       snapshot_result({node(wanted)})};
    between.after_call = [&between_source](const std::size_t calls) {
        if (calls == 1U) {
            between_source.request_stop();
        }
    };
    MacUsbTopologyDiscovery between_discovery(between);
    const auto interrupted = between_discovery.discover(
        wanted,
        MacUsbTopologyClock::now() + 1h,
        between_source.get_token());
    CHECK(!interrupted.has_value());
    CHECK(interrupted.error().kind == MacUsbTopologyErrorKind::Cancelled);
    CHECK(interrupted.error().stage ==
          MacUsbTopologyStage::FinalValidation);
    CHECK(between.calls == 1U);

    std::stop_source after_source;
    ScriptedBackend after;
    after.results = {snapshot_result({node(wanted)}),
                     snapshot_result({node(wanted)})};
    after.after_call = [&after_source](const std::size_t calls) {
        if (calls == 2U) {
            after_source.request_stop();
        }
    };
    MacUsbTopologyDiscovery after_discovery(after);
    const auto cancelled_after_validation = after_discovery.discover(
        wanted,
        MacUsbTopologyClock::now() + 1h,
        after_source.get_token());
    CHECK(!cancelled_after_validation.has_value());
    CHECK(cancelled_after_validation.error().kind ==
          MacUsbTopologyErrorKind::Cancelled);
    CHECK(cancelled_after_validation.error().stage ==
          MacUsbTopologyStage::FinalValidation);
    CHECK(after.calls == 2U);
}

void expired_deadline_never_reaches_the_registry_backend() {
    const auto wanted = query();
    ScriptedBackend backend;
    MacUsbTopologyDiscovery discovery(backend);
    const auto timed_out = discovery.discover(
        wanted, MacUsbTopologyClock::now() - 1ms);
    CHECK(!timed_out.has_value());
    CHECK(timed_out.error().kind == MacUsbTopologyErrorKind::Timeout);
    CHECK(timed_out.error().stage == MacUsbTopologyStage::Validation);
    CHECK(backend.calls == 0U);
}

void backend_timeout_and_native_metadata_are_preserved_verbatim() {
    const auto wanted = query();
    const MacUsbTopologyError expected{
        .kind = MacUsbTopologyErrorKind::Timeout,
        .stage = MacUsbTopologyStage::InterfaceEnumeration,
        .native_code = -536870186,
        .registry_path = "IOService:/device/interface",
        .message = "deadline expired in registry backend",
    };
    ScriptedBackend backend;
    backend.results = {std::unexpected(expected)};
    MacUsbTopologyDiscovery discovery(backend);
    const auto failed = discovery.discover(
        wanted, MacUsbTopologyClock::now() + 1h);
    CHECK(!failed.has_value());
    CHECK(failed.error() == expected);
    CHECK(backend.calls == 1U);
}

void two_snapshot_toctou_changes_fail_closed() {
    const auto wanted = query();
    auto before = node(wanted);
    auto after = before;
    after.session_id += 1U;
    ScriptedBackend backend;
    backend.results = {snapshot_result({before}), snapshot_result({after})};
    MacUsbTopologyDiscovery discovery(backend);
    const auto changed = discovery.discover(
        wanted, MacUsbTopologyClock::now() + 1h);
    CHECK(!changed.has_value());
    CHECK(changed.error().kind ==
          MacUsbTopologyErrorKind::IdentityChanged);
    CHECK(changed.error().stage ==
          MacUsbTopologyStage::FinalValidation);

    auto address_after = before;
    address_after.device_address += 1U;
    address_after.session_id += 1U;
    ScriptedBackend reenumerated;
    reenumerated.results = {snapshot_result({before}),
                            snapshot_result({address_after})};
    MacUsbTopologyDiscovery reenumerated_discovery(reenumerated);
    const auto raced = reenumerated_discovery.discover(
        wanted, MacUsbTopologyClock::now() + 1h);
    CHECK(!raced.has_value());
    CHECK(raced.error().kind ==
          MacUsbTopologyErrorKind::IdentityChanged);
}

void deterministic_snapshot_order_does_not_create_a_false_race() {
    const auto wanted = query();
    auto other_query = query(1U, 8U, {4U});
    other_query.serial_utf8 = wanted.serial_utf8;
    auto matched = node(wanted);
    auto other = node(other_query, 0x101U, 0x201U, 0x301U, 0xA1U);

    ScriptedBackend backend;
    backend.results = {snapshot_result({other, matched}),
                       snapshot_result({matched, other})};
    MacUsbTopologyDiscovery discovery(backend);
    const auto result = discovery.discover(
        wanted, MacUsbTopologyClock::now() + 1h);
    CHECK(result.has_value());
    CHECK(result->registry_entry_id == matched.registry_entry_id);
}

void duplicate_serials_are_never_used_as_the_physical_key() {
    const auto wanted = query();
    auto other_query = query(1U, 8U, {4U});
    other_query.serial_utf8 = wanted.serial_utf8;
    const auto matched = node(wanted);
    const auto same_serial_elsewhere =
        node(other_query, 0x101U, 0x201U, 0x301U, 0xA1U);

    const auto topology = discover_stable(
        wanted, {same_serial_elsewhere, matched});
    CHECK(topology.registry_entry_id == matched.registry_entry_id);
    CHECK(topology.physical_port_path == "usb:0-2.3");
}

void duplicate_device_and_interface_mappings_are_ambiguous() {
    const auto wanted = query();
    const auto first = node(wanted);
    const auto second = node(wanted, 0x101U, 0x201U, 0x301U, 0xA0U);

    ScriptedBackend duplicate_devices;
    duplicate_devices.results = {snapshot_result({first, second}),
                                 snapshot_result({first, second})};
    MacUsbTopologyDiscovery device_discovery(duplicate_devices);
    const auto device_result = device_discovery.discover(
        wanted, MacUsbTopologyClock::now() + 1h);
    CHECK(!device_result.has_value());
    CHECK(device_result.error().kind ==
          MacUsbTopologyErrorKind::AmbiguousMapping);

    auto duplicate_interface = first;
    duplicate_interface.interfaces.push_back(MacUsbRegistryInterface{
        .registry_entry_id = 0x202U,
        .fingerprint = wanted.interface_fingerprint,
        .registry_path = "IOService:/device/interface-514",
    });
    ScriptedBackend duplicate_interfaces;
    duplicate_interfaces.results = {
        snapshot_result({duplicate_interface}),
        snapshot_result({duplicate_interface})};
    MacUsbTopologyDiscovery interface_discovery(duplicate_interfaces);
    const auto interface_result = interface_discovery.discover(
        wanted, MacUsbTopologyClock::now() + 1h);
    CHECK(!interface_result.has_value());
    CHECK(interface_result.error().kind ==
          MacUsbTopologyErrorKind::AmbiguousMapping);
}

void mismatched_and_missing_identities_fail_closed() {
    const auto wanted = query();
    auto wrong_interface = node(wanted);
    wrong_interface.interfaces.front().fingerprint = fingerprint(1U);
    ScriptedBackend interface_backend;
    interface_backend.results = {snapshot_result({wrong_interface}),
                                 snapshot_result({wrong_interface})};
    MacUsbTopologyDiscovery interface_discovery(interface_backend);
    const auto interface_result = interface_discovery.discover(
        wanted, MacUsbTopologyClock::now() + 1h);
    CHECK(!interface_result.has_value());
    CHECK(interface_result.error().kind ==
          MacUsbTopologyErrorKind::IdentityMismatch);

    auto missing_serial = node(wanted);
    missing_serial.serial_utf8.reset();
    ScriptedBackend serial_backend;
    serial_backend.results = {snapshot_result({missing_serial}),
                              snapshot_result({missing_serial})};
    MacUsbTopologyDiscovery serial_discovery(serial_backend);
    const auto serial_result = serial_discovery.discover(
        wanted, MacUsbTopologyClock::now() + 1h);
    CHECK(!serial_result.has_value());
    CHECK(serial_result.error().kind ==
          MacUsbTopologyErrorKind::IdentityMismatch);

    ScriptedBackend absent;
    absent.results = {snapshot_result({}), snapshot_result({})};
    MacUsbTopologyDiscovery absent_discovery(absent);
    const auto not_found = absent_discovery.discover(
        wanted, MacUsbTopologyClock::now() + 1h);
    CHECK(!not_found.has_value());
    CHECK(not_found.error().kind == MacUsbTopologyErrorKind::NotFound);
}

void malformed_backend_nodes_and_invalid_queries_never_pass() {
    const auto wanted = query();
    auto malformed = node(wanted);
    malformed.location_id = 0x00102000U;
    ScriptedBackend malformed_backend;
    malformed_backend.results = {snapshot_result({malformed}),
                                 snapshot_result({malformed})};
    MacUsbTopologyDiscovery malformed_discovery(malformed_backend);
    const auto malformed_result = malformed_discovery.discover(
        wanted, MacUsbTopologyClock::now() + 1h);
    CHECK(!malformed_result.has_value());
    CHECK(malformed_result.error().kind ==
          MacUsbTopologyErrorKind::MalformedRegistry);

    auto zero_port = wanted;
    zero_port.port_numbers = {2U, 0U};
    ScriptedBackend untouched;
    MacUsbTopologyDiscovery invalid_discovery(untouched);
    const auto invalid = invalid_discovery.discover(
        zero_port, MacUsbTopologyClock::now() + 1h);
    CHECK(!invalid.has_value());
    CHECK(invalid.error().kind ==
          MacUsbTopologyErrorKind::InvalidArgument);
    CHECK(untouched.calls == 0U);

    auto too_deep = wanted;
    too_deep.port_numbers.assign(7U, 1U);
    const auto deep = invalid_discovery.discover(
        too_deep, MacUsbTopologyClock::now() + 1h);
    CHECK(!deep.has_value());
    CHECK(deep.error().kind ==
          MacUsbTopologyErrorKind::TopologyTooDeep);
    CHECK(untouched.calls == 0U);
}

}  // namespace

int main() {
    const std::vector<std::pair<std::string_view, void (*)()>> tests{
        {"bus zero and fingerprint", bus_zero_and_complete_fingerprint_map_to_consumers},
        {"location and canonical path", location_id_and_canonical_path_validation_are_strict},
        {"libusb adapter", libusb_snapshot_adapter_preserves_the_interface_fingerprint},
        {"cancellation", cancellation_before_and_between_snapshots_never_publishes_topology},
        {"deadline", expired_deadline_never_reaches_the_registry_backend},
        {"error metadata", backend_timeout_and_native_metadata_are_preserved_verbatim},
        {"TOCTOU", two_snapshot_toctou_changes_fail_closed},
        {"snapshot order", deterministic_snapshot_order_does_not_create_a_false_race},
        {"duplicate serial", duplicate_serials_are_never_used_as_the_physical_key},
        {"ambiguity", duplicate_device_and_interface_mappings_are_ambiguous},
        {"identity mismatch", mismatched_and_missing_identities_fail_closed},
        {"invalid contracts", malformed_backend_nodes_and_invalid_queries_never_pass},
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
