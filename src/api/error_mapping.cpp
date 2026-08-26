// SPDX-License-Identifier: MIT
#include "src/api/error_mapping.hpp"

#include "src/api/device_selection.hpp"
#include "src/fastboot/primitive_service.hpp"
#include "src/image/file_source.hpp"
#include "src/image/sparse_flash_plan.hpp"
#include "src/transport/libusb_runtime.hpp"

#include <cstdint>
#include <string>
#include <utility>

namespace kairosboot::api {
namespace {

[[nodiscard]] OperationErrorPayload make_error(
    const kb_status_t status,
    std::string message,
    const std::int32_t native_code,
    const kb_transfer_state_t transfer_state,
    const std::string_view device_identifier) {
    return {
        status,
        std::move(message),
        native_code,
        transfer_state,
        std::string(device_identifier),
    };
}

[[nodiscard]] kb_status_t runtime_status(
    const transport::LibusbRuntimeErrorKind kind) noexcept {
    using transport::LibusbRuntimeErrorKind;
    switch (kind) {
        case LibusbRuntimeErrorKind::version_mismatch:
            return KB_E_NOT_SUPPORTED;
        case LibusbRuntimeErrorKind::already_running:
        case LibusbRuntimeErrorKind::interface_busy:
            return KB_E_BUSY;
        case LibusbRuntimeErrorKind::device_not_found:
            return KB_E_NO_DEVICE;
        case LibusbRuntimeErrorKind::invalid_function_table:
        case LibusbRuntimeErrorKind::invalid_device:
            return KB_E_INTERNAL;
        case LibusbRuntimeErrorKind::init_failed:
        case LibusbRuntimeErrorKind::event_thread_failed:
        case LibusbRuntimeErrorKind::event_loop_failed:
        case LibusbRuntimeErrorKind::runtime_stopped:
        case LibusbRuntimeErrorKind::enumeration_failed:
        case LibusbRuntimeErrorKind::open_failed:
        case LibusbRuntimeErrorKind::configuration_failed:
        case LibusbRuntimeErrorKind::claim_failed:
        case LibusbRuntimeErrorKind::alternate_setting_failed:
            return KB_E_IO;
    }
    return KB_E_INTERNAL;
}

[[nodiscard]] const char* runtime_message(
    const transport::LibusbRuntimeErrorKind kind) noexcept {
    using transport::LibusbRuntimeErrorKind;
    switch (kind) {
        case LibusbRuntimeErrorKind::invalid_function_table:
            return "The libusb function table is incomplete.";
        case LibusbRuntimeErrorKind::version_mismatch:
            return "KairosBoot requires exactly libusb 1.0.30.";
        case LibusbRuntimeErrorKind::already_running:
            return "A different libusb runtime already owns the process context.";
        case LibusbRuntimeErrorKind::init_failed:
            return "libusb context initialization failed.";
        case LibusbRuntimeErrorKind::event_thread_failed:
            return "The libusb event thread could not be started.";
        case LibusbRuntimeErrorKind::event_loop_failed:
            return "The libusb event loop failed.";
        case LibusbRuntimeErrorKind::runtime_stopped:
            return "The libusb runtime is stopped.";
        case LibusbRuntimeErrorKind::enumeration_failed:
            return "USB device enumeration failed.";
        case LibusbRuntimeErrorKind::invalid_device:
            return "The USB device snapshot is invalid.";
        case LibusbRuntimeErrorKind::device_not_found:
            return "The USB device is no longer present at its physical path.";
        case LibusbRuntimeErrorKind::open_failed:
            return "The USB device could not be opened.";
        case LibusbRuntimeErrorKind::configuration_failed:
            return "The USB device configuration could not be selected.";
        case LibusbRuntimeErrorKind::interface_busy:
            return "The Fastboot USB interface is already in use.";
        case LibusbRuntimeErrorKind::claim_failed:
            return "The Fastboot USB interface could not be claimed.";
        case LibusbRuntimeErrorKind::alternate_setting_failed:
            return "The Fastboot USB alternate setting could not be selected.";
    }
    return "An unknown libusb runtime error occurred.";
}

[[nodiscard]] kb_status_t primitive_status(
    const fastboot::PrimitiveErrorCode code) noexcept {
    using fastboot::PrimitiveErrorCode;
    switch (code) {
        case PrimitiveErrorCode::InvalidArgument:
            return KB_E_INVALID_ARGUMENT;
        case PrimitiveErrorCode::Unsupported:
            return KB_E_NOT_SUPPORTED;
        case PrimitiveErrorCode::Busy:
            return KB_E_BUSY;
        case PrimitiveErrorCode::Cancelled:
            return KB_E_CANCELLED;
        case PrimitiveErrorCode::Timeout:
            return KB_E_TIMEOUT;
        case PrimitiveErrorCode::Disconnected:
            return KB_E_NO_DEVICE;
        case PrimitiveErrorCode::Closed:
        case PrimitiveErrorCode::Poisoned:
        case PrimitiveErrorCode::TransportIo:
        case PrimitiveErrorCode::ProtocolViolation:
        case PrimitiveErrorCode::DeviceFail:
            return KB_E_IO;
    }
    return KB_E_INTERNAL;
}

[[nodiscard]] kb_transfer_state_t transfer_state(
    const protocol::TransferCertainty certainty) noexcept {
    using protocol::TransferCertainty;
    switch (certainty) {
        case TransferCertainty::NotTransferred:
            return KB_TRANSFER_NOT_SENT;
        case TransferCertainty::PartialOrUnknown:
            return KB_TRANSFER_PARTIAL_OR_UNKNOWN;
        case TransferCertainty::FullyTransferred:
            return KB_TRANSFER_FULLY_TRANSFERRED;
    }
    return KB_TRANSFER_PARTIAL_OR_UNKNOWN;
}

[[nodiscard]] std::string primitive_message(
    const fastboot::PrimitiveError& error) {
    if (error.code != fastboot::PrimitiveErrorCode::DeviceFail ||
        error.device_message.empty()) {
        return error.message;
    }
    if (error.message.empty()) {
        return error.device_message;
    }
    return error.message + ": " + error.device_message;
}

}  // namespace

OperationErrorPayload normalize_public_error(
    const DeviceSelectionError& error,
    const std::string_view device_identifier) {
    return make_error(
        error.status,
        error.message,
        0,
        KB_TRANSFER_NOT_SENT,
        device_identifier);
}

OperationErrorPayload normalize_public_error(
    const image::ImageSourceError& error,
    const std::string_view device_identifier) {
    return make_error(
        KB_E_IO,
        error.message,
        0,
        KB_TRANSFER_NOT_SENT,
        device_identifier);
}

OperationErrorPayload normalize_public_error(
    const image::FileSourceError& error,
    const std::string_view device_identifier) {
    const auto status = error.kind == image::FileSourceErrorKind::InvalidArgument
                            ? KB_E_INVALID_ARGUMENT
                            : KB_E_IO;
    return make_error(
        status,
        error.message,
        static_cast<std::int32_t>(error.native_code),
        KB_TRANSFER_NOT_SENT,
        device_identifier);
}

OperationErrorPayload normalize_public_error(
    const image::SparseError& error,
    const std::string_view device_identifier) {
    kb_status_t status = KB_E_IO;
    switch (error.kind) {
        case image::SparseErrorKind::InvalidArgument:
            status = KB_E_INVALID_ARGUMENT;
            break;
        case image::SparseErrorKind::Unsupported:
            status = KB_E_NOT_SUPPORTED;
            break;
        case image::SparseErrorKind::Cancelled:
            status = KB_E_CANCELLED;
            break;
        case image::SparseErrorKind::Malformed:
        case image::SparseErrorKind::Truncated:
        case image::SparseErrorKind::Source:
            status = KB_E_IO;
            break;
    }
    return make_error(
        status,
        error.message + " (input offset " +
            std::to_string(error.input_offset) + ")",
        0,
        KB_TRANSFER_NOT_SENT,
        device_identifier);
}

OperationErrorPayload normalize_public_error(
    const image::SparseFlashPlanError& error,
    const std::string_view device_identifier) {
    kb_status_t status = KB_E_IO;
    switch (error.kind) {
        case image::SparseFlashPlanErrorKind::InvalidArgument:
            status = KB_E_INVALID_ARGUMENT;
            break;
        case image::SparseFlashPlanErrorKind::Unsupported:
            status = KB_E_NOT_SUPPORTED;
            break;
        case image::SparseFlashPlanErrorKind::Source:
        case image::SparseFlashPlanErrorKind::ArithmeticOverflow:
            status = KB_E_IO;
            break;
        case image::SparseFlashPlanErrorKind::Cancelled:
            status = KB_E_CANCELLED;
            break;
    }
    return make_error(
        status,
        error.message + " (output offset " +
            std::to_string(error.output_offset) + ")",
        0,
        KB_TRANSFER_NOT_SENT,
        device_identifier);
}

OperationErrorPayload normalize_public_error(
    const transport::LibusbRuntimeError& error,
    const std::string_view device_identifier) {
    return make_error(
        runtime_status(error.kind),
        runtime_message(error.kind),
        static_cast<std::int32_t>(error.native_code),
        KB_TRANSFER_NOT_SENT,
        device_identifier);
}

OperationErrorPayload normalize_public_error(
    const fastboot::PrimitiveError& error,
    const std::string_view device_identifier) {
    return make_error(
        primitive_status(error.code),
        primitive_message(error),
        static_cast<std::int32_t>(error.native_code),
        transfer_state(error.outbound_certainty),
        device_identifier);
}

void accumulate_flash_transfer_state(
    OperationErrorPayload& payload,
    const fastboot::PrimitiveOperation failed_operation,
    const std::uint64_t completed_before_part,
    const std::uint64_t current_part_size,
    const std::uint64_t total_size) noexcept {
    if (payload.transfer_state == KB_TRANSFER_PARTIAL_OR_UNKNOWN ||
        completed_before_part > total_size ||
        current_part_size > total_size - completed_before_part) {
        payload.transfer_state = KB_TRANSFER_PARTIAL_OR_UNKNOWN;
        return;
    }

    std::uint64_t known_completed = completed_before_part;
    if (failed_operation == fastboot::PrimitiveOperation::Flash ||
        payload.transfer_state == KB_TRANSFER_FULLY_TRANSFERRED) {
        known_completed += current_part_size;
    }
    if (known_completed == 0) {
        payload.transfer_state = KB_TRANSFER_NOT_SENT;
    } else if (known_completed == total_size) {
        payload.transfer_state = KB_TRANSFER_FULLY_TRANSFERRED;
    } else {
        payload.transfer_state = KB_TRANSFER_PARTIAL_OR_UNKNOWN;
    }
}

}  // namespace kairosboot::api
