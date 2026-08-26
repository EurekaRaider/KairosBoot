// SPDX-License-Identifier: MIT
#include "scripted_datagram.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using kairosboot::protocol::TransferCertainty;
using kairosboot::protocol::TransportStatus;
using namespace kairosboot::transport;
using namespace kairosboot::transport::test;

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

void require(const bool condition, const std::string_view message) {
    if (!condition) {
        fail(std::string(message));
    }
}

template <typename Left, typename Right>
void require_equal(
    const Left& left,
    const Right& right,
    const std::string_view message) {
    if (!(left == right)) {
        fail(std::string(message));
    }
}

[[nodiscard]] UdpPeer peer4(
    const std::uint8_t last,
    const std::uint16_t port = kFastbootUdpDefaultPort) {
    UdpPeer peer;
    peer.family = UdpAddressFamily::Ipv4;
    peer.address[0] = std::byte{127};
    peer.address[1] = std::byte{0};
    peer.address[2] = std::byte{0};
    peer.address[3] = static_cast<std::byte>(last);
    peer.port = port;
    return peer;
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
    const std::span<const std::byte> payload = {},
    const std::uint8_t flags = 0) {
    const auto header = encode_udp_header({
        .id = id,
        .flags = flags,
        .sequence = sequence,
    });
    std::vector<std::byte> result(header.begin(), header.end());
    result.insert(result.end(), payload.begin(), payload.end());
    return result;
}

[[nodiscard]] std::vector<std::byte> initialization_payload(
    const std::uint16_t version,
    const std::uint16_t packet_bytes) {
    auto result = be16(version);
    const auto size = be16(packet_bytes);
    result.insert(result.end(), size.begin(), size.end());
    return result;
}

[[nodiscard]] UdpTransportOptions test_options() {
    UdpTransportOptions options;
    options.query_timeout = 5s;
    options.initialization_timeout = 5s;
    options.retransmit_interval = 500ms;
    options.query_attempts = 4;
    options.transmission_attempts = 3;
    return options;
}

void script_initialization(
    DatagramScriptState& script,
    const UdpPeer& peer,
    const std::uint16_t starting_sequence = 0,
    const std::uint16_t target_packet_bytes = kFastbootUdpMinimumPacketBytes,
    const std::uint16_t target_version = kFastbootUdpProtocolVersion,
    const std::uint16_t host_packet_bytes = kFastbootUdpHostMaximumPacketBytes) {
    const auto query = packet(UdpPacketId::Query, 0);
    script.expect_send(query, peer);
    const auto starting_payload = be16(starting_sequence);
    const auto query_response = packet(UdpPacketId::Query, 0, starting_payload);
    script.provide_receive(query_response, peer);

    const auto host_init = initialization_payload(
        kFastbootUdpProtocolVersion, host_packet_bytes);
    const auto init = packet(
        UdpPacketId::Initialization, starting_sequence, host_init);
    script.expect_send(init, peer);
    const auto target_init = initialization_payload(
        target_version, target_packet_bytes);
    const auto init_response = packet(
        UdpPacketId::Initialization, starting_sequence, target_init);
    script.provide_receive(init_response, peer);
}

[[nodiscard]] std::unique_ptr<UdpFastbootTransport> ready_transport(
    const std::shared_ptr<DatagramScriptState>& script,
    const UdpPeer& peer,
    const std::uint16_t starting_sequence = 0,
    const std::uint16_t target_packet_bytes = kFastbootUdpMinimumPacketBytes,
    UdpTransportOptions options = test_options()) {
    script_initialization(
        *script,
        peer,
        starting_sequence,
        target_packet_bytes,
        kFastbootUdpProtocolVersion,
        options.host_max_packet_bytes);
    auto result = UdpFastbootTransport::create(
        make_scripted_datagram_socket(script), peer, options);
    if (!result) {
        fail("transport initialization failed: " + result.error().message);
    }
    return std::move(*result);
}

void test_header_codec_boundaries() {
    const auto encoded = encode_udp_header({
        .id = UdpPacketId::Fastboot,
        .flags = static_cast<std::uint8_t>(UdpPacketFlag::Continuation),
        .sequence = 0xFFFF,
    });
    require_equal(encoded[0], std::byte{3}, "packet ID was not encoded");
    require_equal(encoded[1], std::byte{1}, "packet flags were not encoded");
    require_equal(encoded[2], std::byte{0xFF}, "sequence high byte is wrong");
    require_equal(encoded[3], std::byte{0xFF}, "sequence low byte is wrong");
    const auto decoded = decode_udp_header(encoded);
    require_equal(decoded.id, UdpPacketId::Fastboot, "packet ID did not round-trip");
    require_equal(decoded.flags, std::uint8_t{1}, "packet flags did not round-trip");
    require_equal(decoded.sequence, std::uint16_t{0xFFFF}, "sequence did not round-trip");
}

