// SPDX-License-Identifier: MIT
#include "src/api/device_selection.hpp"
#include "src/api/error_mapping.hpp"
#include "src/fastboot/primitive_service.hpp"
#include "src/image/file_source.hpp"
#include "src/transport/libusb_runtime.hpp"

#include <array>
#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using kairosboot::api::DeviceSelectionError;
using kairosboot::api::OperationErrorPayload;
using kairosboot::api::normalize_public_error;
using kairosboot::fastboot::PrimitiveError;
using kairosboot::fastboot::PrimitiveErrorCode;
using kairosboot::image::FileSourceError;
using kairosboot::image::FileSourceErrorKind;
using kairosboot::image::ImageSourceError;
using kairosboot::protocol::TransferCertainty;
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
        Case{PrimitiveErrorCode::Busy, KB_E_BUSY},
        Case{PrimitiveErrorCode::Closed, KB_E_IO},
        Case{PrimitiveErrorCode::Poisoned, KB_E_IO},
        Case{PrimitiveErrorCode::Cancelled, KB_E_CANCELLED},
        Case{PrimitiveErrorCode::Timeout, KB_E_TIMEOUT},
        Case{PrimitiveErrorCode::Disconnected, KB_E_NO_DEVICE},
        Case{PrimitiveErrorCode::TransportIo, KB_E_IO},
        Case{PrimitiveErrorCode::ProtocolViolation, KB_E_IO},
        Case{PrimitiveErrorCode::DeviceFail, KB_E_IO},
    };

    for (const auto& test : cases) {
        const PrimitiveError error{
            .code = test.code,
            .message = "primitive failed",
            .outbound_certainty = TransferCertainty::NotTransferred,
            .native_code = -19,
        };
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
        const PrimitiveError error{
            .code = PrimitiveErrorCode::TransportIo,
            .message = "transport failed",
            .outbound_certainty = test.source,
            .native_code = 32,
        };
        const auto result = normalize_public_error(error, "SERIAL-C");
        check_common(result, KB_E_IO, 32, test.destination, "SERIAL-C");
    }
}

void device_fail_message_retains_target_payload() {
    const PrimitiveError error{
        .code = PrimitiveErrorCode::DeviceFail,
        .message = "Fastboot device rejected the command",
        .device_message = "partition is locked",
        .outbound_certainty = TransferCertainty::FullyTransferred,
    };
    const auto result = normalize_public_error(error, "设备-三");

    check_common(
        result,
        KB_E_IO,
        0,
        KB_TRANSFER_FULLY_TRANSFERRED,
        "设备-三");
    CHECK(result.message ==
          "Fastboot device rejected the command: partition is locked");
}

}  // namespace

int main() {
    const std::vector<std::pair<std::string_view, void (*)()>> tests{
        {"device selection normalization",
         device_selection_preserves_status_message_and_copies_identifier},
        {"image source normalization", image_source_is_io_before_any_transfer},
        {"file source normalization",
         file_source_kinds_map_before_transfer_and_retain_native_code},
        {"libusb status normalization",
         libusb_kinds_have_stable_status_and_retain_native_code},
        {"libusb message normalization",
         libusb_messages_describe_the_stable_failure},
        {"primitive status normalization",
         primitive_kinds_have_stable_public_status},
        {"primitive certainty normalization",
         primitive_outbound_certainty_maps_to_public_three_state},
        {"primitive device FAIL message",
         device_fail_message_retains_target_payload},
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
