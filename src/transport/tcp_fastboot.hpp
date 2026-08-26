// SPDX-License-Identifier: MIT
#pragma once

#include "../protocol/transport_session.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>

namespace kairosboot::transport {

inline constexpr std::uint16_t kFastbootTcpDefaultPort = 5554;
inline constexpr std::array<std::byte, 4> kFastbootTcpV1Handshake{
    std::byte{'F'}, std::byte{'B'}, std::byte{'0'}, std::byte{'1'},
};
inline constexpr std::uint64_t kDefaultMaxTcpFrameBytes = 0xFFFFFFFFULL;

enum class TcpErrorKind : std::uint8_t {
    InvalidEndpoint,
    ResolveFailed,
    ConnectFailed,
    HandshakeFailed,
    Timeout,
    Cancelled,
    Disconnected,
    Io,
};

struct TcpError {
    TcpErrorKind kind;
    int native_error{0};
    std::string message;
};

struct TcpEndpoint {
    std::string host;
    std::uint16_t port{kFastbootTcpDefaultPort};
};

namespace detail {

using ConnectClock = std::chrono::steady_clock;
using ConnectClockNow = ConnectClock::time_point (*)(void*) noexcept;
using ConnectResolveWork = void (*)(void*) noexcept;

// Internal deterministic seam. Production and tests both use this helper so
// name resolution consumes the same absolute connect deadline as socket setup.
struct ConnectResolvePhaseResult {
    ConnectClock::time_point deadline;
    bool resolver_ran{false};
    bool cancelled{false};
    bool expired{false};
};

[[nodiscard]] ConnectResolvePhaseResult run_connect_resolve_phase(
    std::chrono::milliseconds timeout,
    std::stop_token cancellation,
    ConnectResolveWork resolve,
    void* resolve_context,
    ConnectClockNow now,
    void* clock_context) noexcept;

}  // namespace detail

[[nodiscard]] std::expected<TcpEndpoint, TcpError> parse_tcp_endpoint(
    std::string_view text,
    std::uint16_t default_port = kFastbootTcpDefaultPort);

[[nodiscard]] std::array<std::byte, 8> encode_tcp_frame_length(
    std::uint64_t length) noexcept;

[[nodiscard]] std::uint64_t decode_tcp_frame_length(
    std::span<const std::byte, 8> bytes) noexcept;

enum class SocketIoStatus : std::uint8_t {
    Ok,
    Timeout,
    Cancelled,
    EndOfStream,
    Error,
};

struct SocketIoResult {
    SocketIoStatus status{SocketIoStatus::Ok};
    std::size_t transferred{0};
    int native_error{0};
    std::string detail;
};

// Combines a caller-owned cancellation source with TcpFastbootTransport::cancel().
struct CancellationSignal {
    std::stop_token external;
    std::stop_token local;

    [[nodiscard]] bool stop_requested() const noexcept {
        return external.stop_requested() || local.stop_requested();
    }
};

// One non-blocking socket operation. Implementations must report partial bytes
// even when the same call also ends in timeout/error; callers use this to avoid
// treating an ambiguous Fastboot frame as unsent.
class ITcpSocket {
public:
    virtual ~ITcpSocket() = default;

    [[nodiscard]] virtual SocketIoResult send_some(
        std::span<const std::byte> bytes,
        std::chrono::milliseconds timeout,
        CancellationSignal cancellation) = 0;

    [[nodiscard]] virtual SocketIoResult receive_some(
        std::span<std::byte> destination,
        std::chrono::milliseconds timeout,
        CancellationSignal cancellation) = 0;

    virtual void close() noexcept = 0;
};

struct TcpTransportOptions {
    std::chrono::milliseconds connect_timeout{5'000};
    std::chrono::milliseconds handshake_timeout{2'000};
    std::uint64_t max_frame_bytes{kDefaultMaxTcpFrameBytes};
    std::stop_token cancellation;
};

class TcpFastbootTransport final : public protocol::ITransportSession {
public:
    [[nodiscard]] static std::expected<std::unique_ptr<TcpFastbootTransport>, TcpError> create(
        std::unique_ptr<ITcpSocket> socket,
        TcpTransportOptions options = {});

    ~TcpFastbootTransport() override;

    TcpFastbootTransport(const TcpFastbootTransport&) = delete;
    TcpFastbootTransport& operator=(const TcpFastbootTransport&) = delete;
    TcpFastbootTransport(TcpFastbootTransport&&) = delete;
    TcpFastbootTransport& operator=(TcpFastbootTransport&&) = delete;

    [[nodiscard]] protocol::TransferResult write(
        std::span<const std::byte> bytes,
        std::chrono::milliseconds timeout) override;

    [[nodiscard]] protocol::TransferResult read(
        std::span<std::byte> destination,
        std::chrono::milliseconds timeout) override;

    void cancel() noexcept;
    void close() noexcept override;
    [[nodiscard]] bool is_open() const noexcept;

private:
    TcpFastbootTransport(
        std::unique_ptr<ITcpSocket> socket,
        TcpTransportOptions options);

    [[nodiscard]] std::expected<void, TcpError> initialize();
    void close_locked() noexcept;
    [[nodiscard]] CancellationSignal cancellation_signal() const noexcept;

    std::unique_ptr<ITcpSocket> socket_;
    TcpTransportOptions options_;
    std::stop_source local_cancel_;
    mutable std::mutex mutex_;
    bool open_{true};
};

[[nodiscard]] std::expected<std::unique_ptr<ITcpSocket>, TcpError> connect_native_tcp_socket(
    const TcpEndpoint& endpoint,
    std::chrono::milliseconds timeout,
    std::stop_token cancellation = {});

[[nodiscard]] std::expected<std::unique_ptr<TcpFastbootTransport>, TcpError>
connect_tcp_fastboot(
    std::string_view endpoint,
    TcpTransportOptions options = {});

}  // namespace kairosboot::transport
