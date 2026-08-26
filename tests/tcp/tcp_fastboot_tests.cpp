// SPDX-License-Identifier: MIT
#include "scripted_socket.hpp"
#include "tcp_fastboot.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <exception>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using kairosboot::protocol::TransferCertainty;
using kairosboot::protocol::TransportStatus;
using kairosboot::transport::SocketIoStatus;
using kairosboot::transport::TcpErrorKind;
using kairosboot::transport::TcpFastbootTransport;
using kairosboot::transport::TcpTransportOptions;
using kairosboot::transport::connect_native_tcp_socket;
using kairosboot::transport::decode_tcp_frame_length;
using kairosboot::transport::encode_tcp_frame_length;
using kairosboot::transport::parse_tcp_endpoint;
using kairosboot::transport::detail::ConnectClock;
using kairosboot::transport::detail::run_connect_resolve_phase;
using kairosboot::transport::test::ScriptState;
using kairosboot::transport::test::make_scripted_socket;
using kairosboot::transport::test::to_bytes;

class CheckFailure final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

#define CHECK(condition)                                                                     \
    do {                                                                                     \
        if (!(condition)) {                                                                  \
            throw CheckFailure(std::string("check failed: ") + #condition + " at line " + \
                               std::to_string(__LINE__));                                     \
        }                                                                                    \
    } while (false)

[[nodiscard]] std::shared_ptr<ScriptState> script_with_handshake(
    const std::string_view peer_handshake = "FB01") {
    auto script = std::make_shared<ScriptState>();
    script->expect_send("FB01");
    script->provide_receive(peer_handshake, SocketIoStatus::Ok, std::nullopt, 4);
    return script;
}

[[nodiscard]] std::unique_ptr<TcpFastbootTransport> open_scripted_transport(
    const std::shared_ptr<ScriptState>& script,
    const TcpTransportOptions options = {}) {
    auto transport = TcpFastbootTransport::create(make_scripted_socket(script), options);
    if (!transport) {
        throw CheckFailure("scripted transport handshake failed: " +
                           transport.error().message);
    }
    return std::move(*transport);
}

[[nodiscard]] std::vector<std::byte> as_vector(
    const std::span<const std::byte> bytes) {
    return {bytes.begin(), bytes.end()};
}

struct BlockingSocketState {
    std::mutex mutex;
    std::condition_variable changed;
    bool operation_entered{false};
    bool closed{false};
};

class CancellationBlockingSocket final : public kairosboot::transport::ITcpSocket {
public:
    explicit CancellationBlockingSocket(std::shared_ptr<BlockingSocketState> state)
        : state_(std::move(state)) {}

    [[nodiscard]] kairosboot::transport::SocketIoResult send_some(
        const std::span<const std::byte> bytes,
        std::chrono::milliseconds /*timeout*/,
        const kairosboot::transport::CancellationSignal cancellation) override {
        if (!handshake_sent_) {
            handshake_sent_ = true;
            const auto& expected = kairosboot::transport::kFastbootTcpV1Handshake;
            CHECK(bytes.size() == expected.size());
            CHECK(std::equal(bytes.begin(), bytes.end(), expected.begin(), expected.end()));
            return {.transferred = bytes.size()};
        }

        std::unique_lock lock(state_->mutex);
        state_->operation_entered = true;
        state_->changed.notify_all();
        const auto notify = [state = state_] {
            std::scoped_lock callback_lock(state->mutex);
            state->changed.notify_all();
        };
        std::stop_callback external_callback(cancellation.external, notify);
        std::stop_callback local_callback(cancellation.local, notify);
        state_->changed.wait(lock, [&] { return cancellation.stop_requested(); });
        return {
            .status = SocketIoStatus::Cancelled,
            .detail = "blocking socket observed cancellation",
        };
    }

    [[nodiscard]] kairosboot::transport::SocketIoResult receive_some(
        const std::span<std::byte> destination,
        std::chrono::milliseconds /*timeout*/,
        kairosboot::transport::CancellationSignal /*cancellation*/) override {
        CHECK(destination.size() == 4);
        std::ranges::copy(kairosboot::transport::kFastbootTcpV1Handshake, destination.begin());
        return {.transferred = 4};
    }

    void close() noexcept override {
        std::scoped_lock lock(state_->mutex);
        state_->closed = true;
        state_->changed.notify_all();
    }

private:
    std::shared_ptr<BlockingSocketState> state_;
    bool handshake_sent_{false};
};

