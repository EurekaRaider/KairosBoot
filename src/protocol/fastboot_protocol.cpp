// SPDX-License-Identifier: MIT
#include "fastboot_protocol.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdio>
#include <limits>
#include <utility>

namespace kairosboot::protocol {
namespace {

[[nodiscard]] std::string bytes_to_string(std::span<const std::byte> bytes) {
    std::string value;
    value.reserve(bytes.size());
    for (const auto byte : bytes) {
        value.push_back(static_cast<char>(std::to_integer<unsigned char>(byte)));
    }
    return value;
}

[[nodiscard]] bool has_prefix(
    std::span<const std::byte> packet,
    std::string_view prefix) {
    return std::equal(prefix.begin(), prefix.end(), packet.begin(), [](char left, std::byte right) {
        return static_cast<unsigned char>(left) == std::to_integer<unsigned char>(right);
    });
}

[[nodiscard]] ProtocolError malformed_response_error(const ResponseParseError& error) {
    return {
        .code = ProtocolErrorCode::MalformedResponse,
        .message = error.message,
    };
}

}  // namespace

std::expected<Response, ResponseParseError> parse_response(
    std::span<const std::byte> packet,
    const std::size_t max_response_bytes) {
    if (packet.empty()) {
        return std::unexpected(ResponseParseError{
            .code = ResponseParseErrorCode::ZeroLength,
            .message = "Fastboot logical response is empty",
        });
    }
    if (packet.size() < 4) {
        return std::unexpected(ResponseParseError{
            .code = ResponseParseErrorCode::TooShort,
            .message = "Fastboot response is shorter than its four-byte status prefix",
        });
    }
    if (packet.size() > max_response_bytes) {
        return std::unexpected(ResponseParseError{
            .code = ResponseParseErrorCode::TooLong,
            .message = "Fastboot response exceeds the configured response limit",
        });
    }

    const auto payload_bytes = packet.subspan(4);
    if (has_prefix(packet, "INFO")) {
        return Response{ResponseKind::Info, bytes_to_string(payload_bytes), std::nullopt};
    }
    if (has_prefix(packet, "TEXT")) {
        return Response{ResponseKind::Text, bytes_to_string(payload_bytes), std::nullopt};
    }
    if (has_prefix(packet, "OKAY")) {
        return Response{ResponseKind::Okay, bytes_to_string(payload_bytes), std::nullopt};
    }
    if (has_prefix(packet, "FAIL")) {
        return Response{ResponseKind::Fail, bytes_to_string(payload_bytes), std::nullopt};
    }
    if (!has_prefix(packet, "DATA")) {
        return std::unexpected(ResponseParseError{
            .code = ResponseParseErrorCode::UnknownPrefix,
            .message = "Fastboot response has an unknown status prefix",
        });
    }

    if (payload_bytes.size() != 8) {
        return std::unexpected(ResponseParseError{
            .code = ResponseParseErrorCode::InvalidDataLength,
            .message = "Fastboot DATA response must contain exactly eight hexadecimal digits",
        });
    }

    const auto payload = bytes_to_string(payload_bytes);
    std::uint32_t data_size = 0;
    const auto [end, error] = std::from_chars(
        payload.data(), payload.data() + payload.size(), data_size, 16);
    if (error != std::errc{} || end != payload.data() + payload.size()) {
        return std::unexpected(ResponseParseError{
            .code = ResponseParseErrorCode::InvalidDataHex,
            .message = "Fastboot DATA response contains invalid hexadecimal digits",
        });
    }

    return Response{ResponseKind::Data, payload, data_size};
}

FastbootSession::FastbootSession(
    std::unique_ptr<ITransportSession> transport,
    SessionOptions options)
    : transport_(std::move(transport)), options_(options) {
    if (options_.max_response_bytes < 4) {
        options_.max_response_bytes = 4;
    }
    if (options_.max_command_bytes == 0) {
        options_.max_command_bytes = kDefaultMaxCommandBytes;
    }
    if (options_.max_informational_responses == 0) {
        options_.max_informational_responses = 1;
    }
}

FastbootSession::~FastbootSession() {
    close();
}

std::expected<CommandResult, ProtocolError> FastbootSession::command(
    const std::string_view command_text) {
    std::unique_lock lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock()) {
        return std::unexpected(ProtocolError{
            .code = ProtocolErrorCode::Busy,
            .message = "another Fastboot operation is already using this session",
        });
    }

    if (const auto begin = begin_locked(command_text); !begin) {
        return std::unexpected(begin.error());
    }

    state_ = SessionState::WritingCommand;
    const auto command_bytes = std::as_bytes(std::span(command_text));
    if (const auto write = write_exact_locked(command_bytes); !write) {
        return std::unexpected(write.error());
    }

    state_ = SessionState::AwaitingResponse;
    return read_terminal_locked({});
}

