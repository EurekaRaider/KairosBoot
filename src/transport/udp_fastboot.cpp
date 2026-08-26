// SPDX-License-Identifier: MIT
#include "udp_fastboot.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cctype>
#include <cstring>
#include <limits>
#include <new>
#include <ranges>
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
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace kairosboot::transport {
namespace {

using Clock = std::chrono::steady_clock;

inline constexpr std::size_t kMaximumIgnoredDatagrams = 64;
inline constexpr std::size_t kMaximumErrorTextBytes = 1024;
inline constexpr std::size_t kMaximumHandshakeResponseBytes = 64;
inline constexpr std::uint16_t kMaximumQueryAttempts = 4;
inline constexpr std::uint16_t kMaximumTransmissionAttempts = 120;

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

    explicit Deadline(const Clock::time_point end) : end_(end) {}

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

[[nodiscard]] std::chrono::milliseconds bounded_wait(
    const Deadline& operation,
    const std::chrono::milliseconds interval) noexcept {
    const auto remaining = operation.remaining();
    return remaining == std::chrono::milliseconds::max()
        ? interval
        : std::min(remaining, interval);
}

[[nodiscard]] bool contains_forbidden_endpoint_character(const std::string_view text) {
    return std::ranges::any_of(text, [](const unsigned char character) {
        return std::iscntrl(character) != 0 || std::isspace(character) != 0;
    });
}

[[nodiscard]] std::expected<std::uint16_t, UdpError> parse_port(
    const std::string_view text) {
    if (text.empty()) {
        return std::unexpected(UdpError{
            .kind = UdpErrorKind::InvalidEndpoint,
            .message = "UDP endpoint has an empty port",
        });
    }
    std::uint32_t port = 0;
    const auto [end, error] = std::from_chars(
        text.data(), text.data() + text.size(), port, 10);
    if (error != std::errc{} || end != text.data() + text.size() ||
        port == 0 || port > std::numeric_limits<std::uint16_t>::max()) {
        return std::unexpected(UdpError{
            .kind = UdpErrorKind::InvalidEndpoint,
            .message = "UDP endpoint port must be a decimal value from 1 to 65535",
        });
    }
    return static_cast<std::uint16_t>(port);
}

[[nodiscard]] protocol::TransportStatus transport_status(
    const DatagramIoStatus status) noexcept {
    switch (status) {
        case DatagramIoStatus::Timeout:
            return protocol::TransportStatus::Timeout;
        case DatagramIoStatus::Cancelled:
        case DatagramIoStatus::Truncated:
        case DatagramIoStatus::Error:
        case DatagramIoStatus::Ok:
            return protocol::TransportStatus::IoError;
    }
    return protocol::TransportStatus::IoError;
}

[[nodiscard]] UdpErrorKind error_kind(const protocol::TransportStatus status) noexcept {
    switch (status) {
        case protocol::TransportStatus::Timeout:
            return UdpErrorKind::Timeout;
        case protocol::TransportStatus::Disconnected:
            return UdpErrorKind::Io;
        case protocol::TransportStatus::IoError:
            return UdpErrorKind::Io;
        case protocol::TransportStatus::Ok:
            return UdpErrorKind::HandshakeFailed;
    }
    return UdpErrorKind::HandshakeFailed;
}

[[nodiscard]] std::string datagram_failure_detail(
    const DatagramIoStatus status,
    const std::string_view operation,
    const std::string_view native_detail) {
    std::string message(operation);
    switch (status) {
        case DatagramIoStatus::Timeout:
            message.append(" timed out");
            break;
        case DatagramIoStatus::Cancelled:
            message.append(" was cancelled");
            break;
        case DatagramIoStatus::Truncated:
            message.append(" was truncated");
            break;
        case DatagramIoStatus::Error:
            message.append(" failed");
            break;
        case DatagramIoStatus::Ok:
            break;
    }
    if (!native_detail.empty()) {
        message.append(": ");
        message.append(native_detail);
    }
    return message;
}

[[nodiscard]] bool checked_add(
    const std::size_t left,
    const std::size_t right,
    std::size_t& result) noexcept {
    if (right > std::numeric_limits<std::size_t>::max() - left) {
        return false;
    }
    result = left + right;
    return true;
}

#ifdef _WIN32

using NativeSocketHandle = SOCKET;
inline constexpr NativeSocketHandle kInvalidNativeSocket = INVALID_SOCKET;

class WinsockLease {
public:
    WinsockLease() {
        WSADATA data{};
        error_ = WSAStartup(MAKEWORD(2, 2), &data);
        active_ = error_ == 0;
    }

    ~WinsockLease() {
        if (active_) {
            WSACleanup();
        }
    }

    WinsockLease(const WinsockLease&) = delete;
    WinsockLease& operator=(const WinsockLease&) = delete;

    [[nodiscard]] int error() const noexcept { return error_; }

private:
    int error_{0};
    bool active_{false};
};

[[nodiscard]] int last_socket_error() noexcept { return WSAGetLastError(); }

[[nodiscard]] bool socket_error_would_block(const int error) noexcept {
    return error == WSAEWOULDBLOCK;
}

[[nodiscard]] bool socket_error_interrupted(const int error) noexcept {
    return error == WSAEINTR;
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

[[nodiscard]] int last_socket_error() noexcept { return errno; }

[[nodiscard]] bool socket_error_would_block(const int error) noexcept {
    return error == EAGAIN || error == EWOULDBLOCK;
}

[[nodiscard]] bool socket_error_interrupted(const int error) noexcept {
    return error == EINTR;
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

    ~NativeSocketGuard() noexcept { close_native_socket(socket_); }

    NativeSocketGuard(const NativeSocketGuard&) = delete;
    NativeSocketGuard& operator=(const NativeSocketGuard&) = delete;

    [[nodiscard]] NativeSocketHandle get() const noexcept { return socket_; }

    [[nodiscard]] NativeSocketHandle release() noexcept {
        return std::exchange(socket_, kInvalidNativeSocket);
    }

private:
    NativeSocketHandle socket_{kInvalidNativeSocket};
};

[[nodiscard]] std::string native_error_message(const int error) {
    return std::system_category().message(error);
}

[[nodiscard]] DatagramSendResult wait_for_native_socket(
    const NativeSocketHandle socket,
    const bool writable,
    Deadline& deadline,
    const UdpCancellationSignal cancellation) {
    constexpr auto poll_slice = std::chrono::milliseconds(50);
    for (;;) {
        if (cancellation.stop_requested()) {
            return {
                .status = DatagramIoStatus::Cancelled,
                .detail = "cancellation requested",
            };
        }
        const auto remaining = deadline.remaining();
        if (remaining <= std::chrono::milliseconds::zero()) {
            return {
                .status = DatagramIoStatus::Timeout,
                .detail = "socket readiness deadline expired",
            };
        }
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
                int pending_error = 0;
                int pending_error_size = sizeof(pending_error);
                if (getsockopt(
                        socket,
                        SOL_SOCKET,
                        SO_ERROR,
                        reinterpret_cast<char*>(&pending_error),
                        &pending_error_size) != 0) {
                    pending_error = last_socket_error();
                }
                if (pending_error != 0) {
                    return {
                        .status = DatagramIoStatus::Error,
                        .native_error = pending_error,
                        .detail = native_error_message(pending_error),
                    };
                }
            }
#else
            if ((descriptor.revents & POLLNVAL) != 0) {
                return {
                    .status = DatagramIoStatus::Error,
                    .detail = "poll reported an invalid UDP socket",
                };
            }
#endif
            return {};
        }
        if (ready == 0) {
            continue;
        }
        const auto native_error = last_socket_error();
        if (socket_error_interrupted(native_error)) {
            continue;
        }
        return {
            .status = DatagramIoStatus::Error,
            .native_error = native_error,
            .detail = native_error_message(native_error),
        };
    }
}

[[nodiscard]] std::expected<UdpPeer, UdpError> peer_from_sockaddr(
    const sockaddr* const address,
    const std::size_t address_length) {
    UdpPeer peer;
    if (address->sa_family == AF_INET && address_length >= sizeof(sockaddr_in)) {
        const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(address);
        peer.family = UdpAddressFamily::Ipv4;
        std::memcpy(peer.address.data(), &ipv4->sin_addr, sizeof(ipv4->sin_addr));
        peer.port = ntohs(ipv4->sin_port);
        return peer;
    }
    if (address->sa_family == AF_INET6 && address_length >= sizeof(sockaddr_in6)) {
        const auto* ipv6 = reinterpret_cast<const sockaddr_in6*>(address);
        peer.family = UdpAddressFamily::Ipv6;
        std::memcpy(peer.address.data(), &ipv6->sin6_addr, sizeof(ipv6->sin6_addr));
        peer.port = ntohs(ipv6->sin6_port);
        peer.scope_id = ipv6->sin6_scope_id;
        return peer;
    }
    return std::unexpected(UdpError{
        .kind = UdpErrorKind::SocketFailed,
        .message = "resolved UDP address has an unsupported family or size",
    });
}

class NativeUdpSocket final : public IUdpSocket {
public:
    NativeUdpSocket(
        const NativeSocketHandle socket,
        const sockaddr_storage& peer_address,
        const std::size_t peer_address_length,
        UdpPeer peer
#ifdef _WIN32
        ,
        std::unique_ptr<WinsockLease> winsock
#endif
        )
        : socket_(socket),
          peer_address_(peer_address),
          peer_address_length_(peer_address_length),
          peer_(peer)
#ifdef _WIN32
          ,
          winsock_(std::move(winsock))
#endif
    {}

    ~NativeUdpSocket() override { close(); }

    [[nodiscard]] DatagramSendResult send_datagram(
        const std::span<const std::byte> datagram,
        const UdpPeer& peer,
        const std::chrono::milliseconds timeout,
        const UdpCancellationSignal cancellation) override {
        if (socket_ == kInvalidNativeSocket) {
            return {
                .status = DatagramIoStatus::Error,
                .detail = "UDP socket is closed",
            };
        }
        if (peer != peer_) {
            return {
                .status = DatagramIoStatus::Error,
                .detail = "send peer does not match the resolved UDP endpoint",
            };
        }
        Deadline deadline(timeout);
        for (;;) {
            auto ready = wait_for_native_socket(socket_, true, deadline, cancellation);
            if (ready.status != DatagramIoStatus::Ok) {
                return ready;
            }
#ifdef _WIN32
            const auto amount = static_cast<int>(datagram.size());
            const auto sent = sendto(
                socket_,
                reinterpret_cast<const char*>(datagram.data()),
                amount,
                0,
                reinterpret_cast<const sockaddr*>(&peer_address_),
                static_cast<int>(peer_address_length_));
#else
#ifdef MSG_NOSIGNAL
            constexpr int send_flags = MSG_NOSIGNAL;
#else
            constexpr int send_flags = 0;
#endif
            const auto sent = sendto(
                socket_,
                datagram.data(),
                datagram.size(),
                send_flags,
                reinterpret_cast<const sockaddr*>(&peer_address_),
                static_cast<socklen_t>(peer_address_length_));
#endif
            if (sent >= 0) {
                const auto transferred = static_cast<std::size_t>(sent);
                return {
                    .status = transferred == datagram.size()
                        ? DatagramIoStatus::Ok
                        : DatagramIoStatus::Error,
                    .transferred = transferred,
                    .detail = transferred == datagram.size()
                        ? std::string{}
                        : std::string{"UDP socket reported a partial datagram send"},
                };
            }
            const auto native_error = last_socket_error();
            if (socket_error_would_block(native_error) ||
                socket_error_interrupted(native_error)) {
                continue;
            }
            return {
                .status = DatagramIoStatus::Error,
                .native_error = native_error,
                .detail = native_error_message(native_error),
            };
        }
    }

    [[nodiscard]] DatagramReceiveResult receive_datagram(
        const std::span<std::byte> destination,
        const std::chrono::milliseconds timeout,
        const UdpCancellationSignal cancellation) override {
        if (socket_ == kInvalidNativeSocket) {
            return {
                .status = DatagramIoStatus::Error,
                .detail = "UDP socket is closed",
            };
        }
        Deadline deadline(timeout);
        for (;;) {
            auto ready = wait_for_native_socket(socket_, false, deadline, cancellation);
            if (ready.status != DatagramIoStatus::Ok) {
                return {
                    .status = ready.status,
                    .native_error = ready.native_error,
                    .detail = std::move(ready.detail),
                };
            }

            sockaddr_storage sender{};
#ifdef _WIN32
            int sender_length = sizeof(sender);
            const auto received = recvfrom(
                socket_,
                reinterpret_cast<char*>(destination.data()),
                static_cast<int>(destination.size()),
                0,
                reinterpret_cast<sockaddr*>(&sender),
                &sender_length);
            if (received >= 0) {
                auto sender_peer = peer_from_sockaddr(
                    reinterpret_cast<const sockaddr*>(&sender),
                    static_cast<std::size_t>(sender_length));
                if (!sender_peer) {
                    return {
                        .status = DatagramIoStatus::Error,
                        .detail = sender_peer.error().message,
                    };
                }
                return {
                    .transferred = static_cast<std::size_t>(received),
                    .peer = *sender_peer,
                };
            }
            const auto native_error = last_socket_error();
            if (native_error == WSAEMSGSIZE) {
                auto sender_peer = peer_from_sockaddr(
                    reinterpret_cast<const sockaddr*>(&sender),
                    static_cast<std::size_t>(sender_length));
                if (!sender_peer) {
                    return {
                        .status = DatagramIoStatus::Truncated,
                        .transferred = destination.size(),
                        .native_error = native_error,
                        .detail = "oversized UDP datagram had an invalid source address",
                    };
                }
                return {
                    .status = DatagramIoStatus::Truncated,
                    .transferred = destination.size(),
                    .peer = *sender_peer,
                    .native_error = native_error,
                    .detail = "UDP datagram exceeded the receive buffer",
                };
            }
#else
            iovec data{
                .iov_base = destination.data(),
                .iov_len = destination.size(),
            };
            msghdr message{
                .msg_name = &sender,
                .msg_namelen = sizeof(sender),
                .msg_iov = &data,
                .msg_iovlen = 1,
                .msg_control = nullptr,
                .msg_controllen = 0,
                .msg_flags = 0,
            };
            const auto received = recvmsg(socket_, &message, MSG_TRUNC);
            if (received >= 0) {
                auto sender_peer = peer_from_sockaddr(
                    reinterpret_cast<const sockaddr*>(&sender),
                    static_cast<std::size_t>(message.msg_namelen));
                if (!sender_peer) {
                    return {
                        .status = DatagramIoStatus::Error,
                        .detail = sender_peer.error().message,
                    };
                }
                const auto actual = static_cast<std::size_t>(received);
                return {
                    .status = actual > destination.size()
                        ? DatagramIoStatus::Truncated
                        : DatagramIoStatus::Ok,
                    .transferred = std::min(actual, destination.size()),
                    .peer = *sender_peer,
                    .detail = actual > destination.size()
                        ? std::string{"UDP datagram exceeded the receive buffer"}
                        : std::string{},
                };
            }
            const auto native_error = last_socket_error();
#endif
            if (socket_error_would_block(native_error) ||
                socket_error_interrupted(native_error)) {
                continue;
            }
            return {
                .status = DatagramIoStatus::Error,
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
    sockaddr_storage peer_address_{};
    std::size_t peer_address_length_{0};
    UdpPeer peer_;
#ifdef _WIN32
    // Per-connection ownership avoids a module-static WSACleanup destructor.
    std::unique_ptr<WinsockLease> winsock_;
#endif
};

}  // namespace

std::array<std::byte, kFastbootUdpHeaderBytes> encode_udp_header(
    const UdpPacketHeader header) noexcept {
    return {
        static_cast<std::byte>(header.id),
        static_cast<std::byte>(header.flags),
        static_cast<std::byte>((header.sequence >> 8U) & 0xFFU),
        static_cast<std::byte>(header.sequence & 0xFFU),
    };
}

UdpPacketHeader decode_udp_header(
    const std::span<const std::byte, kFastbootUdpHeaderBytes> bytes) noexcept {
    return {
        .id = static_cast<UdpPacketId>(std::to_integer<std::uint8_t>(bytes[0])),
        .flags = std::to_integer<std::uint8_t>(bytes[1]),
        .sequence = static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(bytes[2])) << 8U) |
            std::to_integer<std::uint8_t>(bytes[3])),
    };
}

