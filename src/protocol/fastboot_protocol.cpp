// SPDX-License-Identifier: MIT
#include "fastboot_protocol.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdio>
#include <limits>
#include <new>
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

[[nodiscard]] ProtocolError malformed_response_error(
    const ResponseParseError& error,
    const ProtocolPhase phase,
    const TransferCertainty outbound_certainty) {
    return {
        .code = ProtocolErrorCode::MalformedResponse,
        .message = error.message,
        .phase = phase,
        .outbound_certainty = outbound_certainty,
    };
}

[[nodiscard]] TransferCertainty aggregate_write_certainty(
    const std::size_t completed,
    const std::size_t requested,
    const TransferResult& result) noexcept {
    if (result.certainty == TransferCertainty::FullyTransferred &&
        result.transferred == requested) {
        return TransferCertainty::FullyTransferred;
    }
    if (completed == 0 && result.transferred == 0 &&
        result.certainty == TransferCertainty::NotTransferred) {
        return TransferCertainty::NotTransferred;
    }
    return TransferCertainty::PartialOrUnknown;
}

[[nodiscard]] ProtocolError with_informational(
    ProtocolError error,
    const std::vector<Response>& informational) {
    error.informational = informational;
    return error;
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
    options_.receive_chunk_bytes = std::clamp(
        options_.receive_chunk_bytes,
        std::size_t{1},
        kMaximumReceiveChunkBytes);
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
    if (const auto write = write_exact_locked(command_bytes, ProtocolPhase::CommandWrite);
        !write) {
        return std::unexpected(write.error());
    }

    state_ = SessionState::AwaitingResponse;
    return read_terminal_locked(
        {}, ProtocolPhase::FinalResponse, TransferCertainty::FullyTransferred);
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

    return download_locked(
        static_cast<std::uint32_t>(bytes.size()), bytes, nullptr, {});
}

std::expected<CommandResult, ProtocolError> FastbootSession::download_source(
    std::shared_ptr<ITransferSource> source,
    const TransferProgressObserver& observer) {
    std::unique_lock lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock()) {
        return std::unexpected(ProtocolError{
            .code = ProtocolErrorCode::Busy,
            .message = "another Fastboot operation is already using this session",
        });
    }
    if (source == nullptr) {
        return std::unexpected(ProtocolError{
            .code = ProtocolErrorCode::InvalidArgument,
            .message = "Fastboot download source is null",
        });
    }

    const auto size = source->size();
    if (size > std::numeric_limits<std::uint32_t>::max()) {
        return std::unexpected(ProtocolError{
            .code = ProtocolErrorCode::InvalidArgument,
            .message = "Fastboot download size exceeds the protocol's 32-bit limit",
        });
    }

    return download_locked(
        static_cast<std::uint32_t>(size), {}, std::move(source), observer);
}

std::expected<CommandResult, ProtocolError> FastbootSession::receive_to_sink(
    const std::string_view command_text,
    std::shared_ptr<ITransferSink> sink,
    const std::uint64_t maximum_bytes,
    const TransferProgressObserver& observer) {
    std::unique_lock lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock()) {
        return std::unexpected(ProtocolError{
            .code = ProtocolErrorCode::Busy,
            .message = "another Fastboot operation is already using this session",
        });
    }
    if (sink == nullptr) {
        return std::unexpected(ProtocolError{
            .code = ProtocolErrorCode::InvalidArgument,
            .message = "Fastboot receive sink is null",
        });
    }
    if (maximum_bytes == 0) {
        return std::unexpected(ProtocolError{
            .code = ProtocolErrorCode::InvalidArgument,
            .message = "Fastboot receive maximum must not be zero",
        });
    }
    return receive_locked(
        command_text, std::move(sink), maximum_bytes, observer);
}

