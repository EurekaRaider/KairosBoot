// SPDX-License-Identifier: MIT
#pragma once

#include "src/transport/libusb_runtime.hpp"

#include <kairosboot/kairosboot.h>

#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace kairosboot::api {

enum class DeviceSelectorKind : std::uint8_t {
    UsbUnique,
    UsbSerial,
    UsbPath,
    Tcp,
    Udp,
};

struct DeviceSelector final {
    DeviceSelectorKind kind{DeviceSelectorKind::UsbUnique};
    std::string value;
    std::uint8_t usb_bus{0};
    std::vector<std::uint8_t> usb_ports;
    std::string identifier;
};

struct DeviceSelectorError final {
    kb_status_t status{KB_E_INVALID_ARGUMENT};
    std::string message;
};

// NULL selects the sole USB Fastboot device. A bare value retains the legacy
// exact USB serial behavior; typed selectors use usb:serial:, usb:<path>,
// tcp:, or udp:.
[[nodiscard]] std::expected<DeviceSelector, DeviceSelectorError>
parse_device_selector(std::optional<std::string_view> text);

[[nodiscard]] std::expected<transport::UsbDeviceInfo, DeviceSelectorError>
select_usb_device(
    std::span<const transport::UsbDeviceInfo> devices,
    const DeviceSelector& selector);

}  // namespace kairosboot::api