void test_endpoint_parser_ipv4_ipv6() {
    auto ipv4 = parse_udp_endpoint("192.0.2.1:6000");
    require(ipv4.has_value(), "IPv4 endpoint was rejected");
    require_equal(ipv4->host, std::string{"192.0.2.1"}, "IPv4 host changed");
    require_equal(ipv4->port, std::uint16_t{6000}, "IPv4 port changed");

    auto ipv6 = parse_udp_endpoint("[::1]:7000");
    require(ipv6.has_value(), "bracketed IPv6 endpoint was rejected");
    require_equal(ipv6->host, std::string{"::1"}, "IPv6 host changed");
    require_equal(ipv6->port, std::uint16_t{7000}, "IPv6 port changed");

    auto ipv6_default = parse_udp_endpoint("2001:db8::1");
    require(ipv6_default.has_value(), "unbracketed IPv6 default-port endpoint was rejected");
    require_equal(
        ipv6_default->port,
        kFastbootUdpDefaultPort,
        "IPv6 default port changed");

    require(!parse_udp_endpoint("").has_value(), "empty endpoint was accepted");
    require(!parse_udp_endpoint("host:0").has_value(), "port zero was accepted");
    require(!parse_udp_endpoint("[::1]junk").has_value(), "IPv6 suffix junk was accepted");
    require(!parse_udp_endpoint("host name").has_value(), "endpoint whitespace was accepted");
}

struct ResolveClock {
    std::array<detail::UdpConnectClock::time_point, 2> values;
    std::size_t index{0};
    std::size_t resolve_calls{0};
};

void test_resolution_uses_absolute_deadline() {
    const auto origin = detail::UdpConnectClock::time_point{};
    ResolveClock clock{{origin, origin + 20ms}};
    const auto now = [](void* opaque) noexcept {
        auto& value = *static_cast<ResolveClock*>(opaque);
        const auto index = std::min(value.index, value.values.size() - 1U);
        ++value.index;
        return value.values[index];
    };
    const auto resolve = [](void* opaque) noexcept {
        ++static_cast<ResolveClock*>(opaque)->resolve_calls;
    };
    const auto result = detail::run_udp_resolve_phase(
        10ms, {}, resolve, &clock, now, &clock);
    require(result.resolver_ran, "resolver seam did not run");
    require(result.expired, "DNS time did not consume the connect deadline");
    require_equal(clock.resolve_calls, std::size_t{1}, "resolver ran more than once");

    std::stop_source cancelled;
    cancelled.request_stop();
    ResolveClock cancelled_clock{{origin, origin}};
    const auto cancelled_result = detail::run_udp_resolve_phase(
        10ms,
        cancelled.get_token(),
        resolve,
        &cancelled_clock,
        now,
        &cancelled_clock);
    require(cancelled_result.cancelled, "pre-resolution cancellation was missed");
    require(!cancelled_result.resolver_ran, "resolver ran after cancellation");
}

void test_initialization_negotiates_version_and_packet_limit() {
    const auto peer = peer4(1);
    auto script = std::make_shared<DatagramScriptState>();
    script_initialization(*script, peer, 0x1234, 65535, 2);
    auto transport = UdpFastbootTransport::create(
        make_scripted_datagram_socket(script), peer, test_options());
    require(transport.has_value(), "compatible higher target version was rejected");
    require_equal(
        (*transport)->negotiated_packet_bytes(),
        kFastbootUdpHostMaximumPacketBytes,
        "packet negotiation did not clamp to the host limit");
    require(script->complete(), "initialization script was not consumed");
}

void test_query_retransmission_is_identical() {
    const auto peer = peer4(1);
    auto script = std::make_shared<DatagramScriptState>();
    const auto query = packet(UdpPacketId::Query, 0);
    script->expect_send(query, peer);
    script->provide_timeout(peer);
    script->expect_send(query, peer);
    const auto sequence = be16(7);
    const auto query_response = packet(UdpPacketId::Query, 0, sequence);
    script->provide_receive(query_response, peer);
    const auto host_init = initialization_payload(1, 8192);
    const auto init = packet(UdpPacketId::Initialization, 7, host_init);
    script->expect_send(init, peer);
    const auto target_init = initialization_payload(1, 512);
    const auto init_response = packet(UdpPacketId::Initialization, 7, target_init);
    script->provide_receive(init_response, peer);

    auto transport = UdpFastbootTransport::create(
        make_scripted_datagram_socket(script), peer, test_options());
    require(transport.has_value(), "query did not recover after one timeout");
    require(script->complete(), "query retry script was not consumed");
    require_equal(script->sent_datagrams[0], script->sent_datagrams[1],
                  "query retransmission changed wire bytes");
}

void test_query_response_continuation_sets_sequence() {
    const auto peer = peer4(1);
    auto script = std::make_shared<DatagramScriptState>();
    const auto query0 = packet(UdpPacketId::Query, 0);
    script->expect_send(query0, peer);
    const std::array high{std::byte{0x12}};
    const auto first = packet(UdpPacketId::Query, 0, high, 1);
    script->provide_receive(first, peer);
    const auto query1 = packet(UdpPacketId::Query, 1);
    script->expect_send(query1, peer);
    const std::array low{std::byte{0x34}};
    const auto second = packet(UdpPacketId::Query, 1, low);
    script->provide_receive(second, peer);
    const auto init_data = initialization_payload(1, 8192);
    const auto init = packet(UdpPacketId::Initialization, 0x1234, init_data);
    script->expect_send(init, peer);
    const auto init_reply_data = initialization_payload(1, 512);
    const auto init_reply = packet(
        UdpPacketId::Initialization, 0x1234, init_reply_data);
    script->provide_receive(init_reply, peer);

    auto transport = UdpFastbootTransport::create(
        make_scripted_datagram_socket(script), peer, test_options());
    require(transport.has_value(), "query continuation failed");
    require(script->complete(), "query continuation sequence was wrong");
}

