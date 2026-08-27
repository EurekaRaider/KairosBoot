// SPDX-License-Identifier: MIT
#include "src/api/device_selector.hpp"
#include "src/transport/macos_usb_topology.hpp"

#include <exception>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using kairosboot::api::DeviceSelectorKind;
using kairosboot::api::parse_device_selector;
using kairosboot::api::select_usb_device;
using kairosboot::transport::canonical_macos_usb_port_path;
using kairosboot::transport::UsbDeviceInfo;

#define CHECK(condition)                                                         \
    do {                                                                         \
        if (!(condition)) {                                                       \
            throw std::runtime_error(                                             \
                std::string("check failed at line ") + std::to_string(__LINE__) + \
                ": " #condition);                                                \
        }                                                                        \
    } while (false)

void parses_public_selector_grammar() {
    const auto unique = parse_device_selector(std::nullopt);
    CHECK(unique.has_value());
    CHECK(unique->kind == DeviceSelectorKind::UsbUnique);

    const auto legacy = parse_device_selector("SERIAL-1");
    CHECK(legacy.has_value());
    CHECK(legacy->kind == DeviceSelectorKind::UsbSerial);
    CHECK(legacy->value == "SERIAL-1");

    const auto encoded = parse_device_selector("usb:serial:%E8%AE%BE%E5%A4%87%2D1");
    CHECK(encoded.has_value());
    CHECK(encoded->value == "设备-1");

    const auto path = parse_device_selector("usb:3-2.4.1");
    CHECK(path.has_value());
    CHECK(path->kind == DeviceSelectorKind::UsbPath);
    CHECK(path->usb_bus == 3);
    CHECK(path->usb_ports == std::vector<std::uint8_t>({2, 4, 1}));

    const auto darwin_path = parse_device_selector("usb:0-2.3");
    CHECK(darwin_path.has_value());
    CHECK(darwin_path->kind == DeviceSelectorKind::UsbPath);
    CHECK(darwin_path->usb_bus == 0);
    CHECK(darwin_path->usb_ports == std::vector<std::uint8_t>({2, 3}));

    const auto maximum_path = parse_device_selector("usb:255-255");
    CHECK(maximum_path.has_value());
    CHECK(maximum_path->usb_bus == 255);
    CHECK(maximum_path->usb_ports == std::vector<std::uint8_t>({255}));

    const auto tcp = parse_device_selector("tcp:flash-host:5554");
    CHECK(tcp.has_value());
    CHECK(tcp->kind == DeviceSelectorKind::Tcp);
    CHECK(tcp->value == "flash-host:5554");

    const auto tcp_ipv6 = parse_device_selector("tcp:[::1]:5554");
    CHECK(tcp_ipv6.has_value());
    CHECK(tcp_ipv6->kind == DeviceSelectorKind::Tcp);

    const auto udp = parse_device_selector("udp:192.0.2.1");
    CHECK(udp.has_value());
    CHECK(udp->kind == DeviceSelectorKind::Udp);
}

void rejects_ambiguous_or_unsafe_selector_text() {
    const std::vector<std::string> invalid{
        "", "serial:ABC", "usb:serial:%", "usb:serial:%00",
        "usb:00-1", "usb:256-1", "usb:1-0", "usb:1-01", "usb:1-1.",
        "tcp:host/path", "tcp:host?query", "udp:host#fragment",
        std::string("tcp:host\n", 9),
    };
    for (const auto& value : invalid) {
        CHECK(!parse_device_selector(value).has_value());
    }
}

void selects_usb_by_unique_serial_or_physical_path() {
    UsbDeviceInfo first{};
    first.bus_number = 1;
    first.port_path = {2, 3};
    first.serial_utf8.push_back('A');
    UsbDeviceInfo second{};
    second.bus_number = 2;
    second.port_path = {4};
    second.serial_utf8.push_back('B');
    const std::vector devices{first, second};

    const auto unique = parse_device_selector(std::nullopt);
    const auto ambiguous = select_usb_device(devices, *unique);
    CHECK(!ambiguous.has_value());
    CHECK(ambiguous.error().status == KB_E_AMBIGUOUS_DEVICE);

    const auto serial = parse_device_selector("usb:serial:B");
    const auto serial_match = select_usb_device(devices, *serial);
    CHECK(serial_match.has_value());
    CHECK(serial_match->bus_number == 2);

    const auto path = parse_device_selector("usb:1-2.3");
    const auto path_match = select_usb_device(devices, *path);
    CHECK(path_match.has_value());
    CHECK(path_match->serial_utf8 == "A");

    const auto missing = parse_device_selector("usb:9-1");
    const auto no_match = select_usb_device(devices, *missing);
    CHECK(!no_match.has_value());
    CHECK(no_match.error().status == KB_E_NO_DEVICE);

    UsbDeviceInfo darwin{};
    darwin.bus_number = 0;
    darwin.port_path = {2, 3};
    darwin.serial_utf8 = "DUPLICATE";
    UsbDeviceInfo elsewhere = darwin;
    elsewhere.port_path = {2, 4};
    const std::vector darwin_devices{darwin, elsewhere};

    const auto enumerated_path =
        canonical_macos_usb_port_path(darwin.bus_number, darwin.port_path);
    CHECK(enumerated_path.has_value());
    const auto round_trip = parse_device_selector(*enumerated_path);
    CHECK(round_trip.has_value());
    const auto path_selected = select_usb_device(darwin_devices, *round_trip);
    CHECK(path_selected.has_value());
    CHECK(path_selected->bus_number == 0);
    CHECK(path_selected->port_path == darwin.port_path);
}

}  // namespace

int main() {
    const std::vector<std::pair<std::string_view, void (*)()>> tests{
        {"public selector grammar", parses_public_selector_grammar},
        {"unsafe selectors", rejects_ambiguous_or_unsafe_selector_text},
        {"USB selector matching", selects_usb_by_unique_serial_or_physical_path},
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
