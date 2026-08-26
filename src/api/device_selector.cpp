// SPDX-License-Identifier: MIT
#include "src/api/device_selector.hpp"

#include "src/transport/tcp_fastboot.hpp"
#include "src/transport/udp_fastboot.hpp"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>

namespace kairosboot::api {
namespace {

[[nodiscard]] std::unexpected<DeviceSelectorError> invalid(
    std::string message) {
    return std::unexpected(DeviceSelectorError{
        KB_E_INVALID_ARGUMENT, std::move(message)});
}

[[nodiscard]] std::unexpected<DeviceSelectorError> selection_error(
    const kb_status_t status,
    std::string message) {
    return std::unexpected(DeviceSelectorError{status, std::move(message)});
}

[[nodiscard]] bool valid_utf8(const std::string_view value) noexcept {
    const auto* bytes = reinterpret_cast<const unsigned char*>(value.data());
    std::size_t index = 0;
    while (index < value.size()) {
        const auto lead = bytes[index];
        if (lead <= 0x7FU) {
            ++index;
            continue;
        }
        std::size_t continuations = 0;
        unsigned char first_minimum = 0x80U;
        unsigned char first_maximum = 0xBFU;
        if (lead >= 0xC2U && lead <= 0xDFU) {
            continuations = 1;
        } else if (lead >= 0xE0U && lead <= 0xEFU) {
            continuations = 2;
            first_minimum = lead == 0xE0U ? 0xA0U : 0x80U;
            first_maximum = lead == 0xEDU ? 0x9FU : 0xBFU;
        } else if (lead >= 0xF0U && lead <= 0xF4U) {
            continuations = 3;
            first_minimum = lead == 0xF0U ? 0x90U : 0x80U;
            first_maximum = lead == 0xF4U ? 0x8FU : 0xBFU;
        } else {
            return false;
        }
        if (continuations > value.size() - index - 1) {
            return false;
        }
        const auto first = bytes[index + 1];
        if (first < first_minimum || first > first_maximum) {
            return false;
        }
        for (std::size_t offset = 2; offset <= continuations; ++offset) {
            if (bytes[index + offset] < 0x80U ||
                bytes[index + offset] > 0xBFU) {
                return false;
            }
        }
        index += continuations + 1;
    }
    return true;
}

[[nodiscard]] int hex_digit(const char value) noexcept {
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F') {
        return value - 'A' + 10;
    }
    return -1;
}

[[nodiscard]] std::expected<std::string, DeviceSelectorError>
decode_serial(const std::string_view encoded) {
    if (encoded.empty()) {
        return invalid("USB serial selector must not be empty");
    }
    std::string decoded;
    decoded.reserve(encoded.size());
    for (std::size_t index = 0; index < encoded.size(); ++index) {
        unsigned char value = static_cast<unsigned char>(encoded[index]);
        if (value == '%') {
            if (encoded.size() - index < 3) {
                return invalid("USB serial selector has an incomplete percent escape");
            }
            const auto high = hex_digit(encoded[index + 1]);
            const auto low = hex_digit(encoded[index + 2]);
            if (high < 0 || low < 0) {
                return invalid("USB serial selector has an invalid percent escape");
            }
            value = static_cast<unsigned char>((high << 4) | low);
            index += 2;
        }
        if (value < 0x20U || value == 0x7FU) {
            return invalid("USB serial selector decodes to a control character");
        }
        decoded.push_back(static_cast<char>(value));
    }
    if (!valid_utf8(decoded)) {
        return invalid("USB serial selector must decode to valid UTF-8");
    }
    return decoded;
}

[[nodiscard]] bool forbidden_endpoint_character(const char value) noexcept {
    const auto byte = static_cast<unsigned char>(value);
    return byte <= 0x20U || byte == 0x7FU || value == '/' || value == '?' ||
        value == '#' || value == '@' || value == '\\';
}

[[nodiscard]] std::expected<DeviceSelector, DeviceSelectorError>
parse_usb_path(const std::string_view text) {
    const auto dash = text.find('-');
    if (dash == std::string_view::npos || dash == 0 || dash + 1 == text.size()) {
        return invalid("USB path selector must use usb:<bus>-<port>[.<port>...]");
    }

    const auto parse_component = [](const std::string_view component)
        -> std::optional<std::uint8_t> {
        if (component.empty() ||
            (component.size() > 1 && component.front() == '0')) {
            return std::nullopt;
        }
        unsigned int parsed = 0;
        const auto result = std::from_chars(
            component.data(), component.data() + component.size(), parsed);
        if (result.ec != std::errc{} ||
            result.ptr != component.data() + component.size() || parsed == 0 ||
            parsed > std::numeric_limits<std::uint8_t>::max()) {
            return std::nullopt;
        }
        return static_cast<std::uint8_t>(parsed);
    };

    auto bus = parse_component(text.substr(0, dash));
    if (!bus) {
        return invalid("USB path selector has an invalid bus number");
    }
    std::vector<std::uint8_t> ports;
    std::size_t start = dash + 1;
    while (start <= text.size()) {
        const auto dot = text.find('.', start);
        const auto end = dot == std::string_view::npos ? text.size() : dot;
        auto port = parse_component(text.substr(start, end - start));
        if (!port) {
            return invalid("USB path selector has an invalid port number");
        }
        ports.push_back(*port);
        if (ports.size() > 7) {
            return invalid("USB path selector exceeds the supported hub depth");
        }
        if (dot == std::string_view::npos) {
            break;
        }
        start = dot + 1;
    }

    return DeviceSelector{
        .kind = DeviceSelectorKind::UsbPath,
        .value = std::string(text),
        .usb_bus = *bus,
        .usb_ports = std::move(ports),
        .identifier = "usb:" + std::string(text),
    };
}

[[nodiscard]] std::expected<DeviceSelector, DeviceSelectorError>
parse_network(
    const DeviceSelectorKind kind,
    const std::string_view endpoint,
    const std::string_view scheme) {
    if (endpoint.empty()) {
        return invalid(std::string(scheme) + " endpoint must not be empty");
    }
    for (const char value : endpoint) {
        if (forbidden_endpoint_character(value)) {
            return invalid(std::string(scheme) + " endpoint contains a forbidden character");
        }
    }
    if (kind == DeviceSelectorKind::Tcp) {
        if (const auto parsed = transport::parse_tcp_endpoint(endpoint); !parsed) {
            return invalid("invalid TCP Fastboot endpoint: " + parsed.error().message);
        }
    } else if (const auto parsed = transport::parse_udp_endpoint(endpoint); !parsed) {
        return invalid("invalid UDP Fastboot endpoint: " + parsed.error().message);
    }
    return DeviceSelector{
        .kind = kind,
        .value = std::string(endpoint),
        .usb_bus = 0,
        .usb_ports = {},
        .identifier = std::string(scheme) + ":" + std::string(endpoint),
    };
}

}  // namespace

