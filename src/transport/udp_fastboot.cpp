// SPDX-License-Identifier: MIT
#include "udp_fastboot.hpp"

#include <boost/asio/buffer.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/udp.hpp>
#include <boost/system/system_error.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cctype>
#include <exception>
#include <limits>
#include <new>
#include <ranges>
#include <system_error>
#include <utility>
#include <vector>

namespace kairosboot::transport {
namespace {

namespace asio = boost::asio;
using AsioUdp = asio::ip::udp;
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
            return protocol::TransportStatus::Cancelled;
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
        case protocol::TransportStatus::Cancelled:
            return UdpErrorKind::Cancelled;
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

enum class AsioOperationStatus : std::uint8_t {
    Completed,
    Timeout,
    Cancelled,
    Error,
};

struct AsioOperationResult {
    AsioOperationStatus status{AsioOperationStatus::Error};
    std::size_t transferred{0};
    boost::system::error_code error{};
    std::string detail{};
};

template <typename Initiate>
[[nodiscard]] AsioOperationResult run_asio_datagram_operation(
    asio::io_context& context,
    AsioUdp::socket& socket,
    const std::chrono::milliseconds timeout,
    const UdpCancellationSignal cancellation,
    Initiate&& initiate) {
    if (cancellation.stop_requested()) {
        return {
            .status = AsioOperationStatus::Cancelled,
            .detail = "cancellation requested",
        };
    }

    Deadline deadline(timeout);
    if (deadline.expired()) {
        return {
            .status = AsioOperationStatus::Timeout,
            .detail = "socket readiness deadline expired",
        };
    }

    struct Completion {
        bool finished{false};
        std::size_t transferred{0};
        boost::system::error_code error;
    } completion;

    if (context.stopped()) {
        context.restart();
    }
    try {
        std::forward<Initiate>(initiate)(
            [&completion](
                const boost::system::error_code& error,
                const std::size_t transferred) noexcept {
                completion.finished = true;
                completion.transferred = transferred;
                completion.error = error;
            });
    } catch (const std::bad_alloc&) {
        return {
            .status = AsioOperationStatus::Error,
            .detail = "allocating a Boost.Asio UDP operation failed",
        };
    } catch (const boost::system::system_error& error) {
        return {
            .status = AsioOperationStatus::Error,
            .error = error.code(),
            .detail = error.what(),
        };
    } catch (const std::exception& error) {
        return {
            .status = AsioOperationStatus::Error,
            .detail = error.what(),
        };
    } catch (...) {
        return {
            .status = AsioOperationStatus::Error,
            .detail = "starting a Boost.Asio UDP operation failed",
        };
    }

    const auto cancel_and_drain = [&context, &socket, &completion](
                                      const AsioOperationStatus status,
                                      std::string detail,
                                      boost::system::error_code error = {}) {
        boost::system::error_code cancel_error;
        socket.cancel(cancel_error);
        if (cancel_error) {
            boost::system::error_code ignored;
            socket.close(ignored);
            if (!error) {
                error = cancel_error;
            }
            detail.append(": ");
            detail.append(cancel_error.message());
        }
        try {
            while (!completion.finished) {
                if (context.stopped()) {
                    context.restart();
                }
                if (context.run_one() == 0) {
                    break;
                }
            }
        } catch (const boost::system::system_error& drain_error) {
            if (!error) {
                error = drain_error.code();
            }
            detail.append(": ");
            detail.append(drain_error.what());
        } catch (...) {
            detail.append(": draining the cancelled UDP operation failed");
        }
        return AsioOperationResult{
            .status = status,
            .transferred = completion.transferred,
            .error = error,
            .detail = std::move(detail),
        };
    };

    constexpr auto cancellation_slice = std::chrono::milliseconds(50);
    for (;;) {
        if (completion.finished) {
            return {
                .status = AsioOperationStatus::Completed,
                .transferred = completion.transferred,
                .error = completion.error,
            };
        }
        if (cancellation.stop_requested()) {
            return cancel_and_drain(
                AsioOperationStatus::Cancelled,
                "cancellation requested");
        }
        if (deadline.expired()) {
            return cancel_and_drain(
                AsioOperationStatus::Timeout,
                "socket readiness deadline expired");
        }

        const auto slice = bounded_wait(deadline, cancellation_slice);
        try {
            static_cast<void>(context.run_for(slice));
        } catch (const boost::system::system_error& error) {
            return cancel_and_drain(
                AsioOperationStatus::Error,
                error.what(),
                error.code());
        } catch (const std::exception& error) {
            return cancel_and_drain(
                AsioOperationStatus::Error,
                error.what());
        } catch (...) {
            return cancel_and_drain(
                AsioOperationStatus::Error,
                "running a Boost.Asio UDP operation failed");
        }
    }
}

[[nodiscard]] std::expected<UdpPeer, UdpError> peer_from_endpoint(
    const AsioUdp::endpoint& endpoint) {
    UdpPeer peer;
    const auto address = endpoint.address();
    if (address.is_v4()) {
        const auto bytes = address.to_v4().to_bytes();
        peer.family = UdpAddressFamily::Ipv4;
        for (std::size_t index = 0; index < bytes.size(); ++index) {
            peer.address[index] = static_cast<std::byte>(bytes[index]);
        }
        peer.port = endpoint.port();
        return peer;
    }
    if (address.is_v6()) {
        const auto ipv6 = address.to_v6();
        const auto bytes = ipv6.to_bytes();
        if (ipv6.scope_id() > std::numeric_limits<std::uint32_t>::max()) {
            return std::unexpected(UdpError{
                .kind = UdpErrorKind::SocketFailed,
                .message = "resolved UDP IPv6 scope ID exceeds 32 bits",
            });
        }
        peer.family = UdpAddressFamily::Ipv6;
        for (std::size_t index = 0; index < bytes.size(); ++index) {
            peer.address[index] = static_cast<std::byte>(bytes[index]);
        }
        peer.port = endpoint.port();
        peer.scope_id = static_cast<std::uint32_t>(ipv6.scope_id());
        return peer;
    }
    return std::unexpected(UdpError{
        .kind = UdpErrorKind::SocketFailed,
        .message = "resolved UDP address has an unsupported family or size",
    });
}

class NativeUdpSocket final : public IUdpSocket {
public:
    NativeUdpSocket(AsioUdp::endpoint peer_endpoint, UdpPeer peer)
        : socket_(context_),
          peer_endpoint_(std::move(peer_endpoint)),
          peer_(peer) {}