std::expected<UdpEndpoint, UdpError> parse_udp_endpoint(
    const std::string_view text,
    const std::uint16_t default_port) {
    if (text.empty() || default_port == 0 ||
        contains_forbidden_endpoint_character(text)) {
        return std::unexpected(UdpError{
            .kind = UdpErrorKind::InvalidEndpoint,
            .message = "UDP endpoint and default port must be non-empty and valid",
        });
    }

    std::string_view host;
    std::uint16_t port = default_port;
    if (text.front() == '[') {
        const auto closing = text.find(']');
        if (closing == std::string_view::npos || closing == 1) {
            return std::unexpected(UdpError{
                .kind = UdpErrorKind::InvalidEndpoint,
                .message = "bracketed IPv6 UDP endpoint is incomplete",
            });
        }
        host = text.substr(1, closing - 1);
        const auto suffix = text.substr(closing + 1);
        if (!suffix.empty()) {
            if (suffix.front() != ':') {
                return std::unexpected(UdpError{
                    .kind = UdpErrorKind::InvalidEndpoint,
                    .message = "unexpected data follows bracketed IPv6 UDP host",
                });
            }
            auto parsed = parse_port(suffix.substr(1));
            if (!parsed) {
                return std::unexpected(parsed.error());
            }
            port = *parsed;
        }
    } else {
        if (text.find('[') != std::string_view::npos ||
            text.find(']') != std::string_view::npos) {
            return std::unexpected(UdpError{
                .kind = UdpErrorKind::InvalidEndpoint,
                .message = "UDP endpoint contains an unmatched bracket",
            });
        }
        const auto colon_count = std::ranges::count(text, ':');
        if (colon_count == 1) {
            const auto separator = text.find(':');
            host = text.substr(0, separator);
            auto parsed = parse_port(text.substr(separator + 1));
            if (!parsed) {
                return std::unexpected(parsed.error());
            }
            port = *parsed;
        } else {
            host = text;
        }
    }

    if (host.empty()) {
        return std::unexpected(UdpError{
            .kind = UdpErrorKind::InvalidEndpoint,
            .message = "UDP endpoint host is empty",
        });
    }
    return UdpEndpoint{std::string(host), port};
}

