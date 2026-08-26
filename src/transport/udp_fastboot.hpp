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
#include <vector>

namespace kairosboot::transport {

inline constexpr std::uint16_t kFastbootUdpDefaultPort = 5554;
inline constexpr std::uint16_t kFastbootUdpProtocolVersion = 1;
inline constexpr std::uint16_t kFastbootUdpMinimumPacketBytes = 512;
inline constexpr std::uint16_t kFastbootUdpHostMaximumPacketBytes = 8192;
inline constexpr std::size_t kFastbootUdpHeaderBytes = 4;

enum class UdpPacketId : std::uint8_t {
    Error = 0x00,
    Query = 0x01,
    Initialization = 0x02,
    Fastboot = 0x03,
};

enum class UdpPacketFlag : std::uint8_t {
    None = 0x00,
    Continuation = 0x01,
};

struct UdpPacketHeader {
    UdpPacketId id{UdpPacketId::Error};
    std::uint8_t flags{0};
    std::uint16_t sequence{0};
};

[[nodiscard]] std::array<std::byte, kFastbootUdpHeaderBytes> encode_udp_header(
    UdpPacketHeader header) noexcept;

[[nodiscard]] UdpPacketHeader decode_udp_header(
    std::span<const std::byte, kFastbootUdpHeaderBytes> bytes) noexcept;

enum class UdpErrorKind : std::uint8_t {
    InvalidEndpoint,
    ResolveFailed,
    SocketFailed,
    HandshakeFailed,
    Timeout,
    Cancelled,
    Protocol,
    Io,
};

struct UdpError {
    UdpErrorKind kind{UdpErrorKind::Io};
    int native_error{0};
    std::string message;
};

struct UdpEndpoint {
    std::string host;
    std::uint16_t port{kFastbootUdpDefaultPort};
};

enum class UdpAddressFamily : std::uint8_t {
    Ipv4,
    Ipv6,
};

// Binary peer identity avoids textual IPv6 aliases during source validation.
struct UdpPeer {
    UdpAddressFamily family{UdpAddressFamily::Ipv4};
    std::array<std::byte, 16> address{};
    std::uint16_t port{0};
    std::uint32_t scope_id{0};

    [[nodiscard]] bool operator==(const UdpPeer&) const noexcept = default;
};

[[nodiscard]] std::expected<UdpEndpoint, UdpError> parse_udp_endpoint(
    std::string_view text,
    std::uint16_t default_port = kFastbootUdpDefaultPort);

namespace detail {

using UdpConnectClock = std::chrono::steady_clock;
using UdpConnectClockNow = UdpConnectClock::time_point (*)(void*) noexcept;
using UdpResolveWork = void (*)(void*) noexcept;

// Deterministic seam ensuring DNS consumes the same absolute deadline as
// native socket creation.
struct UdpResolvePhaseResult {
    UdpConnectClock::time_point deadline;
    bool resolver_ran{false};
    bool cancelled{false};
    bool expired{false};
};

[[nodiscard]] UdpResolvePhaseResult run_udp_resolve_phase(
    std::chrono::milliseconds timeout,
    std::stop_token cancellation,
    UdpResolveWork resolve,
    void* resolve_context,
    UdpConnectClockNow now,
    void* clock_context) noexcept;

}  // namespace detail

enum class DatagramIoStatus : std::uint8_t {
    Ok,
    Timeout,
    Cancelled,
    Truncated,
    Error,
};

struct DatagramSendResult {
    DatagramIoStatus status{DatagramIoStatus::Ok};
    std::size_t transferred{0};
    int native_error{0};
    std::string detail;
};

struct DatagramReceiveResult {
    DatagramIoStatus status{DatagramIoStatus::Ok};
    std::size_t transferred{0};
    UdpPeer peer{};
    int native_error{0};
    std::string detail;
};

struct UdpCancellationSignal {
    std::stop_token external{};
    std::stop_token local{};

    [[nodiscard]] bool stop_requested() const noexcept {
        return external.stop_requested() || local.stop_requested();
    }
};

class IUdpSocket {
public:
    virtual ~IUdpSocket() = default;

    [[nodiscard]] virtual DatagramSendResult send_datagram(
        std::span<const std::byte> datagram,
        const UdpPeer& peer,
        std::chrono::milliseconds timeout,
        UdpCancellationSignal cancellation) = 0;

