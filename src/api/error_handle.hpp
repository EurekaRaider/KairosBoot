// SPDX-License-Identifier: MIT
#pragma once

#include "src/api/command_result_handle.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// Internal definition of the opaque public kb_error_t handle. Only the shared
// library's C entry points construct this structure; consumers see the opaque
// pointer plus the kb_error_* getter functions. Keep every field in sync with
// the getters in the public translation units.
struct kb_error {
  kb_status_t status{KB_OK};
  std::string message;
  std::string device_identifier;
  std::int32_t native_code{0};
  kb_transfer_state_t transfer_state{KB_TRANSFER_NOT_SENT};
  std::string device_message;
  std::vector<kairosboot::api::CommandMessagePayload> command_messages;
  std::optional<std::uint64_t> inbound_expected;
  std::uint64_t inbound_transferred{0};
  kb_transfer_state_t inbound_transfer_state{KB_TRANSFER_NOT_SENT};
  bool session_poisoned{false};
};
