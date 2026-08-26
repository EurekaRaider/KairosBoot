// SPDX-License-Identifier: MIT
#include "tcp_fastboot.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cctype>
#include <cstring>
#include <limits>
#include <string>
#include <system_error>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace kairosboot::transport {
namespace {

using Clock = std::chrono::steady_clock;

[[nodiscard]] Clock::time_point deadline_from(
    const Clock::time_point started,
    const std::chrono::milliseconds timeout) noexcept {
    if (timeout <= std::chrono::milliseconds::zero()) {
        return started;
    }
    const auto room = std::chrono::duration_cast<std::chrono::milliseconds>(
        Clock::time_point::max() - started);
    return timeout >= room ? Clock::time_point::max() : started + timeout;
}

[[nodiscard]] bool deadline_expired_at(
    const Clock::time_point deadline,
    const Clock::time_point observed) noexcept {
    return deadline != Clock::time_point::max() && observed >= deadline;
}

class Deadline {
public:
    explicit Deadline(const std::chrono::milliseconds timeout)
        : end_(deadline_from(Clock::now(), timeout)) {}

    explicit Deadline(const Clock::time_point absolute_end) : end_(absolute_end) {}

    [[nodiscard]] std::chrono::milliseconds remaining() const noexcept {
        if (end_ == Clock::time_point::max()) {
            return std::chrono::milliseconds::max();
        }
        const auto now = Clock::now();
        if (now >= end_) {
            return std::chrono::milliseconds::zero();
        }
        return std::chrono::ceil<std::chrono::milliseconds>(end_ - now);
    }