detail::UdpResolvePhaseResult detail::run_udp_resolve_phase(
    const std::chrono::milliseconds timeout,
    const std::stop_token cancellation,
    const UdpResolveWork resolve,
    void* const resolve_context,
    const UdpConnectClockNow now,
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

UdpFastbootTransport::UdpFastbootTransport(
    std::unique_ptr<IUdpSocket> socket,
    UdpPeer peer,
    UdpTransportOptions options)
    : socket_(std::move(socket)),
      peer_(peer),
      options_(options),
      receive_packet_(static_cast<std::size_t>(options.host_max_packet_bytes) + 1U) {}

UdpFastbootTransport::~UdpFastbootTransport() { close(); }

std::expected<std::unique_ptr<UdpFastbootTransport>, UdpError>
UdpFastbootTransport::create(
    std::unique_ptr<IUdpSocket> socket,
    UdpPeer peer,
    UdpTransportOptions options) {
    if (!socket || peer.port == 0) {
        return std::unexpected(UdpError{
            .kind = UdpErrorKind::SocketFailed,
            .message = "Fastboot UDP transport requires a socket and non-zero peer port",
        });
    }
    if (options.connect_timeout <= std::chrono::milliseconds::zero() ||
        options.query_timeout <= std::chrono::milliseconds::zero() ||
        options.initialization_timeout <= std::chrono::milliseconds::zero() ||
        options.retransmit_interval < std::chrono::milliseconds(500) ||
        options.host_max_packet_bytes < kFastbootUdpMinimumPacketBytes ||
        options.host_max_packet_bytes > kFastbootUdpHostMaximumPacketBytes ||
        options.query_attempts == 0 || options.query_attempts > kMaximumQueryAttempts ||
        options.transmission_attempts == 0 ||
        options.transmission_attempts > kMaximumTransmissionAttempts) {
        return std::unexpected(UdpError{
            .kind = UdpErrorKind::HandshakeFailed,
            .message = "Fastboot UDP options violate protocol or amplification limits",
        });
    }

    std::unique_ptr<UdpFastbootTransport> transport;
    try {
        transport.reset(new UdpFastbootTransport(
            std::move(socket), peer, options));
    } catch (const std::bad_alloc&) {
        return std::unexpected(UdpError{
            .kind = UdpErrorKind::Io,
            .message = "allocating Fastboot UDP transport buffers failed",
        });
    }
    if (auto initialized = transport->initialize(); !initialized) {
        transport->close();
        return std::unexpected(initialized.error());
    }
    return transport;
}

std::expected<void, UdpError> UdpFastbootTransport::initialize() {
    std::array<std::byte, kMaximumHandshakeResponseBytes> query_response{};
    auto query = exchange_message(
        UdpPacketId::Query,
        {},
        query_response,
        query_response.size(),
        options_.query_timeout,
        options_.query_attempts);
    if (!query.success) {
        if (query.poison) {
            poison_locked();
        }
        return std::unexpected(UdpError{
            .kind = query.cancelled
                ? UdpErrorKind::Cancelled
                : (query.target_error
                       ? UdpErrorKind::Protocol
                       : error_kind(query.status)),
            .native_error = query.native_error,
            .message = "Fastboot UDP query failed: " + query.detail,
        });
    }
    if (query.response_bytes < 2) {
        poison_locked();
        return std::unexpected(UdpError{
            .kind = UdpErrorKind::HandshakeFailed,
            .message = "Fastboot UDP query response is shorter than two bytes",
        });
    }
    next_sequence_ = static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(
             std::to_integer<std::uint8_t>(query_response[0])) << 8U) |
        std::to_integer<std::uint8_t>(query_response[1]));

    const std::array<std::byte, 4> initialization_request{
        static_cast<std::byte>((kFastbootUdpProtocolVersion >> 8U) & 0xFFU),
        static_cast<std::byte>(kFastbootUdpProtocolVersion & 0xFFU),
        static_cast<std::byte>((options_.host_max_packet_bytes >> 8U) & 0xFFU),
        static_cast<std::byte>(options_.host_max_packet_bytes & 0xFFU),
    };
    std::array<std::byte, kMaximumHandshakeResponseBytes> initialization_response{};
    auto initialization = exchange_message(
        UdpPacketId::Initialization,
        initialization_request,
        initialization_response,
        initialization_response.size(),
        options_.initialization_timeout,
        options_.transmission_attempts);
    if (!initialization.success) {
        if (initialization.poison) {
            poison_locked();
        }
        return std::unexpected(UdpError{
            .kind = initialization.cancelled
                ? UdpErrorKind::Cancelled
                : (initialization.target_error
                       ? UdpErrorKind::Protocol
                       : error_kind(initialization.status)),
            .native_error = initialization.native_error,
            .message = "Fastboot UDP initialization failed: " + initialization.detail,
        });
    }
    if (initialization.response_bytes < 4) {
        poison_locked();
        return std::unexpected(UdpError{
            .kind = UdpErrorKind::HandshakeFailed,
            .message = "Fastboot UDP initialization response is shorter than four bytes",
        });
    }
    const auto version = static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(
             std::to_integer<std::uint8_t>(initialization_response[0])) << 8U) |
        std::to_integer<std::uint8_t>(initialization_response[1]));
    const auto target_packet_bytes = static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(
             std::to_integer<std::uint8_t>(initialization_response[2])) << 8U) |
        std::to_integer<std::uint8_t>(initialization_response[3]));
    if (version < kFastbootUdpProtocolVersion) {
        poison_locked();
        return std::unexpected(UdpError{
            .kind = UdpErrorKind::HandshakeFailed,
            .message = "target Fastboot UDP version is lower than version 1",
        });
    }
    if (target_packet_bytes < kFastbootUdpMinimumPacketBytes) {
        poison_locked();
        return std::unexpected(UdpError{
            .kind = UdpErrorKind::HandshakeFailed,
            .message = "target Fastboot UDP packet size is lower than 512 bytes",
        });
    }
    packet_bytes_ = std::min(options_.host_max_packet_bytes, target_packet_bytes);
    return {};
}