std::expected<DeviceSelector, DeviceSelectorError> parse_device_selector(
    const std::optional<std::string_view> text) {
    if (!text.has_value()) {
        return DeviceSelector{
            .kind = DeviceSelectorKind::UsbUnique,
            .value = {},
            .usb_bus = 0,
            .usb_ports = {},
            .identifier = "usb:unique",
        };
    }
    if (text->empty()) {
        return invalid("explicit device selector must not be empty");
    }
    if (text->starts_with("usb:serial:")) {
        auto serial = decode_serial(text->substr(std::string_view{"usb:serial:"}.size()));
        if (!serial) {
            return std::unexpected(std::move(serial.error()));
        }
        return DeviceSelector{
            .kind = DeviceSelectorKind::UsbSerial,
            .value = std::move(*serial),
            .usb_bus = 0,
            .usb_ports = {},
            .identifier = std::string(*text),
        };
    }
    if (text->starts_with("usb:")) {
        return parse_usb_path(text->substr(std::string_view{"usb:"}.size()));
    }
    if (text->starts_with("tcp:")) {
        return parse_network(
            DeviceSelectorKind::Tcp,
            text->substr(std::string_view{"tcp:"}.size()),
            "tcp");
    }
    if (text->starts_with("udp:")) {
        return parse_network(
            DeviceSelectorKind::Udp,
            text->substr(std::string_view{"udp:"}.size()),
            "udp");
    }
    if (text->find(':') != std::string_view::npos) {
        return invalid("device selector uses an unknown scheme");
    }
    if (!valid_utf8(*text)) {
        return invalid("legacy USB serial selector must be valid UTF-8");
    }
    for (const unsigned char value : *text) {
        if (value < 0x20U || value == 0x7FU) {
            return invalid("legacy USB serial selector contains a control character");
        }
    }
    return DeviceSelector{
        .kind = DeviceSelectorKind::UsbSerial,
        .value = std::string(*text),
        .usb_bus = 0,
        .usb_ports = {},
        .identifier = std::string(*text),
    };
}

std::expected<transport::UsbDeviceInfo, DeviceSelectorError> select_usb_device(
    const std::span<const transport::UsbDeviceInfo> devices,
    const DeviceSelector& selector) {
    if (selector.kind != DeviceSelectorKind::UsbUnique &&
        selector.kind != DeviceSelectorKind::UsbSerial &&
        selector.kind != DeviceSelectorKind::UsbPath) {
        return invalid("network selector cannot select a USB device");
    }
    if (selector.kind == DeviceSelectorKind::UsbUnique) {
        if (devices.empty()) {
            return selection_error(
                KB_E_NO_DEVICE, "no Fastboot USB device is available");
        }
        if (devices.size() != 1) {
            return selection_error(
                KB_E_AMBIGUOUS_DEVICE,
                "more than one Fastboot USB device is available");
        }
        return devices.front();
    }

    const transport::UsbDeviceInfo* match = nullptr;
    for (const auto& device : devices) {
        const bool matches = selector.kind == DeviceSelectorKind::UsbSerial
            ? device.serial_utf8 == selector.value
            : device.bus_number == selector.usb_bus &&
                device.port_path == selector.usb_ports;
        if (!matches) {
            continue;
        }
        if (match != nullptr) {
            return selection_error(
                KB_E_AMBIGUOUS_DEVICE,
                "more than one Fastboot USB device matches the selector");
        }
        match = &device;
    }
    if (match == nullptr) {
        return selection_error(
            KB_E_NO_DEVICE, "no Fastboot USB device matches the selector");
    }
    return *match;
}

}  // namespace kairosboot::api