void test_invalid_handshake_responses_are_rejected() {
    const auto peer = peer4(1);
    {
        auto script = std::make_shared<DatagramScriptState>();
        const auto query = packet(UdpPacketId::Query, 0);
        script->expect_send(query, peer);
        const std::array short_payload{std::byte{0}};
        const auto response = packet(UdpPacketId::Query, 0, short_payload);
        script->provide_receive(response, peer);
        auto transport = UdpFastbootTransport::create(
            make_scripted_datagram_socket(script), peer, test_options());
        require(!transport.has_value(), "short query response was accepted");
    }
    {
        auto script = std::make_shared<DatagramScriptState>();
        script_initialization(*script, peer, 0, 511, 1);
        auto transport = UdpFastbootTransport::create(
            make_scripted_datagram_socket(script), peer, test_options());
        require(!transport.has_value(), "target packet size below 512 was accepted");
    }
    {
        auto script = std::make_shared<DatagramScriptState>();
        script_initialization(*script, peer, 0, 512, 0);
        auto transport = UdpFastbootTransport::create(
            make_scripted_datagram_socket(script), peer, test_options());
        require(!transport.has_value(), "target protocol version zero was accepted");
    }
}

void test_handshake_error_packet_fails_immediately() {
    const auto peer = peer4(1);
    auto script = std::make_shared<DatagramScriptState>();
    const auto query = packet(UdpPacketId::Query, 0);
    script->expect_send(query, peer);
    const auto message = udp_bytes("no device");
    const auto response = packet(UdpPacketId::Error, 0, message);
    script->provide_receive(response, peer);
    auto transport = UdpFastbootTransport::create(
        make_scripted_datagram_socket(script), peer, test_options());
    require(!transport.has_value(), "query error packet was accepted");
    require(
        transport.error().message.find("no device") != std::string::npos,
        "target error text was lost");
    require(script->complete(), "error response caused an unwanted retry");
}

void test_small_write_and_read() {
    const auto peer = peer4(1);
    auto script = std::make_shared<DatagramScriptState>();
    auto transport = ready_transport(script, peer, 10);
    const auto command = udp_bytes("getvar:product");
    const auto command_packet = packet(UdpPacketId::Fastboot, 11, command);
    script->expect_send(command_packet, peer);
    const auto command_ack = packet(UdpPacketId::Fastboot, 11);
    script->provide_receive(command_ack, peer);
    const auto write = transport->write(command, 5s);
    require_equal(write.status, TransportStatus::Ok, "small write failed");
    require_equal(write.transferred, command.size(), "small write byte count changed");

    const auto read_request = packet(UdpPacketId::Fastboot, 12);
    script->expect_send(read_request, peer);
    const auto reply_bytes = udp_bytes("OKAYproduct_a");
    const auto reply = packet(UdpPacketId::Fastboot, 12, reply_bytes);
    script->provide_receive(reply, peer);
    std::array<std::byte, 64> destination{};
    const auto read = transport->read(destination, 5s);
    require_equal(read.status, TransportStatus::Ok, "small read failed");
    require_equal(read.transferred, reply_bytes.size(), "small read length changed");
    require(std::ranges::equal(
        std::span(destination).first(read.transferred), reply_bytes),
        "small read payload changed");
    require(script->complete(), "small I/O script was not consumed");
}

void test_write_fragments_at_negotiated_boundary() {
    const auto peer = peer4(1);
    auto script = std::make_shared<DatagramScriptState>();
    auto transport = ready_transport(script, peer, 0, 512);
    std::vector<std::byte> payload(509);
    for (std::size_t index = 0; index < payload.size(); ++index) {
        payload[index] = static_cast<std::byte>(index & 0xFFU);
    }
    const auto first = packet(
        UdpPacketId::Fastboot,
        1,
        std::span(payload).first(508),
        static_cast<std::uint8_t>(UdpPacketFlag::Continuation));
    script->expect_send(first, peer);
    const auto first_ack = packet(UdpPacketId::Fastboot, 1);
    script->provide_receive(first_ack, peer);
    const auto second = packet(
        UdpPacketId::Fastboot,
        2,
        std::span(payload).subspan(508));
    script->expect_send(second, peer);
    const auto second_ack = packet(UdpPacketId::Fastboot, 2);
    script->provide_receive(second_ack, peer);
    const auto result = transport->write(payload, 5s);
    require_equal(result.status, TransportStatus::Ok, "fragmented write failed");
    require_equal(result.transferred, payload.size(), "fragmented write count changed");
    require(script->complete(), "fragmented write did not use exact packet boundaries");
}