std::expected<CommandResult, ProtocolError> FastbootSession::receive_locked(
    const std::string_view command_text,
    std::shared_ptr<ITransferSink> sink,
    const std::uint64_t maximum_bytes,
    const TransferProgressObserver& observer) {
    if (const auto begin = begin_locked(command_text); !begin) {
        return std::unexpected(begin.error());
    }

    state_ = SessionState::WritingCommand;
    if (const auto write = write_exact_locked(
            std::as_bytes(std::span(command_text)), ProtocolPhase::CommandWrite);
        !write) {
        return std::unexpected(write.error());
    }

    state_ = SessionState::AwaitingResponse;
    std::vector<Response> informational;
    std::uint32_t announced_size = 0;
    for (;;) {
        auto response = read_response_locked(
            ProtocolPhase::InitialResponse,
            TransferCertainty::FullyTransferred,
            informational);
        if (!response) {
            return std::unexpected(response.error());
        }

        if (response->kind == ResponseKind::Info || response->kind == ResponseKind::Text) {
            if (informational.size() >= options_.max_informational_responses) {
                return poison_locked(ProtocolError{
                    .code = ProtocolErrorCode::TooManyInformationalResponses,
                    .message = "Fastboot device exceeded the informational response limit",
                    .informational = informational,
                    .phase = ProtocolPhase::InitialResponse,
                    .outbound_certainty = TransferCertainty::FullyTransferred,
                });
            }
            informational.push_back(std::move(*response));
            continue;
        }

        if (response->kind == ResponseKind::Fail) {
            state_ = SessionState::Ready;
            return CommandResult{
                .terminal = std::move(*response),
                .informational = std::move(informational),
                .phase = ProtocolPhase::InitialResponse,
                .outbound_certainty = TransferCertainty::FullyTransferred,
            };
        }

        if (response->kind == ResponseKind::Okay) {
            state_ = SessionState::Ready;
            return std::unexpected(ProtocolError{
                .code = ProtocolErrorCode::UnexpectedResponse,
                .message = "Fastboot receive command completed without a DATA response",
                .informational = informational,
                .phase = ProtocolPhase::InitialResponse,
                .outbound_certainty = TransferCertainty::FullyTransferred,
            });
        }

        if (response->kind != ResponseKind::Data || !response->data_size) {
            return poison_locked(ProtocolError{
                .code = ProtocolErrorCode::UnexpectedResponse,
                .message = "Fastboot receive expected DATA or FAIL before the payload",
                .informational = informational,
                .phase = ProtocolPhase::InitialResponse,
                .outbound_certainty = TransferCertainty::FullyTransferred,
            });
        }

        announced_size = *response->data_size;
        if (announced_size == 0 || announced_size > maximum_bytes) {
            transport_->request_cancel();
            transport_->close();
            return poison_locked(ProtocolError{
                .code = announced_size == 0
                    ? ProtocolErrorCode::UnexpectedResponse
                    : ProtocolErrorCode::DataLengthMismatch,
                .message = announced_size == 0
                    ? "Fastboot receive DATA size must not be zero"
                    : "Fastboot receive DATA size exceeds the caller's limit",
                .informational = informational,
                .phase = ProtocolPhase::InitialResponse,
                .outbound_certainty = TransferCertainty::FullyTransferred,
                .inbound_expected = announced_size,
                .inbound_transferred = 0,
                .inbound_certainty = announced_size == 0
                    ? TransferCertainty::NotTransferred
                    : TransferCertainty::PartialOrUnknown,
            });
        }
        break;
    }

    std::vector<std::byte> buffer;
    try {
        buffer.resize(std::min<std::size_t>(
            options_.receive_chunk_bytes,
            static_cast<std::size_t>(announced_size)));
    } catch (const std::bad_alloc&) {
        transport_->request_cancel();
        transport_->close();
        return poison_locked(ProtocolError{
            .code = ProtocolErrorCode::TransportIo,
            .message = "allocating the bounded Fastboot receive buffer failed",
            .informational = informational,
            .transport_status = TransportStatus::IoError,
            .phase = ProtocolPhase::DataRead,
            .outbound_certainty = TransferCertainty::FullyTransferred,
            .inbound_expected = announced_size,
            .inbound_transferred = 0,
            .inbound_certainty = TransferCertainty::PartialOrUnknown,
        });
    }

    const auto receive_failure = [this, announced_size, &informational](
                                     ProtocolError error,
                                     const std::uint64_t committed,
                                     const bool partial_or_unknown) {
        error.informational = informational;
        error.inbound_expected = announced_size;
        error.inbound_transferred = committed;
        if (committed == announced_size && !partial_or_unknown) {
            error.inbound_certainty = TransferCertainty::FullyTransferred;
        } else if (committed == 0 && !partial_or_unknown) {
            error.inbound_certainty = TransferCertainty::NotTransferred;
        } else {
            error.inbound_certainty = TransferCertainty::PartialOrUnknown;
        }
        transport_->request_cancel();
        transport_->close();
        return poison_locked(std::move(error));
    };
    const auto phase_failure = [&receive_failure](
                                   const ProtocolErrorCode code,
                                   const std::string_view message,
                                   const TransferResult& io,
                                   const std::uint64_t committed,
                                   const bool partial_or_unknown) {
        return receive_failure(
            ProtocolError{
                .code = code,
                .message = std::string(message),
                .transport_status = io.status,
                .transfer_certainty = io.certainty,
                .phase = ProtocolPhase::DataRead,
                .outbound_certainty = TransferCertainty::FullyTransferred,
                .native_code = io.native_code,
            },
            committed,
            partial_or_unknown);
    };

    state_ = SessionState::ReceivingData;
    std::uint64_t committed = 0;
    while (committed < announced_size) {
        const auto remaining = static_cast<std::size_t>(announced_size - committed);
        const auto requested = std::min(buffer.size(), remaining);
        auto transfer = transport_->read_data(
            std::span(buffer).first(requested), options_.io_timeout);
        if (transfer.transferred > requested) {
            return phase_failure(
                ProtocolErrorCode::TransportContractViolation,
                "transport reported receiving more Fastboot data than requested",
                transfer,
                committed,
                true);
        }
        if (transfer.status != TransportStatus::Ok) {
            auto error = transport_error_locked(
                transfer,
                "data read",
                ProtocolPhase::DataRead,
                TransferCertainty::FullyTransferred);
            return receive_failure(
                std::move(error),
                committed,
                transfer.transferred != 0 ||
                    transfer.certainty == TransferCertainty::PartialOrUnknown);
        }
        if (transfer.certainty != TransferCertainty::FullyTransferred ||
            transfer.truncated) {
            return phase_failure(
                ProtocolErrorCode::TransportContractViolation,
                transfer.truncated
                    ? "Fastboot inbound data exceeded the receive chunk"
                    : "successful Fastboot data read has uncertain transfer outcome",
                transfer,
                committed,
                true);
        }
        if (transfer.transferred == 0) {
            return phase_failure(
                ProtocolErrorCode::ZeroProgress,
                "transport made no progress while receiving Fastboot data",
                transfer,
                committed,
                false);
        }

        const auto payload = std::span<const std::byte>(
            buffer.data(), transfer.transferred);
        auto stored = sink->write(committed, payload);
        if (stored.transferred > payload.size()) {
            return phase_failure(
                ProtocolErrorCode::TransportContractViolation,
                "Fastboot receive sink reported storing more bytes than provided",
                stored,
                committed,
                true);
        }
        if (stored.status != TransportStatus::Ok) {
            auto error = transport_error_locked(
                stored,
                "receive sink write",
                ProtocolPhase::DataRead,
                TransferCertainty::FullyTransferred);
            return receive_failure(
                std::move(error),
                committed + stored.transferred,
                stored.transferred != payload.size() ||
                    stored.certainty != TransferCertainty::FullyTransferred);
        }
        if (stored.certainty != TransferCertainty::FullyTransferred ||
            stored.truncated || stored.transferred != payload.size()) {
            return phase_failure(
                ProtocolErrorCode::TransportContractViolation,
                "Fastboot receive sink did not commit the complete data chunk",
                stored,
                committed + stored.transferred,
                true);
        }

        committed += transfer.transferred;
        if (observer) {
            auto action = TransferProgressAction::continue_transfer;
            try {
                action = observer(committed, announced_size);
            } catch (...) {
                return phase_failure(
                    ProtocolErrorCode::TransportIo,
                    "Fastboot receive progress observer failed",
                    TransferResult{
                        .status = TransportStatus::IoError,
                        .certainty = TransferCertainty::NotTransferred,
                    },
                    committed,
                    committed != announced_size);
            }
            if (action == TransferProgressAction::cancel) {
                return phase_failure(
                    ProtocolErrorCode::TransportCancelled,
                    "Fastboot receive was cancelled by progress observer",
                    TransferResult{
                        .status = TransportStatus::Cancelled,
                        .certainty = TransferCertainty::NotTransferred,
                    },
                    committed,
                    committed != announced_size);
            }
        }
    }

    state_ = SessionState::AwaitingResponse;
    auto terminal = read_terminal_locked(
        std::move(informational),
        ProtocolPhase::FinalResponse,
        TransferCertainty::FullyTransferred);
    if (!terminal) {
        auto error = std::move(terminal.error());
        error.inbound_expected = announced_size;
        error.inbound_transferred = committed;
        error.inbound_certainty = TransferCertainty::FullyTransferred;
        return std::unexpected(std::move(error));
    }
    terminal->inbound_expected = announced_size;
    terminal->inbound_transferred = committed;
    terminal->inbound_certainty = TransferCertainty::FullyTransferred;
    return terminal;
}