    [[nodiscard]] bool expired() const noexcept {
        return deadline_expired_at(end_, Clock::now());
    }

private:
    Clock::time_point end_{};
};

struct ExactIoResult {
    SocketIoStatus status{SocketIoStatus::Ok};
    std::size_t transferred{0};
    bool any_wire_bytes{false};
    int native_error{0};
    std::string detail{};
};

[[nodiscard]] ExactIoResult send_exact(
    ITcpSocket& socket,
    const std::span<const std::byte> bytes,
    Deadline& deadline,
    const CancellationSignal cancellation) {
    std::size_t completed = 0;
    while (completed < bytes.size()) {
        auto result = socket.send_some(
            bytes.subspan(completed), deadline.remaining(), cancellation);
        if (result.transferred > bytes.size() - completed) {
            return {
                .status = SocketIoStatus::Error,
                .transferred = completed,
                .any_wire_bytes = completed != 0 || result.transferred != 0,
                .detail = "socket reported sending more bytes than requested",
            };
        }
        completed += result.transferred;
        if (result.status != SocketIoStatus::Ok) {
            return {
                .status = result.status,
                .transferred = completed,
                .any_wire_bytes = completed != 0,
                .native_error = result.native_error,
                .detail = std::move(result.detail),
            };
        }
        if (result.transferred == 0) {
            return {
                .status = SocketIoStatus::Error,
                .transferred = completed,
                .any_wire_bytes = completed != 0,
                .detail = "socket made no progress while sending a TCP frame",
            };
        }
    }
    return {
        .transferred = completed,
        .any_wire_bytes = completed != 0,
    };
}

[[nodiscard]] ExactIoResult receive_exact(
    ITcpSocket& socket,
    const std::span<std::byte> destination,
    Deadline& deadline,
    const CancellationSignal cancellation) {
    std::size_t completed = 0;
    while (completed < destination.size()) {
        auto result = socket.receive_some(
            destination.subspan(completed), deadline.remaining(), cancellation);
        if (result.transferred > destination.size() - completed) {
            return {
                .status = SocketIoStatus::Error,
                .transferred = completed,
                .any_wire_bytes = completed != 0 || result.transferred != 0,
                .detail = "socket reported receiving more bytes than requested",
            };
        }
        completed += result.transferred;
        if (result.status != SocketIoStatus::Ok) {
            return {
                .status = result.status,
                .transferred = completed,
                .any_wire_bytes = completed != 0,
                .native_error = result.native_error,
                .detail = std::move(result.detail),
            };
        }
        if (result.transferred == 0) {
            return {
                .status = SocketIoStatus::Error,
                .transferred = completed,
                .any_wire_bytes = completed != 0,
                .detail = "socket made no progress while receiving a TCP frame",
            };
        }
    }
    return {
        .transferred = completed,
        .any_wire_bytes = completed != 0,
    };
}

[[nodiscard]] TcpErrorKind tcp_error_kind(const SocketIoStatus status) noexcept {
    switch (status) {
        case SocketIoStatus::Timeout:
            return TcpErrorKind::Timeout;
        case SocketIoStatus::Cancelled:
            return TcpErrorKind::Cancelled;
        case SocketIoStatus::EndOfStream:
            return TcpErrorKind::Disconnected;
        case SocketIoStatus::Error:
        case SocketIoStatus::Ok:
            return TcpErrorKind::Io;
    }
    return TcpErrorKind::Io;
}

[[nodiscard]] protocol::TransportStatus transport_status(
    const SocketIoStatus status) noexcept {
    switch (status) {
        case SocketIoStatus::Timeout:
            return protocol::TransportStatus::Timeout;
        case SocketIoStatus::EndOfStream:
            return protocol::TransportStatus::Disconnected;
        case SocketIoStatus::Cancelled:
        case SocketIoStatus::Error:
        case SocketIoStatus::Ok:
            return protocol::TransportStatus::IoError;
    }
    return protocol::TransportStatus::IoError;
}

[[nodiscard]] std::string io_failure_detail(
    const ExactIoResult& result,
    const std::string_view operation) {
    std::string message(operation);
    switch (result.status) {
        case SocketIoStatus::Timeout:
            message.append(" timed out");
            break;
        case SocketIoStatus::Cancelled:
            message.append(" was cancelled");
            break;
        case SocketIoStatus::EndOfStream:
            message.append(" reached end of stream");
            break;
        case SocketIoStatus::Error:
            message.append(" failed");
            break;
        case SocketIoStatus::Ok:
            break;
    }
    if (!result.detail.empty()) {
        message.append(": ");
        message.append(result.detail);
    }
    return message;
}

[[nodiscard]] protocol::TransferResult transfer_failure(
    const ExactIoResult& result,
    const std::size_t logical_bytes,
    const bool any_wire_bytes,
    const std::string_view operation,
    const bool truncated = false) {
    return {
        .status = transport_status(result.status),
        .transferred = logical_bytes,
        .certainty = any_wire_bytes
            ? protocol::TransferCertainty::PartialOrUnknown
            : protocol::TransferCertainty::NotTransferred,
        .truncated = truncated,
        .detail = io_failure_detail(result, operation),
    };
}

[[nodiscard]] bool contains_forbidden_endpoint_character(const std::string_view text) {
    return std::ranges::any_of(text, [](const unsigned char character) {
        return std::iscntrl(character) != 0 || std::isspace(character) != 0;
    });
}

[[nodiscard]] std::expected<std::uint16_t, TcpError> parse_port(
    const std::string_view text) {
    if (text.empty()) {
        return std::unexpected(TcpError{
            .kind = TcpErrorKind::InvalidEndpoint,
            .message = "TCP endpoint has an empty port",
        });
    }
    std::uint32_t port = 0;
    const auto [end, parse_error] = std::from_chars(
        text.data(), text.data() + text.size(), port, 10);
    if (parse_error != std::errc{} || end != text.data() + text.size() ||
        port == 0 || port > std::numeric_limits<std::uint16_t>::max()) {
        return std::unexpected(TcpError{
            .kind = TcpErrorKind::InvalidEndpoint,
            .message = "TCP endpoint port must be a decimal value from 1 to 65535",
        });
    }
    return static_cast<std::uint16_t>(port);
}

#ifdef _WIN32

using NativeSocketHandle = SOCKET;
inline constexpr NativeSocketHandle kInvalidNativeSocket = INVALID_SOCKET;

class WinsockRuntime {
public:
    WinsockRuntime() {
        WSADATA data{};
        error_ = WSAStartup(MAKEWORD(2, 2), &data);
    }
    ~WinsockRuntime() {
        if (error_ == 0) {
            WSACleanup();
        }
    }
    [[nodiscard]] int error() const noexcept { return error_; }

private:
    int error_{0};
};

[[nodiscard]] int last_socket_error() noexcept {
    return WSAGetLastError();
}

[[nodiscard]] bool socket_error_would_block(const int error) noexcept {
    return error == WSAEWOULDBLOCK;
}

[[nodiscard]] bool socket_error_interrupted(const int error) noexcept {
    return error == WSAEINTR;
}

[[nodiscard]] bool socket_error_connecting(const int error) noexcept {
    return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS || error == WSAEINVAL;
}

void close_native_socket(const NativeSocketHandle socket) noexcept {
    if (socket != kInvalidNativeSocket) {
        closesocket(socket);
    }
}

[[nodiscard]] bool set_nonblocking(const NativeSocketHandle socket) noexcept {
    u_long enabled = 1;
    return ioctlsocket(socket, FIONBIO, &enabled) == 0;
}

#else

using NativeSocketHandle = int;
inline constexpr NativeSocketHandle kInvalidNativeSocket = -1;

[[nodiscard]] int last_socket_error() noexcept {
    return errno;
}

[[nodiscard]] bool socket_error_would_block(const int error) noexcept {
    return error == EAGAIN || error == EWOULDBLOCK;
}

[[nodiscard]] bool socket_error_interrupted(const int error) noexcept {
    return error == EINTR;
}

[[nodiscard]] bool socket_error_connecting(const int error) noexcept {
    return error == EINPROGRESS || error == EALREADY || socket_error_would_block(error);
}

void close_native_socket(const NativeSocketHandle socket) noexcept {
    if (socket != kInvalidNativeSocket) {
        ::close(socket);
    }
}

[[nodiscard]] bool set_nonblocking(const NativeSocketHandle socket) noexcept {
    const auto flags = fcntl(socket, F_GETFL, 0);
    if (flags == -1 || fcntl(socket, F_SETFL, flags | O_NONBLOCK) == -1) {
        return false;
    }
    const auto descriptor_flags = fcntl(socket, F_GETFD, 0);
    return descriptor_flags != -1 &&
           fcntl(socket, F_SETFD, descriptor_flags | FD_CLOEXEC) != -1;
}

#endif

class NativeSocketGuard {
public:
    explicit NativeSocketGuard(const NativeSocketHandle socket) noexcept
        : socket_(socket) {}

