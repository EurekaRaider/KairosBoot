// SPDX-License-Identifier: MIT
#pragma once

#include "transport_session.hpp"

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
    Poisoned,
    Closed,
};

enum class ProtocolErrorCode : std::uint8_t {
    InvalidArgument,
    Busy,
    Closed,
    Poisoned,
    TransportTimeout,
    TransportDisconnected,
    TransportIo,
    TransportContractViolation,
    ZeroProgress,
    MalformedResponse,
    UnexpectedResponse,
    DataLengthMismatch,
    TooManyInformationalResponses,
};

struct ProtocolError {
    ProtocolErrorCode code;
    std::string message;
    TransportStatus transport_status{TransportStatus::Ok};
    TransferCertainty transfer_certainty{TransferCertainty::FullyTransferred};
};

struct CommandResult {
    Response terminal;
    std::vector<Response> informational;

    [[nodiscard]] bool succeeded() const noexcept {
        return terminal.kind == ResponseKind::Okay;
    }
};

struct SessionOptions {
    std::chrono::milliseconds io_timeout{30'000};
    std::size_t max_command_bytes{kDefaultMaxCommandBytes};
    std::size_t max_response_bytes{kDefaultMaxResponseBytes};
    std::size_t max_informational_responses{kDefaultMaxInformationalResponses};
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

    [[nodiscard]] SessionState state() const noexcept;
    [[nodiscard]] std::optional<ProtocolError> poison_error() const;
    void close() noexcept;

private:
    [[nodiscard]] std::expected<void, ProtocolError> begin_locked(
        std::string_view command);
    [[nodiscard]] std::expected<void, ProtocolError> write_exact_locked(
        std::span<const std::byte> bytes);
    [[nodiscard]] std::expected<Response, ProtocolError> read_response_locked();
    [[nodiscard]] std::expected<CommandResult, ProtocolError> read_terminal_locked(
        std::vector<Response> informational);
    [[nodiscard]] ProtocolError transport_error_locked(
        const TransferResult& result,
        std::string_view operation);
    [[nodiscard]] std::unexpected<ProtocolError> poison_locked(ProtocolError error);
    [[nodiscard]] ProtocolError unavailable_error_locked() const;

    std::unique_ptr<ITransportSession> transport_;
    SessionOptions options_;
    mutable std::mutex mutex_;
    SessionState state_{SessionState::Ready};
    std::optional<ProtocolError> poison_error_;
};

}  // namespace kairosboot::protocol
