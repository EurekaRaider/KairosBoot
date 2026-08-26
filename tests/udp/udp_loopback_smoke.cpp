// SPDX-License-Identifier: MIT
#include "udp_fastboot.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <span>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {

using namespace std::chrono_literals;
using namespace kairosboot::transport;

#ifdef _WIN32
using SocketHandle = SOCKET;
inline constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
void close_socket(const SocketHandle socket) noexcept {
    if (socket != kInvalidSocket) {
        closesocket(socket);
    }
}
#else
using SocketHandle = int;
inline constexpr SocketHandle kInvalidSocket = -1;
void close_socket(const SocketHandle socket) noexcept {
    if (socket != kInvalidSocket) {
        ::close(socket);
    }
}
#endif

[[nodiscard]] std::vector<std::byte> bytes(const std::string_view value) {
    std::vector<std::byte> result;
    result.reserve(value.size());
    for (const auto character : value) {
        result.push_back(static_cast<std::byte>(
            static_cast<unsigned char>(character)));
    }
    return result;
}

[[nodiscard]] std::vector<std::byte> be16(const std::uint16_t value) {
    return {
        static_cast<std::byte>((value >> 8U) & 0xFFU),
        static_cast<std::byte>(value & 0xFFU),
    };
}

[[nodiscard]] std::vector<std::byte> packet(
    const UdpPacketId id,
    const std::uint16_t sequence,
    const std::span<const std::byte> payload = {}) {
    const auto header = encode_udp_header({
        .id = id,
        .sequence = sequence,
    });
    std::vector<std::byte> result(header.begin(), header.end());
    result.insert(result.end(), payload.begin(), payload.end());
    return result;
}

[[nodiscard]] std::vector<std::byte> initialization_payload(
    const std::uint16_t packet_bytes) {
    auto result = be16(kFastbootUdpProtocolVersion);
    const auto size = be16(packet_bytes);
    result.insert(result.end(), size.begin(), size.end());
    return result;
}

class SocketOwner {
public:
    explicit SocketOwner(const SocketHandle socket) noexcept : socket_(socket) {}
    ~SocketOwner() { close_socket(socket_); }
    SocketOwner(const SocketOwner&) = delete;
    SocketOwner& operator=(const SocketOwner&) = delete;
    [[nodiscard]] SocketHandle get() const noexcept { return socket_; }
    void close() noexcept {
        close_socket(socket_);
        socket_ = kInvalidSocket;
    }

private:
    SocketHandle socket_{kInvalidSocket};
};

struct ReceivedDatagram {
    std::vector<std::byte> bytes;
    sockaddr_storage peer{};
#ifdef _WIN32
    int peer_length{0};
#else
    socklen_t peer_length{0};
#endif
};

[[nodiscard]] bool receive_datagram(
    const SocketHandle socket,
    ReceivedDatagram& result,
    std::string& error) {
    std::array<std::byte, 8193> buffer{};
    result.peer_length = sizeof(result.peer);
#ifdef _WIN32
    const auto received = recvfrom(
        socket,
        reinterpret_cast<char*>(buffer.data()),
        static_cast<int>(buffer.size()),
        0,
        reinterpret_cast<sockaddr*>(&result.peer),
        &result.peer_length);
    if (received == SOCKET_ERROR) {
        error = "loopback recvfrom failed with " + std::to_string(WSAGetLastError());
        return false;
    }
#else
    const auto received = recvfrom(
        socket,
        buffer.data(),
        buffer.size(),
        0,
        reinterpret_cast<sockaddr*>(&result.peer),
        &result.peer_length);
    if (received < 0) {
        error = "loopback recvfrom failed with " + std::to_string(errno);
        return false;
    }
#endif
    result.bytes.assign(buffer.begin(), buffer.begin() + received);
    return true;
}

[[nodiscard]] bool send_datagram(
    const SocketHandle socket,
    const std::span<const std::byte> datagram,
    const ReceivedDatagram& destination,
    std::string& error) {
#ifdef _WIN32
    const auto sent = sendto(
        socket,
        reinterpret_cast<const char*>(datagram.data()),
        static_cast<int>(datagram.size()),
        0,
        reinterpret_cast<const sockaddr*>(&destination.peer),
        destination.peer_length);
    if (sent == SOCKET_ERROR) {
        error = "loopback sendto failed with " + std::to_string(WSAGetLastError());
        return false;
    }
#else
#ifdef MSG_NOSIGNAL
    constexpr int send_flags = MSG_NOSIGNAL;
#else
    constexpr int send_flags = 0;
#endif
    const auto sent = sendto(
        socket,
        datagram.data(),
        datagram.size(),
        send_flags,
        reinterpret_cast<const sockaddr*>(&destination.peer),
        destination.peer_length);
    if (sent < 0) {
        error = "loopback sendto failed with " + std::to_string(errno);
        return false;
    }
#endif
    if (static_cast<std::size_t>(sent) != datagram.size()) {
        error = "loopback server sent a partial UDP datagram";
        return false;
    }
    return true;
}

[[nodiscard]] bool expect_datagram(
    const SocketHandle socket,
    const std::span<const std::byte> expected,
    ReceivedDatagram& received,
    std::string& error) {
    if (!receive_datagram(socket, received, error)) {
        return false;
    }
    if (!std::ranges::equal(received.bytes, expected)) {
        error = "loopback server received unexpected wire bytes";
        return false;
    }
    return true;
}

}  // namespace

