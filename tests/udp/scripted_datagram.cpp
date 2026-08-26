// SPDX-License-Identifier: MIT
#include "scripted_datagram.hpp"

#include <algorithm>
#include <utility>

namespace kairosboot::transport::test {

std::vector<std::byte> udp_bytes(const std::string_view value) {
    std::vector<std::byte> result;
    result.reserve(value.size());
    for (const auto character : value) {
        result.push_back(static_cast<std::byte>(
            static_cast<unsigned char>(character)));
    }
    return result;
}

void DatagramScriptState::expect_send(
    const std::span<const std::byte> datagram,
    const UdpPeer& peer,
    const DatagramIoStatus status,
    const std::optional<std::size_t> reported_transferred,
    std::string detail) {
    steps.emplace_back(DatagramSendStep{
        .expected_datagram = {datagram.begin(), datagram.end()},
        .expected_peer = peer,
        .result = {
            .status = status,
            .transferred = reported_transferred.value_or(
                status == DatagramIoStatus::Ok ? datagram.size() : 0U),
            .detail = std::move(detail),
        },
    });
}

void DatagramScriptState::provide_receive(
    const std::span<const std::byte> datagram,
    const UdpPeer& peer,
    const DatagramIoStatus status,
    const std::optional<std::size_t> reported_transferred,
    const std::optional<std::size_t> expected_destination_size,
    std::string detail) {
    steps.emplace_back(DatagramReceiveStep{
        .datagram = {datagram.begin(), datagram.end()},
        .result = {
            .status = status,
            .transferred = reported_transferred.value_or(
                status == DatagramIoStatus::Timeout ? 0U : datagram.size()),
            .peer = peer,
            .detail = std::move(detail),
        },
        .expected_destination_size = expected_destination_size,
    });
}

void DatagramScriptState::provide_timeout(const UdpPeer& peer) {
    provide_receive({}, peer, DatagramIoStatus::Timeout);
}

bool DatagramScriptState::complete() const noexcept {
    return steps.empty() && failure.empty();
}

ScriptedDatagramSocket::ScriptedDatagramSocket(
    std::shared_ptr<DatagramScriptState> state)
    : state_(std::move(state)) {}

DatagramSendResult ScriptedDatagramSocket::send_datagram(
    const std::span<const std::byte> datagram,
    const UdpPeer& peer,
    const std::chrono::milliseconds,
    const UdpCancellationSignal cancellation) {
    if (cancellation.stop_requested()) {
        return {
            .status = DatagramIoStatus::Cancelled,
            .detail = "script observed cancellation",
        };
    }
    if (state_->steps.empty() ||
        !std::holds_alternative<DatagramSendStep>(state_->steps.front())) {
        return unexpected_send("script expected a receive operation");
    }
    auto step = std::get<DatagramSendStep>(std::move(state_->steps.front()));
    state_->steps.pop_front();
    state_->sent_datagrams.emplace_back(datagram.begin(), datagram.end());
    if (!std::ranges::equal(step.expected_datagram, datagram)) {
        return unexpected_send("outbound datagram did not match the script");
    }
    if (step.expected_peer != peer) {
        return unexpected_send("outbound peer did not match the script");
    }
    return step.result;
}

DatagramReceiveResult ScriptedDatagramSocket::receive_datagram(
    const std::span<std::byte> destination,
    const std::chrono::milliseconds,
    const UdpCancellationSignal cancellation) {
    if (cancellation.stop_requested()) {
        return {
            .status = DatagramIoStatus::Cancelled,
            .detail = "script observed cancellation",
        };
    }
    if (state_->steps.empty() ||
        !std::holds_alternative<DatagramReceiveStep>(state_->steps.front())) {
        return unexpected_receive("script expected a send operation");
    }
    auto step = std::get<DatagramReceiveStep>(std::move(state_->steps.front()));
    state_->steps.pop_front();
    if (step.expected_destination_size.has_value() &&
        *step.expected_destination_size != destination.size()) {
        return unexpected_receive("receive buffer size did not match the script");
    }
    const auto copy_size = std::min(destination.size(), step.datagram.size());
    std::ranges::copy(
        std::span<const std::byte>(step.datagram).first(copy_size),
        destination.begin());
    return step.result;
}

void ScriptedDatagramSocket::close() noexcept {
    state_->closed = true;
    ++state_->close_count;
}

DatagramSendResult ScriptedDatagramSocket::unexpected_send(
    const std::string_view detail) {
    if (state_->failure.empty()) {
        state_->failure = std::string(detail);
    }
    return {
        .status = DatagramIoStatus::Error,
        .detail = state_->failure,
    };
}

DatagramReceiveResult ScriptedDatagramSocket::unexpected_receive(
    const std::string_view detail) {
    if (state_->failure.empty()) {
        state_->failure = std::string(detail);
    }
    return {
        .status = DatagramIoStatus::Error,
        .detail = state_->failure,
    };
}

std::unique_ptr<IUdpSocket> make_scripted_datagram_socket(
    const std::shared_ptr<DatagramScriptState>& state) {
    return std::make_unique<ScriptedDatagramSocket>(state);
}

}  // namespace kairosboot::transport::test
