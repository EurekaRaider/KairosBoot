// SPDX-License-Identifier: MIT
#pragma once

#include <kairosboot/kairosboot.h>

#include "src/transport/transfer_ring.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string>

namespace kairosboot::api {

struct DeviceBatchSchedulingInfo final {
  std::string device_key;
  std::string controller_id;
};

struct DeviceBatchTransferPermits final {
  kb_device_t* device{};
  std::shared_ptr<transport::TransferPermitProvider> provider;
  transport::TransferRingConfig config{};
};

// The batch start callback is synchronous: it must return the child operation
// it just created. A thread-local scope therefore carries one scheduler binding
// into that operation without mutating the Device or racing another batch that
// attempts to use the same handle concurrently.
class ScopedDeviceBatchTransferPermits final {
public:
  ScopedDeviceBatchTransferPermits(
      kb_device_t* device,
      std::shared_ptr<transport::TransferPermitProvider> provider,
      transport::TransferRingConfig config) noexcept;
  ~ScopedDeviceBatchTransferPermits();

  ScopedDeviceBatchTransferPermits(
      const ScopedDeviceBatchTransferPermits&) = delete;
  ScopedDeviceBatchTransferPermits& operator=(
      const ScopedDeviceBatchTransferPermits&) = delete;

private:
  std::optional<DeviceBatchTransferPermits> previous_;
};

[[nodiscard]] DeviceBatchSchedulingInfo device_batch_scheduling_info(
    const kb_device_t* device);

[[nodiscard]] std::optional<DeviceBatchTransferPermits>
current_device_batch_transfer_permits(const kb_device_t* device) noexcept;

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