std::expected<CommandResult, ProtocolError> FastbootSession::download_locked(
    const std::uint32_t size,
    const std::span<const std::byte> bytes,
    std::shared_ptr<ITransferSource> source,
    const TransferProgressObserver& observer) {
    std::array<char, 18> command_buffer{};
    const auto command_length = std::snprintf(
        command_buffer.data(),
        command_buffer.size(),
        "download:%08x",
        static_cast<unsigned int>(size));
    const std::string_view command_text(
        command_buffer.data(), static_cast<std::size_t>(command_length));

    if (const auto begin = begin_locked(command_text); !begin) {
        return std::unexpected(begin.error());
    }

    auto* streaming = source == nullptr
        ? nullptr
        : dynamic_cast<IStreamingTransportSession*>(transport_.get());
    if (source != nullptr && streaming == nullptr) {
        return std::unexpected(ProtocolError{
            .code = ProtocolErrorCode::StreamingUnsupported,
            .message = "Fastboot transport does not support source streaming",
        });
    }

    state_ = SessionState::WritingCommand;
    if (const auto write = write_exact_locked(
            std::as_bytes(std::span(command_text)), ProtocolPhase::CommandWrite);
        !write) {
        return std::unexpected(write.error());
    }

    state_ = SessionState::AwaitingResponse;
    std::vector<Response> informational;
    for (;;) {
        auto response = read_response_locked(
            ProtocolPhase::InitialResponse,
            TransferCertainty::FullyTransferred,
            informational);
        if (!response) {
            return std::unexpected(response.error());
        }

        if (response->kind == ResponseKind::Info || response->kind == ResponseKind::Text) {
            if (informational.size() >= options_.max_informational_responses) {
                return poison_locked(ProtocolError{
                    .code = ProtocolErrorCode::TooManyInformationalResponses,
                    .message = "Fastboot device exceeded the informational response limit",
                    .informational = informational,
                    .phase = ProtocolPhase::InitialResponse,
                    .outbound_certainty = TransferCertainty::FullyTransferred,
                });
            }
            informational.push_back(std::move(*response));
            continue;
        }

        if (response->kind == ResponseKind::Fail) {
            state_ = SessionState::Ready;
            return CommandResult{
                .terminal = std::move(*response),
                .informational = std::move(informational),
                .phase = ProtocolPhase::InitialResponse,
                .outbound_certainty = TransferCertainty::FullyTransferred,
            };
        }

        if (response->kind != ResponseKind::Data) {
            return poison_locked(ProtocolError{
                .code = ProtocolErrorCode::UnexpectedResponse,
                .message = "Fastboot download expected DATA or FAIL before sending payload",
                .informational = informational,
                .phase = ProtocolPhase::InitialResponse,
                .outbound_certainty = TransferCertainty::FullyTransferred,
            });
        }

        if (!response->data_size || *response->data_size != size) {
            return poison_locked(ProtocolError{
                .code = ProtocolErrorCode::DataLengthMismatch,
                .message = "Fastboot device accepted a different download length",
                .informational = informational,
                .phase = ProtocolPhase::InitialResponse,
                .outbound_certainty = TransferCertainty::FullyTransferred,
            });
        }
        break;
    }

    state_ = SessionState::Downloading;
    if (source != nullptr) {
        if (const auto write = write_source_locked(
                *streaming, std::move(source), size, observer);
            !write) {
            return poison_locked(with_informational(
                std::move(write.error()), informational));
        }
    } else {
        if (const auto write = write_exact_locked(bytes, ProtocolPhase::DataWrite);
            !write) {
            return poison_locked(with_informational(
                std::move(write.error()), informational));
        }
    }

    state_ = SessionState::AwaitingResponse;
    return read_terminal_locked(
        std::move(informational),
        ProtocolPhase::FinalResponse,
        TransferCertainty::FullyTransferred);
}