void test_read_continuation_prompts_for_each_fragment() {
    const auto peer = peer4(1);
    auto script = std::make_shared<DatagramScriptState>();
    auto transport = ready_transport(script, peer);
    const auto request1 = packet(UdpPacketId::Fastboot, 1);
    script->expect_send(request1, peer);
    const auto first_payload = udp_bytes("OK");
    const auto first = packet(UdpPacketId::Fastboot, 1, first_payload, 1);
    script->provide_receive(first, peer);
    const auto request2 = packet(UdpPacketId::Fastboot, 2);
    script->expect_send(request2, peer);
    const auto second_payload = udp_bytes("AY");
    const auto second = packet(UdpPacketId::Fastboot, 2, second_payload);
    script->provide_receive(second, peer);
    std::array<std::byte, 4> destination{};
    const auto result = transport->read(destination, 5s);
    require_equal(result.status, TransportStatus::Ok, "continuation read failed");
    require_equal(udp_bytes("OKAY"), std::vector<std::byte>(
        destination.begin(), destination.end()), "continuation payload changed");
    require(script->complete(), "continuation prompt sequence was wrong");
}

void test_out_of_order_duplicate_id_and_peer_are_ignored() {
    const auto peer = peer4(1);
    const auto stranger = peer4(2);
    auto script = std::make_shared<DatagramScriptState>();
    auto transport = ready_transport(script, peer);
    const auto request = packet(UdpPacketId::Fastboot, 1);
    script->expect_send(request, peer);
    const auto duplicate = packet(UdpPacketId::Fastboot, 0, udp_bytes("old"));
    script->provide_receive(duplicate, peer);
    const auto future = packet(UdpPacketId::Fastboot, 2, udp_bytes("future"));
    script->provide_receive(future, peer);
    const auto wrong_id = packet(UdpPacketId::Query, 1, udp_bytes("query"));
    script->provide_receive(wrong_id, peer);
    const auto wrong_peer = packet(UdpPacketId::Fastboot, 1, udp_bytes("spoof"));
    script->provide_receive(wrong_peer, stranger);
    const auto correct_payload = udp_bytes("OKAY");
    const auto correct = packet(UdpPacketId::Fastboot, 1, correct_payload);
    script->provide_receive(correct, peer);
    std::array<std::byte, 4> destination{};
    const auto result = transport->read(destination, 5s);
    require_equal(result.status, TransportStatus::Ok, "valid response after noise failed");
    require(std::ranges::equal(destination, correct_payload), "spoofed payload was accepted");
    require(script->complete(), "ignored datagram script was not consumed");
}

void test_peer_mismatch_does_not_suppress_retransmission() {
    const auto peer = peer4(1);
    const auto stranger = peer4(2);
    auto options = test_options();
    options.transmission_attempts = 2;
    auto script = std::make_shared<DatagramScriptState>();
    auto transport = ready_transport(script, peer, 0, 512, options);
    const auto command = udp_bytes("flash:x");
    const auto request = packet(UdpPacketId::Fastboot, 1, command);
    script->expect_send(request, peer);
    const auto spoof = packet(UdpPacketId::Fastboot, 1);
    script->provide_receive(spoof, stranger);
    script->provide_timeout(peer);
    script->expect_send(request, peer);
    const auto ack = packet(UdpPacketId::Fastboot, 1);
    script->provide_receive(ack, peer);
    const auto result = transport->write(command, 5s);
    require_equal(result.status, TransportStatus::Ok, "peer mismatch retry failed");
    require(script->complete(), "peer mismatch retry script was not consumed");
}

void test_unknown_flags_poison_transport() {
    const auto peer = peer4(1);
    auto script = std::make_shared<DatagramScriptState>();
    auto transport = ready_transport(script, peer);
    const auto request = packet(UdpPacketId::Fastboot, 1);
    script->expect_send(request, peer);
    const auto response = packet(UdpPacketId::Fastboot, 1, udp_bytes("OKAY"), 0x80);
    script->provide_receive(response, peer);
    std::array<std::byte, 8> destination{};
    const auto result = transport->read(destination, 5s);
    require_equal(result.status, TransportStatus::IoError, "unknown flag was accepted");
    require(transport->is_poisoned(), "unknown flag did not poison the session");
    require(!transport->is_open(), "poisoned session stayed open");
}

void test_truncated_and_oversized_datagrams_poison_transport() {
    const auto peer = peer4(1);
    {
        auto script = std::make_shared<DatagramScriptState>();
        auto transport = ready_transport(script, peer);
        const auto request = packet(UdpPacketId::Fastboot, 1);
        script->expect_send(request, peer);
        const std::array truncated{std::byte{3}, std::byte{0}, std::byte{0}};
        script->provide_receive(truncated, peer);
        std::array<std::byte, 8> destination{};
        const auto result = transport->read(destination, 5s);
        require(result.truncated, "short header was not marked truncated");
        require(transport->is_poisoned(), "short header did not poison the session");
    }
    {
        auto script = std::make_shared<DatagramScriptState>();
        auto transport = ready_transport(script, peer);
        const auto request = packet(UdpPacketId::Fastboot, 1);
        script->expect_send(request, peer);
        std::vector<std::byte> oversized(513, std::byte{0});
        const auto header = encode_udp_header({
            .id = UdpPacketId::Fastboot,
            .sequence = 1,
        });
        std::ranges::copy(header, oversized.begin());
        script->provide_receive(oversized, peer);
        std::array<std::byte, 8> destination{};
        const auto result = transport->read(destination, 5s);
        require(result.truncated, "oversized datagram was not marked truncated");
        require(transport->is_poisoned(), "oversized datagram did not poison the session");
    }
}

