// SPDX-License-Identifier: MIT
#include "src/api/device_selection.hpp"
#include "src/api/error_mapping.hpp"
#include "src/fastboot/primitive_service.hpp"
#include "src/image/file_source.hpp"
#include "src/image/sparse_flash_plan.hpp"
#include "src/transport/libusb_runtime.hpp"

#include <array>
#include <cstdint>
#include <exception>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using kairosboot::api::DeviceSelectionError;
using kairosboot::api::OperationErrorPayload;
using kairosboot::api::accumulate_flash_transfer_state;
using kairosboot::api::normalize_public_error;
using kairosboot::fastboot::PrimitiveError;
using kairosboot::fastboot::PrimitiveErrorCode;
using kairosboot::fastboot::PrimitiveOperation;
using kairosboot::image::FileSourceError;
using kairosboot::image::FileSourceErrorKind;
using kairosboot::image::ImageSourceError;
using kairosboot::image::SparseError;
using kairosboot::image::SparseErrorKind;
using kairosboot::image::SparseFlashPlanError;
using kairosboot::image::SparseFlashPlanErrorKind;
using kairosboot::protocol::TransferCertainty;
using kairosboot::protocol::Response;
using kairosboot::protocol::ResponseKind;
using kairosboot::transport::LibusbRuntimeError;
using kairosboot::transport::LibusbRuntimeErrorKind;

