// SPDX-License-Identifier: MIT
#pragma once

#include "src/api/operation_state.hpp"

#include <string_view>

namespace kairosboot::fastboot {
struct PrimitiveError;
enum class PrimitiveOperation : std::uint8_t;
}

namespace kairosboot::image {
struct FileSourceError;
struct ImageSourceError;
struct SparseError;
struct SparseFlashPlanError;
}

namespace kairosboot::transport {
struct LibusbRuntimeError;
}

namespace kairosboot::api {

struct DeviceSelectionError;

[[nodiscard]] OperationErrorPayload normalize_public_error(
    const DeviceSelectionError& error,
    std::string_view device_identifier);

[[nodiscard]] OperationErrorPayload normalize_public_error(
    const image::ImageSourceError& error,
    std::string_view device_identifier);

[[nodiscard]] OperationErrorPayload normalize_public_error(
    const image::FileSourceError& error,
    std::string_view device_identifier);

[[nodiscard]] OperationErrorPayload normalize_public_error(
    const image::SparseError& error,
    std::string_view device_identifier);

[[nodiscard]] OperationErrorPayload normalize_public_error(
    const image::SparseFlashPlanError& error,
    std::string_view device_identifier);

[[nodiscard]] OperationErrorPayload normalize_public_error(
    const transport::LibusbRuntimeError& error,
    std::string_view device_identifier);

[[nodiscard]] OperationErrorPayload normalize_public_error(
    const fastboot::PrimitiveError& error,
    std::string_view device_identifier);

// Converts the per-command certainty reported by PrimitiveService into the
// certainty for a complete (possibly multi-part) flash payload. A Flash-stage
// error occurs only after the current DATA payload was fully downloaded.
void accumulate_flash_transfer_state(
    OperationErrorPayload& payload,
    fastboot::PrimitiveOperation failed_operation,
    std::uint64_t completed_before_part,
    std::uint64_t current_part_size,
    std::uint64_t total_size) noexcept;

}  // namespace kairosboot::api
