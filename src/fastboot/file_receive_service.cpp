// SPDX-License-Identifier: MIT
#include "src/fastboot/file_receive_service.hpp"

#include <utility>

namespace kairosboot::fastboot {
namespace {

[[nodiscard]] FileReceiveError invalid_maximum(
    const PrimitiveOperation operation,
    const std::uint64_t maximum_bytes) {
    const auto zero = maximum_bytes == 0;
    return {
        .code = zero ? FileReceiveErrorCode::InvalidArgument
                     : FileReceiveErrorCode::LimitExceeded,
        .operation = operation,
        .phase = protocol::ProtocolPhase::Validation,
        .message = zero
            ? "Fastboot file receive maximum must not be zero"
            : "Fastboot file receive maximum exceeds the 32-bit DATA limit",
        .device_message = {},
        .primitive_code = PrimitiveErrorCode::InvalidArgument,
        .file_code = std::nullopt,
        .terminal = std::nullopt,
        .informational = {},
        .transport_status = protocol::TransportStatus::Ok,
        .transport_certainty = protocol::TransferCertainty::NotTransferred,
        .outbound_certainty = protocol::TransferCertainty::NotTransferred,
        .inbound_expected = std::nullopt,
        .inbound_transferred = 0,
        .inbound_certainty = protocol::TransferCertainty::NotTransferred,
        .native_code = 0,
        .session_poisoned = false,
    };
}

[[nodiscard]] FileReceiveError create_error(
    const PrimitiveOperation operation,
    protocol::FileTransferSinkError error) {
    const auto invalid =
        error.kind == protocol::FileTransferSinkErrorKind::InvalidArgument ||
        error.kind == protocol::FileTransferSinkErrorKind::UnsafePath;
    return {
        .code = invalid ? FileReceiveErrorCode::InvalidArgument
                        : FileReceiveErrorCode::DestinationCreate,
        .operation = operation,
        .phase = protocol::ProtocolPhase::Validation,
        .message = std::move(error.message),
        .device_message = {},
        .primitive_code = std::nullopt,
        .file_code = error.kind,
        .terminal = std::nullopt,
        .informational = {},
        .transport_status = protocol::TransportStatus::Ok,
        .transport_certainty = protocol::TransferCertainty::NotTransferred,
        .outbound_certainty = protocol::TransferCertainty::NotTransferred,
        .inbound_expected = std::nullopt,
        .inbound_transferred = 0,
        .inbound_certainty = protocol::TransferCertainty::NotTransferred,
        .native_code = error.native_code,
        .session_poisoned = false,
    };
}

[[nodiscard]] FileReceiveError primitive_error(
    PrimitiveError error,
    const std::uint64_t maximum_bytes) {
    std::optional<protocol::Response> terminal;
    if (error.code == PrimitiveErrorCode::DeviceFail) {
        terminal = protocol::Response{
            .kind = protocol::ResponseKind::Fail,
            .payload = error.device_message,
            .data_size = std::nullopt,
        };
    }
    const auto receive_limit_exceeded =
        error.code == PrimitiveErrorCode::ProtocolViolation &&
        error.phase == protocol::ProtocolPhase::InitialResponse &&
        error.inbound_expected.has_value() &&
        *error.inbound_expected > maximum_bytes;
    const auto code = error.code == PrimitiveErrorCode::InvalidArgument
        ? FileReceiveErrorCode::InvalidArgument
        : error.code == PrimitiveErrorCode::DeviceFail
            ? FileReceiveErrorCode::DeviceFail
            : receive_limit_exceeded
                ? FileReceiveErrorCode::LimitExceeded
                : FileReceiveErrorCode::Protocol;
    return {
        .code = code,
        .operation = error.operation,
        .phase = error.phase,
        .message = std::move(error.message),
        .device_message = std::move(error.device_message),
        .primitive_code = error.code,
        .file_code = std::nullopt,
        .terminal = std::move(terminal),
        .informational = std::move(error.informational),
        .transport_status = error.transport_status,
        .transport_certainty = error.transport_certainty,
        .outbound_certainty = error.outbound_certainty,
        .inbound_expected = error.inbound_expected,
        .inbound_transferred = error.inbound_transferred,
        .inbound_certainty = error.inbound_certainty,
        .native_code = error.native_code,
        .session_poisoned = error.session_poisoned,
    };
}

[[nodiscard]] FileReceiveError exact_length_error(
    const PrimitiveOperation operation,
    PrimitiveReply reply,
    const std::uint64_t bytes_written) {
    const auto expected = reply.inbound_expected;
    std::string message =
        "Fastboot receive completed without an exact DATA byte count";
    if (expected.has_value() && reply.inbound_transferred == *expected &&
        bytes_written != *expected) {
        message = "file receive sink byte count does not match Fastboot DATA";
    }
    return {
        .code = FileReceiveErrorCode::Protocol,
        .operation = operation,
        .phase = protocol::ProtocolPhase::FinalResponse,
        .message = std::move(message),
        .device_message = {},
        .primitive_code = PrimitiveErrorCode::ProtocolViolation,
        .file_code = std::nullopt,
        .terminal = std::move(reply.terminal),
        .informational = std::move(reply.informational),
        .transport_status = protocol::TransportStatus::Ok,
        .transport_certainty = protocol::TransferCertainty::FullyTransferred,
        .outbound_certainty = reply.outbound_certainty,
        .inbound_expected = expected,
        .inbound_transferred = reply.inbound_transferred,
        .inbound_certainty = reply.inbound_certainty,
        .native_code = 0,
        .session_poisoned = false,
    };
}

[[nodiscard]] FileReceiveError publish_error(
    const PrimitiveOperation operation,
    PrimitiveReply reply,
    protocol::FileTransferSinkError error) {
    return {
        .code = FileReceiveErrorCode::DestinationPublish,
        .operation = operation,
        .phase = protocol::ProtocolPhase::FinalResponse,
        .message = std::move(error.message),
        .device_message = {},
        .primitive_code = std::nullopt,
        .file_code = error.kind,
        .terminal = std::move(reply.terminal),
        .informational = std::move(reply.informational),
        .transport_status = protocol::TransportStatus::Ok,
        .transport_certainty = protocol::TransferCertainty::FullyTransferred,
        .outbound_certainty = reply.outbound_certainty,
        .inbound_expected = reply.inbound_expected,
        .inbound_transferred = reply.inbound_transferred,
        .inbound_certainty = reply.inbound_certainty,
        .native_code = error.native_code,
        .session_poisoned = false,
    };
}

}  // namespace

FileReceiveService::FileReceiveService(PrimitiveService& primitives) noexcept
    : primitives_(primitives) {}

std::expected<FileReceiveResult, FileReceiveError> FileReceiveService::upload(
    const std::filesystem::path& destination,
    const std::uint64_t maximum_bytes,
    const protocol::TransferProgressObserver& observer) {
    if (maximum_bytes == 0 || maximum_bytes > kMaximumFileReceiveBytes) {
        return std::unexpected(invalid_maximum(
            PrimitiveOperation::Upload, maximum_bytes));
    }
    auto sink = create_sink(PrimitiveOperation::Upload, destination);
    if (!sink) {
        return std::unexpected(std::move(sink.error()));
    }
    return finish(
        PrimitiveOperation::Upload,
        *sink,
        maximum_bytes,
        primitives_.upload_to_sink(*sink, maximum_bytes, observer));
}

std::expected<FileReceiveResult, FileReceiveError> FileReceiveService::fetch(
    const std::string_view partition,
    const FetchRange range,
    const std::filesystem::path& destination,
    const std::uint64_t maximum_bytes,
    const protocol::TransferProgressObserver& observer) {
    if (maximum_bytes == 0 || maximum_bytes > kMaximumFileReceiveBytes) {
        return std::unexpected(invalid_maximum(
            PrimitiveOperation::Fetch, maximum_bytes));
    }
    auto sink = create_sink(PrimitiveOperation::Fetch, destination);
    if (!sink) {
        return std::unexpected(std::move(sink.error()));
    }
    return finish(
        PrimitiveOperation::Fetch,
        *sink,
        maximum_bytes,
        primitives_.fetch_to_sink(
            partition, range, *sink, maximum_bytes, observer));
}

void FileReceiveService::request_cancel() noexcept {
    primitives_.request_cancel();
}

std::expected<std::shared_ptr<protocol::FileTransferSink>, FileReceiveError>
FileReceiveService::create_sink(
    const PrimitiveOperation operation,
    const std::filesystem::path& destination) const {
    auto sink = protocol::FileTransferSink::create(destination);
    if (!sink) {
        return std::unexpected(create_error(
            operation, std::move(sink.error())));
    }
    return *sink;
}

std::expected<FileReceiveResult, FileReceiveError> FileReceiveService::finish(
    const PrimitiveOperation operation,
    std::shared_ptr<protocol::FileTransferSink> sink,
    const std::uint64_t maximum_bytes,
    std::expected<PrimitiveReply, PrimitiveError> reply) const {
    if (!reply) {
        sink->discard();
        return std::unexpected(primitive_error(
            std::move(reply.error()), maximum_bytes));
    }

    const auto expected = reply->inbound_expected;
    const auto bytes_written = sink->bytes_written();
    if (reply->terminal.kind != protocol::ResponseKind::Okay ||
        !expected.has_value() || reply->inbound_transferred != *expected ||
        reply->inbound_certainty !=
            protocol::TransferCertainty::FullyTransferred ||
        bytes_written != *expected) {
        sink->discard();
        return std::unexpected(exact_length_error(
            operation, std::move(*reply), bytes_written));
    }

    auto sealed = sink->seal(*expected);
    if (!sealed) {
        sink->discard();
        return std::unexpected(publish_error(
            operation, std::move(*reply), std::move(sealed.error())));
    }
    return FileReceiveResult{
        .reply = std::move(*reply),
        .bytes_published = *expected,
    };
}

}  // namespace kairosboot::fastboot