#define CHECK(condition)                                                         \
    do {                                                                         \
        if (!(condition)) {                                                       \
            throw std::runtime_error(                                             \
                std::string("check failed at line ") + std::to_string(__LINE__) + \
                ": " #condition);                                                \
        }                                                                        \
    } while (false)

void check_common(
    const OperationErrorPayload& result,
    const kb_status_t status,
    const std::int32_t native_code,
    const kb_transfer_state_t transfer_state,
    const std::string_view device_identifier) {
    CHECK(result.status == status);
    CHECK(result.native_code == native_code);
    CHECK(result.transfer_state == transfer_state);
    CHECK(result.device_identifier == device_identifier);
}

void device_selection_preserves_status_message_and_copies_identifier() {
    std::string identifier = "usb:1-2.3/设备";
    const auto result = normalize_public_error(
        DeviceSelectionError{KB_E_AMBIGUOUS_DEVICE, "duplicate serial"},
        identifier);
    identifier.assign("mutated");

    check_common(
        result,
        KB_E_AMBIGUOUS_DEVICE,
        0,
        KB_TRANSFER_NOT_SENT,
        "usb:1-2.3/设备");
    CHECK(result.message == "duplicate serial");
}

void image_source_is_io_before_any_transfer() {
    const auto result = normalize_public_error(
        ImageSourceError{"image read failed"}, "SERIAL-A");
    check_common(result, KB_E_IO, 0, KB_TRANSFER_NOT_SENT, "SERIAL-A");
    CHECK(result.message == "image read failed");
}

void file_source_kinds_map_before_transfer_and_retain_native_code() {
    struct Case final {
        FileSourceErrorKind kind;
        kb_status_t status;
    };
    constexpr std::array cases{
        Case{FileSourceErrorKind::InvalidArgument, KB_E_INVALID_ARGUMENT},
        Case{FileSourceErrorKind::NotFound, KB_E_IO},
        Case{FileSourceErrorKind::NotRegularFile, KB_E_IO},
        Case{FileSourceErrorKind::SizeUnavailable, KB_E_IO},
        Case{FileSourceErrorKind::OpenFailed, KB_E_IO},
    };

    for (const auto& test : cases) {
        const auto result = normalize_public_error(
            FileSourceError{test.kind, 13, "unable to open image"},
            "SERIAL-F");
        check_common(
            result,
            test.status,
            13,
            KB_TRANSFER_NOT_SENT,
            "SERIAL-F");
        CHECK(result.message == "unable to open image");
    }
}

void sparse_kinds_map_before_transfer_and_retain_offset() {
    struct Case final {
        SparseErrorKind kind;
        kb_status_t status;
    };
    constexpr std::array cases{
        Case{SparseErrorKind::Malformed, KB_E_IO},
        Case{SparseErrorKind::Truncated, KB_E_IO},
        Case{SparseErrorKind::Unsupported, KB_E_NOT_SUPPORTED},
        Case{SparseErrorKind::Source, KB_E_IO},
        Case{SparseErrorKind::InvalidArgument, KB_E_INVALID_ARGUMENT},
        Case{SparseErrorKind::Cancelled, KB_E_CANCELLED},
    };

    for (const auto& test : cases) {
        const auto result = normalize_public_error(
            SparseError{test.kind, 37, "invalid sparse image"}, "SERIAL-S");
        check_common(
            result,
            test.status,
            0,
            KB_TRANSFER_NOT_SENT,
            "SERIAL-S");
        CHECK(result.message == "invalid sparse image (input offset 37)");
    }
}

void sparse_plan_kinds_map_before_transfer_and_retain_offset() {
    struct Case final {
        SparseFlashPlanErrorKind kind;
        kb_status_t status;
    };
    constexpr std::array cases{
        Case{SparseFlashPlanErrorKind::InvalidArgument,
             KB_E_INVALID_ARGUMENT},
        Case{SparseFlashPlanErrorKind::Unsupported, KB_E_NOT_SUPPORTED},
        Case{SparseFlashPlanErrorKind::Source, KB_E_IO},
        Case{SparseFlashPlanErrorKind::ArithmeticOverflow, KB_E_IO},
        Case{SparseFlashPlanErrorKind::Cancelled, KB_E_CANCELLED},
    };

    for (const auto& test : cases) {
        const auto result = normalize_public_error(
            SparseFlashPlanError{test.kind, 8192, "unable to split image"},
            "SERIAL-SP");
        check_common(
            result,
            test.status,
            0,
            KB_TRANSFER_NOT_SENT,
            "SERIAL-SP");
        CHECK(result.message ==
              "unable to split image (output offset 8192)");
    }
}

void libusb_kinds_have_stable_status_and_retain_native_code() {
    struct Case final {
        LibusbRuntimeErrorKind kind;
        kb_status_t status;
    };
    constexpr std::array cases{
        Case{LibusbRuntimeErrorKind::invalid_function_table, KB_E_INTERNAL},
        Case{LibusbRuntimeErrorKind::version_mismatch, KB_E_NOT_SUPPORTED},
        Case{LibusbRuntimeErrorKind::already_running, KB_E_BUSY},
        Case{LibusbRuntimeErrorKind::init_failed, KB_E_IO},
        Case{LibusbRuntimeErrorKind::event_thread_failed, KB_E_IO},
        Case{LibusbRuntimeErrorKind::event_loop_failed, KB_E_IO},
        Case{LibusbRuntimeErrorKind::runtime_stopped, KB_E_IO},
        Case{LibusbRuntimeErrorKind::enumeration_failed, KB_E_IO},
        Case{LibusbRuntimeErrorKind::invalid_device, KB_E_INTERNAL},
        Case{LibusbRuntimeErrorKind::device_not_found, KB_E_NO_DEVICE},
        Case{LibusbRuntimeErrorKind::open_failed, KB_E_IO},
        Case{LibusbRuntimeErrorKind::configuration_failed, KB_E_IO},
        Case{LibusbRuntimeErrorKind::interface_busy, KB_E_BUSY},
        Case{LibusbRuntimeErrorKind::claim_failed, KB_E_IO},
        Case{LibusbRuntimeErrorKind::alternate_setting_failed, KB_E_IO},
    };

    for (const auto& test : cases) {
        const auto result = normalize_public_error(
            LibusbRuntimeError{test.kind, -71, 1, 0, 29}, "SERIAL-USB");
        check_common(
            result,
            test.status,
            -71,
            KB_TRANSFER_NOT_SENT,
            "SERIAL-USB");
        CHECK(!result.message.empty());
    }
}

void libusb_messages_describe_the_stable_failure() {
    const auto version = normalize_public_error(
        LibusbRuntimeError{
            LibusbRuntimeErrorKind::version_mismatch, 0, 1, 0, 29},
        "");
    CHECK(version.message == "KairosBoot requires exactly libusb 1.0.30.");

    const auto busy = normalize_public_error(
        LibusbRuntimeError{LibusbRuntimeErrorKind::interface_busy, -6}, "");
    CHECK(busy.message == "The Fastboot USB interface is already in use.");
}

void primitive_kinds_have_stable_public_status() {
    struct Case final {
        PrimitiveErrorCode code;
        kb_status_t status;
    };
    constexpr std::array cases{
        Case{PrimitiveErrorCode::InvalidArgument, KB_E_INVALID_ARGUMENT},
        Case{PrimitiveErrorCode::Unsupported, KB_E_NOT_SUPPORTED},
        Case{PrimitiveErrorCode::Busy, KB_E_BUSY},
        Case{PrimitiveErrorCode::Closed, KB_E_IO},
        Case{PrimitiveErrorCode::Poisoned, KB_E_IO},
        Case{PrimitiveErrorCode::Cancelled, KB_E_CANCELLED},
        Case{PrimitiveErrorCode::Timeout, KB_E_TIMEOUT},
        Case{PrimitiveErrorCode::Disconnected, KB_E_NO_DEVICE},
        Case{PrimitiveErrorCode::TransportIo, KB_E_IO},
        Case{PrimitiveErrorCode::ProtocolViolation, KB_E_PROTOCOL},
        Case{PrimitiveErrorCode::DeviceFail, KB_E_DEVICE_FAIL},
    };

    for (const auto& test : cases) {
        PrimitiveError error{};
        error.code = test.code;
        error.message = "primitive failed";
        error.outbound_certainty = TransferCertainty::NotTransferred;
        error.native_code = -19;
        const auto result = normalize_public_error(error, "SERIAL-P");
        check_common(
            result,
            test.status,
            -19,
            KB_TRANSFER_NOT_SENT,
            "SERIAL-P");
    }
}

void primitive_outbound_certainty_maps_to_public_three_state() {
    struct Case final {
        TransferCertainty source;
        kb_transfer_state_t destination;
    };
    constexpr std::array cases{
        Case{TransferCertainty::NotTransferred, KB_TRANSFER_NOT_SENT},
        Case{TransferCertainty::PartialOrUnknown,
             KB_TRANSFER_PARTIAL_OR_UNKNOWN},
        Case{TransferCertainty::FullyTransferred,
             KB_TRANSFER_FULLY_TRANSFERRED},
    };

    for (const auto& test : cases) {
        PrimitiveError error{};
        error.code = PrimitiveErrorCode::TransportIo;
        error.message = "transport failed";
        error.outbound_certainty = test.source;
        error.native_code = 32;
        const auto result = normalize_public_error(error, "SERIAL-C");
        check_common(result, KB_E_IO, 32, test.destination, "SERIAL-C");
    }
}

void multipart_flash_certainty_includes_downloaded_current_part() {
    const auto accumulated = [](
                                 const kb_transfer_state_t initial,
                                 const PrimitiveOperation failed_operation,
                                 const std::uint64_t completed,
                                 const std::uint64_t current,
                                 const std::uint64_t total) {
        OperationErrorPayload payload{
            .status = KB_E_IO,
            .message = "failed",
            .native_code = 0,
            .transfer_state = initial,
            .device_identifier = "SERIAL-M",
            .device_message = {},
            .command_messages = {},
            .inbound_expected = std::nullopt,
            .inbound_transferred = 0,
            .inbound_transfer_state = KB_TRANSFER_NOT_SENT,
            .session_poisoned = false,
        };
        accumulate_flash_transfer_state(
            payload, failed_operation, completed, current, total);
        return payload.transfer_state;
    };

    CHECK(accumulated(KB_TRANSFER_NOT_SENT, PrimitiveOperation::Download,
                      0, 40, 100) == KB_TRANSFER_NOT_SENT);
    CHECK(accumulated(KB_TRANSFER_NOT_SENT, PrimitiveOperation::Download,
                      40, 30, 100) == KB_TRANSFER_PARTIAL_OR_UNKNOWN);
    CHECK(accumulated(KB_TRANSFER_NOT_SENT, PrimitiveOperation::Flash,
                      0, 100, 100) == KB_TRANSFER_FULLY_TRANSFERRED);
    CHECK(accumulated(KB_TRANSFER_NOT_SENT, PrimitiveOperation::Flash,
                      40, 60, 100) == KB_TRANSFER_FULLY_TRANSFERRED);
    CHECK(accumulated(KB_TRANSFER_NOT_SENT, PrimitiveOperation::Flash,
                      40, 30, 100) == KB_TRANSFER_PARTIAL_OR_UNKNOWN);
    CHECK(accumulated(KB_TRANSFER_FULLY_TRANSFERRED,
                      PrimitiveOperation::Download, 0, 100, 100) ==
          KB_TRANSFER_FULLY_TRANSFERRED);
    CHECK(accumulated(KB_TRANSFER_PARTIAL_OR_UNKNOWN,
                      PrimitiveOperation::Download, 0, 100, 100) ==
          KB_TRANSFER_PARTIAL_OR_UNKNOWN);
    CHECK(accumulated(KB_TRANSFER_NOT_SENT, PrimitiveOperation::Flash,
                      90, 20, 100) == KB_TRANSFER_PARTIAL_OR_UNKNOWN);
}

void device_fail_message_retains_target_payload() {
    PrimitiveError error{};
    error.code = PrimitiveErrorCode::DeviceFail;
    error.message = "Fastboot device rejected the command";
    error.device_message = "partition is locked";
    error.outbound_certainty = TransferCertainty::FullyTransferred;
    const auto result = normalize_public_error(error, "设备-三");

    check_common(
        result,
        KB_E_DEVICE_FAIL,
        0,
        KB_TRANSFER_FULLY_TRANSFERRED,
        "设备-三");
    CHECK(result.message == "Fastboot device rejected the command");
    CHECK(result.device_message == "partition is locked");
}

void primitive_error_retains_binary_safe_diagnostics() {
    PrimitiveError error{};
    error.code = PrimitiveErrorCode::ProtocolViolation;
    error.message = "malformed response";
    error.device_message = std::string("bad\0payload", 11);
    error.informational = {
        Response{ResponseKind::Info, std::string("one\0two", 7), std::nullopt},
        Response{ResponseKind::Text, "human text", std::nullopt},
    };
    error.outbound_certainty = TransferCertainty::FullyTransferred;
    error.inbound_expected = 16;
    error.inbound_transferred = 7;
    error.inbound_certainty = TransferCertainty::PartialOrUnknown;
    error.session_poisoned = true;

    const auto result = normalize_public_error(error, "tcp:host:5554");
    CHECK(result.status == KB_E_PROTOCOL);
    CHECK(result.device_message == std::string("bad\0payload", 11));
    CHECK(result.command_messages.size() == 2);
    CHECK(result.command_messages[0].kind ==
          kairosboot::api::CommandMessageKind::Info);
    CHECK(result.command_messages[0].text == std::string("one\0two", 7));
    CHECK(result.command_messages[1].kind ==
          kairosboot::api::CommandMessageKind::Text);
    CHECK(result.inbound_expected == 16);
    CHECK(result.inbound_transferred == 7);
    CHECK(result.inbound_transfer_state == KB_TRANSFER_PARTIAL_OR_UNKNOWN);
    CHECK(result.session_poisoned);
}

}  // namespace

