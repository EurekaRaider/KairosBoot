// SPDX-License-Identifier: MIT
#pragma once

#include "src/api/operation_state.hpp"
#include "src/transport/libusb_runtime.hpp"

#include <kairosboot/kairosboot.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
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

namespace detail {

template <typename T>
void initialize_known_prefix(T* value, const std::uint32_t struct_size) noexcept {
  if (value == nullptr) {
    return;
  }
  const auto writable = std::min<std::size_t>(struct_size, sizeof(T));
  std::memset(static_cast<void*>(value), 0, writable);
}

template <typename T, typename Field>
void initialize_field(T* value,
                      const std::uint32_t struct_size,
                      const std::size_t offset,
                      const Field& field) noexcept {
  if (value == nullptr) {
    return;
  }
  const auto writable = std::min<std::size_t>(struct_size, sizeof(T));
  if (offset > writable || sizeof(Field) > writable - offset) {
    return;
  }
  std::memcpy(static_cast<unsigned char*>(static_cast<void*>(value)) + offset,
              &field, sizeof(Field));
}

template <typename T>
void initialize_struct_header(T* value,
                              const std::uint32_t struct_size) noexcept {
  initialize_known_prefix(value, struct_size);
  initialize_field(value, struct_size, offsetof(T, struct_size), struct_size);
  initialize_field(value, struct_size, offsetof(T, api_version),
                   std::uint32_t{KB_API_VERSION});
}

}  // namespace detail

}  // namespace kairosboot::api
