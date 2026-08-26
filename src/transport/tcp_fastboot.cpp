// SPDX-License-Identifier: MIT
#include "tcp_fastboot.hpp"

#include <boost/asio/buffer.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/system/error_code.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cctype>
#include <limits>
#include <new>
#include <string>
#include <system_error>
#include <utility>

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
            return protocol::TransportStatus::Cancelled;
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
        .native_code = result.native_error,
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

using AsioErrorCode = boost::system::error_code;
using AsioTcp = boost::asio::ip::tcp;

struct AsioOperationResult {
    AsioErrorCode error{};
    std::size_t transferred{0};
    bool completed{false};
};

[[nodiscard]] SocketIoResult socket_error_result(
    const AsioErrorCode& error,
    const std::size_t transferred = 0) {
    return {
        .status = SocketIoStatus::Error,
        .transferred = transferred,
        .native_error = error.value(),
        .detail = error.message(),
    };
}

void cancel_pending_operation(
    boost::asio::io_context& context,
    AsioTcp::socket& socket,
    AsioOperationResult& operation) {
    AsioErrorCode ignored;
    socket.cancel(ignored);
    if (ignored) {
        socket.close(ignored);
    }
    if (context.stopped()) {
        context.restart();
    }
    while (!operation.completed && context.run_one() != 0) {
    }
}

[[nodiscard]] SocketIoResult run_until_complete(
    boost::asio::io_context& context,
    AsioTcp::socket& socket,
    AsioOperationResult& operation,
    Deadline& deadline,
    const CancellationSignal cancellation) {
    constexpr auto poll_slice = std::chrono::milliseconds(50);
    while (!operation.completed) {
        if (cancellation.stop_requested()) {
            cancel_pending_operation(context, socket, operation);
            return {
                .status = SocketIoStatus::Cancelled,
                .transferred = operation.transferred,
                .detail = "cancellation requested",
            };
        }

        if (context.stopped()) {
            context.restart();
        }
        static_cast<void>(context.poll_one());
        if (operation.completed) {
            break;
        }

        const auto remaining = deadline.remaining();
        if (remaining <= std::chrono::milliseconds::zero()) {
            cancel_pending_operation(context, socket, operation);
            return {
                .status = SocketIoStatus::Timeout,
                .transferred = operation.transferred,
                .detail = "socket operation deadline expired",
            };
        }
        const auto slice = remaining == std::chrono::milliseconds::max()
            ? poll_slice
            : std::min(remaining, poll_slice);
        static_cast<void>(context.run_one_for(slice));
    }
    return {};
}

class NativeTcpSocket final : public ITcpSocket {
public:
    NativeTcpSocket() : socket_(context_) {}
    ~NativeTcpSocket() override { close(); }

    [[nodiscard]] SocketIoResult connect(
        const AsioTcp::endpoint& endpoint,
        Deadline& deadline,
        const std::stop_token cancellation) {
        AsioErrorCode error;
        socket_.open(endpoint.protocol(), error);
        if (error) {
            return socket_error_result(error);
        }

        AsioOperationResult operation;
        context_.restart();
        socket_.async_connect(
            endpoint,
            [&](const AsioErrorCode& connect_error) noexcept {
                operation.error = connect_error;
                operation.completed = true;
            });
        auto connect_result = run_until_complete(
            context_,
            socket_,
            operation,
            deadline,
            CancellationSignal{.external = cancellation});
        if (connect_result.status != SocketIoStatus::Ok) {
            return connect_result;
        }
        if (operation.error) {
            return socket_error_result(operation.error);
        }
        AsioErrorCode ignored;
        socket_.set_option(AsioTcp::no_delay(true), ignored);
        return {};
    }