void test_socket_truncation_status_poison_transport() {
    const auto peer = peer4(1);
    auto script = std::make_shared<DatagramScriptState>();
    auto transport = ready_transport(script, peer);
    const auto request = packet(UdpPacketId::Fastboot, 1);
    script->expect_send(request, peer);
    const auto header = packet(UdpPacketId::Fastboot, 1);
    script->provide_receive(
        header,
        peer,
        DatagramIoStatus::Truncated,
        header.size(),
        std::nullopt,
        "kernel truncated datagram");
    std::array<std::byte, 8> destination{};
    const auto result = transport->read(destination, 5s);
    require(result.truncated, "socket truncation status was lost");
    require(transport->is_poisoned(), "socket truncation did not poison the session");
}

void test_out_of_turn_write_response_is_rejected() {
    const auto peer = peer4(1);
    auto script = std::make_shared<DatagramScriptState>();
    auto transport = ready_transport(script, peer);
    const auto command = udp_bytes("erase:x");
    const auto request = packet(UdpPacketId::Fastboot, 1, command);
    script->expect_send(request, peer);
    const auto response = packet(UdpPacketId::Fastboot, 1, udp_bytes("unexpected"));
    script->provide_receive(response, peer);
    const auto result = transport->write(command, 5s);
    require_equal(result.status, TransportStatus::IoError, "write response data was accepted");
    require(result.truncated, "out-of-turn data did not report the overflow");
    require_equal(
        result.certainty,
        TransferCertainty::FullyTransferred,
        "matching response did not prove request delivery");
    require(transport->is_poisoned(), "out-of-turn response did not poison the session");
}

void test_read_overflow_and_empty_response_are_rejected() {
    const auto peer = peer4(1);
    {
        auto script = std::make_shared<DatagramScriptState>();
        auto transport = ready_transport(script, peer);
        const auto request = packet(UdpPacketId::Fastboot, 1);
        script->expect_send(request, peer);
        const auto response = packet(UdpPacketId::Fastboot, 1, udp_bytes("four"));
        script->provide_receive(response, peer);
        std::array<std::byte, 3> destination{};
        const auto result = transport->read(destination, 5s);
        require(result.truncated, "logical read overflow was not marked truncated");
        require(transport->is_poisoned(), "logical read overflow did not poison the session");
    }
    {
        auto script = std::make_shared<DatagramScriptState>();
        auto transport = ready_transport(script, peer);
        const auto request = packet(UdpPacketId::Fastboot, 1);
        script->expect_send(request, peer);
        const auto response = packet(UdpPacketId::Fastboot, 1);
        script->provide_receive(response, peer);
        std::array<std::byte, 8> destination{};
        const auto result = transport->read(destination, 5s);
        require_equal(result.status, TransportStatus::IoError, "empty logical response succeeded");
        require(transport->is_poisoned(), "empty logical response did not poison the session");
    }
}

void test_lost_ack_maps_unknown_certainty_and_poison() {
    const auto peer = peer4(1);
    auto options = test_options();
    options.transmission_attempts = 2;
    auto script = std::make_shared<DatagramScriptState>();
    auto transport = ready_transport(script, peer, 0, 512, options);
    const auto command = udp_bytes("boot");
    const auto request = packet(UdpPacketId::Fastboot, 1, command);
    script->expect_send(request, peer);
    script->provide_timeout(peer);
    script->expect_send(request, peer);
    script->provide_timeout(peer);
    const auto result = transport->write(command, 5s);
    require_equal(result.status, TransportStatus::Timeout, "lost ACK did not time out");
    require_equal(
        result.certainty,
        TransferCertainty::PartialOrUnknown,
        "lost ACK was incorrectly reported as unsent");
    require(transport->is_poisoned(), "lost ACK did not poison sequence state");
    require(script->complete(), "retransmission limit was not exact");
}

void test_unsent_and_partial_send_certainty() {
    const auto peer = peer4(1);
    {
        auto script = std::make_shared<DatagramScriptState>();
        auto transport = ready_transport(script, peer);
        const auto command = udp_bytes("continue");
        const auto request = packet(UdpPacketId::Fastboot, 1, command);
        script->expect_send(
            request, peer, DatagramIoStatus::Error, 0, "route unavailable");
        const auto result = transport->write(command, 5s);
        require_equal(
            result.certainty,
            TransferCertainty::NotTransferred,
            "zero-byte send error became unknown");
        require(!transport->is_poisoned(), "known-unsent write poisoned the session");
        require(transport->is_open(), "known-unsent write closed the session");
    }
    {
        auto script = std::make_shared<DatagramScriptState>();
        auto transport = ready_transport(script, peer);
        const auto command = udp_bytes("continue");
        const auto request = packet(UdpPacketId::Fastboot, 1, command);
        script->expect_send(
            request, peer, DatagramIoStatus::Error, 2, "partial fake send");
        const auto result = transport->write(command, 5s);
        require_equal(
            result.certainty,
            TransferCertainty::PartialOrUnknown,
            "partial datagram send was not unknown");
        require(transport->is_poisoned(), "partial datagram send did not poison the session");
    }
}

