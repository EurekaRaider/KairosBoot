// SPDX-License-Identifier: MIT
#pragma once

#include "src/transport/libusb_runtime.hpp"

#include <kairosboot/kairosboot.h>

#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace kairosboot::api {

struct DeviceSelectionError final {
    kb_status_t status{KB_E_INTERNAL};
    std::string message;
};

// Selects one copied USB snapshot without opening, probing, or otherwise
// touching a device. An absent serial enables the single-device convenience
// rule; a present serial is always an exact UTF-8 match.
[[nodiscard]] std::expected<transport::UsbDeviceInfo, DeviceSelectionError>
select_usb_device(
    std::span<const transport::UsbDeviceInfo> devices,
    std::optional<std::string_view> serial);

}  // namespace kairosboot::api
