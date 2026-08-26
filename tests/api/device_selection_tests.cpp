// SPDX-License-Identifier: MIT
#include "src/api/device_selection.hpp"

#include <cstdint>
#include <exception>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using kairosboot::api::DeviceSelectionError;
using kairosboot::api::select_usb_device;
using kairosboot::transport::LinuxUsbTopology;
using kairosboot::transport::UsbDeviceInfo;

#define CHECK(condition)                                                         \
    do {                                                                         \
        if (!(condition)) {                                                       \
            throw std::runtime_error(                                             \
                std::string("check failed at line ") + std::to_string(__LINE__) + \
                ": " #condition);                                                \
        }                                                                        \
    } while (false)

[[nodiscard]] UsbDeviceInfo device(
    std::string serial,
    const std::uint8_t identity) {
    UsbDeviceInfo result;
    result.vendor_id = static_cast<std::uint16_t>(0x1800U + identity);
    result.product_id = static_cast<std::uint16_t>(0x4E00U + identity);
    result.bus_number = identity;
    result.device_address = static_cast<std::uint8_t>(identity + 10U);
    result.configuration_value = 1;
    result.port_path = {identity, static_cast<std::uint8_t>(identity + 1U)};
    result.serial_utf8 = std::move(serial);
    result.interface_number = static_cast<std::uint8_t>(identity + 2U);
    result.alternate_setting = static_cast<std::uint8_t>(identity + 3U);
    result.interface_class = 0xFFU;
    result.interface_subclass = 0x42U;
    result.interface_protocol = 0x03U;
    result.bulk_out_endpoint = static_cast<std::uint8_t>(0x01U + identity);
    result.bulk_out_max_packet_size = static_cast<std::uint16_t>(512U + identity);
    result.bulk_in_endpoint = static_cast<std::uint8_t>(0x81U + identity);
    result.bulk_in_max_packet_size = static_cast<std::uint16_t>(1024U + identity);
    return result;
}

[[nodiscard]] bool same_snapshot(
    const UsbDeviceInfo& left,
    const UsbDeviceInfo& right) {
    return left.vendor_id == right.vendor_id &&
           left.product_id == right.product_id &&
           left.bus_number == right.bus_number &&
           left.device_address == right.device_address &&
           left.configuration_value == right.configuration_value &&
           left.port_path == right.port_path &&
           left.serial_utf8 == right.serial_utf8 &&
           left.interface_number == right.interface_number &&
           left.alternate_setting == right.alternate_setting &&
           left.interface_class == right.interface_class &&
           left.interface_subclass == right.interface_subclass &&
           left.interface_protocol == right.interface_protocol &&
           left.bulk_out_endpoint == right.bulk_out_endpoint &&
           left.bulk_out_max_packet_size == right.bulk_out_max_packet_size &&
           left.bulk_in_endpoint == right.bulk_in_endpoint &&
           left.bulk_in_max_packet_size == right.bulk_in_max_packet_size &&
           left.linux_topology == right.linux_topology &&
           left.linux_topology_error == right.linux_topology_error;
}

template <typename Result>
void check_error(
    const Result& result,
    const kb_status_t expected_status,
    const std::string_view expected_message) {
    CHECK(!result.has_value());
    CHECK(result.error().status == expected_status);
    CHECK(result.error().message == expected_message);
}

void absent_serial_requires_exactly_one_device() {
    const std::vector<UsbDeviceInfo> none;
    check_error(
        select_usb_device(none, std::nullopt),
        KB_E_NO_DEVICE,
        "no Fastboot USB device is available");

    const std::vector<UsbDeviceInfo> multiple{
        device("SERIAL-A", 1),
        device("SERIAL-B", 2),
    };
    check_error(
        select_usb_device(multiple, std::nullopt),
        KB_E_AMBIGUOUS_DEVICE,
        "more than one Fastboot USB device is available");
}

void absent_serial_returns_an_independent_full_snapshot() {
    auto expected = device("设备-一", 4);
    expected.linux_topology = LinuxUsbTopology{
        .physical_port_path = "usb:4-4.5",
        .root_controller_id = "linux-sysfs:pci0000:00/0000:00:14.0",
        .hub_port_chain = {4U, 5U},
        .vendor_id = expected.vendor_id,
        .product_id = expected.product_id,
        .bus_number = expected.bus_number,
        .device_address = expected.device_address,
        .serial_utf8 = expected.serial_utf8,
        .product_utf8 = std::string{"Kairos device"},
        .sysfs_device_path =
            "devices/pci0000:00/0000:00:14.0/usb4/4-4/4-4.5",
    };
    std::vector<UsbDeviceInfo> devices{expected};

    const auto selected = select_usb_device(devices, std::nullopt);
    CHECK(selected.has_value());
    CHECK(same_snapshot(*selected, expected));

    devices.front().serial_utf8 = "mutated";
    devices.front().port_path.clear();
    devices.front().bulk_in_max_packet_size = 0;
    devices.front().linux_topology->root_controller_id = "mutated";
    CHECK(same_snapshot(*selected, expected));
}

void explicit_serial_matches_exactly_one_device() {
    const auto first = device("SERIAL-A", 1);
    const auto expected = device("设备-二", 7);
    const auto third = device("SERIAL-C", 3);
    const std::vector<UsbDeviceInfo> devices{first, expected, third};

    const auto selected = select_usb_device(
        devices, std::optional<std::string_view>{"设备-二"});
    CHECK(selected.has_value());
    CHECK(same_snapshot(*selected, expected));
}

void empty_explicit_serial_is_invalid() {
    const std::vector<UsbDeviceInfo> devices{device("", 1)};
    check_error(
        select_usb_device(devices, std::optional<std::string_view>{""}),
        KB_E_INVALID_ARGUMENT,
        "explicit device serial must not be empty");
}

void missing_explicit_serial_returns_no_device() {
    const std::vector<UsbDeviceInfo> devices{
        device("SERIAL-A", 1),
        device("SERIAL-B", 2),
    };
    check_error(
        select_usb_device(
            devices, std::optional<std::string_view>{"SERIAL-C"}),
        KB_E_NO_DEVICE,
        "no Fastboot USB device matches the requested serial");
}

void duplicate_explicit_serial_is_ambiguous() {
    const std::vector<UsbDeviceInfo> devices{
        device("DUPLICATE", 1),
        device("OTHER", 2),
        device("DUPLICATE", 3),
    };
    check_error(
        select_usb_device(
            devices, std::optional<std::string_view>{"DUPLICATE"}),
        KB_E_AMBIGUOUS_DEVICE,
        "more than one Fastboot USB device matches the requested serial");
}

void explicit_matching_is_case_sensitive_and_exact() {
    const std::vector<UsbDeviceInfo> devices{device("Serial-01", 1)};
    check_error(
        select_usb_device(
            devices, std::optional<std::string_view>{"serial-01"}),
        KB_E_NO_DEVICE,
        "no Fastboot USB device matches the requested serial");
}

}  // namespace

int main() {
    const std::vector<std::pair<std::string_view, void (*)()>> tests{
        {"absent serial cardinality", absent_serial_requires_exactly_one_device},
        {"absent serial copied snapshot",
         absent_serial_returns_an_independent_full_snapshot},
        {"explicit serial unique match", explicit_serial_matches_exactly_one_device},
        {"empty explicit serial", empty_explicit_serial_is_invalid},
        {"missing explicit serial", missing_explicit_serial_returns_no_device},
        {"duplicate explicit serial", duplicate_explicit_serial_is_ambiguous},
        {"explicit serial exact match", explicit_matching_is_case_sensitive_and_exact},
    };

    std::size_t failures = 0;
    for (const auto& [name, test] : tests) {
        try {
            test();
            std::cout << "PASS: " << name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "FAIL: " << name << ": " << error.what() << '\n';
        }
    }

    if (failures != 0) {
        std::cerr << failures << " device-selection test(s) failed\n";
        return 1;
    }
    std::cout << tests.size() << " device-selection tests passed\n";
    return 0;
}