int main() {
#ifdef _WIN32
    WSADATA winsock_data{};
    if (WSAStartup(MAKEWORD(2, 2), &winsock_data) != 0) {
        std::cerr << "WSAStartup failed\n";
        return 1;
    }
    struct WinsockCleanup {
        ~WinsockCleanup() { WSACleanup(); }
    };
    [[maybe_unused]] WinsockCleanup winsock_cleanup;
#endif

    SocketOwner server(socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
    if (server.get() == kInvalidSocket) {
        std::cerr << "creating loopback UDP server failed\n";
        return 1;
    }
#ifdef _WIN32
    DWORD timeout_ms = 5000;
    static_cast<void>(setsockopt(
        server.get(),
        SOL_SOCKET,
        SO_RCVTIMEO,
        reinterpret_cast<const char*>(&timeout_ms),
        sizeof(timeout_ms)));
#else
    timeval timeout_value{
        .tv_sec = 5,
        .tv_usec = 0,
    };
    static_cast<void>(setsockopt(
        server.get(), SOL_SOCKET, SO_RCVTIMEO, &timeout_value, sizeof(timeout_value)));
#endif
    sockaddr_in bind_address{};
    bind_address.sin_family = AF_INET;
    bind_address.sin_port = 0;
    bind_address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
#ifdef _WIN32
    const auto bind_result = bind(
        server.get(),
        reinterpret_cast<const sockaddr*>(&bind_address),
        static_cast<int>(sizeof(bind_address)));
#else
    const auto bind_result = bind(
        server.get(),
        reinterpret_cast<const sockaddr*>(&bind_address),
        sizeof(bind_address));
#endif
    if (bind_result != 0) {
        std::cerr << "binding loopback UDP server failed\n";
        return 1;
    }

    sockaddr_in actual_address{};
#ifdef _WIN32
    int actual_length = sizeof(actual_address);
#else
    socklen_t actual_length = sizeof(actual_address);
#endif
    if (getsockname(
            server.get(),
            reinterpret_cast<sockaddr*>(&actual_address),
            &actual_length) != 0) {
        std::cerr << "reading loopback UDP port failed\n";
        return 1;
    }
    const auto port = ntohs(actual_address.sin_port);
    std::string server_error;
    std::thread server_thread([&] {
        ReceivedDatagram client;
        const auto query = packet(UdpPacketId::Query, 0);
        if (!expect_datagram(server.get(), query, client, server_error)) {
            return;
        }
        const auto query_response = packet(UdpPacketId::Query, 0, be16(100));
        SocketOwner spoof_socket(socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
        if (spoof_socket.get() == kInvalidSocket) {
            server_error = "creating spoof-source UDP socket failed";
            return;
        }
        // A syntactically valid response from the wrong source port must not
        // establish the session.
        if (!send_datagram(
                spoof_socket.get(), query_response, client, server_error)) {
            return;
        }
        if (!send_datagram(server.get(), query_response, client, server_error)) {
            return;
        }

        const auto init = packet(
            UdpPacketId::Initialization,
            100,
            initialization_payload(kFastbootUdpHostMaximumPacketBytes));
        if (!expect_datagram(server.get(), init, client, server_error)) {
            return;
        }
        const auto init_response = packet(
            UdpPacketId::Initialization,
            100,
            initialization_payload(kFastbootUdpMinimumPacketBytes));
        if (!send_datagram(server.get(), init_response, client, server_error)) {
            return;
        }

        const auto command_payload = bytes("getvar:product");
        const auto command = packet(UdpPacketId::Fastboot, 101, command_payload);
        if (!expect_datagram(server.get(), command, client, server_error)) {
            return;
        }
        const auto command_ack = packet(UdpPacketId::Fastboot, 101);
        if (!send_datagram(server.get(), command_ack, client, server_error)) {
            return;
        }

        const auto read_request = packet(UdpPacketId::Fastboot, 102);
        if (!expect_datagram(server.get(), read_request, client, server_error)) {
            return;
        }
        const auto reply = packet(
            UdpPacketId::Fastboot, 102, bytes("OKAYloopback"));
        static_cast<void>(send_datagram(server.get(), reply, client, server_error));
    });

    UdpTransportOptions options;
    options.connect_timeout = 2s;
    options.query_timeout = 2s;
    options.initialization_timeout = 2s;
    options.retransmit_interval = 500ms;
    options.query_attempts = 4;
    options.transmission_attempts = 1;
    auto transport = connect_udp_fastboot(
        "127.0.0.1:" + std::to_string(port), options);
    if (!transport) {
        server_thread.join();
        std::cerr << "native UDP initialization failed: " << transport.error().message << '\n';
        return 1;
    }
    const auto command = bytes("getvar:product");
    const auto write = (*transport)->write(command, 2s);
    if (write.status != kairosboot::protocol::TransportStatus::Ok) {
        server_thread.join();
        std::cerr << "native UDP loopback write failed: " << write.detail << '\n';
        return 1;
    }
    std::array<std::byte, 32> reply{};
    const auto read = (*transport)->read(reply, 2s);
    server_thread.join();
    server.close();
    if (!server_error.empty()) {
        std::cerr << server_error << '\n';
        return 1;
    }
    const auto expected_reply = bytes("OKAYloopback");
    if (read.status != kairosboot::protocol::TransportStatus::Ok ||
        read.transferred != expected_reply.size() ||
        !std::ranges::equal(
            std::span(reply).first(read.transferred), expected_reply)) {
        std::cerr << "native UDP loopback read failed: " << read.detail << '\n';
        return 1;
    }

    // The peer is now closed. A bounded failure proves the POSIX send path
    // returns normally instead of delivering a process-wide signal.
    const auto after_peer_exit = (*transport)->write(bytes("continue"), 700ms);
    if (after_peer_exit.status == kairosboot::protocol::TransportStatus::Ok ||
        !(*transport)->is_poisoned()) {
        std::cerr << "unserviced UDP peer did not fail closed\n";
        return 1;
    }

    std::cout << "UDP native loopback and closed-peer regression passed\n";
    return 0;
}
