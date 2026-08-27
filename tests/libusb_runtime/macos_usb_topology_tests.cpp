// SPDX-License-Identifier: MIT
#include "src/transport/macos_usb_topology.hpp"

#include "src/transport/libusb_runtime.hpp"

#include <array>
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
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using kairosboot::transport::IMacUsbRegistryBackend;
using kairosboot::transport::IMacUsbRegistrySnapshotSource;
using kairosboot::transport::IokitMacUsbRegistryBackend;
using kairosboot::transport::MacUsbInterfaceFingerprint;
using kairosboot::transport::MacUsbRegistryAncestor;
using kairosboot::transport::MacUsbRegistryEntryKind;
using kairosboot::transport::MacUsbRegistryInterface;
using kairosboot::transport::MacUsbRegistryNode;
using kairosboot::transport::MacUsbTopology;
using kairosboot::transport::MacUsbTopologyClock;
using kairosboot::transport::MacUsbTopologyDiscovery;
using kairosboot::transport::MacUsbTopologyDeviceQuery;
using kairosboot::transport::MacUsbTopologyDeviceResult;
using kairosboot::transport::MacUsbTopologyError;
using kairosboot::transport::MacUsbTopologyErrorKind;
using kairosboot::transport::MacUsbTopologyQuery;
using kairosboot::transport::MacUsbTopologyStage;
using kairosboot::transport::MacUsbTopologyTimePoint;
using kairosboot::transport::UsbDeviceInfo;
using kairosboot::transport::canonical_macos_usb_port_path;
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

[[nodiscard]] std::uint32_t location_id(const std::uint8_t bus) {
    // The low 24 bits are deliberately unrelated to the port chain. Frozen
    // libusb 1.0.30 consumes only the top byte for Darwin bus numbering.
    return (static_cast<std::uint32_t>(bus) << 24U) | 0x00FEDCBAU;
}