struct ResolvePhaseProbe {
    ConnectClock::time_point now;
    std::chrono::milliseconds resolution_time{0};
    std::stop_source* cancellation{nullptr};
    std::size_t calls{0};
};

[[nodiscard]] ConnectClock::time_point probe_clock_now(void* const opaque) noexcept {
    return static_cast<ResolvePhaseProbe*>(opaque)->now;
}

void run_probe_resolver(void* const opaque) noexcept {
    auto& probe = *static_cast<ResolvePhaseProbe*>(opaque);
    ++probe.calls;
    probe.now += probe.resolution_time;
    if (probe.cancellation != nullptr) {
        probe.cancellation->request_stop();
    }
}

void frame_length_codec_is_big_endian() {
    const std::array values{
        std::uint64_t{0},
        std::uint64_t{1},
        std::uint64_t{0x0102030405060708ULL},
        std::numeric_limits<std::uint64_t>::max(),
    };
    for (const auto value : values) {
        const auto encoded = encode_tcp_frame_length(value);
        CHECK(decode_tcp_frame_length(encoded) == value);
    }

    const auto encoded = encode_tcp_frame_length(0x0102030405060708ULL);
    for (std::size_t index = 0; index < encoded.size(); ++index) {
        CHECK(std::to_integer<unsigned int>(encoded[index]) == index + 1);
    }
}

void endpoint_parser_accepts_ipv4_names_and_ipv6() {
    const auto ipv4_default = parse_tcp_endpoint("127.0.0.1");
    CHECK(ipv4_default);
    CHECK(ipv4_default->host == "127.0.0.1");
    CHECK(ipv4_default->port == 5554);

    const auto ipv4_port = parse_tcp_endpoint("127.0.0.1:1234");
    CHECK(ipv4_port);
    CHECK(ipv4_port->host == "127.0.0.1");
    CHECK(ipv4_port->port == 1234);

    const auto name = parse_tcp_endpoint("fastboot-device.local", 4321);
    CHECK(name);
    CHECK(name->host == "fastboot-device.local");
    CHECK(name->port == 4321);

    const auto raw_ipv6 = parse_tcp_endpoint("::1");
    CHECK(raw_ipv6);
    CHECK(raw_ipv6->host == "::1");
    CHECK(raw_ipv6->port == 5554);

    const auto bracketed_ipv6 = parse_tcp_endpoint("[::1]:6000");
    CHECK(bracketed_ipv6);
    CHECK(bracketed_ipv6->host == "::1");
    CHECK(bracketed_ipv6->port == 6000);

    const auto bracketed_default = parse_tcp_endpoint("[2001:db8::1]");
    CHECK(bracketed_default);
    CHECK(bracketed_default->host == "2001:db8::1");
    CHECK(bracketed_default->port == 5554);
}

void endpoint_parser_rejects_ambiguous_or_invalid_forms() {
    const std::array<std::string_view, 12> invalid{
        "",
        ":5554",
        "host:",
        "host:0",
        "host:65536",
        "host:not-a-port",
        "[::1",
        "[]:5554",
        "[::1]junk",
        "[::1]:",
        "[::1]:0",
        "host name:5554",
    };
    for (const auto endpoint : invalid) {
        const auto parsed = parse_tcp_endpoint(endpoint);
        CHECK(!parsed);
        CHECK(parsed.error().kind == TcpErrorKind::InvalidEndpoint);
    }
    CHECK(!parse_tcp_endpoint("device", 0));
    CHECK(!parse_tcp_endpoint(std::string_view{"host\0evil", 9}));
}

void connect_deadline_includes_name_resolution_time() {
    const auto started = ConnectClock::time_point{10s};
    ResolvePhaseProbe slow{
        .now = started,
        .resolution_time = 6s,
    };
    const auto expired = run_connect_resolve_phase(
        5s,
        {},
        run_probe_resolver,
        &slow,
        probe_clock_now,
        &slow);
    CHECK(expired.resolver_ran);
    CHECK(expired.expired);
    CHECK(!expired.cancelled);
    CHECK(expired.deadline == started + 5s);
    CHECK(slow.calls == 1);

    ResolvePhaseProbe fast{
        .now = started,
        .resolution_time = 2s,
    };
    const auto remaining = run_connect_resolve_phase(
        5s,
        {},
        run_probe_resolver,
        &fast,
        probe_clock_now,
        &fast);
    CHECK(remaining.resolver_ran);
    CHECK(!remaining.expired);
    CHECK(remaining.deadline == started + 5s);
    CHECK(fast.now == started + 2s);

    ResolvePhaseProbe no_budget{
        .now = started,
    };
    const auto already_expired = run_connect_resolve_phase(
        0ms,
        {},
        run_probe_resolver,
        &no_budget,
        probe_clock_now,
        &no_budget);
    CHECK(already_expired.expired);
    CHECK(!already_expired.resolver_ran);
    CHECK(no_budget.calls == 0);
}