    ~NativeUdpSocket() override { close(); }

    [[nodiscard]] boost::system::error_code open() noexcept {
        boost::system::error_code error;
        socket_.open(peer_endpoint_.protocol(), error);
        return error;
    }

    [[nodiscard]] DatagramSendResult send_datagram(
        const std::span<const std::byte> datagram,
        const UdpPeer& peer,
        const std::chrono::milliseconds timeout,
        const UdpCancellationSignal cancellation) override {
        if (!socket_.is_open()) {
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

        auto result = run_asio_datagram_operation(
            context_,
            socket_,
            timeout,
            cancellation,
            [this, datagram](auto&& handler) {
                socket_.async_send_to(
                    asio::buffer(datagram.data(), datagram.size()),
                    peer_endpoint_,
                    std::forward<decltype(handler)>(handler));
            });
        if (result.status != AsioOperationStatus::Completed) {
            return {
                .status = result.status == AsioOperationStatus::Timeout
                    ? DatagramIoStatus::Timeout
                    : (result.status == AsioOperationStatus::Cancelled
                           ? DatagramIoStatus::Cancelled
                           : DatagramIoStatus::Error),
                .transferred = result.transferred,
                .native_error = result.error.value(),
                .detail = std::move(result.detail),
            };
        }
        if (result.error) {
            return {
                .status = DatagramIoStatus::Error,
                .transferred = result.transferred,
                .native_error = result.error.value(),
                .detail = result.error.message(),
            };
        }
        return {
            .status = result.transferred == datagram.size()
                ? DatagramIoStatus::Ok
                : DatagramIoStatus::Error,
            .transferred = result.transferred,
            .detail = result.transferred == datagram.size()
                ? std::string{}
                : std::string{"UDP socket reported a partial datagram send"},
        };
    }

    [[nodiscard]] DatagramReceiveResult receive_datagram(
        const std::span<std::byte> destination,
        const std::chrono::milliseconds timeout,
        const UdpCancellationSignal cancellation) override {
        if (!socket_.is_open()) {
            return {
                .status = DatagramIoStatus::Error,
                .detail = "UDP socket is closed",
            };
        }
        if (destination.size() == std::numeric_limits<std::size_t>::max()) {
            return {
                .status = DatagramIoStatus::Error,
                .detail = "UDP receive buffer size cannot be represented",
            };
        }
        try {
            // Boost.Asio does not report datagram truncation consistently on
            // every platform. One extra byte makes oversized datagrams
            // observable even when the native receive completes successfully.
            receive_buffer_.resize(destination.size() + 1U);
        } catch (const std::bad_alloc&) {
            return {
                .status = DatagramIoStatus::Error,
                .detail = "allocating the Boost.Asio UDP receive buffer failed",
            };
        }

        AsioUdp::endpoint sender;
        auto result = run_asio_datagram_operation(
            context_,
            socket_,
            timeout,
            cancellation,
            [this, &sender](auto&& handler) {
                socket_.async_receive_from(
                    asio::buffer(receive_buffer_.data(), receive_buffer_.size()),
                    sender,
                    std::forward<decltype(handler)>(handler));
            });
        if (result.status != AsioOperationStatus::Completed) {
            return {
                .status = result.status == AsioOperationStatus::Timeout
                    ? DatagramIoStatus::Timeout
                    : (result.status == AsioOperationStatus::Cancelled
                           ? DatagramIoStatus::Cancelled
                           : DatagramIoStatus::Error),
                .transferred = result.transferred,
                .native_error = result.error.value(),
                .detail = std::move(result.detail),
            };
        }

        const bool truncated = result.error == asio::error::message_size ||
                               result.transferred > destination.size();
        if (result.error && !truncated) {
            return {
                .status = DatagramIoStatus::Error,
                .transferred = result.transferred,
                .native_error = result.error.value(),
                .detail = result.error.message(),
            };
        }
        const auto copied = std::min(result.transferred, destination.size());
        std::ranges::copy_n(receive_buffer_.begin(), copied, destination.begin());
        auto sender_peer = peer_from_endpoint(sender);
        if (!sender_peer) {
            return {
                .status = truncated
                    ? DatagramIoStatus::Truncated
                    : DatagramIoStatus::Error,
                .transferred = truncated
                    ? destination.size()
                    : result.transferred,
                .native_error = result.error.value(),
                .detail = truncated
                    ? "oversized UDP datagram had an invalid source address"
                    : sender_peer.error().message,
            };
        }
        return {
            .status = truncated
                ? DatagramIoStatus::Truncated
                : DatagramIoStatus::Ok,
            .transferred = truncated
                ? destination.size()
                : result.transferred,
            .peer = *sender_peer,
            .native_error = result.error.value(),
            .detail = truncated
                ? std::string{"UDP datagram exceeded the receive buffer"}
                : std::string{},
        };
    }

    void close() noexcept override {
        boost::system::error_code ignored;
        socket_.cancel(ignored);
        socket_.close(ignored);
    }

private:
    asio::io_context context_;
    AsioUdp::socket socket_;
    AsioUdp::endpoint peer_endpoint_;
    UdpPeer peer_;
    std::vector<std::byte> receive_buffer_;
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
        outcome.status = protocol::TransportStatus::Cancelled;
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
                        protocol::TransportStatus::Cancelled,
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
        .native_code = outcome.native_error,
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
        .native_code = outcome.native_error,
    };
}

protocol::TransferResult UdpFastbootTransport::read_data(
    const std::span<std::byte> destination,
    const std::chrono::milliseconds timeout) {
    return read(destination, timeout);
}

void UdpFastbootTransport::request_cancel() noexcept {
    local_cancel_.request_stop();
}

void UdpFastbootTransport::cancel() noexcept { request_cancel(); }

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
    if (endpoint.host.empty() || endpoint.port == 0) {
        return std::unexpected(UdpError{
            .kind = UdpErrorKind::InvalidEndpoint,
            .message = "native UDP endpoint is invalid",
        });
    }
    if (timeout <= std::chrono::milliseconds::zero()) {
        return std::unexpected(UdpError{
            .kind = UdpErrorKind::Timeout,
            .message = "UDP connect deadline expired before name resolution",
        });
    }