void test_cancellation_before_write_is_not_sent_and_closes() {
    const auto peer = peer4(1);
    auto script = std::make_shared<DatagramScriptState>();
    auto transport = ready_transport(script, peer);
    transport->cancel();
    const auto result = transport->write(udp_bytes("reboot"), 5s);
    require_equal(
        result.certainty,
        TransferCertainty::NotTransferred,
        "pre-send cancellation became a partial transfer");
    require(transport->is_poisoned(), "cancelled session was not poisoned");
    require(script->complete(), "cancelled write touched the socket");
}

void test_handshake_cancellation_has_distinct_error_kind() {
    const auto peer = peer4(1);
    std::stop_source cancellation;
    cancellation.request_stop();
    auto options = test_options();
    options.cancellation = cancellation.get_token();
    auto script = std::make_shared<DatagramScriptState>();
    auto transport = UdpFastbootTransport::create(
        make_scripted_datagram_socket(script), peer, options);
    require(!transport.has_value(), "cancelled handshake succeeded");
    require_equal(
        transport.error().kind,
        UdpErrorKind::Cancelled,
        "handshake cancellation was flattened to generic I/O");
    require(script->steps.empty(), "cancelled handshake touched the network");
}

void test_ignored_datagram_limit_prevents_receive_amplification() {
    const auto peer = peer4(1);
    const auto stranger = peer4(2);
    auto script = std::make_shared<DatagramScriptState>();
    auto transport = ready_transport(script, peer);
    const auto request = packet(UdpPacketId::Fastboot, 1);
    script->expect_send(request, peer);
    const auto spoof = packet(UdpPacketId::Fastboot, 1, udp_bytes("noise"));
    for (std::size_t count = 0; count < 65; ++count) {
        script->provide_receive(spoof, stranger);
    }
    std::array<std::byte, 8> destination{};
    const auto result = transport->read(destination, 5s);
    require_equal(result.status, TransportStatus::IoError, "spoof flood was accepted");
    require(transport->is_poisoned(), "spoof flood did not poison the session");
    require(script->complete(), "ignored datagram cap consumed an unexpected count");
}

void test_empty_continuation_prompts_next_sequence() {
    const auto peer = peer4(1);
    auto script = std::make_shared<DatagramScriptState>();
    auto transport = ready_transport(script, peer);
    const auto first_request = packet(UdpPacketId::Fastboot, 1);
    script->expect_send(first_request, peer);
    const auto empty_continuation = packet(UdpPacketId::Fastboot, 1, {}, 1);
    script->provide_receive(empty_continuation, peer);
    const auto second_request = packet(UdpPacketId::Fastboot, 2);
    script->expect_send(second_request, peer);
    const auto final_response = packet(UdpPacketId::Fastboot, 2, udp_bytes("OKAY"));
    script->provide_receive(final_response, peer);
    std::array<std::byte, 8> destination{};
    const auto result = transport->read(destination, 5s);
    require_equal(result.status, TransportStatus::Ok, "empty continuation was rejected");
    require_equal(result.transferred, std::size_t{4}, "continued payload size changed");
    require(script->complete(), "empty continuation used the wrong next sequence");
}

void test_outbound_fragments_wrap_modulo_16_bits() {
    const auto peer = peer4(1);
    auto script = std::make_shared<DatagramScriptState>();
    auto transport = ready_transport(script, peer, 0xFFFD, 512);
    std::vector<std::byte> payload(508U * 2U + 1U, std::byte{1});
    const auto first = packet(
        UdpPacketId::Fastboot,
        0xFFFE,
        std::span(payload).first(508),
        1);
    script->expect_send(first, peer);
    script->provide_receive(packet(UdpPacketId::Fastboot, 0xFFFE), peer);
    const auto second = packet(
        UdpPacketId::Fastboot,
        0xFFFF,
        std::span(payload).subspan(508, 508),
        1);
    script->expect_send(second, peer);
    script->provide_receive(packet(UdpPacketId::Fastboot, 0xFFFF), peer);
    const auto third = packet(
        UdpPacketId::Fastboot,
        0x0000,
        std::span(payload).last(1));
    script->expect_send(third, peer);
    script->provide_receive(packet(UdpPacketId::Fastboot, 0x0000), peer);
    const auto result = transport->write(payload, 5s);
    require_equal(result.status, TransportStatus::Ok, "outbound sequence wrap failed");
    require_equal(result.transferred, payload.size(), "wrapped write count changed");
    require(script->complete(), "outbound sequence did not wrap to zero");
}