void connect_cancel_is_checked_around_name_resolution() {
    {
        std::stop_source cancellation;
        ResolvePhaseProbe probe{
            .now = ConnectClock::time_point{10s},
            .resolution_time = 1s,
            .cancellation = &cancellation,
        };
        const auto result = run_connect_resolve_phase(
            5s,
            cancellation.get_token(),
            run_probe_resolver,
            &probe,
            probe_clock_now,
            &probe);
        CHECK(result.resolver_ran);
        CHECK(result.cancelled);
        CHECK(probe.calls == 1);
    }
    {
        std::stop_source cancellation;
        cancellation.request_stop();
        ResolvePhaseProbe probe{
            .now = ConnectClock::time_point{10s},
        };
        const auto result = run_connect_resolve_phase(
            5s,
            cancellation.get_token(),
            run_probe_resolver,
            &probe,
            probe_clock_now,
            &probe);
        CHECK(!result.resolver_ran);
        CHECK(result.cancelled);
        CHECK(probe.calls == 0);
    }
}

void native_connect_short_circuits_before_name_resolution() {
    const kairosboot::transport::TcpEndpoint endpoint{
        .host = "must-not-resolve.invalid",
        .port = 5554,
    };
    {
        std::stop_source cancellation;
        cancellation.request_stop();
        const auto result = connect_native_tcp_socket(
            endpoint, 5s, cancellation.get_token());
        CHECK(!result);
        CHECK(result.error().kind == TcpErrorKind::Cancelled);
    }
    {
        const auto result = connect_native_tcp_socket(endpoint, 0ms);
        CHECK(!result);
        CHECK(result.error().kind == TcpErrorKind::Timeout);
    }
}

void handshake_completes_partial_send_and_receive() {
    auto script = std::make_shared<ScriptState>();
    script->expect_send("FB01", SocketIoStatus::Ok, 2);
    script->expect_send("01");
    script->provide_receive("F", SocketIoStatus::Ok, 1, 4);
    script->provide_receive("B0", SocketIoStatus::Ok, 2, 3);
    script->provide_receive("1", SocketIoStatus::Ok, 1, 1);

    auto transport = open_scripted_transport(script);
    CHECK(transport->is_open());
    CHECK(script->accepted_bytes == to_bytes("FB01"));
    CHECK(script->complete());
    transport.reset();
    CHECK(script->closed);
    CHECK(script->close_count == 1);
}

void handshake_accepts_a_newer_peer_version() {
    auto script = script_with_handshake("FB02");
    auto transport = open_scripted_transport(script);
    CHECK(transport->is_open());
    CHECK(script->complete());
}

void handshake_rejects_malformed_and_version_zero() {
    const std::array<std::string_view, 5> invalid{
        "XX01", "FBA1", "FB1A", "FB00", "FB0!",
    };
    for (const auto peer_handshake : invalid) {
        auto script = script_with_handshake(peer_handshake);
        auto result = TcpFastbootTransport::create(make_scripted_socket(script));
        CHECK(!result);
        CHECK(result.error().kind == TcpErrorKind::HandshakeFailed);
        CHECK(script->complete());
        CHECK(script->closed);
        CHECK(script->close_count == 1);
    }
}

void handshake_eof_timeout_and_cancel_are_not_success() {
    {
        auto script = std::make_shared<ScriptState>();
        script->expect_send("FB01");
        script->provide_receive("FB", SocketIoStatus::EndOfStream, 2, 4);
        auto result = TcpFastbootTransport::create(make_scripted_socket(script));
        CHECK(!result);
        CHECK(result.error().kind == TcpErrorKind::Disconnected);
        CHECK(script->closed);
    }
    {
        auto script = std::make_shared<ScriptState>();
        script->expect_send(
            "FB01", SocketIoStatus::Timeout, 2, "scripted handshake timeout");
        auto result = TcpFastbootTransport::create(make_scripted_socket(script));
        CHECK(!result);
        CHECK(result.error().kind == TcpErrorKind::Timeout);
        CHECK(script->accepted_bytes == to_bytes("FB"));
        CHECK(script->closed);
    }
    {
        std::stop_source stop;
        stop.request_stop();
        auto script = std::make_shared<ScriptState>();
        TcpTransportOptions options;
        options.cancellation = stop.get_token();
        auto result = TcpFastbootTransport::create(
            make_scripted_socket(script), options);
        CHECK(!result);
        CHECK(result.error().kind == TcpErrorKind::Cancelled);
        CHECK(script->complete());
        CHECK(script->closed);
    }
}