std::expected<CommandResult, ProtocolError> FastbootSession::download(
    const std::span<const std::byte> bytes) {
    std::unique_lock lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock()) {
        return std::unexpected(ProtocolError{
            .code = ProtocolErrorCode::Busy,
            .message = "another Fastboot operation is already using this session",
        });
    }

    if (bytes.size() > std::numeric_limits<std::uint32_t>::max()) {
        return std::unexpected(ProtocolError{
            .code = ProtocolErrorCode::InvalidArgument,
            .message = "Fastboot download size exceeds the protocol's 32-bit limit",
        });
    }

    std::array<char, 18> command_buffer{};
    const auto command_length = std::snprintf(
        command_buffer.data(),
        command_buffer.size(),
        "download:%08x",
        static_cast<unsigned int>(bytes.size()));
    const std::string_view command_text(
        command_buffer.data(), static_cast<std::size_t>(command_length));

    if (const auto begin = begin_locked(command_text); !begin) {
        return std::unexpected(begin.error());
    }

    state_ = SessionState::WritingCommand;
    if (const auto write = write_exact_locked(std::as_bytes(std::span(command_text))); !write) {
        return std::unexpected(write.error());
    }

    state_ = SessionState::AwaitingResponse;
    std::vector<Response> informational;
    for (;;) {
        auto response = read_response_locked();
        if (!response) {
            return std::unexpected(response.error());
        }

        if (response->kind == ResponseKind::Info || response->kind == ResponseKind::Text) {
            if (informational.size() >= options_.max_informational_responses) {
                return poison_locked(ProtocolError{
                    .code = ProtocolErrorCode::TooManyInformationalResponses,
                    .message = "Fastboot device exceeded the informational response limit",
                });
            }
            informational.push_back(std::move(*response));
            continue;
        }

        if (response->kind == ResponseKind::Fail) {
            state_ = SessionState::Ready;
            return CommandResult{std::move(*response), std::move(informational)};
        }

        if (response->kind != ResponseKind::Data) {
            return poison_locked(ProtocolError{
                .code = ProtocolErrorCode::UnexpectedResponse,
                .message = "Fastboot download expected DATA or FAIL before sending payload",
            });
        }

        if (!response->data_size || *response->data_size != bytes.size()) {
            return poison_locked(ProtocolError{
                .code = ProtocolErrorCode::DataLengthMismatch,
                .message = "Fastboot device accepted a different download length",
            });
        }
        break;
    }

    state_ = SessionState::Downloading;
    if (const auto write = write_exact_locked(bytes); !write) {
        return std::unexpected(write.error());
    }

    state_ = SessionState::AwaitingResponse;
    return read_terminal_locked(std::move(informational));
}

SessionState FastbootSession::state() const noexcept {
    std::scoped_lock lock(mutex_);
    return state_;
}

std::optional<ProtocolError> FastbootSession::poison_error() const {
    std::scoped_lock lock(mutex_);
    return poison_error_;
}

void FastbootSession::close() noexcept {
    std::scoped_lock lock(mutex_);
    if (state_ == SessionState::Closed) {
        return;
    }
    if (transport_) {
        transport_->close();
    }
    state_ = SessionState::Closed;
}

std::expected<void, ProtocolError> FastbootSession::begin_locked(
    const std::string_view command_text) {
    if (state_ != SessionState::Ready) {
        return std::unexpected(unavailable_error_locked());
    }
    if (!transport_) {
        return std::unexpected(ProtocolError{
            .code = ProtocolErrorCode::InvalidArgument,
            .message = "Fastboot session has no transport",
        });
    }
    if (command_text.empty() || command_text.size() > options_.max_command_bytes) {
        return std::unexpected(ProtocolError{
            .code = ProtocolErrorCode::InvalidArgument,
            .message = "Fastboot command is empty or exceeds the configured command limit",
        });
    }
    const auto invalid = std::ranges::find_if(command_text, [](const char value) {
        return value == '\0' || value == '\r' || value == '\n';
    });
    if (invalid != command_text.end()) {
        return std::unexpected(ProtocolError{
            .code = ProtocolErrorCode::InvalidArgument,
            .message = "Fastboot command contains a forbidden control character",
        });
    }
    return {};
}

std::expected<void, ProtocolError> FastbootSession::write_exact_locked(
    const std::span<const std::byte> bytes) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto remaining = bytes.subspan(offset);
        const auto result = transport_->write(remaining, options_.io_timeout);
        if (result.transferred > remaining.size()) {
            return poison_locked(ProtocolError{
                .code = ProtocolErrorCode::TransportContractViolation,
                .message = "transport reported writing more bytes than requested",
                .transport_status = result.status,
                .transfer_certainty = result.certainty,
            });
        }
        if (result.status != TransportStatus::Ok) {
            return poison_locked(transport_error_locked(result, "write"));
        }
        if (result.certainty != TransferCertainty::FullyTransferred) {
            return poison_locked(ProtocolError{
                .code = ProtocolErrorCode::TransportContractViolation,
                .message = "successful transport write has an uncertain transfer outcome",
                .transport_status = result.status,
                .transfer_certainty = result.certainty,
            });
        }
        if (result.truncated) {
            return poison_locked(ProtocolError{
                .code = ProtocolErrorCode::TransportContractViolation,
                .message = "transport marked a write result as truncated",
                .transport_status = result.status,
                .transfer_certainty = result.certainty,
            });
        }
        if (result.transferred == 0) {
            return poison_locked(ProtocolError{
                .code = ProtocolErrorCode::ZeroProgress,
                .message = "transport made no progress during a Fastboot write",
                .transport_status = result.status,
                .transfer_certainty = result.certainty,
            });
        }
        offset += result.transferred;
    }
    return {};
}