UdpFastbootTransport::ExchangeOutcome UdpFastbootTransport::exchange_message(
    const UdpPacketId id,
    const std::span<const std::byte> request,
    const std::span<std::byte> response,
    const std::size_t response_limit,
    const std::chrono::milliseconds timeout,
    const std::uint16_t max_attempts) {
    ExchangeOutcome outcome;
    outcome.certainty = protocol::TransferCertainty::NotTransferred;
    if (!open_ || !socket_) {
        outcome.status = protocol::TransportStatus::Disconnected;
        outcome.detail = "Fastboot UDP transport is closed";
        return outcome;
    }
    if (response_limit > response.size() || max_attempts == 0) {
        outcome.status = protocol::TransportStatus::IoError;
        outcome.poison = true;
        outcome.detail = "invalid internal Fastboot UDP exchange bounds";
        return outcome;
    }
    if (cancellation_signal().stop_requested()) {
        outcome.status = protocol::TransportStatus::IoError;
        outcome.cancelled = true;
        outcome.poison = true;
        outcome.detail = "Fastboot UDP exchange was cancelled before transmission";
        return outcome;
    }
    Deadline operation_deadline(timeout);
    if (operation_deadline.expired()) {
        outcome.status = protocol::TransportStatus::Timeout;
        outcome.detail = "Fastboot UDP operation deadline expired before transmission";
        return outcome;
    }

    const auto maximum_payload =
        static_cast<std::size_t>(packet_bytes_) - kFastbootUdpHeaderBytes;
    std::vector<std::byte> outbound;
    try {
        outbound.reserve(packet_bytes_);
    } catch (const std::bad_alloc&) {
        outcome.status = protocol::TransportStatus::IoError;
        outcome.detail = "allocating a Fastboot UDP packet failed";
        return outcome;
    }

    bool any_wire_datagram = false;
    bool unacknowledged_wire_datagram = false;
    std::size_t ignored_datagrams = 0;
    std::size_t request_offset = 0;
    std::string target_error_text;

    const auto set_certainty = [&] {
        if (!any_wire_datagram) {
            outcome.certainty = protocol::TransferCertainty::NotTransferred;
        } else if (!unacknowledged_wire_datagram &&
                   outcome.acknowledged_request_bytes == request.size()) {
            outcome.certainty = protocol::TransferCertainty::FullyTransferred;
        } else {
            outcome.certainty = protocol::TransferCertainty::PartialOrUnknown;
        }
    };
    const auto fail = [&](
                          const protocol::TransportStatus status,
                          std::string detail,
                          const bool poison,
                          const bool truncated = false,
                          const int native_error = 0) {
        outcome.status = status;
        outcome.detail = std::move(detail);
        outcome.poison = poison;
        outcome.truncated = truncated;
        outcome.native_error = native_error;
        set_certainty();
        return outcome;
    };

    bool send_once_for_empty_request = true;
    while (request_offset < request.size() || send_once_for_empty_request) {
        send_once_for_empty_request = false;
        const auto request_bytes_remaining = request.size() - request_offset;
        const auto fragment_size = std::min(request_bytes_remaining, maximum_payload);
        const auto fragment = request.subspan(request_offset, fragment_size);
        const bool more_request_fragments = request_bytes_remaining > fragment_size;

        UdpPacketId packet_id = id;
        std::uint8_t packet_flags = more_request_fragments
            ? static_cast<std::uint8_t>(UdpPacketFlag::Continuation)
            : static_cast<std::uint8_t>(UdpPacketFlag::None);
        std::span<const std::byte> packet_payload = fragment;
        bool fragment_accounted = false;
        bool fragment_complete = false;

        while (!fragment_complete) {
            const auto sequence = next_sequence_;
            const auto encoded_header = encode_udp_header({
                .id = packet_id,
                .flags = packet_flags,
                .sequence = sequence,
            });
            outbound.clear();
            outbound.insert(outbound.end(), encoded_header.begin(), encoded_header.end());
            outbound.insert(outbound.end(), packet_payload.begin(), packet_payload.end());
            if (outbound.size() > packet_bytes_) {
                return fail(
                    protocol::TransportStatus::IoError,
                    "outbound Fastboot UDP datagram exceeds the negotiated packet size",
                    true);
            }

            bool received_matching_response = false;
            UdpPacketHeader response_header;
            std::span<const std::byte> response_payload;
            for (std::uint16_t attempt = 0; attempt < max_attempts; ++attempt) {
                const auto cancellation = cancellation_signal();
                if (cancellation.stop_requested()) {
                    outcome.cancelled = true;
                    return fail(
                        protocol::TransportStatus::IoError,
                        "Fastboot UDP exchange was cancelled",
                        true);
                }
                if (operation_deadline.expired()) {
                    return fail(
                        protocol::TransportStatus::Timeout,
                        "Fastboot UDP operation deadline expired",
                        any_wire_datagram);
                }

                auto sent = socket_->send_datagram(
                    outbound,
                    peer_,
                    operation_deadline.remaining(),
                    cancellation);
                if (sent.transferred > outbound.size()) {
                    any_wire_datagram = true;
                    unacknowledged_wire_datagram = true;
                    return fail(
                        protocol::TransportStatus::IoError,
                        "UDP socket reported sending more bytes than requested",
                        true);
                }
                if (sent.status != DatagramIoStatus::Ok ||
                    sent.transferred != outbound.size()) {
                    outcome.cancelled = sent.status == DatagramIoStatus::Cancelled;
                    if (sent.transferred != 0) {
                        any_wire_datagram = true;
                        unacknowledged_wire_datagram = true;
                    }
                    return fail(
                        transport_status(sent.status),
                        datagram_failure_detail(
                            sent.status,
                            "sending Fastboot UDP datagram",
                            sent.detail),
                        sent.transferred != 0 ||
                            sent.status == DatagramIoStatus::Cancelled ||
                            sent.status == DatagramIoStatus::Truncated,
                        false,
                        sent.native_error);
                }
                any_wire_datagram = true;
                unacknowledged_wire_datagram = true;

                Deadline response_deadline(bounded_wait(
                    operation_deadline, options_.retransmit_interval));
                for (;;) {
                    const auto wait = bounded_wait(
                        operation_deadline, response_deadline.remaining());
                    if (wait <= std::chrono::milliseconds::zero()) {
                        break;
                    }
                    auto received = socket_->receive_datagram(
                        receive_packet_, wait, cancellation_signal());
                    if (received.status == DatagramIoStatus::Timeout) {
                        break;
                    }
                    if (received.status == DatagramIoStatus::Cancelled ||
                        received.status == DatagramIoStatus::Error) {
                        outcome.cancelled =
                            received.status == DatagramIoStatus::Cancelled;
                        return fail(
                            transport_status(received.status),
                            datagram_failure_detail(
                                received.status,
                                "receiving Fastboot UDP datagram",
                                received.detail),
                            true,
                            false,
                            received.native_error);
                    }
                    if (received.peer != peer_) {
                        ++ignored_datagrams;
                        if (ignored_datagrams > kMaximumIgnoredDatagrams) {
                            return fail(
                                protocol::TransportStatus::IoError,
                                "too many UDP datagrams arrived from an unexpected peer",
                                true);
                        }
                        continue;
                    }
                    if (received.status != DatagramIoStatus::Ok) {
                        return fail(
                            transport_status(received.status),
                            datagram_failure_detail(
                                received.status,
                                "receiving Fastboot UDP datagram",
                                received.detail),
                            true,
                            received.status == DatagramIoStatus::Truncated,
                            received.native_error);
                    }
                    if (received.transferred > receive_packet_.size()) {
                        return fail(
                            protocol::TransportStatus::IoError,
                            "UDP socket reported receiving more bytes than requested",
                            true,
                            true);
                    }
                    if (received.transferred < kFastbootUdpHeaderBytes) {
                        return fail(
                            protocol::TransportStatus::IoError,
                            "Fastboot UDP datagram has a truncated header",
                            true,
                            true);
                    }
                    if (received.transferred > packet_bytes_) {
                        return fail(
                            protocol::TransportStatus::IoError,
                            "Fastboot UDP datagram exceeds the negotiated packet size",
                            true,
                            true);
                    }

                    response_header = decode_udp_header(
                        std::span<const std::byte, kFastbootUdpHeaderBytes>(
                            receive_packet_.data(), kFastbootUdpHeaderBytes));
                    if (response_header.sequence != sequence ||
                        (response_header.id != packet_id &&
                         response_header.id != UdpPacketId::Error)) {
                        ++ignored_datagrams;
                        if (ignored_datagrams > kMaximumIgnoredDatagrams) {
                            return fail(
                                protocol::TransportStatus::IoError,
                                "too many out-of-order or unrelated Fastboot UDP datagrams",
                                true);
                        }
                        continue;
                    }
                    const auto continuation =
                        static_cast<std::uint8_t>(UdpPacketFlag::Continuation);
                    if ((response_header.flags & ~continuation) != 0) {
                        return fail(
                            protocol::TransportStatus::IoError,
                            "Fastboot UDP response contains unknown flag bits",
                            true);
                    }
                    response_payload = std::span<const std::byte>(
                        receive_packet_.data() + kFastbootUdpHeaderBytes,
                        received.transferred - kFastbootUdpHeaderBytes);
                    received_matching_response = true;
                    break;
                }

                if (received_matching_response) {
                    break;
                }
                if (operation_deadline.expired()) {
                    return fail(
                        protocol::TransportStatus::Timeout,
                        "Fastboot UDP operation deadline expired awaiting a response",
                        true);
                }
                if (attempt + 1U == max_attempts) {
                    return fail(
                        protocol::TransportStatus::Timeout,
                        "Fastboot UDP peer did not respond within the retransmission limit",
                        true);
                }
            }

            if (!received_matching_response) {
                return fail(
                    protocol::TransportStatus::Timeout,
                    "Fastboot UDP peer did not provide a matching response",
                    true);
            }
            unacknowledged_wire_datagram = false;
            next_sequence_ = static_cast<std::uint16_t>(next_sequence_ + 1U);
            if (!fragment_accounted) {
                std::size_t acknowledged = 0;
                if (!checked_add(
                        outcome.acknowledged_request_bytes,
                        fragment.size(),
                        acknowledged)) {
                    return fail(
                        protocol::TransportStatus::IoError,
                        "Fastboot UDP acknowledged-byte count overflowed",
                        true);
                }
                outcome.acknowledged_request_bytes = acknowledged;
                fragment_accounted = true;
            }

            if (response_header.id == UdpPacketId::Error) {
                if (response_payload.size() >
                    kMaximumErrorTextBytes - target_error_text.size()) {
                    return fail(
                        protocol::TransportStatus::IoError,
                        "Fastboot UDP target error text exceeds the bounded limit",
                        true,
                        true);
                }
                target_error_text.append(
                    reinterpret_cast<const char*>(response_payload.data()),
                    response_payload.size());
            } else {
                std::size_t new_response_size = 0;
                if (!checked_add(
                        outcome.response_bytes,
                        response_payload.size(),
                        new_response_size) ||
                    new_response_size > response_limit) {
                    return fail(
                        protocol::TransportStatus::IoError,
                        "Fastboot UDP response exceeds the allowed logical size",
                        true,
                        true);
                }
                if (!response_payload.empty()) {
                    std::ranges::copy(
                        response_payload,
                        response.begin() +
                            static_cast<std::ptrdiff_t>(outcome.response_bytes));
                }
                outcome.response_bytes = new_response_size;
            }

            const auto continuation =
                static_cast<std::uint8_t>(UdpPacketFlag::Continuation);
            if ((response_header.flags & continuation) != 0) {
                packet_id = response_header.id;
                packet_flags = static_cast<std::uint8_t>(UdpPacketFlag::None);
                packet_payload = {};
                continue;
            }
            if (response_header.id == UdpPacketId::Error) {
                outcome.target_error = true;
                auto detail = std::string{"Fastboot UDP target reported an error"};
                if (!target_error_text.empty()) {
                    detail.append(": ");
                    detail.append(target_error_text);
                }
                return fail(
                    protocol::TransportStatus::IoError,
                    std::move(detail),
                    true);
            }
            fragment_complete = true;
        }
        request_offset += fragment_size;
    }

    outcome.success = true;
    outcome.status = protocol::TransportStatus::Ok;
    outcome.certainty = protocol::TransferCertainty::FullyTransferred;
    return outcome;
}

