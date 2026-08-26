// SPDX-License-Identifier: MIT
#include "src/api/device_selection.hpp"

namespace kairosboot::api {
namespace {

[[nodiscard]] std::unexpected<DeviceSelectionError> selection_error(
    const kb_status_t status,
    const char* message) {
    return std::unexpected(DeviceSelectionError{status, message});
}

}  // namespace

std::expected<transport::UsbDeviceInfo, DeviceSelectionError>
select_usb_device(
    const std::span<const transport::UsbDeviceInfo> devices,
    const std::optional<std::string_view> serial) {
    if (!serial.has_value()) {
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

    if (serial->empty()) {
        return selection_error(
            KB_E_INVALID_ARGUMENT, "explicit device serial must not be empty");
    }

    const transport::UsbDeviceInfo* match = nullptr;
    for (const auto& device : devices) {
        if (device.serial_utf8 != *serial) {
            continue;
        }
        if (match != nullptr) {
            return selection_error(
                KB_E_AMBIGUOUS_DEVICE,
                "more than one Fastboot USB device matches the requested serial");
        }
        match = &device;
    }

    if (match == nullptr) {
        return selection_error(
            KB_E_NO_DEVICE,
            "no Fastboot USB device matches the requested serial");
    }
    return *match;
}

}  // namespace kairosboot::api