void framed_write_completes_partial_header_and_payload() {
    auto script = script_with_handshake();
    const auto header = encode_tcp_frame_length(6);
    script->expect_send(header, SocketIoStatus::Ok, 3);
    script->expect_send(std::span<const std::byte>(header).subspan(3));
    script->expect_send("abcdef", SocketIoStatus::Ok, 2);
    script->expect_send("cdef");

    auto transport = open_scripted_transport(script);
    const auto payload = to_bytes("abcdef");
    const auto result = transport->write(payload, 100ms);
    CHECK(result.status == TransportStatus::Ok);
    CHECK(result.transferred == payload.size());
    CHECK(result.certainty == TransferCertainty::FullyTransferred);
    CHECK(script->complete());

    auto expected = to_bytes("FB01");
    expected.insert(expected.end(), header.begin(), header.end());
    expected.insert(expected.end(), payload.begin(), payload.end());
    CHECK(script->accepted_bytes == expected);
}

void framed_read_completes_partial_header_and_payload() {
    auto script = script_with_handshake();
    const auto header = encode_tcp_frame_length(6);
    script->provide_receive(
        std::span<const std::byte>(header).first(3), SocketIoStatus::Ok, 3, 8);
    script->provide_receive(
        std::span<const std::byte>(header).subspan(3), SocketIoStatus::Ok, 5, 5);
    script->provide_receive("ab", SocketIoStatus::Ok, 2, 6);
    script->provide_receive("cdef", SocketIoStatus::Ok, 4, 4);

    auto transport = open_scripted_transport(script);
    std::array<std::byte, 16> destination{};
    const auto result = transport->read(destination, 100ms);
    CHECK(result.status == TransportStatus::Ok);
    CHECK(result.transferred == 6);
    CHECK(result.certainty == TransferCertainty::FullyTransferred);
    CHECK(as_vector(std::span(destination).first(6)) == to_bytes("abcdef"));
    CHECK(script->complete());
}

void each_read_consumes_exactly_one_frame() {
    auto script = script_with_handshake();
    const auto first_header = encode_tcp_frame_length(3);
    const auto second_header = encode_tcp_frame_length(4);
    script->provide_receive(first_header);
    script->provide_receive("one");
    script->provide_receive(second_header);
    script->provide_receive("two2");

    auto transport = open_scripted_transport(script);
    std::array<std::byte, 8> destination{};
    const auto first = transport->read(destination, 100ms);
    CHECK(first.status == TransportStatus::Ok);
    CHECK(first.transferred == 3);
    CHECK(as_vector(std::span(destination).first(3)) == to_bytes("one"));
    const auto second = transport->read(destination, 100ms);
    CHECK(second.status == TransportStatus::Ok);
    CHECK(second.transferred == 4);
    CHECK(as_vector(std::span(destination).first(4)) == to_bytes("two2"));
    CHECK(script->complete());
}

void data_reads_split_frames_without_consuming_final_status() {
    auto script = script_with_handshake();
    script->provide_receive(encode_tcp_frame_length(6), SocketIoStatus::Ok, 8, 8);
    script->provide_receive("abcd", SocketIoStatus::Ok, 4, 4);
    script->provide_receive("ef", SocketIoStatus::Ok, 2, 2);
    script->provide_receive(encode_tcp_frame_length(8), SocketIoStatus::Ok, 8, 8);
    script->provide_receive("OKAYdone", SocketIoStatus::Ok, 8, 8);

    auto transport = open_scripted_transport(script);
    std::array<std::byte, 4> destination{};
    const auto first = transport->read_data(destination, 100ms);
    CHECK(first.status == TransportStatus::Ok);
    CHECK(first.transferred == 4);
    CHECK(as_vector(destination) == to_bytes("abcd"));

    const auto second = transport->read_data(destination, 100ms);
    CHECK(second.status == TransportStatus::Ok);
    CHECK(second.transferred == 2);
    CHECK(as_vector(std::span(destination).first(2)) == to_bytes("ef"));

    std::array<std::byte, 16> status{};
    const auto terminal = transport->read(status, 100ms);
    CHECK(terminal.status == TransportStatus::Ok);
    CHECK(terminal.transferred == 8);
    CHECK(as_vector(std::span(status).first(8)) == to_bytes("OKAYdone"));
    CHECK(transport->is_open());
    CHECK(script->complete());
}

