// SPDX-License-Identifier: MIT
#pragma once

#include "src/api/operation_state.hpp"

#include <memory>

namespace kairosboot::api {

// Internal ownership primitive behind kb_command_result_t. Extraction retains
// the immutable operation payload instead of copying potentially large receive
// data, and the retained payload outlives the operation handle.
class CommandResultHandle final {
public:
    explicit CommandResultHandle(
        std::shared_ptr<const CommandResultPayload> payload) noexcept
        : payload_(std::move(payload)) {}

    [[nodiscard]] const CommandResultPayload* get() const noexcept {
        return payload_.get();
    }

private:
    std::shared_ptr<const CommandResultPayload> payload_;
};

}  // namespace kairosboot::api
