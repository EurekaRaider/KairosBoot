// SPDX-License-Identifier: MIT
#pragma once

#include "src/api/operation_state.hpp"
#include "src/transport/libusb_runtime.hpp"

#include <kairosboot/kairosboot.h>

#include <expected>
#include <cstdint>
#include <memory>

namespace kairosboot::api {

// Internal bridge used by the fleet C lifecycle. The context remains opaque to
// the fleet layer while all public USB entry points continue to share the same
// process-wide libusb runtime and per-context strong owner.
[[nodiscard]] std::expected<std::shared_ptr<transport::LibusbRuntime>,
                            OperationErrorPayload>
acquire_fleet_usb_runtime(kb_context_t& context);

[[nodiscard]] std::uint16_t
fleet_usb_vendor_id(const kb_context_t& context) noexcept;

}  // namespace kairosboot::api