    [[nodiscard]] SocketIoResult send_some(
        const std::span<const std::byte> bytes,
        const std::chrono::milliseconds timeout,
        const CancellationSignal cancellation) override {
        if (bytes.empty()) {
            return {};
        }
        if (!socket_.is_open()) {
            return {
                .status = SocketIoStatus::EndOfStream,
                .detail = "socket is closed",
            };
        }

        Deadline deadline(timeout);
        AsioOperationResult operation;
        if (context_.stopped()) {
            context_.restart();
        }
        socket_.async_send(
            boost::asio::buffer(bytes.data(), bytes.size()),
            [&](const AsioErrorCode& error, const std::size_t transferred) noexcept {
                operation.error = error;
                operation.transferred = transferred;
                operation.completed = true;
            });
        auto send_result = run_until_complete(
            context_, socket_, operation, deadline, cancellation);
        if (send_result.status != SocketIoStatus::Ok) {
            return send_result;
        }
        if (operation.error) {
            return socket_error_result(operation.error, operation.transferred);
        }
        if (operation.transferred == 0) {
            return {
                .status = SocketIoStatus::EndOfStream,
                .detail = "socket closed during send",
            };
        }
        return {
            .transferred = operation.transferred,
        };
    }

    [[nodiscard]] SocketIoResult receive_some(
        const std::span<std::byte> destination,
        const std::chrono::milliseconds timeout,
        const CancellationSignal cancellation) override {
        if (destination.empty()) {
            return {};
        }
        if (!socket_.is_open()) {
            return {
                .status = SocketIoStatus::EndOfStream,
                .detail = "socket is closed",
            };
        }

        Deadline deadline(timeout);
        AsioOperationResult operation;
        if (context_.stopped()) {
            context_.restart();
        }
        socket_.async_receive(
            boost::asio::buffer(destination.data(), destination.size()),
            [&](const AsioErrorCode& error, const std::size_t transferred) noexcept {
                operation.error = error;
                operation.transferred = transferred;
                operation.completed = true;
            });
        auto receive_result = run_until_complete(
            context_, socket_, operation, deadline, cancellation);
        if (receive_result.status != SocketIoStatus::Ok) {
            return receive_result;
        }
        if (operation.error == boost::asio::error::eof) {
            return {
                .status = SocketIoStatus::EndOfStream,
                .transferred = operation.transferred,
                .detail = "peer closed the TCP connection",
            };
        }
        if (operation.error) {
            return socket_error_result(operation.error, operation.transferred);
        }
        if (operation.transferred == 0) {
            return {
                .status = SocketIoStatus::EndOfStream,
                .detail = "peer closed the TCP connection",
            };
        }
        return {
            .transferred = operation.transferred,
        };
    }