void final_status_rejects_unconsumed_data_frame_bytes() {
    auto script = script_with_handshake();
    script->provide_receive(encode_tcp_frame_length(5));
    script->provide_receive("abcd", SocketIoStatus::Ok, 4, 4);

    auto transport = open_scripted_transport(script);
    std::array<std::byte, 4> data{};
    const auto payload = transport->read_data(data, 100ms);
    CHECK(payload.status == TransportStatus::Ok);
    CHECK(payload.transferred == 4);

    std::array<std::byte, 16> status{};
    const auto terminal = transport->read(status, 100ms);
    CHECK(terminal.status == TransportStatus::IoError);
    CHECK(terminal.certainty == TransferCertainty::PartialOrUnknown);
    CHECK(terminal.truncated);
    CHECK(!transport->is_open());
    CHECK(script->complete());
}

void eof_during_header_disconnects_and_closes() {
    auto script = script_with_handshake();
    script->provide_receive("\x00\x00", SocketIoStatus::EndOfStream, 2, 8);
    auto transport = open_scripted_transport(script);
    std::array<std::byte, 16> destination{};
    const auto result = transport->read(destination, 100ms);
    CHECK(result.status == TransportStatus::Disconnected);
    CHECK(result.transferred == 0);
    CHECK(result.certainty == TransferCertainty::PartialOrUnknown);
    CHECK(!transport->is_open());
    CHECK(script->closed);
    CHECK(script->close_count == 1);
}

void eof_after_partial_payload_reports_unknown_partial() {
    auto script = script_with_handshake();
    const auto header = encode_tcp_frame_length(6);
    script->provide_receive(header);
    script->provide_receive("ab", SocketIoStatus::EndOfStream, 2, 6);
    auto transport = open_scripted_transport(script);
    std::array<std::byte, 8> destination{};
    const auto result = transport->read(destination, 100ms);
    CHECK(result.status == TransportStatus::Disconnected);
    CHECK(result.transferred == 2);
    CHECK(result.certainty == TransferCertainty::PartialOrUnknown);
    CHECK(as_vector(std::span(destination).first(2)) == to_bytes("ab"));
    CHECK(!transport->is_open());
}

void write_timeout_preserves_transfer_certainty() {
    {
        auto script = script_with_handshake();
        const auto header = encode_tcp_frame_length(3);
        script->expect_send(header, SocketIoStatus::Timeout, 0);
        auto transport = open_scripted_transport(script);
        const auto result = transport->write(to_bytes("abc"), 100ms);
        CHECK(result.status == TransportStatus::Timeout);
        CHECK(result.transferred == 0);
        CHECK(result.certainty == TransferCertainty::NotTransferred);
        CHECK(!transport->is_open());
    }
    {
        auto script = script_with_handshake();
        const auto header = encode_tcp_frame_length(3);
        script->expect_send(header, SocketIoStatus::Timeout, 3);
        auto transport = open_scripted_transport(script);
        const auto result = transport->write(to_bytes("abc"), 100ms);
        CHECK(result.status == TransportStatus::Timeout);
        CHECK(result.transferred == 0);
        CHECK(result.certainty == TransferCertainty::PartialOrUnknown);
        CHECK(!transport->is_open());
    }
    {
        auto script = script_with_handshake();
        const auto header = encode_tcp_frame_length(3);
        script->expect_send(header);
        script->expect_send("abc", SocketIoStatus::Timeout, 2);
        auto transport = open_scripted_transport(script);
        const auto result = transport->write(to_bytes("abc"), 100ms);
        CHECK(result.status == TransportStatus::Timeout);
        CHECK(result.transferred == 2);
        CHECK(result.certainty == TransferCertainty::PartialOrUnknown);
        CHECK(!transport->is_open());
    }
}