protocol::TransferResult UdpFastbootTransport::write(
    const std::span<const std::byte> bytes,
    const std::chrono::milliseconds timeout) {
    std::scoped_lock lock(mutex_);
    if (!open_ || !socket_) {
        return {
            .status = protocol::TransportStatus::Disconnected,
            .certainty = protocol::TransferCertainty::NotTransferred,
            .detail = "Fastboot UDP transport is closed",
        };
    }
    auto outcome = exchange_message(
        UdpPacketId::Fastboot,
        bytes,
        {},
        0,
        timeout,
        options_.transmission_attempts);
    if (!outcome.success && outcome.poison) {
        poison_locked();
    }
    return {
        .status = outcome.status,
        .transferred = outcome.acknowledged_request_bytes,
        .certainty = outcome.certainty,
        .truncated = outcome.truncated,
        .detail = std::move(outcome.detail),
    };
}

protocol::TransferResult UdpFastbootTransport::read(
    const std::span<std::byte> destination,
    const std::chrono::milliseconds timeout) {
    std::scoped_lock lock(mutex_);
    if (!open_ || !socket_) {
        return {
            .status = protocol::TransportStatus::Disconnected,
            .certainty = protocol::TransferCertainty::NotTransferred,
            .detail = "Fastboot UDP transport is closed",
        };
    }
    auto outcome = exchange_message(
        UdpPacketId::Fastboot,
        {},
        destination,
        destination.size(),
        timeout,
        options_.transmission_attempts);
    if (outcome.success && outcome.response_bytes == 0) {
        outcome.success = false;
        outcome.status = protocol::TransportStatus::IoError;
        outcome.certainty = protocol::TransferCertainty::PartialOrUnknown;
        outcome.poison = true;
        outcome.detail = "Fastboot UDP returned an empty logical response";
    }
    if (!outcome.success && outcome.poison) {
        poison_locked();
    }
    return {
        .status = outcome.status,
        .transferred = outcome.response_bytes,
        .certainty = outcome.certainty,
        .truncated = outcome.truncated,
        .detail = std::move(outcome.detail),
    };
}