void test_response_continuation_wraps_modulo_16_bits() {
    const auto peer = peer4(1);
    auto script = std::make_shared<DatagramScriptState>();
    auto transport = ready_transport(script, peer, 0xFFFE, 512);
    const auto request = packet(UdpPacketId::Fastboot, 0xFFFF);
    script->expect_send(request, peer);
    const auto first = packet(
        UdpPacketId::Fastboot, 0xFFFF, udp_bytes("OK"), 1);
    script->provide_receive(first, peer);
    const auto prompt = packet(UdpPacketId::Fastboot, 0x0000);
    script->expect_send(prompt, peer);
    const auto second = packet(UdpPacketId::Fastboot, 0x0000, udp_bytes("AY"));
    script->provide_receive(second, peer);
    std::array<std::byte, 4> destination{};
    const auto result = transport->read(destination, 5s);
    require_equal(result.status, TransportStatus::Ok, "response continuation wrap failed");
    require(std::ranges::equal(destination, udp_bytes("OKAY")),
            "wrapped continuation payload changed");
    require(script->complete(), "response continuation did not wrap to zero");
}

void test_query_sequence_ffff_initializes_then_wraps() {
    const auto peer = peer4(1);
    auto script = std::make_shared<DatagramScriptState>();
    auto transport = ready_transport(script, peer, 0xFFFF, 512);
    const auto command = udp_bytes("continue");
    const auto request = packet(UdpPacketId::Fastboot, 0x0000, command);
    script->expect_send(request, peer);
    script->provide_receive(packet(UdpPacketId::Fastboot, 0x0000), peer);
    const auto result = transport->write(command, 5s);
    require_equal(result.status, TransportStatus::Ok, "query sequence FFFF was rejected");
    require(script->complete(), "post-initialization sequence did not wrap to zero");
}

void test_large_download_is_not_rejected_before_wire() {
    const auto peer = peer4(1);
    auto script = std::make_shared<DatagramScriptState>();
    auto transport = ready_transport(script, peer, 0, 512);
    std::vector<std::byte> payload(508U * 0x10000U + 1U, std::byte{1});
    const auto first = packet(
        UdpPacketId::Fastboot,
        1,
        std::span(payload).first(508),
        1);
    script->expect_send(
        first,
        peer,
        DatagramIoStatus::Error,
        0,
        "injected unsent failure");
    const auto result = transport->write(payload, 5s);
    require_equal(result.status, TransportStatus::IoError,
                  "injected large-download send failure was lost");
    require_equal(result.certainty, TransferCertainty::NotTransferred,
                  "known-unsent large download became ambiguous");
    require(!transport->is_poisoned(),
            "large download was rejected by a protocol-external sequence cap");
    require(script->complete(), "large download did not reach the socket backend");
}

void test_target_error_maps_delivery_and_poison() {
    const auto peer = peer4(1);
    auto script = std::make_shared<DatagramScriptState>();
    auto transport = ready_transport(script, peer);
    const auto command = udp_bytes("oem bad");
    const auto request = packet(UdpPacketId::Fastboot, 1, command);
    script->expect_send(request, peer);
    const auto response = packet(UdpPacketId::Error, 1, udp_bytes("denied"));
    script->provide_receive(response, peer);
    const auto result = transport->write(command, 5s);
    require_equal(result.status, TransportStatus::IoError, "target UDP error succeeded");
    require_equal(
        result.certainty,
        TransferCertainty::FullyTransferred,
        "matching target error did not prove datagram receipt");
    require(result.detail.find("denied") != std::string::npos, "target error text was lost");
    require(transport->is_poisoned(), "target UDP error did not poison the session");
}

void test_target_error_continuation_uses_error_id() {
    const auto peer = peer4(1);
    auto script = std::make_shared<DatagramScriptState>();
    auto transport = ready_transport(script, peer);
    const auto command = udp_bytes("oem bad");
    const auto request = packet(UdpPacketId::Fastboot, 1, command);
    script->expect_send(request, peer);
    const auto first = packet(UdpPacketId::Error, 1, udp_bytes("den"), 1);
    script->provide_receive(first, peer);
    const auto prompt = packet(UdpPacketId::Error, 2);
    script->expect_send(prompt, peer);
    const auto second = packet(UdpPacketId::Error, 2, udp_bytes("ied"));
    script->provide_receive(second, peer);
    const auto result = transport->write(command, 5s);
    require_equal(result.status, TransportStatus::IoError, "continued target error succeeded");
    require(result.detail.find("denied") != std::string::npos,
            "continued error text was not assembled");
    require(script->complete(), "error continuation used the wrong packet ID or sequence");
}

void test_exact_host_maximum_packet_is_accepted() {
    const auto peer = peer4(1);
    auto script = std::make_shared<DatagramScriptState>();
    auto transport = ready_transport(script, peer, 0, 8192);
    std::vector<std::byte> payload(8192U - kFastbootUdpHeaderBytes, std::byte{0x5A});
    const auto request = packet(UdpPacketId::Fastboot, 1, payload);
    require_equal(request.size(), std::size_t{8192}, "test packet is not exactly 8192 bytes");
    script->expect_send(request, peer);
    const auto response = packet(UdpPacketId::Fastboot, 1);
    script->provide_receive(response, peer);
    const auto result = transport->write(payload, 5s);
    require_equal(result.status, TransportStatus::Ok, "8192-byte datagram was rejected");
    require(script->complete(), "8192-byte datagram was fragmented");
}

