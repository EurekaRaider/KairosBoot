// SPDX-License-Identifier: MIT
#pragma once

#include "src/api/operation_state.hpp"

#include <string_view>

namespace kairosboot::fastboot {
struct PrimitiveError;
}

namespace kairosboot::image {
struct FileSourceError;
struct ImageSourceError;
struct SparseError;
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
    const transport::LibusbRuntimeError& error,
    std::string_view device_identifier);

[[nodiscard]] OperationErrorPayload normalize_public_error(
    const fastboot::PrimitiveError& error,
    std::string_view device_identifier);

}  // namespace kairosboot::api