    ~NativeSocketGuard() noexcept {
        close_native_socket(socket_);
    }

    NativeSocketGuard(const NativeSocketGuard&) = delete;
    NativeSocketGuard& operator=(const NativeSocketGuard&) = delete;

    [[nodiscard]] NativeSocketHandle get() const noexcept {
        return socket_;
    }

    [[nodiscard]] NativeSocketHandle release() noexcept {
        return std::exchange(socket_, kInvalidNativeSocket);
    }

private:
    NativeSocketHandle socket_{kInvalidNativeSocket};
};

[[nodiscard]] std::string native_error_message(const int error) {
    return std::system_category().message(error);
}

[[nodiscard]] SocketIoResult wait_for_socket(
    const NativeSocketHandle socket,
    const bool writable,
    Deadline& deadline,
    const CancellationSignal cancellation) {
    constexpr auto poll_slice = std::chrono::milliseconds(50);
    for (;;) {
        if (cancellation.stop_requested()) {
            return {
                .status = SocketIoStatus::Cancelled,
                .detail = "cancellation requested",
            };
        }
        const auto remaining = deadline.remaining();
        const auto slice = remaining == std::chrono::milliseconds::max()
            ? poll_slice
            : std::min(remaining, poll_slice);

#ifdef _WIN32
        fd_set requested;
        FD_ZERO(&requested);
        FD_SET(socket, &requested);
        fd_set exceptions;
        FD_ZERO(&exceptions);
        FD_SET(socket, &exceptions);
        timeval wait_time{
            .tv_sec = static_cast<long>(slice.count() / 1000),
            .tv_usec = static_cast<long>((slice.count() % 1000) * 1000),
        };
        const auto ready = select(
            0,
            writable ? nullptr : &requested,
            writable ? &requested : nullptr,
            &exceptions,
            &wait_time);
#else
        pollfd descriptor{
            .fd = socket,
            .events = static_cast<short>(writable ? POLLOUT : POLLIN),
            .revents = 0,
        };
        const auto ready = poll(&descriptor, 1, static_cast<int>(slice.count()));
#endif
        if (ready > 0) {
#ifdef _WIN32
            if (FD_ISSET(socket, &exceptions)) {
                // A successful select() does not set WSAGetLastError(). Query
                // SO_ERROR so failed non-blocking connects report the actual
                // socket error rather than stale thread-local state.
                int pending_error = 0;
                int pending_error_size = sizeof(pending_error);
                const auto option_result = getsockopt(
                    socket,
                    SOL_SOCKET,
                    SO_ERROR,
                    reinterpret_cast<char*>(&pending_error),
                    &pending_error_size);
                if (option_result != 0) {
                    const auto native_error = last_socket_error();
                    return {
                        .status = SocketIoStatus::Error,
                        .native_error = native_error,
                        .detail = native_error_message(native_error),
                    };
                }
                if (pending_error != 0) {
                    return {
                        .status = SocketIoStatus::Error,
                        .native_error = pending_error,
                        .detail = native_error_message(pending_error),
                    };
                }
            }
#else
            if ((descriptor.revents & POLLNVAL) != 0) {
                return {
                    .status = SocketIoStatus::Error,
                    .detail = "poll reported an invalid socket",
                };
            }
#endif
            return {};
        }
        if (ready == 0) {
            if (deadline.expired()) {
                return {
                    .status = SocketIoStatus::Timeout,
                    .detail = "socket readiness deadline expired",
                };
            }
            continue;
        }

        const auto native_error = last_socket_error();
        if (socket_error_interrupted(native_error)) {
            continue;
        }
        return {
            .status = SocketIoStatus::Error,
            .native_error = native_error,
            .detail = native_error_message(native_error),
        };
    }
}

class NativeTcpSocket final : public ITcpSocket {
public:
    explicit NativeTcpSocket(const NativeSocketHandle socket) : socket_(socket) {}
    ~NativeTcpSocket() override { close(); }