    asio::io_context resolve_io;
    AsioUdp::resolver resolver(resolve_io);
    const auto service = std::to_string(endpoint.port);
    struct ResolveContext {
        AsioUdp::resolver* resolver;
        const std::string* host;
        const std::string* service;
        AsioUdp::resolver::results_type results{};
        boost::system::error_code error{};
        bool allocation_failed{false};
        bool unexpected_failure{false};
    } resolve_context{
        .resolver = &resolver,
        .host = &endpoint.host,
        .service = &service,
    };
    const auto resolve = [](void* const opaque) noexcept {
        auto& context = *static_cast<ResolveContext*>(opaque);
        try {
            context.results = context.resolver->resolve(
                *context.host, *context.service, context.error);
        } catch (const std::bad_alloc&) {
            context.allocation_failed = true;
        } catch (const boost::system::system_error& error) {
            context.error = error.code();
        } catch (...) {
            context.unexpected_failure = true;
        }
    };
    const auto now = [](void*) noexcept { return Clock::now(); };
    const auto resolve_phase = detail::run_udp_resolve_phase(
        timeout,
        cancellation,
        resolve,
        &resolve_context,
        now,
        nullptr);
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
    if (resolve_context.allocation_failed) {
        return std::unexpected(UdpError{
            .kind = UdpErrorKind::Io,
            .message = "allocating Boost.Asio UDP resolver results failed",
        });
    }
    if (resolve_context.unexpected_failure) {
        return std::unexpected(UdpError{
            .kind = UdpErrorKind::ResolveFailed,
            .message = "Boost.Asio UDP resolution failed unexpectedly",
        });
    }
    if (resolve_context.error) {
        return std::unexpected(UdpError{
            .kind = UdpErrorKind::ResolveFailed,
            .native_error = resolve_context.error.value(),
            .message = "Boost.Asio UDP resolution failed: " +
                       resolve_context.error.message(),
        });
    }

    Deadline deadline(resolve_phase.deadline);
    int last_error = 0;
    std::string last_detail = "no usable UDP address candidate";
    for (const auto& resolved : resolve_context.results) {
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
        const auto peer_endpoint = resolved.endpoint();
        auto peer = peer_from_endpoint(peer_endpoint);
        if (!peer) {
            last_detail = peer.error().message;
            continue;
        }

        try {
            auto socket = std::make_unique<NativeUdpSocket>(
                peer_endpoint, *peer);
            const auto open_error = socket->open();
            if (open_error) {
                last_error = open_error.value();
                last_detail = open_error.message();
                continue;
            }
            return UdpSocketConnection{
                .socket = std::unique_ptr<IUdpSocket>(std::move(socket)),
                .peer = *peer,
            };
        } catch (const std::bad_alloc&) {
            return std::unexpected(UdpError{
                .kind = UdpErrorKind::Io,
                .message = "allocating native UDP socket ownership failed",
            });
        } catch (const boost::system::system_error& error) {
            last_error = error.code().value();
            last_detail = error.what();
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