int main() {
    const std::vector<std::pair<std::string_view, void (*)()>> tests{
        {"device selection normalization",
         device_selection_preserves_status_message_and_copies_identifier},
        {"image source normalization", image_source_is_io_before_any_transfer},
        {"file source normalization",
         file_source_kinds_map_before_transfer_and_retain_native_code},
        {"sparse source normalization",
         sparse_kinds_map_before_transfer_and_retain_offset},
        {"sparse plan normalization",
         sparse_plan_kinds_map_before_transfer_and_retain_offset},
        {"libusb status normalization",
         libusb_kinds_have_stable_status_and_retain_native_code},
        {"libusb message normalization",
         libusb_messages_describe_the_stable_failure},
        {"primitive status normalization",
         primitive_kinds_have_stable_public_status},
        {"primitive certainty normalization",
         primitive_outbound_certainty_maps_to_public_three_state},
        {"multipart flash certainty",
         multipart_flash_certainty_includes_downloaded_current_part},
        {"primitive device FAIL message",
         device_fail_message_retains_target_payload},
        {"primitive binary diagnostics",
         primitive_error_retains_binary_safe_diagnostics},
    };

    std::size_t failures = 0;
    for (const auto& [name, test] : tests) {
        try {
            test();
            std::cout << "PASS: " << name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "FAIL: " << name << ": " << error.what() << '\n';
        }
    }

    if (failures != 0) {
        std::cerr << failures << " error-mapping test(s) failed\n";
        return 1;
    }
    std::cout << tests.size() << " error-mapping tests passed\n";
    return 0;
}
