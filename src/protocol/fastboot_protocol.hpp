// SPDX-License-Identifier: MIT
#pragma once

#include "transport_session.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace kairosboot::protocol {

inline constexpr std::size_t kDefaultMaxCommandBytes = 4096;
inline constexpr std::size_t kDefaultMaxResponseBytes = 256;
inline constexpr std::size_t kDefaultMaxInformationalResponses = 1024;
inline constexpr std::size_t kDefaultReceiveChunkBytes = 1024U * 1024U;
inline constexpr std::size_t kMaximumReceiveChunkBytes = 4U * 1024U * 1024U;

enum class ResponseKind : std::uint8_t {
    Info,
    Text,
    Okay,
    Fail,
    Data,
};

struct Response {
    ResponseKind kind;
    std::string payload;
    std::optional<std::uint32_t> data_size;
};

enum class ResponseParseErrorCode : std::uint8_t {
    ZeroLength,
    TooShort,
    TooLong,
    UnknownPrefix,
    InvalidDataLength,
    InvalidDataHex,
};

struct ResponseParseError {
    ResponseParseErrorCode code;
    std::string message;
};

[[nodiscard]] std::expected<Response, ResponseParseError> parse_response(
    std::span<const std::byte> packet,
    std::size_t max_response_bytes = kDefaultMaxResponseBytes);

enum class SessionState : std::uint8_t {
    Ready,
    WritingCommand,
    AwaitingResponse,
    Downloading,
    ReceivingData,
    Poisoned,
    Closed,
};

enum class ProtocolErrorCode : std::uint8_t {
    InvalidArgument,
    StreamingUnsupported,
    Busy,
    Closed,
    Poisoned,
    TransportTimeout,
    TransportCancelled,
    TransportDisconnected,
    TransportIo,
    TransportContractViolation,
    ZeroProgress,
    MalformedResponse,
    UnexpectedResponse,
    DataLengthMismatch,
    TooManyInformationalResponses,
};

enum class ProtocolPhase : std::uint8_t {
    Validation,
    CommandWrite,
    InitialResponse,
    DataWrite,
    DataRead,
    FinalResponse,
};

struct ProtocolError {
    ProtocolErrorCode code;
    std::string message;
    // Informational responses successfully received before the terminal error.
    // The session's configured informational-response limit bounds this list.
    std::vector<Response> informational;
    TransportStatus transport_status{TransportStatus::Ok};
    // Certainty reported by the individual transport call that failed.
    TransferCertainty transfer_certainty{TransferCertainty::FullyTransferred};
    ProtocolPhase phase{ProtocolPhase::Validation};
    // Certainty for the outbound unit preceding phase. During a response read,
    // this is FullyTransferred even when no response bytes were received.
    TransferCertainty outbound_certainty{TransferCertainty::NotTransferred};
    // Device-to-host payload state. inbound_expected is unset for ordinary
    // command/download operations and set after accepting a DATA response;
    // inbound_transferred counts bytes committed to the caller's sink.
    std::optional<std::uint64_t> inbound_expected;
    std::uint64_t inbound_transferred{0};
    TransferCertainty inbound_certainty{TransferCertainty::NotTransferred};
    int native_code{0};
};

struct CommandResult {
    Response terminal;
    std::vector<Response> informational;
    ProtocolPhase phase{ProtocolPhase::FinalResponse};
    TransferCertainty outbound_certainty{TransferCertainty::FullyTransferred};
    std::optional<std::uint64_t> inbound_expected;
    std::uint64_t inbound_transferred{0};
    TransferCertainty inbound_certainty{TransferCertainty::NotTransferred};

    [[nodiscard]] bool succeeded() const noexcept {
        return terminal.kind == ResponseKind::Okay;
    }
};

struct SessionOptions {
    std::chrono::milliseconds io_timeout{30'000};
    std::size_t max_command_bytes{kDefaultMaxCommandBytes};
    std::size_t max_response_bytes{kDefaultMaxResponseBytes};
    std::size_t max_informational_responses{kDefaultMaxInformationalResponses};
    std::size_t receive_chunk_bytes{kDefaultReceiveChunkBytes};
};

// A synchronous protocol state machine. Higher layers may place it behind an
// operation/actor, but may not issue overlapping Fastboot commands on one
// session. Any ambiguous transport outcome or malformed wire response poisons
// the session; a valid device FAIL response does not.
class FastbootSession final {
public:
    explicit FastbootSession(
        std::unique_ptr<ITransportSession> transport,
        SessionOptions options = {});
    ~FastbootSession();

    FastbootSession(const FastbootSession&) = delete;
    FastbootSession& operator=(const FastbootSession&) = delete;
    FastbootSession(FastbootSession&&) = delete;
    FastbootSession& operator=(FastbootSession&&) = delete;

    [[nodiscard]] std::expected<CommandResult, ProtocolError> command(
        std::string_view command);

    [[nodiscard]] std::expected<CommandResult, ProtocolError> download(
        std::span<const std::byte> bytes);

    [[nodiscard]] std::expected<CommandResult, ProtocolError> download_source(
        std::shared_ptr<ITransferSource> source,
        const TransferProgressObserver& observer = {});

    [[nodiscard]] std::expected<CommandResult, ProtocolError> receive_to_sink(
        std::string_view command,
        std::shared_ptr<ITransferSink> sink,
        std::uint64_t maximum_bytes,
        const TransferProgressObserver& observer = {});

    [[nodiscard]] SessionState state() const noexcept;
    [[nodiscard]] std::optional<ProtocolError> poison_error() const;
    void request_cancel() noexcept;
    void close() noexcept;

private:
    [[nodiscard]] std::expected<void, ProtocolError> begin_locked(
        std::string_view command);
    [[nodiscard]] std::expected<void, ProtocolError> write_exact_locked(
        std::span<const std::byte> bytes,
        ProtocolPhase phase);
    [[nodiscard]] std::expected<void, ProtocolError> write_source_locked(
        IStreamingTransportSession& streaming,
        std::shared_ptr<ITransferSource> source,
        std::uint32_t size,
        const TransferProgressObserver& observer);
    [[nodiscard]] std::expected<CommandResult, ProtocolError> download_locked(
        std::uint32_t size,
        std::span<const std::byte> bytes,
        std::shared_ptr<ITransferSource> source,
        const TransferProgressObserver& observer);
    [[nodiscard]] std::expected<CommandResult, ProtocolError> receive_locked(
        std::string_view command,
        std::shared_ptr<ITransferSink> sink,
        std::uint64_t maximum_bytes,
        const TransferProgressObserver& observer);
    [[nodiscard]] std::expected<Response, ProtocolError> read_response_locked(
        ProtocolPhase phase,
        TransferCertainty outbound_certainty,
        const std::vector<Response>& informational);
    [[nodiscard]] std::expected<CommandResult, ProtocolError> read_terminal_locked(
        std::vector<Response> informational,
        ProtocolPhase phase,
        TransferCertainty outbound_certainty);
    [[nodiscard]] ProtocolError transport_error_locked(
        const TransferResult& result,
        std::string_view operation,
        ProtocolPhase phase,
        TransferCertainty outbound_certainty);
    [[nodiscard]] std::unexpected<ProtocolError> poison_locked(ProtocolError error);
    [[nodiscard]] ProtocolError unavailable_error_locked() const;

    std::unique_ptr<ITransportSession> transport_;
    SessionOptions options_;
    mutable std::mutex mutex_;
    std::atomic<bool> cancellation_requested_{false};
    SessionState state_{SessionState::Ready};
    std::optional<ProtocolError> poison_error_;
};

}  // namespace kairosboot::protocol