    void close() noexcept override {
        if (!socket_.is_open()) {
            return;
        }
        AsioErrorCode ignored;
        socket_.cancel(ignored);
        socket_.close(ignored);
    }

private:
    boost::asio::io_context context_;
    AsioTcp::socket socket_;
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
    if (inbound_data_frame_bytes_left_ != 0) {
        close_locked();
        return {
            .status = protocol::TransportStatus::IoError,
            .certainty = protocol::TransferCertainty::PartialOrUnknown,
            .truncated = true,
            .detail = "Fastboot TCP DATA frame exceeded the announced payload size",
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

protocol::TransferResult TcpFastbootTransport::read_data(
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
    if (destination.empty()) {
        return {
            .status = protocol::TransportStatus::IoError,
            .certainty = protocol::TransferCertainty::NotTransferred,
            .detail = "Fastboot TCP DATA destination is empty",
        };
    }

    Deadline deadline(timeout);
    const auto cancellation = cancellation_signal();
    bool consumed_frame_header = false;
    if (inbound_data_frame_bytes_left_ == 0) {
        std::array<std::byte, 8> header{};
        const auto header_result = receive_exact(
            *socket_, header, deadline, cancellation);
        if (header_result.status != SocketIoStatus::Ok) {
            auto failure = transfer_failure(
                header_result,
                0,
                header_result.any_wire_bytes,
                "receiving Fastboot TCP DATA frame header");
            close_locked();
            return failure;
        }

        inbound_data_frame_bytes_left_ = decode_tcp_frame_length(header);
        consumed_frame_header = true;
        if (inbound_data_frame_bytes_left_ > options_.max_frame_bytes ||
            inbound_data_frame_bytes_left_ >
                std::numeric_limits<std::size_t>::max()) {
            close_locked();
            return {
                .status = protocol::TransportStatus::IoError,
                .certainty = protocol::TransferCertainty::PartialOrUnknown,
                .truncated = true,
                .detail = "inbound Fastboot TCP DATA frame exceeds its configured limit",
            };
        }
        if (inbound_data_frame_bytes_left_ == 0) {
            return {
                .status = protocol::TransportStatus::Ok,
                .transferred = 0,
                .certainty = protocol::TransferCertainty::FullyTransferred,
            };
        }
    }

    const auto payload_size = std::min(
        destination.size(),
        static_cast<std::size_t>(inbound_data_frame_bytes_left_));
    const auto payload_result = receive_exact(
        *socket_, destination.first(payload_size), deadline, cancellation);
    if (payload_result.status != SocketIoStatus::Ok) {
        auto failure = transfer_failure(
            payload_result,
            payload_result.transferred,
            consumed_frame_header || payload_result.any_wire_bytes,
            "receiving Fastboot TCP DATA frame payload");
        close_locked();
        return failure;
    }

    inbound_data_frame_bytes_left_ -= payload_size;
    return {
        .status = protocol::TransportStatus::Ok,
        .transferred = payload_size,
        .certainty = protocol::TransferCertainty::FullyTransferred,
    };
}

void TcpFastbootTransport::request_cancel() noexcept {
    local_cancel_.request_stop();
}

void TcpFastbootTransport::cancel() noexcept { request_cancel(); }

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
    inbound_data_frame_bytes_left_ = 0;
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

    boost::asio::io_context resolver_context;
    AsioTcp::resolver resolver(resolver_context);
    AsioTcp::resolver::results_type addresses;
    const auto service = std::to_string(endpoint.port);
    struct ResolveContext {
        AsioTcp::resolver* resolver;
        const std::string* host;
        const std::string* service;
        AsioTcp::resolver::results_type* addresses;
        AsioErrorCode error{};
        bool allocation_failed{false};
        bool unexpected_failure{false};
    } resolve_context{
        .resolver = &resolver,
        .host = &endpoint.host,
        .service = &service,
        .addresses = &addresses,
    };
    const auto resolve = [](void* const opaque) noexcept {
        auto& context = *static_cast<ResolveContext*>(opaque);
        try {
            *context.addresses = context.resolver->resolve(
                *context.host, *context.service, context.error);
        } catch (const std::bad_alloc&) {
            context.allocation_failed = true;
        } catch (...) {
            context.unexpected_failure = true;
        }
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
    if (resolve_context.allocation_failed) {
        return std::unexpected(TcpError{
            .kind = TcpErrorKind::Io,
            .message = "allocating TCP resolver results failed",
        });
    }
    if (resolve_context.unexpected_failure) {
        return std::unexpected(TcpError{
            .kind = TcpErrorKind::ResolveFailed,
            .message = "TCP resolver failed unexpectedly",
        });
    }
    if (resolve_context.error) {
        return std::unexpected(TcpError{
            .kind = TcpErrorKind::ResolveFailed,
            .native_error = resolve_context.error.value(),
            .message = "getaddrinfo failed for TCP endpoint with code " +
                       std::to_string(resolve_context.error.value()),
        });
    }

    Deadline deadline(resolve_phase.deadline);
    int last_error = 0;
    std::string last_detail = "no address candidate connected";
    for (const auto& address : addresses) {
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

        auto owned_socket = std::make_unique<NativeTcpSocket>();
        const auto connected = owned_socket->connect(
            address.endpoint(), deadline, cancellation);
        if (connected.status == SocketIoStatus::Timeout) {
            return std::unexpected(TcpError{
                .kind = TcpErrorKind::Timeout,
                .message = "TCP connect deadline expired",
            });
        }
        if (connected.status == SocketIoStatus::Cancelled) {
            return std::unexpected(TcpError{
                .kind = TcpErrorKind::Cancelled,
                .message = "TCP connect was cancelled",
            });
        }
        if (connected.status != SocketIoStatus::Ok) {
            last_error = connected.native_error;
            last_detail = connected.detail;
            continue;
        }
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