    [[nodiscard]] virtual DatagramReceiveResult receive_datagram(
        std::span<std::byte> destination,
        std::chrono::milliseconds timeout,
        UdpCancellationSignal cancellation) = 0;

    virtual void close() noexcept = 0;
};

struct UdpTransportOptions {
    std::chrono::milliseconds connect_timeout{5'000};
    std::chrono::milliseconds query_timeout{2'000};
    std::chrono::milliseconds initialization_timeout{60'000};
    std::chrono::milliseconds retransmit_interval{500};
    std::uint16_t host_max_packet_bytes{kFastbootUdpHostMaximumPacketBytes};
    std::uint16_t query_attempts{4};
    std::uint16_t transmission_attempts{120};
    std::stop_token cancellation;
};

class UdpFastbootTransport final : public protocol::ITransportSession {
public:
    [[nodiscard]] static std::expected<std::unique_ptr<UdpFastbootTransport>, UdpError> create(
        std::unique_ptr<IUdpSocket> socket,
        UdpPeer peer,
        UdpTransportOptions options = {});

    ~UdpFastbootTransport() override;

    UdpFastbootTransport(const UdpFastbootTransport&) = delete;
    UdpFastbootTransport& operator=(const UdpFastbootTransport&) = delete;
    UdpFastbootTransport(UdpFastbootTransport&&) = delete;
    UdpFastbootTransport& operator=(UdpFastbootTransport&&) = delete;

    [[nodiscard]] protocol::TransferResult write(
        std::span<const std::byte> bytes,
        std::chrono::milliseconds timeout) override;

    [[nodiscard]] protocol::TransferResult read(
        std::span<std::byte> destination,
        std::chrono::milliseconds timeout) override;

    void request_cancel() noexcept override;
    void cancel() noexcept;
    void close() noexcept override;
    [[nodiscard]] bool is_open() const noexcept;
    [[nodiscard]] bool is_poisoned() const noexcept;
    [[nodiscard]] std::uint16_t negotiated_packet_bytes() const noexcept;

private:
    struct ExchangeOutcome {
        protocol::TransportStatus status{protocol::TransportStatus::Ok};
        protocol::TransferCertainty certainty{
            protocol::TransferCertainty::FullyTransferred};
        std::size_t acknowledged_request_bytes{0};
        std::size_t response_bytes{0};
        bool success{false};
        bool truncated{false};
        bool target_error{false};
        bool cancelled{false};
        bool poison{false};
        int native_error{0};
        std::string detail;
    };

    UdpFastbootTransport(
        std::unique_ptr<IUdpSocket> socket,
        UdpPeer peer,
        UdpTransportOptions options);

    [[nodiscard]] std::expected<void, UdpError> initialize();
    [[nodiscard]] ExchangeOutcome exchange_message(
        UdpPacketId id,
        std::span<const std::byte> request,
        std::span<std::byte> response,
        std::size_t response_limit,
        std::chrono::milliseconds timeout,
        std::uint16_t max_attempts);
    [[nodiscard]] UdpCancellationSignal cancellation_signal() const noexcept;
    void poison_locked() noexcept;
    void close_locked() noexcept;

    std::unique_ptr<IUdpSocket> socket_;
    UdpPeer peer_;
    UdpTransportOptions options_;
    std::stop_source local_cancel_;
    std::vector<std::byte> receive_packet_;
    std::uint16_t next_sequence_{0};
    std::uint16_t packet_bytes_{kFastbootUdpMinimumPacketBytes};
    mutable std::mutex mutex_;
    bool open_{true};
    bool poisoned_{false};
};

struct UdpSocketConnection {
    std::unique_ptr<IUdpSocket> socket;
    UdpPeer peer;
};

[[nodiscard]] std::expected<UdpSocketConnection, UdpError> connect_native_udp_socket(
    const UdpEndpoint& endpoint,
    std::chrono::milliseconds timeout,
    std::stop_token cancellation = {});

[[nodiscard]] std::expected<std::unique_ptr<UdpFastbootTransport>, UdpError>
connect_udp_fastboot(
    std::string_view endpoint,
    UdpTransportOptions options = {});

}  // namespace kairosboot::transport