void test_option_limits_prevent_retry_amplification() {
    const auto peer = peer4(1);
    {
        const UdpTransportOptions defaults;
        require_equal(defaults.query_attempts, std::uint16_t{4},
                      "default query attempt count diverged from AOSP");
        require_equal(defaults.transmission_attempts, std::uint16_t{120},
                      "default transmission attempt count diverged from AOSP");
        require(defaults.retransmit_interval * defaults.transmission_attempts >= 60s,
                "default connected retry window is shorter than one minute");
    }
    {
        auto options = test_options();
        options.retransmit_interval = 499ms;
        auto script = std::make_shared<DatagramScriptState>();
        auto result = UdpFastbootTransport::create(
            make_scripted_datagram_socket(script), peer, options);
        require(!result.has_value(), "sub-500ms retransmission was accepted");
        require(script->steps.empty(), "invalid retry interval touched the network");
    }
    {
        auto options = test_options();
        options.transmission_attempts = 121;
        auto script = std::make_shared<DatagramScriptState>();
        auto result = UdpFastbootTransport::create(
            make_scripted_datagram_socket(script), peer, options);
        require(!result.has_value(), "excessive retry count was accepted");
        require(script->steps.empty(), "invalid retry count touched the network");
    }
    {
        auto options = test_options();
        options.host_max_packet_bytes = 8193;
        auto script = std::make_shared<DatagramScriptState>();
        auto result = UdpFastbootTransport::create(
            make_scripted_datagram_socket(script), peer, options);
        require(!result.has_value(), "host packet limit above 8192 was accepted");
    }
}

void test_close_is_idempotent_and_blocks_io() {
    const auto peer = peer4(1);
    auto script = std::make_shared<DatagramScriptState>();
    auto transport = ready_transport(script, peer);
    transport->close();
    transport->close();
    require_equal(script->close_count, std::size_t{1}, "socket closed more than once");
    const auto result = transport->write(udp_bytes("boot"), 5s);
    require_equal(result.status, TransportStatus::Disconnected, "closed write was not disconnected");
    require_equal(
        result.certainty,
        TransferCertainty::NotTransferred,
        "closed write reported wire transfer");
}

using Test = std::pair<std::string_view, std::function<void()>>;

}  // namespace

int main() {
    const std::vector<Test> tests{
        {"header codec boundaries", test_header_codec_boundaries},
        {"endpoint parser IPv4 IPv6", test_endpoint_parser_ipv4_ipv6},
        {"resolution absolute deadline", test_resolution_uses_absolute_deadline},
        {"initialization negotiation", test_initialization_negotiates_version_and_packet_limit},
        {"query retransmission", test_query_retransmission_is_identical},
        {"query continuation", test_query_response_continuation_sets_sequence},
        {"invalid handshake responses", test_invalid_handshake_responses_are_rejected},
        {"handshake error packet", test_handshake_error_packet_fails_immediately},
        {"small write and read", test_small_write_and_read},
        {"write fragmentation", test_write_fragments_at_negotiated_boundary},
        {"read continuation", test_read_continuation_prompts_for_each_fragment},
        {"out of order duplicate peer", test_out_of_order_duplicate_id_and_peer_are_ignored},
        {"peer mismatch retransmission", test_peer_mismatch_does_not_suppress_retransmission},
        {"unknown flags poison", test_unknown_flags_poison_transport},
        {"truncated oversized poison", test_truncated_and_oversized_datagrams_poison_transport},
        {"socket truncation poison", test_socket_truncation_status_poison_transport},
        {"out of turn write response", test_out_of_turn_write_response_is_rejected},
        {"read overflow and empty", test_read_overflow_and_empty_response_are_rejected},
        {"lost ACK certainty", test_lost_ack_maps_unknown_certainty_and_poison},
        {"unsent and partial certainty", test_unsent_and_partial_send_certainty},
        {"cancellation", test_cancellation_before_write_is_not_sent_and_closes},
        {"handshake cancellation kind", test_handshake_cancellation_has_distinct_error_kind},
        {"ignored datagram cap", test_ignored_datagram_limit_prevents_receive_amplification},
        {"empty continuation", test_empty_continuation_prompts_next_sequence},
        {"outbound sequence wrap", test_outbound_fragments_wrap_modulo_16_bits},
        {"continuation sequence wrap", test_response_continuation_wraps_modulo_16_bits},
        {"query sequence FFFF", test_query_sequence_ffff_initializes_then_wraps},
        {"large download wrap support", test_large_download_is_not_rejected_before_wire},
        {"target error certainty", test_target_error_maps_delivery_and_poison},
        {"target error continuation", test_target_error_continuation_uses_error_id},
        {"exact host maximum packet", test_exact_host_maximum_packet_is_accepted},
        {"option amplification limits", test_option_limits_prevent_retry_amplification},
        {"close idempotence", test_close_is_idempotent_and_blocks_io},
    };

    std::size_t failures = 0;
    for (const auto& [name, test] : tests) {
        try {
            test();
            std::cout << "PASS " << name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "FAIL " << name << ": " << error.what() << '\n';
        } catch (...) {
            ++failures;
            std::cerr << "FAIL " << name << ": unknown exception\n";
        }
    }
    if (failures != 0) {
        std::cerr << failures << " UDP test(s) failed\n";
        return 1;
    }
    std::cout << tests.size() << " UDP tests passed\n";
    return 0;
}