std::expected<Response, ProtocolError> FastbootSession::read_response_locked() {
    std::vector<std::byte> buffer(options_.max_response_bytes);
    const auto result = transport_->read(buffer, options_.io_timeout);
    if (result.status != TransportStatus::Ok) {
        return poison_locked(transport_error_locked(result, "read"));
    }
    if (result.certainty != TransferCertainty::FullyTransferred) {
        return poison_locked(ProtocolError{
            .code = ProtocolErrorCode::TransportContractViolation,
            .message = "successful transport read has an uncertain transfer outcome",
            .transport_status = result.status,
            .transfer_certainty = result.certainty,
        });
    }
    if (result.truncated || result.transferred > options_.max_response_bytes ||
        result.transferred > buffer.size()) {
        return poison_locked(ProtocolError{
            .code = ProtocolErrorCode::MalformedResponse,
            .message = "Fastboot response exceeded the configured response limit",
            .transport_status = result.status,
            .transfer_certainty = result.certainty,
        });
    }

    const auto parsed = parse_response(
        std::span<const std::byte>(buffer.data(), result.transferred),
        options_.max_response_bytes);
    if (!parsed) {
        return poison_locked(malformed_response_error(parsed.error()));
    }
    return *parsed;
}

std::expected<CommandResult, ProtocolError> FastbootSession::read_terminal_locked(
    std::vector<Response> informational) {
    for (;;) {
        auto response = read_response_locked();
        if (!response) {
            return std::unexpected(response.error());
        }

        if (response->kind == ResponseKind::Info || response->kind == ResponseKind::Text) {
            if (informational.size() >= options_.max_informational_responses) {
                return poison_locked(ProtocolError{
                    .code = ProtocolErrorCode::TooManyInformationalResponses,
                    .message = "Fastboot device exceeded the informational response limit",
                });
            }
            informational.push_back(std::move(*response));
            continue;
        }

        if (response->kind == ResponseKind::Okay || response->kind == ResponseKind::Fail) {
            state_ = SessionState::Ready;
            return CommandResult{std::move(*response), std::move(informational)};
        }

        return poison_locked(ProtocolError{
            .code = ProtocolErrorCode::UnexpectedResponse,
            .message = "Fastboot DATA response is not valid in this session state",
        });
    }
}

ProtocolError FastbootSession::transport_error_locked(
    const TransferResult& result,
    const std::string_view operation) {
    ProtocolErrorCode code = ProtocolErrorCode::TransportIo;
    switch (result.status) {
        case TransportStatus::Timeout:
            code = ProtocolErrorCode::TransportTimeout;
            break;
        case TransportStatus::Disconnected:
            code = ProtocolErrorCode::TransportDisconnected;
            break;
        case TransportStatus::IoError:
        case TransportStatus::Ok:
            break;
    }

    std::string message = "Fastboot transport ";
    message.append(operation);
    message.append(" failed");
    if (result.certainty == TransferCertainty::PartialOrUnknown) {
        message.append(" with a partial or unknown transfer outcome");
    }
    if (!result.detail.empty()) {
        message.append(": ");
        message.append(result.detail);
    }
    return {
        .code = code,
        .message = std::move(message),
        .transport_status = result.status,
        .transfer_certainty = result.certainty,
    };
}

std::unexpected<ProtocolError> FastbootSession::poison_locked(ProtocolError error) {
    state_ = SessionState::Poisoned;
    poison_error_ = error;
    return std::unexpected(std::move(error));
}

ProtocolError FastbootSession::unavailable_error_locked() const {
    if (state_ == SessionState::Closed) {
        return {
            .code = ProtocolErrorCode::Closed,
            .message = "Fastboot session is closed",
        };
    }
    if (state_ == SessionState::Poisoned) {
        auto error = poison_error_.value_or(ProtocolError{
            .code = ProtocolErrorCode::Poisoned,
            .message = "Fastboot session is poisoned",
        });
        error.code = ProtocolErrorCode::Poisoned;
        error.message = "Fastboot session is poisoned: " + error.message;
        return error;
    }
    return {
        .code = ProtocolErrorCode::Busy,
        .message = "Fastboot session is busy",
    };
}

}  // namespace kairosboot::protocol