void UdpFastbootTransport::cancel() noexcept { local_cancel_.request_stop(); }

void UdpFastbootTransport::close() noexcept {
    std::scoped_lock lock(mutex_);
    close_locked();
}

bool UdpFastbootTransport::is_open() const noexcept {
    std::scoped_lock lock(mutex_);
    return open_;
}

bool UdpFastbootTransport::is_poisoned() const noexcept {
    std::scoped_lock lock(mutex_);
    return poisoned_;
}

std::uint16_t UdpFastbootTransport::negotiated_packet_bytes() const noexcept {
    std::scoped_lock lock(mutex_);
    return packet_bytes_;
}

UdpCancellationSignal UdpFastbootTransport::cancellation_signal() const noexcept {
    return {
        .external = options_.cancellation,
        .local = local_cancel_.get_token(),
    };
}

void UdpFastbootTransport::poison_locked() noexcept {
    poisoned_ = true;
    close_locked();
}

void UdpFastbootTransport::close_locked() noexcept {
    if (!open_) {
        return;
    }
    open_ = false;
    if (socket_) {
        socket_->close();
    }
}

std::expected<UdpSocketConnection, UdpError> connect_native_udp_socket(
    const UdpEndpoint& endpoint,
    const std::chrono::milliseconds timeout,
    const std::stop_token cancellation) {
    if (endpoint.host.empty() || endpoint.port == 0 ||
        timeout <= std::chrono::milliseconds::zero()) {
        return std::unexpected(UdpError{
            .kind = UdpErrorKind::InvalidEndpoint,
            .message = "native UDP endpoint or timeout is invalid",
        });
    }

#ifdef _WIN32
    std::unique_ptr<WinsockLease> winsock;
    try {
        winsock = std::make_unique<WinsockLease>();
    } catch (const std::bad_alloc&) {
        return std::unexpected(UdpError{
            .kind = UdpErrorKind::Io,
            .message = "allocating Winsock lifetime state failed",
        });
    }
    if (winsock->error() != 0) {
        return std::unexpected(UdpError{
            .kind = UdpErrorKind::SocketFailed,
            .native_error = winsock->error(),
            .message = "Winsock 2.2 initialization failed",
        });
    }
#endif

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
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
    const auto now = [](void*) noexcept { return Clock::now(); };
    const auto resolve_phase = detail::run_udp_resolve_phase(
        timeout,
        cancellation,
        resolve,
        &resolve_context,
        now,
        nullptr);
    const std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> address_list(
        addresses, &freeaddrinfo);
    if (resolve_phase.cancelled) {
        return std::unexpected(UdpError{
            .kind = UdpErrorKind::Cancelled,
            .message = resolve_phase.resolver_ran
                ? "UDP connect was cancelled during name resolution"
                : "UDP connect was cancelled before name resolution",
        });
    }
    if (resolve_phase.expired) {
        return std::unexpected(UdpError{
            .kind = UdpErrorKind::Timeout,
            .message = "UDP connect deadline expired during name resolution",
        });
    }
    if (resolve_context.result != 0) {
        return std::unexpected(UdpError{
            .kind = UdpErrorKind::ResolveFailed,
            .native_error = resolve_context.result,
            .message = "getaddrinfo failed for UDP endpoint with code " +
                       std::to_string(resolve_context.result),
        });
    }

    Deadline deadline(resolve_phase.deadline);
    int last_error = 0;
    std::string last_detail = "no usable UDP address candidate";
    for (auto* address = address_list.get(); address != nullptr; address = address->ai_next) {
        if (cancellation.stop_requested()) {
            return std::unexpected(UdpError{
                .kind = UdpErrorKind::Cancelled,
                .message = "UDP socket creation was cancelled",
            });
        }
        if (deadline.expired()) {
            return std::unexpected(UdpError{
                .kind = UdpErrorKind::Timeout,
                .message = "UDP socket creation deadline expired",
            });
        }
        if (address->ai_addr == nullptr || address->ai_addrlen == 0 ||
            static_cast<std::size_t>(address->ai_addrlen) > sizeof(sockaddr_storage)) {
            last_detail = "resolver returned an invalid UDP address length";
            continue;
        }
        auto peer = peer_from_sockaddr(
            address->ai_addr, static_cast<std::size_t>(address->ai_addrlen));
        if (!peer) {
            last_detail = peer.error().message;
            continue;
        }

        NativeSocketGuard socket_guard(::socket(
            address->ai_family, address->ai_socktype, address->ai_protocol));
        if (socket_guard.get() == kInvalidNativeSocket) {
            last_error = last_socket_error();
            last_detail = native_error_message(last_error);
            continue;
        }
        if (!set_nonblocking(socket_guard.get())) {
            last_error = last_socket_error();
            last_detail = native_error_message(last_error);
            continue;
        }
#ifndef _WIN32
#ifdef SO_NOSIGPIPE
        int enabled = 1;
        static_cast<void>(setsockopt(
            socket_guard.get(), SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)));
#endif
#endif

        sockaddr_storage peer_address{};
        std::memcpy(
            &peer_address,
            address->ai_addr,
            static_cast<std::size_t>(address->ai_addrlen));
        try {
            auto socket = std::make_unique<NativeUdpSocket>(
                socket_guard.get(),
                peer_address,
                static_cast<std::size_t>(address->ai_addrlen),
                *peer
#ifdef _WIN32
                ,
                std::move(winsock)
#endif
                );
            static_cast<void>(socket_guard.release());
            return UdpSocketConnection{
                .socket = std::unique_ptr<IUdpSocket>(std::move(socket)),
                .peer = *peer,
            };
        } catch (const std::bad_alloc&) {
            return std::unexpected(UdpError{
                .kind = UdpErrorKind::Io,
                .message = "allocating native UDP socket ownership failed",
            });
        }
    }

    return std::unexpected(UdpError{
        .kind = UdpErrorKind::SocketFailed,
        .native_error = last_error,
        .message = "failed to create UDP socket: " + last_detail,
    });
}

std::expected<std::unique_ptr<UdpFastbootTransport>, UdpError>
connect_udp_fastboot(
    const std::string_view endpoint,
    UdpTransportOptions options) {
    auto parsed = parse_udp_endpoint(endpoint);
    if (!parsed) {
        return std::unexpected(parsed.error());
    }
    auto connection = connect_native_udp_socket(
        *parsed, options.connect_timeout, options.cancellation);
    if (!connection) {
        return std::unexpected(connection.error());
    }
    return UdpFastbootTransport::create(
        std::move(connection->socket), connection->peer, options);
}

}  // namespace kairosboot::transport