SessionState FastbootSession::state() const noexcept {
    std::scoped_lock lock(mutex_);
    return state_;
}

std::optional<ProtocolError> FastbootSession::poison_error() const {
    std::scoped_lock lock(mutex_);
    return poison_error_;
}

void FastbootSession::request_cancel() noexcept {
    cancellation_requested_.store(true, std::memory_order_release);
    if (transport_) {
        transport_->request_cancel();
    }
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
    if (cancellation_requested_.load(std::memory_order_acquire)) {
        return poison_locked(ProtocolError{
            .code = ProtocolErrorCode::TransportCancelled,
            .message = "Fastboot operation was cancelled",
            .transport_status = TransportStatus::Cancelled,
            .transfer_certainty = TransferCertainty::NotTransferred,
            .phase = ProtocolPhase::Validation,
            .outbound_certainty = TransferCertainty::NotTransferred,
        });
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
    const std::span<const std::byte> bytes,
    const ProtocolPhase phase) {
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
                .phase = phase,
                .outbound_certainty = aggregate_write_certainty(
                    offset, remaining.size(), result),
                .native_code = result.native_code,
            });
        }
        if (result.status != TransportStatus::Ok) {
            return poison_locked(transport_error_locked(
                result,
                "write",
                phase,
                aggregate_write_certainty(offset, remaining.size(), result)));
        }
        if (result.certainty != TransferCertainty::FullyTransferred) {
            return poison_locked(ProtocolError{
                .code = ProtocolErrorCode::TransportContractViolation,
                .message = "successful transport write has an uncertain transfer outcome",
                .transport_status = result.status,
                .transfer_certainty = result.certainty,
                .phase = phase,
                .outbound_certainty = aggregate_write_certainty(
                    offset, remaining.size(), result),
                .native_code = result.native_code,
            });
        }
        if (result.truncated) {
            return poison_locked(ProtocolError{
                .code = ProtocolErrorCode::TransportContractViolation,
                .message = "transport marked a write result as truncated",
                .transport_status = result.status,
                .transfer_certainty = result.certainty,
                .phase = phase,
                .outbound_certainty = aggregate_write_certainty(
                    offset, remaining.size(), result),
                .native_code = result.native_code,
            });
        }
        if (result.transferred == 0) {
            return poison_locked(ProtocolError{
                .code = ProtocolErrorCode::ZeroProgress,
                .message = "transport made no progress during a Fastboot write",
                .transport_status = result.status,
                .transfer_certainty = result.certainty,
                .phase = phase,
                .outbound_certainty = offset == 0
                    ? TransferCertainty::NotTransferred
                    : TransferCertainty::PartialOrUnknown,
                .native_code = result.native_code,
            });
        }
        offset += result.transferred;
    }
    return {};
}