[[nodiscard]] MacUsbTopologyQuery query(
    const std::uint8_t bus = 0U,
    const std::uint8_t address = 5U,
    std::vector<std::uint8_t> ports = {2U, 3U},
    const std::uint64_t session_id = 0x300U) {
    return MacUsbTopologyQuery{
        .vendor_id = 0x18D1U,
        .product_id = 0x4EE0U,
        .bus_number = bus,
        .device_address = address,
        .session_id = session_id,
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
    const std::uint64_t session_id = 0U,
    const std::uint64_t controller_entry_id = 0xA0U,
    const MacUsbRegistryEntryKind controller_kind =
        MacUsbRegistryEntryKind::HostController) {
    return MacUsbRegistryNode{
        .registry_entry_id = entry_id,
        .session_id = session_id == 0U ? wanted.session_id : session_id,
        .location_id = location_id(wanted.bus_number),
        .vendor_id = wanted.vendor_id,
        .product_id = wanted.product_id,
        .bus_number = wanted.bus_number,
        .device_address = wanted.device_address,
        .port_numbers = wanted.port_numbers,
        .serial_utf8 = wanted.serial_utf8,
        .product_utf8 = wanted.product_utf8,
        .registry_path = "IOUSB:/controller/device-" + std::to_string(entry_id),
        .service_ancestry = {
            MacUsbRegistryAncestor{
                .registry_entry_id = controller_entry_id + 1U,
                .kind = MacUsbRegistryEntryKind::UsbRootHub,
                .registry_path = "IOService:/root-hub-" +
                    std::to_string(controller_entry_id + 1U),
            },
            MacUsbRegistryAncestor{
                .registry_entry_id = controller_entry_id,
                .kind = controller_kind,
                .registry_path = "IOService:/controller-" +
                    std::to_string(controller_entry_id),
            },
        },
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
    mutable std::vector<std::size_t> query_counts;
    mutable std::vector<MacUsbTopologyTimePoint> deadlines;

    [[nodiscard]] Result snapshot(
        const std::span<const MacUsbTopologyQuery> queries,
        const MacUsbTopologyTimePoint deadline,
        const std::stop_token) const override {
        query_counts.push_back(queries.size());
        deadlines.push_back(deadline);
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

class ScriptedSource final : public IMacUsbRegistrySnapshotSource {
public:
    using Result = std::expected<std::vector<MacUsbRegistryNode>,
                                 MacUsbTopologyError>;

    Result result{std::vector<MacUsbRegistryNode>{}};
    std::function<void()> before_return;
    mutable std::size_t calls{};
    mutable std::vector<std::size_t> query_counts;
    mutable std::vector<MacUsbTopologyTimePoint> deadlines;

    [[nodiscard]] Result snapshot(
        const std::span<const MacUsbTopologyQuery> queries,
        const MacUsbTopologyTimePoint deadline,
        const std::stop_token) const override {
        ++calls;
        query_counts.push_back(queries.size());
        deadlines.push_back(deadline);
        if (before_return) {
            before_return();
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
    CHECK(topology.location_id == 0x00FEDCBAU);
    CHECK(topology.bus_number == 0U);
    CHECK(topology.device_address == wanted.device_address);
    CHECK(topology.interface_fingerprint == wanted.interface_fingerprint);
    CHECK(topology.serial_utf8 == wanted.serial_utf8);
    CHECK(topology.product_utf8 == wanted.product_utf8);
}

void location_id_is_bus_only_and_uint8_ports_are_valid() {
    auto wanted = query(0xABU, 5U, {16U, 255U});
    auto observed = node(wanted);
    observed.location_id = 0xAB102000U;
    const auto topology = discover_stable(wanted, {observed});
    CHECK(topology.bus_number == 0xABU);
    CHECK(topology.hub_port_chain ==
          std::vector<std::uint8_t>({16U, 255U}));

    const auto path = canonical_macos_usb_port_path(0U, {255U, 16U});
    CHECK((path ==
           std::expected<std::string, MacUsbTopologyError>{"usb:0-255.16"}));
    const auto invalid_port = canonical_macos_usb_port_path(0U, {0U});
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
    device.backend_session_id = 0x1122334455667788ULL;
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
    CHECK(adapted.session_id == device.backend_session_id);
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
    auto other_query = query(1U, 8U, {4U}, 0x301U);
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

void multi_interface_device_uses_one_generation_or_fails_closed() {
    const auto first_query = query();
    auto second_query = first_query;
    second_query.interface_fingerprint = fingerprint(1U);
    const std::array queries{first_query, second_query};

    auto complete = node(first_query);
    complete.interfaces.push_back(MacUsbRegistryInterface{
        .registry_entry_id = 0x201U,
        .fingerprint = second_query.interface_fingerprint,
        .registry_path = "IOService:/device/interface-513",
    });
    ScriptedBackend stable;
    stable.results = {snapshot_result({complete}), snapshot_result({complete})};
    MacUsbTopologyDiscovery stable_discovery(stable);
    const auto resolved = stable_discovery.discover_device(
        queries, MacUsbTopologyClock::now() + 1h);
    CHECK(resolved.has_value());
    CHECK(resolved->size() == 2U);
    CHECK(stable.calls == 2U);
    CHECK((*resolved)[0].session_id == first_query.session_id);
    CHECK((*resolved)[1].session_id == first_query.session_id);
    CHECK((*resolved)[0].interface_registry_entry_id == 0x200U);
    CHECK((*resolved)[1].interface_registry_entry_id == 0x201U);

    auto next_generation = complete;
    next_generation.interfaces.back().registry_entry_id = 0x301U;
    next_generation.interfaces.back().registry_path =
        "IOService:/device/interface-769";
    ScriptedBackend raced;
    raced.results = {snapshot_result({complete}),
                     snapshot_result({next_generation})};
    MacUsbTopologyDiscovery raced_discovery(raced);
    const auto changed = raced_discovery.discover_device(
        queries, MacUsbTopologyClock::now() + 1h);
    CHECK(!changed.has_value());
    CHECK(changed.error().kind == MacUsbTopologyErrorKind::IdentityChanged);
    CHECK(changed.error().stage == MacUsbTopologyStage::FinalValidation);

    auto incomplete = complete;
    incomplete.interfaces.pop_back();
    ScriptedBackend missing_interface;
    missing_interface.results = {snapshot_result({incomplete}),
                                 snapshot_result({incomplete})};
    MacUsbTopologyDiscovery missing_discovery(missing_interface);
    const auto missing = missing_discovery.discover_device(
        queries, MacUsbTopologyClock::now() + 1h);
    CHECK(!missing.has_value());
    CHECK(missing.error().kind == MacUsbTopologyErrorKind::IdentityMismatch);
}

void thirty_two_devices_share_exactly_two_global_snapshot_passes() {
    constexpr std::size_t device_count = 32U;
    std::vector<MacUsbTopologyDeviceQuery> devices;
    std::vector<MacUsbRegistryNode> nodes;
    devices.reserve(device_count);
    nodes.reserve(device_count);

    for (std::size_t index = 0U; index < device_count; ++index) {
        auto first = query(
            0U,
            static_cast<std::uint8_t>(index + 1U),
            {static_cast<std::uint8_t>(index + 1U)},
            0x1000U + index);
        first.serial_utf8 = std::string{"DUPLICATE-SERIAL"};
        auto second = first;
        second.interface_fingerprint = fingerprint(1U);
        devices.push_back(MacUsbTopologyDeviceQuery{
            .interfaces = {first, second},
        });

        auto registry_node = node(
            first,
            0x2000U + index,
            0x3000U + (index * 2U),
            first.session_id,
            0x4000U + (index % 2U));
        registry_node.interfaces.push_back(MacUsbRegistryInterface{
            .registry_entry_id = 0x3001U + (index * 2U),
            .fingerprint = second.interface_fingerprint,
            .registry_path = "IOService:/device/interface-second-" +
                std::to_string(index),
        });
        nodes.push_back(std::move(registry_node));
    }

    ScriptedSource source;
    source.result = nodes;
    IokitMacUsbRegistryBackend backend(source);
    MacUsbTopologyDiscovery discovery(backend);
    const auto deadline = MacUsbTopologyClock::now() + 1h;
    const auto resolved = discovery.discover_devices(devices, deadline);

    CHECK(resolved.has_value());
    CHECK(resolved->size() == device_count);
    CHECK(source.calls == 2U);
    CHECK(source.query_counts == std::vector<std::size_t>({device_count,
                                                          device_count}));
    for (std::size_t index = 0U; index < device_count; ++index) {
        const auto& device = (*resolved)[index];
        CHECK(device.has_value());
        CHECK(device->size() == 2U);
        CHECK((*device)[0].session_id == (*device)[1].session_id);
        CHECK((*device)[0].registry_entry_id == (*device)[1].registry_entry_id);
        CHECK((*device)[0].root_controller_id ==
              (*device)[1].root_controller_id);
        CHECK((*device)[0].interface_fingerprint ==
              devices[index].interfaces[0].interface_fingerprint);
        CHECK((*device)[1].interface_fingerprint ==
              devices[index].interfaces[1].interface_fingerprint);
        CHECK((*device)[0].serial_utf8 ==
              std::optional<std::string>{"DUPLICATE-SERIAL"});
        CHECK((*device)[0].interface_registry_entry_id !=
              (*device)[1].interface_registry_entry_id);
    }
}

void batch_generation_change_fails_the_whole_device_only() {
    auto first_device = query(0U, 1U, {1U}, 0x501U);
    auto first_device_second = first_device;
    first_device_second.interface_fingerprint = fingerprint(1U);
    auto second_device = query(0U, 2U, {2U}, 0x502U);
    auto second_device_second = second_device;
    second_device_second.interface_fingerprint = fingerprint(1U);
    const std::array devices{
        MacUsbTopologyDeviceQuery{
            .interfaces = {first_device, first_device_second},
        },
        MacUsbTopologyDeviceQuery{
            .interfaces = {second_device, second_device_second},
        },
    };

    auto first_node = node(first_device, 0x601U, 0x701U);
    first_node.interfaces.push_back(MacUsbRegistryInterface{
        .registry_entry_id = 0x702U,
        .fingerprint = first_device_second.interface_fingerprint,
        .registry_path = "IOService:/first/interface-2",
    });
    auto second_node = node(second_device, 0x602U, 0x703U);
    second_node.interfaces.push_back(MacUsbRegistryInterface{
        .registry_entry_id = 0x704U,
        .fingerprint = second_device_second.interface_fingerprint,
        .registry_path = "IOService:/second/interface-2",
    });
    auto changed_first_node = first_node;
    changed_first_node.interfaces.back().registry_entry_id = 0x705U;

    ScriptedBackend backend;
    backend.results = {
        snapshot_result({first_node, second_node}),
        snapshot_result({changed_first_node, second_node}),
    };
    MacUsbTopologyDiscovery discovery(backend);
    const auto resolved = discovery.discover_devices(
        devices, MacUsbTopologyClock::now() + 1h);

    CHECK(resolved.has_value());
    CHECK(resolved->size() == 2U);
    CHECK(!(*resolved)[0].has_value());
    CHECK((*resolved)[0].error().kind ==
          MacUsbTopologyErrorKind::IdentityChanged);
    CHECK((*resolved)[1].has_value());
    CHECK((*resolved)[1]->size() == 2U);
    CHECK(backend.calls == 2U);
}

void batch_cancel_and_deadline_after_second_pass_publish_nothing() {
    const auto wanted = query();
    const std::array devices{MacUsbTopologyDeviceQuery{
        .interfaces = {wanted},
    }};

    std::stop_source cancellation;
    ScriptedSource cancelled_source;
    cancelled_source.result = std::vector<MacUsbRegistryNode>{node(wanted)};
    cancelled_source.before_return = [&cancellation, &cancelled_source] {
        if (cancelled_source.calls == 2U) {
            cancellation.request_stop();
        }
    };
    IokitMacUsbRegistryBackend cancelled_backend(cancelled_source);
    MacUsbTopologyDiscovery cancelled_discovery(cancelled_backend);
    const auto cancelled = cancelled_discovery.discover_devices(
        devices,
        MacUsbTopologyClock::now() + 1h,
        cancellation.get_token());
    CHECK(!cancelled.has_value());
    CHECK(cancelled.error().kind == MacUsbTopologyErrorKind::Cancelled);
    CHECK(cancelled_source.calls == 2U);
    CHECK(cancelled_source.deadlines[0] == cancelled_source.deadlines[1]);

    ScriptedSource timed_source;
    timed_source.result = std::vector<MacUsbRegistryNode>{node(wanted)};
    const auto deadline = MacUsbTopologyClock::now() + 100ms;
    timed_source.before_return = [deadline, &timed_source] {
        if (timed_source.calls == 2U) {
            std::this_thread::sleep_until(deadline);
        }
    };
    IokitMacUsbRegistryBackend timed_backend(timed_source);
    MacUsbTopologyDiscovery timed_discovery(timed_backend);
    const auto timed_out = timed_discovery.discover_devices(devices, deadline);
    CHECK(!timed_out.has_value());
    CHECK(timed_out.error().kind == MacUsbTopologyErrorKind::Timeout);
    CHECK(timed_source.calls == 2U);
    CHECK(timed_source.deadlines ==
          std::vector<MacUsbTopologyTimePoint>({deadline, deadline}));
}

void duplicate_serials_are_never_used_as_the_physical_key() {
    const auto wanted = query();
    auto other_query = query(1U, 8U, {4U}, 0x301U);
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
    const auto second = node(
        wanted, 0x101U, 0x201U, wanted.session_id, 0xA0U);

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

void unrelated_malformed_nodes_are_ignored_before_strict_snapshot_reads() {
    const auto wanted = query();
    auto unrelated = node(wanted);
    unrelated.session_id += 1U;
    unrelated.registry_entry_id = 0U;
    unrelated.registry_path.clear();
    unrelated.service_ancestry.clear();
    unrelated.interfaces.clear();

    const auto topology = discover_stable(wanted, {unrelated, node(wanted)});
    CHECK(topology.session_id == wanted.session_id);
    CHECK(topology.registry_entry_id == 0x100U);
}

void malformed_backend_nodes_and_invalid_queries_never_pass() {
    const auto wanted = query();
    auto malformed = node(wanted);
    malformed.location_id = 0x01102000U;
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

    auto zero_session = wanted;
    zero_session.session_id = 0U;
    const auto invalid_session = invalid_discovery.discover(
        zero_session, MacUsbTopologyClock::now() + 1h);
    CHECK(!invalid_session.has_value());
    CHECK(invalid_session.error().kind ==
          MacUsbTopologyErrorKind::InvalidArgument);
    CHECK(untouched.calls == 0U);

    auto too_deep = wanted;
    too_deep.port_numbers.assign(8U, 1U);
    const auto deep = invalid_discovery.discover(
        too_deep, MacUsbTopologyClock::now() + 1h);
    CHECK(!deep.has_value());
    CHECK(deep.error().kind ==
          MacUsbTopologyErrorKind::TopologyTooDeep);
    CHECK(untouched.calls == 0U);
}

void modern_and_intel_controller_ancestry_allow_root_hubs() {
    const auto modern_query = query();
    auto modern = node(modern_query);
    modern.service_ancestry.insert(
        modern.service_ancestry.begin() + 1,
        MacUsbRegistryAncestor{
            .registry_entry_id = 0xB0U,
            .kind = MacUsbRegistryEntryKind::Other,
            .registry_path = "IOService:/AppleUSBHostPort",
        });
    const auto modern_topology = discover_stable(modern_query, {modern});
    CHECK(modern_topology.root_controller_id ==
          "macos-iokit:00000000000000a0");

    const auto intel_query = query(7U, 9U, {31U}, 0x400U);
    auto intel = node(
        intel_query,
        0x500U,
        0x600U,
        intel_query.session_id,
        0x700U,
        MacUsbRegistryEntryKind::LegacyHostController);
    intel.service_ancestry.insert(
        intel.service_ancestry.begin(),
        MacUsbRegistryAncestor{
            .registry_entry_id = 0x702U,
            .kind = MacUsbRegistryEntryKind::UsbDevice,
            .registry_path = "IOService:/Intel-root-hub-device",
        });
    const auto intel_topology = discover_stable(intel_query, {intel});
    CHECK(intel_topology.root_controller_id ==
          "macos-iokit:0000000000000700");
    CHECK(intel_topology.hub_port_chain ==
          std::vector<std::uint8_t>({31U}));
}

void injectable_native_source_handles_empty_iterator_and_final_cancel() {
    const auto wanted = query();
    const std::array queries{wanted};
    ScriptedSource empty_source;
    IokitMacUsbRegistryBackend empty_backend(empty_source);
    const auto empty = empty_backend.snapshot(
        queries, MacUsbTopologyClock::now() + 1h, {});
    CHECK(empty.has_value());
    CHECK(empty->empty());
    CHECK(empty_source.calls == 1U);

    std::stop_source cancellation;
    ScriptedSource interrupted_source;
    interrupted_source.result = std::vector<MacUsbRegistryNode>{node(wanted)};
    interrupted_source.before_return = [&cancellation] {
        cancellation.request_stop();
    };
    IokitMacUsbRegistryBackend interrupted_backend(interrupted_source);
    const auto interrupted = interrupted_backend.snapshot(
        queries,
        MacUsbTopologyClock::now() + 1h,
        cancellation.get_token());
    CHECK(!interrupted.has_value());
    CHECK(interrupted.error().kind == MacUsbTopologyErrorKind::Cancelled);
    CHECK(interrupted.error().stage == MacUsbTopologyStage::FinalValidation);
    CHECK(interrupted_source.calls == 1U);

    ScriptedSource untouched;
    IokitMacUsbRegistryBackend timed_backend(untouched);
    const auto timed_out = timed_backend.snapshot(
        queries, MacUsbTopologyClock::now() - 1ms, {});
    CHECK(!timed_out.has_value());
    CHECK(timed_out.error().kind == MacUsbTopologyErrorKind::Timeout);
    CHECK(untouched.calls == 0U);
}

void source_error_never_masks_post_call_cancel() {
    const auto wanted = query();
    const std::array queries{wanted};
    const MacUsbTopologyError source_error{
        .kind = MacUsbTopologyErrorKind::IoError,
        .stage = MacUsbTopologyStage::DeviceEnumeration,
        .native_code = -1,
        .registry_path = {},
        .message = "scripted native failure",
    };

    std::stop_source cancellation;
    ScriptedSource cancelled_source;
    cancelled_source.result = std::unexpected(source_error);
    cancelled_source.before_return = [&cancellation] {
        cancellation.request_stop();
    };
    IokitMacUsbRegistryBackend cancelled_backend(cancelled_source);
    const auto cancelled = cancelled_backend.snapshot(
        queries,
        MacUsbTopologyClock::now() + 1h,
        cancellation.get_token());
    CHECK(!cancelled.has_value());
    CHECK(cancelled.error().kind == MacUsbTopologyErrorKind::Cancelled);
    CHECK(cancelled.error().stage == MacUsbTopologyStage::FinalValidation);
    CHECK(cancelled_source.calls == 1U);
}

void backend_error_never_masks_crossed_deadline() {
    const auto wanted = query();
    const MacUsbTopologyError backend_error{
        .kind = MacUsbTopologyErrorKind::IoError,
        .stage = MacUsbTopologyStage::DeviceEnumeration,
        .native_code = -1,
        .registry_path = {},
        .message = "scripted backend failure",
    };

    ScriptedBackend timed_backend;
    timed_backend.results = {
        snapshot_result({node(wanted)}),
        std::unexpected(backend_error),
    };
    const auto deadline = MacUsbTopologyClock::now() + 100ms;
    timed_backend.after_call = [deadline](const std::size_t calls) {
        if (calls == 2U) {
            std::this_thread::sleep_until(deadline);
        }
    };
    MacUsbTopologyDiscovery timed_discovery(timed_backend);
    const auto timed_out = timed_discovery.discover(wanted, deadline);
    CHECK(!timed_out.has_value());
    CHECK(timed_out.error().kind == MacUsbTopologyErrorKind::Timeout);
    CHECK(timed_out.error().stage == MacUsbTopologyStage::FinalValidation);
    CHECK(timed_backend.calls == 2U);
}

}  // namespace

int main() {
    const std::vector<std::pair<std::string_view, void (*)()>> tests{
        {"bus zero and fingerprint", bus_zero_and_complete_fingerprint_map_to_consumers},
        {"location bus and UInt8 ports", location_id_is_bus_only_and_uint8_ports_are_valid},
        {"libusb adapter", libusb_snapshot_adapter_preserves_the_interface_fingerprint},
        {"cancellation", cancellation_before_and_between_snapshots_never_publishes_topology},
        {"deadline", expired_deadline_never_reaches_the_registry_backend},
        {"error metadata", backend_timeout_and_native_metadata_are_preserved_verbatim},
        {"TOCTOU", two_snapshot_toctou_changes_fail_closed},
        {"snapshot order", deterministic_snapshot_order_does_not_create_a_false_race},
        {"multi-interface generation", multi_interface_device_uses_one_generation_or_fails_closed},
        {"32-device batch snapshots", thirty_two_devices_share_exactly_two_global_snapshot_passes},
        {"batch device generation", batch_generation_change_fails_the_whole_device_only},
        {"batch interruption", batch_cancel_and_deadline_after_second_pass_publish_nothing},
        {"duplicate serial", duplicate_serials_are_never_used_as_the_physical_key},
        {"ambiguity", duplicate_device_and_interface_mappings_are_ambiguous},
        {"identity mismatch", mismatched_and_missing_identities_fail_closed},
        {"unrelated malformed nodes", unrelated_malformed_nodes_are_ignored_before_strict_snapshot_reads},
        {"invalid contracts", malformed_backend_nodes_and_invalid_queries_never_pass},
        {"modern and Intel ancestry", modern_and_intel_controller_ancestry_allow_root_hubs},
        {"native source interruption", injectable_native_source_handles_empty_iterator_and_final_cancel},
        {"source error cancellation", source_error_never_masks_post_call_cancel},
        {"backend error deadline", backend_error_never_masks_crossed_deadline},
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