void read_timeout_preserves_transfer_certainty() {
    {
        auto script = script_with_handshake();
        script->provide_receive(std::string_view{}, SocketIoStatus::Timeout, 0, 8);
        auto transport = open_scripted_transport(script);
        std::array<std::byte, 8> destination{};
        const auto result = transport->read(destination, 100ms);
        CHECK(result.status == TransportStatus::Timeout);
        CHECK(result.certainty == TransferCertainty::NotTransferred);
        CHECK(!transport->is_open());
    }
    {
        auto script = script_with_handshake();
        const auto header = encode_tcp_frame_length(3);
        script->provide_receive(
            std::span<const std::byte>(header).first(2),
            SocketIoStatus::Timeout,
            2,
            8);
        auto transport = open_scripted_transport(script);
        std::array<std::byte, 8> destination{};
        const auto result = transport->read(destination, 100ms);
        CHECK(result.status == TransportStatus::Timeout);
        CHECK(result.certainty == TransferCertainty::PartialOrUnknown);
        CHECK(!transport->is_open());
    }
    {
        auto script = script_with_handshake();
        script->provide_receive(encode_tcp_frame_length(3));
        script->provide_receive("ab", SocketIoStatus::Timeout, 2, 3);
        auto transport = open_scripted_transport(script);
        std::array<std::byte, 3> destination{};
        const auto result = transport->read(destination, 100ms);
        CHECK(result.status == TransportStatus::Timeout);
        CHECK(result.transferred == 2);
        CHECK(result.certainty == TransferCertainty::PartialOrUnknown);
        CHECK(!transport->is_open());
    }
}

void local_cancel_interrupts_the_next_operation() {
    auto script = script_with_handshake();
    auto transport = open_scripted_transport(script);
    transport->request_cancel();
    const auto result = transport->write(to_bytes("abc"), 100ms);
    CHECK(result.status == TransportStatus::Cancelled);
    CHECK(result.certainty == TransferCertainty::NotTransferred);
    CHECK(result.detail.find("cancelled") != std::string::npos);
    CHECK(!transport->is_open());
    CHECK(script->complete());
    CHECK(script->closed);
}

void local_cancel_interrupts_an_in_flight_operation() {
    auto state = std::make_shared<BlockingSocketState>();
    auto created = TcpFastbootTransport::create(
        std::make_unique<CancellationBlockingSocket>(state));
    CHECK(created);
    auto transport = std::move(*created);
    std::optional<kairosboot::protocol::TransferResult> result;
    std::jthread worker([&] {
        result = transport->write(to_bytes("abc"), 10s);
    });

    {
        std::unique_lock lock(state->mutex);
        CHECK(state->changed.wait_for(
            lock, 1s, [&] { return state->operation_entered; }));
    }
    transport->request_cancel();
    worker.join();

    CHECK(result.has_value());
    CHECK(result->status == TransportStatus::Cancelled);
    CHECK(result->certainty == TransferCertainty::NotTransferred);
    CHECK(result->detail.find("cancelled") != std::string::npos);
    CHECK(!transport->is_open());
    {
        std::scoped_lock lock(state->mutex);
        CHECK(state->closed);
    }
}

void outbound_limit_rejects_before_any_frame_bytes() {
    auto script = script_with_handshake();
    TcpTransportOptions options;
    options.max_frame_bytes = 4;
    auto transport = open_scripted_transport(script, options);
    const auto accepted_before = script->accepted_bytes;
    const auto result = transport->write(to_bytes("12345"), 100ms);
    CHECK(result.status == TransportStatus::IoError);
    CHECK(result.transferred == 0);
    CHECK(result.certainty == TransferCertainty::NotTransferred);
    CHECK(transport->is_open());
    CHECK(script->accepted_bytes == accepted_before);
    CHECK(script->complete());
}

void configured_frame_limit_is_inclusive() {
    auto script = script_with_handshake();
    const auto header = encode_tcp_frame_length(4);
    script->expect_send(header);
    script->expect_send("1234");
    script->provide_receive(header);
    script->provide_receive("abcd");
    TcpTransportOptions options;
    options.max_frame_bytes = 4;
    auto transport = open_scripted_transport(script, options);

    const auto write = transport->write(to_bytes("1234"), 100ms);
    CHECK(write.status == TransportStatus::Ok);
    CHECK(write.transferred == 4);
    std::array<std::byte, 4> destination{};
    const auto read = transport->read(destination, 100ms);
    CHECK(read.status == TransportStatus::Ok);
    CHECK(read.transferred == 4);
    CHECK(as_vector(destination) == to_bytes("abcd"));
    CHECK(transport->is_open());
    CHECK(script->complete());
}

void inbound_limit_or_destination_overflow_closes() {
    {
        auto script = script_with_handshake();
        script->provide_receive(encode_tcp_frame_length(5));
        TcpTransportOptions options;
        options.max_frame_bytes = 4;
        auto transport = open_scripted_transport(script, options);
        std::array<std::byte, 8> destination{};
        const auto result = transport->read(destination, 100ms);
        CHECK(result.status == TransportStatus::IoError);
        CHECK(result.truncated);
        CHECK(result.certainty == TransferCertainty::PartialOrUnknown);
        CHECK(!transport->is_open());
    }
    {
        auto script = script_with_handshake();
        script->provide_receive(encode_tcp_frame_length(5));
        auto transport = open_scripted_transport(script);
        std::array<std::byte, 4> destination{};
        const auto result = transport->read(destination, 100ms);
        CHECK(result.status == TransportStatus::IoError);
        CHECK(result.truncated);
        CHECK(!transport->is_open());
    }
}