std::expected<void, ProtocolError> FastbootSession::write_source_locked(
    IStreamingTransportSession& streaming,
    std::shared_ptr<ITransferSource> source,
    const std::uint32_t size,
    const TransferProgressObserver& observer) {
    const auto result = streaming.write_source(
        std::move(source), options_.io_timeout, observer);
    const auto expected = static_cast<std::size_t>(size);
    const auto outbound_certainty = aggregate_write_certainty(
        0, expected, result);

    if (result.transferred > expected) {
        return poison_locked(ProtocolError{
            .code = ProtocolErrorCode::TransportContractViolation,
            .message = "streaming transport reported writing more bytes than requested",
            .transport_status = result.status,
            .transfer_certainty = result.certainty,
            .phase = ProtocolPhase::DataWrite,
            .outbound_certainty = outbound_certainty,
            .native_code = result.native_code,
        });
    }
    if (result.status != TransportStatus::Ok) {
        return poison_locked(transport_error_locked(
            result,
            "source write",
            ProtocolPhase::DataWrite,
            outbound_certainty));
    }
    if (result.certainty != TransferCertainty::FullyTransferred) {
        return poison_locked(ProtocolError{
            .code = ProtocolErrorCode::TransportContractViolation,
            .message = "successful source write has an uncertain transfer outcome",
            .transport_status = result.status,
            .transfer_certainty = result.certainty,
            .phase = ProtocolPhase::DataWrite,
            .outbound_certainty = outbound_certainty,
            .native_code = result.native_code,
        });
    }
    if (result.truncated) {
        return poison_locked(ProtocolError{
            .code = ProtocolErrorCode::TransportContractViolation,
            .message = "streaming transport marked a source write as truncated",
            .transport_status = result.status,
            .transfer_certainty = result.certainty,
            .phase = ProtocolPhase::DataWrite,
            .outbound_certainty = outbound_certainty,
            .native_code = result.native_code,
        });
    }
    if (result.transferred != expected) {
        return poison_locked(ProtocolError{
            .code = result.transferred == 0
                ? ProtocolErrorCode::ZeroProgress
                : ProtocolErrorCode::TransportContractViolation,
            .message = "streaming transport did not write the complete source",
            .transport_status = result.status,
            .transfer_certainty = result.certainty,
            .phase = ProtocolPhase::DataWrite,
            .outbound_certainty = outbound_certainty,
            .native_code = result.native_code,
        });
    }
    return {};
}