    [[nodiscard]] SocketIoResult send_some(
        const std::span<const std::byte> bytes,
        const std::chrono::milliseconds timeout,
        const CancellationSignal cancellation) override {
        if (bytes.empty()) {
            return {};
        }
        if (socket_ == kInvalidNativeSocket) {
            return {
                .status = SocketIoStatus::EndOfStream,
                .detail = "socket is closed",
            };
        }

        Deadline deadline(timeout);
        for (;;) {
            auto ready = wait_for_socket(socket_, true, deadline, cancellation);
            if (ready.status != SocketIoStatus::Ok) {
                return ready;
            }
#ifdef _WIN32
            const auto amount = static_cast<int>(std::min<std::size_t>(
                bytes.size(), static_cast<std::size_t>(std::numeric_limits<int>::max())));
            const auto sent = ::send(
                socket_, reinterpret_cast<const char*>(bytes.data()), amount, 0);
#else
            const auto amount = std::min<std::size_t>(
                bytes.size(), static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
#ifdef MSG_NOSIGNAL
            // Linux otherwise delivers SIGPIPE to the whole host process when
            // a peer closes between readiness and send().
            constexpr int send_flags = MSG_NOSIGNAL;
#else
            constexpr int send_flags = 0;
#endif
            const auto sent = ::send(socket_, bytes.data(), amount, send_flags);
#endif
            if (sent > 0) {
                return {
                    .transferred = static_cast<std::size_t>(sent),
                };
            }
            if (sent == 0) {
                return {
                    .status = SocketIoStatus::EndOfStream,
                    .detail = "socket closed during send",
                };
            }
            const auto native_error = last_socket_error();
            if (socket_error_would_block(native_error) || socket_error_interrupted(native_error)) {
                continue;
            }
            return {
                .status = SocketIoStatus::Error,
                .native_error = native_error,
                .detail = native_error_message(native_error),
            };
        }
    }

    [[nodiscard]] SocketIoResult receive_some(
        const std::span<std::byte> destination,
        const std::chrono::milliseconds timeout,
        const CancellationSignal cancellation) override {
        if (destination.empty()) {
            return {};
        }
        if (socket_ == kInvalidNativeSocket) {
            return {
                .status = SocketIoStatus::EndOfStream,
                .detail = "socket is closed",
            };
        }

        Deadline deadline(timeout);
        for (;;) {
            auto ready = wait_for_socket(socket_, false, deadline, cancellation);
            if (ready.status != SocketIoStatus::Ok) {
                return ready;
            }
#ifdef _WIN32
            const auto amount = static_cast<int>(std::min<std::size_t>(
                destination.size(),
                static_cast<std::size_t>(std::numeric_limits<int>::max())));
            const auto received = ::recv(
                socket_, reinterpret_cast<char*>(destination.data()), amount, 0);
#else
            const auto amount = std::min<std::size_t>(
                destination.size(),
                static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
            const auto received = ::recv(socket_, destination.data(), amount, 0);
#endif
            if (received > 0) {
                return {
                    .transferred = static_cast<std::size_t>(received),
                };
            }
            if (received == 0) {
                return {
                    .status = SocketIoStatus::EndOfStream,
                    .detail = "peer closed the TCP connection",
                };
            }
            const auto native_error = last_socket_error();
            if (socket_error_would_block(native_error) || socket_error_interrupted(native_error)) {
                continue;
            }
            return {
                .status = SocketIoStatus::Error,
                .native_error = native_error,
                .detail = native_error_message(native_error),
            };
        }
    }

    void close() noexcept override {
        if (socket_ != kInvalidNativeSocket) {
            close_native_socket(socket_);
            socket_ = kInvalidNativeSocket;
        }
    }

private:
    NativeSocketHandle socket_{kInvalidNativeSocket};
};

}  // namespace

detail::ConnectResolvePhaseResult detail::run_connect_resolve_phase(
    const std::chrono::milliseconds timeout,
    const std::stop_token cancellation,
    const ConnectResolveWork resolve,
    void* const resolve_context,
    const ConnectClockNow now,
    void* const clock_context) noexcept {
    const auto started = now(clock_context);
    const auto deadline = deadline_from(started, timeout);
    if (cancellation.stop_requested()) {
        return {
            .deadline = deadline,
            .cancelled = true,
        };
    }
    if (deadline_expired_at(deadline, started)) {
        return {
            .deadline = deadline,
            .expired = true,
        };
    }

    resolve(resolve_context);
    const auto observed = now(clock_context);
    return {
        .deadline = deadline,
        .resolver_ran = true,
        .cancelled = cancellation.stop_requested(),
        .expired = deadline_expired_at(deadline, observed),
    };
}

std::expected<TcpEndpoint, TcpError> parse_tcp_endpoint(
    const std::string_view text,
    const std::uint16_t default_port) {
    if (text.empty() || default_port == 0 ||
        contains_forbidden_endpoint_character(text)) {
        return std::unexpected(TcpError{
            .kind = TcpErrorKind::InvalidEndpoint,
            .message = "TCP endpoint and default port must be non-empty and valid",
        });
    }

    std::string_view host;
    std::uint16_t port = default_port;
    if (text.front() == '[') {
        const auto closing = text.find(']');
        if (closing == std::string_view::npos || closing == 1) {
            return std::unexpected(TcpError{
                .kind = TcpErrorKind::InvalidEndpoint,
                .message = "bracketed IPv6 endpoint is missing its host or closing bracket",
            });
        }
        host = text.substr(1, closing - 1);
        const auto suffix = text.substr(closing + 1);
        if (!suffix.empty()) {
            if (suffix.front() != ':') {
                return std::unexpected(TcpError{
                    .kind = TcpErrorKind::InvalidEndpoint,
                    .message = "unexpected data follows bracketed IPv6 host",
                });
            }
            auto parsed_port = parse_port(suffix.substr(1));
            if (!parsed_port) {
                return std::unexpected(parsed_port.error());
            }
            port = *parsed_port;
        }
    } else {
        if (text.find('[') != std::string_view::npos ||
            text.find(']') != std::string_view::npos) {
            return std::unexpected(TcpError{
                .kind = TcpErrorKind::InvalidEndpoint,
                .message = "TCP endpoint contains an unmatched bracket",
            });
        }
        const auto colon_count = std::ranges::count(text, ':');
        if (colon_count == 1) {
            const auto separator = text.find(':');
            host = text.substr(0, separator);
            auto parsed_port = parse_port(text.substr(separator + 1));
            if (!parsed_port) {
                return std::unexpected(parsed_port.error());
            }
            port = *parsed_port;
        } else {
            // Multiple colons are an unbracketed IPv6 literal using the default
            // port. Brackets are mandatory when an explicit port is present.
            host = text;
        }
    }

    if (host.empty()) {
        return std::unexpected(TcpError{
            .kind = TcpErrorKind::InvalidEndpoint,
            .message = "TCP endpoint host is empty",
        });
    }
    return TcpEndpoint{std::string(host), port};
}

std::array<std::byte, 8> encode_tcp_frame_length(const std::uint64_t length) noexcept {
    std::array<std::byte, 8> encoded{};
    for (std::size_t index = 0; index < encoded.size(); ++index) {
        encoded[index] = static_cast<std::byte>(
            (length >> ((encoded.size() - 1 - index) * 8U)) & 0xFFU);
    }
    return encoded;
}

std::uint64_t decode_tcp_frame_length(
    const std::span<const std::byte, 8> bytes) noexcept {
    std::uint64_t decoded = 0;
    for (const auto byte : bytes) {
        decoded = (decoded << 8U) | std::to_integer<std::uint8_t>(byte);
    }
    return decoded;
}

TcpFastbootTransport::TcpFastbootTransport(
    std::unique_ptr<ITcpSocket> socket,
    TcpTransportOptions options)
    : socket_(std::move(socket)), options_(options) {}

TcpFastbootTransport::~TcpFastbootTransport() {
    close();
}

std::expected<std::unique_ptr<TcpFastbootTransport>, TcpError>
TcpFastbootTransport::create(
    std::unique_ptr<ITcpSocket> socket,
    TcpTransportOptions options) {
    if (!socket) {
        return std::unexpected(TcpError{
            .kind = TcpErrorKind::Io,
            .message = "cannot create Fastboot TCP transport with a null socket",
        });
    }
    auto transport = std::unique_ptr<TcpFastbootTransport>(
        new TcpFastbootTransport(std::move(socket), options));
    if (auto initialized = transport->initialize(); !initialized) {
        transport->close();
        return std::unexpected(initialized.error());
    }
    return transport;
}

std::expected<void, TcpError> TcpFastbootTransport::initialize() {
    Deadline deadline(options_.handshake_timeout);
    const auto cancellation = cancellation_signal();
    const auto sent = send_exact(*socket_, kFastbootTcpV1Handshake, deadline, cancellation);
    if (sent.status != SocketIoStatus::Ok) {
        return std::unexpected(TcpError{
            .kind = tcp_error_kind(sent.status),
            .native_error = sent.native_error,
            .message = io_failure_detail(sent, "sending FB01 handshake"),
        });
    }

    std::array<std::byte, 4> peer_handshake{};
    const auto received = receive_exact(*socket_, peer_handshake, deadline, cancellation);
    if (received.status != SocketIoStatus::Ok) {
        return std::unexpected(TcpError{
            .kind = tcp_error_kind(received.status),
            .native_error = received.native_error,
            .message = io_failure_detail(received, "receiving Fastboot TCP handshake"),
        });
    }

    if (peer_handshake[0] != std::byte{'F'} || peer_handshake[1] != std::byte{'B'} ||
        peer_handshake[2] < std::byte{'0'} || peer_handshake[2] > std::byte{'9'} ||
        peer_handshake[3] < std::byte{'0'} || peer_handshake[3] > std::byte{'9'}) {
        return std::unexpected(TcpError{
            .kind = TcpErrorKind::HandshakeFailed,
            .message = "peer returned a malformed Fastboot TCP handshake",
        });
    }
    const auto peer_version =
        (std::to_integer<unsigned int>(peer_handshake[2]) - '0') * 10U +
        (std::to_integer<unsigned int>(peer_handshake[3]) - '0');
    if (peer_version < 1U) {
        return std::unexpected(TcpError{
            .kind = TcpErrorKind::HandshakeFailed,
            .message = "peer Fastboot TCP version is lower than host version 01",
        });
    }
    return {};
}

protocol::TransferResult TcpFastbootTransport::write(
    const std::span<const std::byte> bytes,
    const std::chrono::milliseconds timeout) {
    std::scoped_lock lock(mutex_);
    if (!open_ || !socket_) {
        return {
            .status = protocol::TransportStatus::Disconnected,
            .certainty = protocol::TransferCertainty::NotTransferred,
            .detail = "Fastboot TCP transport is closed",
        };
    }
    const auto frame_size = static_cast<std::uint64_t>(bytes.size());
    if (frame_size > options_.max_frame_bytes) {
        return {
            .status = protocol::TransportStatus::IoError,
            .certainty = protocol::TransferCertainty::NotTransferred,
            .detail = "outbound Fastboot TCP frame exceeds the configured limit",
        };
    }

    Deadline deadline(timeout);
    const auto cancellation = cancellation_signal();
    const auto header = encode_tcp_frame_length(frame_size);
    const auto header_result = send_exact(*socket_, header, deadline, cancellation);
    if (header_result.status != SocketIoStatus::Ok) {
        auto failure = transfer_failure(
            header_result,
            0,
            header_result.any_wire_bytes,
            "sending Fastboot TCP frame header");
        close_locked();
        return failure;
    }

    const auto payload_result = send_exact(*socket_, bytes, deadline, cancellation);
    if (payload_result.status != SocketIoStatus::Ok) {
        auto failure = transfer_failure(
            payload_result,
            payload_result.transferred,
            true,
            "sending Fastboot TCP frame payload");
        close_locked();
        return failure;
    }
    return {
        .status = protocol::TransportStatus::Ok,
        .transferred = bytes.size(),
        .certainty = protocol::TransferCertainty::FullyTransferred,
        .detail = {},
    };
}

protocol::TransferResult TcpFastbootTransport::read(
    const std::span<std::byte> destination,
    const std::chrono::milliseconds timeout) {
    std::scoped_lock lock(mutex_);
    if (!open_ || !socket_) {
        return {
            .status = protocol::TransportStatus::Disconnected,
            .certainty = protocol::TransferCertainty::NotTransferred,
            .detail = "Fastboot TCP transport is closed",
        };
    }

    Deadline deadline(timeout);
    const auto cancellation = cancellation_signal();
    std::array<std::byte, 8> header{};
    const auto header_result = receive_exact(*socket_, header, deadline, cancellation);
    if (header_result.status != SocketIoStatus::Ok) {
        auto failure = transfer_failure(
            header_result,
            0,
            header_result.any_wire_bytes,
            "receiving Fastboot TCP frame header");
        close_locked();
        return failure;
    }

    const auto frame_size = decode_tcp_frame_length(header);
    if (frame_size > options_.max_frame_bytes ||
        frame_size > std::numeric_limits<std::size_t>::max() ||
        frame_size > destination.size()) {
        close_locked();
        return {
            .status = protocol::TransportStatus::IoError,
            .certainty = protocol::TransferCertainty::PartialOrUnknown,
            .truncated = true,
            .detail = "inbound Fastboot TCP frame exceeds its configured or destination limit",
        };
    }

    const auto payload_size = static_cast<std::size_t>(frame_size);
    const auto payload_result = receive_exact(
        *socket_, destination.first(payload_size), deadline, cancellation);
    if (payload_result.status != SocketIoStatus::Ok) {
        auto failure = transfer_failure(
            payload_result,
            payload_result.transferred,
            true,
            "receiving Fastboot TCP frame payload");
        close_locked();
        return failure;
    }
    return {
        .status = protocol::TransportStatus::Ok,
        .transferred = payload_size,
        .certainty = protocol::TransferCertainty::FullyTransferred,
        .detail = {},
    };
}

void TcpFastbootTransport::cancel() noexcept {
    local_cancel_.request_stop();
}

void TcpFastbootTransport::close() noexcept {
    std::scoped_lock lock(mutex_);
    close_locked();
}

bool TcpFastbootTransport::is_open() const noexcept {
    std::scoped_lock lock(mutex_);
    return open_;
}

void TcpFastbootTransport::close_locked() noexcept {
    if (!open_) {
        return;
    }
    open_ = false;
    if (socket_) {
        socket_->close();
    }
}

CancellationSignal TcpFastbootTransport::cancellation_signal() const noexcept {
    return {
        .external = options_.cancellation,
        .local = local_cancel_.get_token(),
    };
}

std::expected<std::unique_ptr<ITcpSocket>, TcpError> connect_native_tcp_socket(
    const TcpEndpoint& endpoint,
    const std::chrono::milliseconds timeout,
    const std::stop_token cancellation) {
    if (endpoint.host.empty() || endpoint.port == 0) {
        return std::unexpected(TcpError{
            .kind = TcpErrorKind::InvalidEndpoint,
            .message = "native TCP endpoint is empty or has port zero",
        });
    }
#ifdef _WIN32
    static WinsockRuntime winsock;
    if (winsock.error() != 0) {
        return std::unexpected(TcpError{
            .kind = TcpErrorKind::Io,
            .native_error = winsock.error(),
            .message = "Winsock 2.2 initialization failed",
        });
    }
#endif

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* addresses = nullptr;
    const auto service = std::to_string(endpoint.port);
    struct ResolveContext {
        const char* host;
        const char* service;
        const addrinfo* hints;
        addrinfo** addresses;
        int result{0};
    } resolve_context{
        .host = endpoint.host.c_str(),
        .service = service.c_str(),
        .hints = &hints,
        .addresses = &addresses,
    };
    const auto resolve = [](void* const opaque) noexcept {
        auto& context = *static_cast<ResolveContext*>(opaque);
        context.result = getaddrinfo(
            context.host, context.service, context.hints, context.addresses);
    };
    const auto now = [](void*) noexcept {
        return Clock::now();
    };
    const auto resolve_phase = detail::run_connect_resolve_phase(
        timeout,
        cancellation,
        resolve,
        &resolve_context,
        now,
        nullptr);
    const std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> address_list(
        addresses, &freeaddrinfo);
    if (resolve_phase.cancelled) {
        return std::unexpected(TcpError{
            .kind = TcpErrorKind::Cancelled,
            .message = resolve_phase.resolver_ran
                ? "TCP connect was cancelled during name resolution"
                : "TCP connect was cancelled before name resolution",
        });
    }
    if (resolve_phase.expired) {
        return std::unexpected(TcpError{
            .kind = TcpErrorKind::Timeout,
            .message = "TCP connect deadline expired during name resolution",
        });
    }
    const auto resolve_result = resolve_context.result;
    if (resolve_result != 0) {
        return std::unexpected(TcpError{
            .kind = TcpErrorKind::ResolveFailed,
            .native_error = resolve_result,
            .message = "getaddrinfo failed for TCP endpoint with code " +
                       std::to_string(resolve_result),
        });
    }

    Deadline deadline(resolve_phase.deadline);
    int last_error = 0;
    std::string last_detail = "no address candidate connected";
    for (auto* address = address_list.get(); address != nullptr; address = address->ai_next) {
        if (cancellation.stop_requested()) {
            return std::unexpected(TcpError{
                .kind = TcpErrorKind::Cancelled,
                .message = "TCP connect was cancelled",
            });
        }
        if (deadline.expired()) {
            return std::unexpected(TcpError{
                .kind = TcpErrorKind::Timeout,
                .message = "TCP connect deadline expired",
            });
        }

        NativeSocketGuard native_socket(::socket(
            address->ai_family, address->ai_socktype, address->ai_protocol));
        if (native_socket.get() == kInvalidNativeSocket) {
            last_error = last_socket_error();
            last_detail = native_error_message(last_error);
            continue;
        }
        if (!set_nonblocking(native_socket.get())) {
            last_error = last_socket_error();
            last_detail = native_error_message(last_error);
            continue;
        }

#ifdef _WIN32
        const auto connect_result = ::connect(
            native_socket.get(),
            address->ai_addr,
            static_cast<int>(address->ai_addrlen));
#else
        const auto connect_result = ::connect(
            native_socket.get(),
            address->ai_addr,
            static_cast<socklen_t>(address->ai_addrlen));
#endif
        if (connect_result != 0) {
            const auto connect_error = last_socket_error();
            if (!socket_error_connecting(connect_error)) {
                last_error = connect_error;
                last_detail = native_error_message(last_error);
                continue;
            }

            auto ready = wait_for_socket(
                native_socket.get(),
                true,
                deadline,
                CancellationSignal{.external = cancellation});
            if (ready.status == SocketIoStatus::Timeout) {
                return std::unexpected(TcpError{
                    .kind = TcpErrorKind::Timeout,
                    .message = "TCP connect deadline expired",
                });
            }
            if (ready.status == SocketIoStatus::Cancelled) {
                return std::unexpected(TcpError{
                    .kind = TcpErrorKind::Cancelled,
                    .message = "TCP connect was cancelled",
                });
            }
            if (ready.status != SocketIoStatus::Ok) {
                last_error = ready.native_error;
                last_detail = ready.detail;
                continue;
            }

            int pending_error = 0;
#ifdef _WIN32
            int pending_error_size = sizeof(pending_error);
            const auto option_result = getsockopt(
                native_socket.get(),
                SOL_SOCKET,
                SO_ERROR,
                reinterpret_cast<char*>(&pending_error),
                &pending_error_size);
#else
            socklen_t pending_error_size = sizeof(pending_error);
            const auto option_result = getsockopt(
                native_socket.get(),
                SOL_SOCKET,
                SO_ERROR,
                &pending_error,
                &pending_error_size);
#endif
            if (option_result != 0 || pending_error != 0) {
                last_error = option_result != 0 ? last_socket_error() : pending_error;
                last_detail = native_error_message(last_error);
                continue;
            }
        }

        int enabled = 1;
#ifdef _WIN32
        setsockopt(
            native_socket.get(),
            IPPROTO_TCP,
            TCP_NODELAY,
            reinterpret_cast<const char*>(&enabled),
            sizeof(enabled));
#else
        setsockopt(
            native_socket.get(), IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled));
#ifdef SO_NOSIGPIPE
        setsockopt(
            native_socket.get(), SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled));
#endif
#endif
        auto owned_socket = std::make_unique<NativeTcpSocket>(native_socket.get());
        static_cast<void>(native_socket.release());
        return std::unique_ptr<ITcpSocket>(std::move(owned_socket));
    }

    return std::unexpected(TcpError{
        .kind = TcpErrorKind::ConnectFailed,
        .native_error = last_error,
        .message = "failed to connect TCP endpoint: " + last_detail,
    });
}

std::expected<std::unique_ptr<TcpFastbootTransport>, TcpError> connect_tcp_fastboot(
    const std::string_view endpoint,
    TcpTransportOptions options) {
    auto parsed = parse_tcp_endpoint(endpoint);
    if (!parsed) {
        return std::unexpected(parsed.error());
    }
    auto socket = connect_native_tcp_socket(
        *parsed, options.connect_timeout, options.cancellation);
    if (!socket) {
        return std::unexpected(socket.error());
    }
    return TcpFastbootTransport::create(std::move(*socket), options);
}

}  // namespace kairosboot::transport