void uint64_max_frame_length_never_wraps() {
    auto script = script_with_handshake();
    script->provide_receive(
        encode_tcp_frame_length(std::numeric_limits<std::uint64_t>::max()));
    TcpTransportOptions options;
    options.max_frame_bytes = std::numeric_limits<std::uint64_t>::max();
    auto transport = open_scripted_transport(script, options);
    std::array<std::byte, 16> destination{};
    const auto result = transport->read(destination, 100ms);
    CHECK(result.status == TransportStatus::IoError);
    CHECK(result.truncated);
    CHECK(!transport->is_open());
}

void zero_length_frame_is_a_complete_transport_frame() {
    auto script = script_with_handshake();
    script->provide_receive(encode_tcp_frame_length(0));
    auto transport = open_scripted_transport(script);
    std::array<std::byte, 1> destination{};
    const auto result = transport->read(destination, 100ms);
    CHECK(result.status == TransportStatus::Ok);
    CHECK(result.transferred == 0);
    CHECK(result.certainty == TransferCertainty::FullyTransferred);
    CHECK(transport->is_open());
    CHECK(script->complete());
}

void zero_progress_socket_result_is_an_io_failure() {
    {
        auto script = script_with_handshake();
        const auto header = encode_tcp_frame_length(1);
        script->expect_send(header, SocketIoStatus::Ok, 0);
        auto transport = open_scripted_transport(script);
        const auto result = transport->write(to_bytes("x"), 100ms);
        CHECK(result.status == TransportStatus::IoError);
        CHECK(result.certainty == TransferCertainty::NotTransferred);
        CHECK(!transport->is_open());
    }
    {
        auto script = script_with_handshake();
        script->provide_receive(std::string_view{}, SocketIoStatus::Ok, 0, 8);
        auto transport = open_scripted_transport(script);
        std::array<std::byte, 1> destination{};
        const auto result = transport->read(destination, 100ms);
        CHECK(result.status == TransportStatus::IoError);
        CHECK(result.certainty == TransferCertainty::NotTransferred);
        CHECK(!transport->is_open());
    }
}

void socket_overreport_is_rejected() {
    {
        auto script = script_with_handshake();
        const auto header = encode_tcp_frame_length(1);
        script->expect_send(header, SocketIoStatus::Ok, 9);
        auto transport = open_scripted_transport(script);
        const auto result = transport->write(to_bytes("x"), 100ms);
        CHECK(result.status == TransportStatus::IoError);
        CHECK(result.certainty == TransferCertainty::PartialOrUnknown);
        CHECK(!transport->is_open());
    }
    {
        auto script = script_with_handshake();
        script->provide_receive(std::string_view{}, SocketIoStatus::Ok, 9, 8);
        auto transport = open_scripted_transport(script);
        std::array<std::byte, 1> destination{};
        const auto result = transport->read(destination, 100ms);
        CHECK(result.status == TransportStatus::IoError);
        CHECK(result.certainty == TransferCertainty::PartialOrUnknown);
        CHECK(!transport->is_open());
    }
}

void partial_error_bytes_are_reported_and_never_succeed() {
    {
        auto script = script_with_handshake();
        const auto header = encode_tcp_frame_length(3);
        script->expect_send(header);
        script->expect_send("abc", SocketIoStatus::Error, 2, "injected send error");
        auto transport = open_scripted_transport(script);
        const auto result = transport->write(to_bytes("abc"), 100ms);
        CHECK(result.status == TransportStatus::IoError);
        CHECK(result.transferred == 2);
        CHECK(result.certainty == TransferCertainty::PartialOrUnknown);
        CHECK(!transport->is_open());
    }
    {
        auto script = script_with_handshake();
        script->provide_receive(encode_tcp_frame_length(3));
        script->provide_receive(
            "ab", SocketIoStatus::Error, 2, 3, "injected receive error");
        auto transport = open_scripted_transport(script);
        std::array<std::byte, 3> destination{};
        const auto result = transport->read(destination, 100ms);
        CHECK(result.status == TransportStatus::IoError);
        CHECK(result.transferred == 2);
        CHECK(result.certainty == TransferCertainty::PartialOrUnknown);
        CHECK(as_vector(std::span(destination).first(2)) == to_bytes("ab"));
        CHECK(!transport->is_open());
    }
}