std::expected<Response, ProtocolError> FastbootSession::read_response_locked(
    const ProtocolPhase phase,
    const TransferCertainty outbound_certainty,
    const std::vector<Response>& informational) {
    std::vector<std::byte> buffer(options_.max_response_bytes);
    const auto result = transport_->read(buffer, options_.io_timeout);
    if (result.status != TransportStatus::Ok) {
        return poison_locked(with_informational(
            transport_error_locked(result, "read", phase, outbound_certainty),
            informational));
    }
    if (result.certainty != TransferCertainty::FullyTransferred) {
        return poison_locked(ProtocolError{
            .code = ProtocolErrorCode::TransportContractViolation,
            .message = "successful transport read has an uncertain transfer outcome",
            .informational = informational,
            .transport_status = result.status,
            .transfer_certainty = result.certainty,
            .phase = phase,
            .outbound_certainty = outbound_certainty,
            .native_code = result.native_code,
        });
    }
    if (result.truncated || result.transferred > options_.max_response_bytes ||
        result.transferred > buffer.size()) {
        return poison_locked(ProtocolError{
            .code = ProtocolErrorCode::MalformedResponse,
            .message = "Fastboot response exceeded the configured response limit",
            .informational = informational,
            .transport_status = result.status,
            .transfer_certainty = result.certainty,
            .phase = phase,
            .outbound_certainty = outbound_certainty,
            .native_code = result.native_code,
        });
    }

    const auto parsed = parse_response(
        std::span<const std::byte>(buffer.data(), result.transferred),
        options_.max_response_bytes);
    if (!parsed) {
        return poison_locked(with_informational(
            malformed_response_error(parsed.error(), phase, outbound_certainty),
            informational));
    }
    return *parsed;
}

std::expected<CommandResult, ProtocolError> FastbootSession::read_terminal_locked(
    std::vector<Response> informational,
    const ProtocolPhase phase,
    const TransferCertainty outbound_certainty) {
    for (;;) {
        auto response = read_response_locked(
            phase, outbound_certainty, informational);
        if (!response) {
            return std::unexpected(response.error());
        }

        if (response->kind == ResponseKind::Info || response->kind == ResponseKind::Text) {
            if (informational.size() >= options_.max_informational_responses) {
                return poison_locked(ProtocolError{
                    .code = ProtocolErrorCode::TooManyInformationalResponses,
                    .message = "Fastboot device exceeded the informational response limit",
                    .informational = informational,
                    .phase = phase,
                    .outbound_certainty = outbound_certainty,
                });
            }
            informational.push_back(std::move(*response));
            continue;
        }

        if (response->kind == ResponseKind::Okay || response->kind == ResponseKind::Fail) {
            state_ = SessionState::Ready;
            return CommandResult{
                .terminal = std::move(*response),
                .informational = std::move(informational),
                .phase = phase,
                .outbound_certainty = outbound_certainty,
            };
        }

        return poison_locked(ProtocolError{
            .code = ProtocolErrorCode::UnexpectedResponse,
            .message = "Fastboot DATA response is not valid in this session state",
            .informational = informational,
            .phase = phase,
            .outbound_certainty = outbound_certainty,
        });
    }
}

ProtocolError FastbootSession::transport_error_locked(
    const TransferResult& result,
    const std::string_view operation,
    const ProtocolPhase phase,
    const TransferCertainty outbound_certainty) {
    ProtocolErrorCode code = ProtocolErrorCode::TransportIo;
    switch (result.status) {
        case TransportStatus::Timeout:
            code = ProtocolErrorCode::TransportTimeout;
            break;
        case TransportStatus::Cancelled:
            code = ProtocolErrorCode::TransportCancelled;
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
        .phase = phase,
        .outbound_certainty = outbound_certainty,
        .native_code = result.native_code,
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