void closed_transport_never_reuses_the_socket() {
    auto script = script_with_handshake();
    script->provide_receive(std::string_view{}, SocketIoStatus::EndOfStream, 0, 8);
    auto transport = open_scripted_transport(script);
    std::array<std::byte, 8> destination{};
    const auto first = transport->read(destination, 100ms);
    CHECK(first.status == TransportStatus::Disconnected);
    const auto second = transport->write(to_bytes("retry"), 100ms);
    CHECK(second.status == TransportStatus::Disconnected);
    CHECK(second.certainty == TransferCertainty::NotTransferred);
    CHECK(script->complete());
    CHECK(script->close_count == 1);
}

void null_socket_is_rejected() {
    auto result = TcpFastbootTransport::create(nullptr);
    CHECK(!result);
    CHECK(result.error().kind == TcpErrorKind::Io);
}

struct TestCase {
    std::string_view name;
    std::function<void()> run;
};

}  // namespace

int main() {
    const std::vector<TestCase> tests{
        {"frame length codec is big endian", frame_length_codec_is_big_endian},
        {"endpoint parser accepts IPv4 names and IPv6",
         endpoint_parser_accepts_ipv4_names_and_ipv6},
        {"endpoint parser rejects ambiguous or invalid forms",
         endpoint_parser_rejects_ambiguous_or_invalid_forms},
        {"connect deadline includes name resolution time",
         connect_deadline_includes_name_resolution_time},
        {"connect cancel is checked around name resolution",
         connect_cancel_is_checked_around_name_resolution},
        {"native connect short circuits before name resolution",
         native_connect_short_circuits_before_name_resolution},
        {"handshake completes partial send and receive",
         handshake_completes_partial_send_and_receive},
        {"handshake accepts a newer peer version", handshake_accepts_a_newer_peer_version},
        {"handshake rejects malformed and version zero",
         handshake_rejects_malformed_and_version_zero},
        {"handshake EOF timeout and cancel are not success",
         handshake_eof_timeout_and_cancel_are_not_success},
        {"framed write completes partial header and payload",
         framed_write_completes_partial_header_and_payload},
        {"framed read completes partial header and payload",
         framed_read_completes_partial_header_and_payload},
        {"each read consumes exactly one frame", each_read_consumes_exactly_one_frame},
        {"DATA reads split frames without consuming status",
         data_reads_split_frames_without_consuming_final_status},
        {"status rejects unconsumed DATA frame bytes",
         final_status_rejects_unconsumed_data_frame_bytes},
        {"EOF during header disconnects and closes", eof_during_header_disconnects_and_closes},
        {"EOF after partial payload reports unknown partial",
         eof_after_partial_payload_reports_unknown_partial},
        {"write timeout preserves transfer certainty", write_timeout_preserves_transfer_certainty},
        {"read timeout preserves transfer certainty", read_timeout_preserves_transfer_certainty},
        {"local cancel interrupts the next operation", local_cancel_interrupts_the_next_operation},
        {"local cancel interrupts an in-flight operation",
         local_cancel_interrupts_an_in_flight_operation},
        {"outbound limit rejects before any frame bytes",
         outbound_limit_rejects_before_any_frame_bytes},
        {"configured frame limit is inclusive", configured_frame_limit_is_inclusive},
        {"inbound limit or destination overflow closes",
         inbound_limit_or_destination_overflow_closes},
        {"uint64 max frame length never wraps", uint64_max_frame_length_never_wraps},
        {"zero length frame is a complete transport frame",
         zero_length_frame_is_a_complete_transport_frame},
        {"zero progress socket result is an IO failure",
         zero_progress_socket_result_is_an_io_failure},
        {"socket overreport is rejected", socket_overreport_is_rejected},
        {"partial error bytes are reported and never succeed",
         partial_error_bytes_are_reported_and_never_succeed},
        {"closed transport never reuses the socket", closed_transport_never_reuses_the_socket},
        {"null socket is rejected", null_socket_is_rejected},
    };

    std::size_t passed = 0;
    for (const auto& test : tests) {
        try {
            test.run();
            ++passed;
            std::cout << "[PASS] " << test.name << '\n';
        } catch (const std::exception& error) {
            std::cerr << "[FAIL] " << test.name << ": " << error.what() << '\n';
            return 1;
        }
    }
    std::cout << passed << " deterministic TCP tests passed\n";
    return 0;
}
