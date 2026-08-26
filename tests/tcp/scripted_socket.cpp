// SPDX-License-Identifier: MIT
#include "scripted_socket.hpp"

#include <algorithm>
#include <utility>

namespace kairosboot::transport::test {

std::vector<std::byte> to_bytes(const std::string_view value) {
    std::vector<std::byte> result;
    result.reserve(value.size());
    for (const char character : value) {
        result.push_back(
            static_cast<std::byte>(static_cast<unsigned char>(character)));
    }
    return result;
}

void ScriptState::expect_send(
    const std::span<const std::byte> expected_call,
    const SocketIoStatus status,
    const std::optional<std::size_t> reported_transferred,
    std::string detail) {
    steps.push_back(SendStep{
        .expected_call = {expected_call.begin(), expected_call.end()},
        .result = {
            .status = status,
            .transferred = reported_transferred.value_or(expected_call.size()),
            .detail = std::move(detail),
        },
    });
}

void ScriptState::expect_send(
    const std::string_view expected_call,
    const SocketIoStatus status,
    const std::optional<std::size_t> reported_transferred,
    std::string detail) {
    const auto data = to_bytes(expected_call);
    expect_send(data, status, reported_transferred, std::move(detail));
}

void ScriptState::provide_receive(
    const std::span<const std::byte> bytes,
    const SocketIoStatus status,
    const std::optional<std::size_t> reported_transferred,
    const std::optional<std::size_t> expected_destination_size,
    std::string detail) {
    steps.push_back(ReceiveStep{
        .bytes = {bytes.begin(), bytes.end()},
        .result = {
            .status = status,
            .transferred = reported_transferred.value_or(bytes.size()),
            .detail = std::move(detail),
        },
        .expected_destination_size = expected_destination_size,
    });
}

void ScriptState::provide_receive(
    const std::string_view bytes,
    const SocketIoStatus status,
    const std::optional<std::size_t> reported_transferred,
    const std::optional<std::size_t> expected_destination_size,
    std::string detail) {
    const auto data = to_bytes(bytes);
    provide_receive(
        data,
        status,
        reported_transferred,
        expected_destination_size,
        std::move(detail));
}

bool ScriptState::complete() const noexcept {
    return steps.empty() && failure.empty();
}

ScriptedSocket::ScriptedSocket(std::shared_ptr<ScriptState> state)
    : state_(std::move(state)) {}

SocketIoResult ScriptedSocket::send_some(
    const std::span<const std::byte> bytes,
    std::chrono::milliseconds /*timeout*/,
    const CancellationSignal cancellation) {
    if (cancellation.stop_requested()) {
        return {
            .status = SocketIoStatus::Cancelled,
            .detail = "script observed cancellation",
        };
    }
    if (state_->steps.empty() ||
        !std::holds_alternative<SendStep>(state_->steps.front())) {
        return unexpected_call("send");
    }

    auto step = std::get<SendStep>(std::move(state_->steps.front()));
    state_->steps.pop_front();
    if (!std::ranges::equal(bytes, step.expected_call) && state_->failure.empty()) {
        state_->failure = "send call did not match the scripted bytes";
    }
    const auto accepted = std::min({
        step.result.transferred,
        bytes.size(),
        step.expected_call.size(),
    });
    state_->accepted_bytes.insert(
        state_->accepted_bytes.end(),
        bytes.begin(),
        bytes.begin() + static_cast<std::ptrdiff_t>(accepted));
    return step.result;
}

SocketIoResult ScriptedSocket::receive_some(
    const std::span<std::byte> destination,
    std::chrono::milliseconds /*timeout*/,
    const CancellationSignal cancellation) {
    if (cancellation.stop_requested()) {
        return {
            .status = SocketIoStatus::Cancelled,
            .detail = "script observed cancellation",
        };
    }
    if (state_->steps.empty() ||
        !std::holds_alternative<ReceiveStep>(state_->steps.front())) {
        return unexpected_call("receive");
    }

    auto step = std::get<ReceiveStep>(std::move(state_->steps.front()));
    state_->steps.pop_front();
    if (step.expected_destination_size.has_value() &&
        *step.expected_destination_size != destination.size() &&
        state_->failure.empty()) {
        state_->failure = "receive destination size did not match the script";
    }
    const auto copied = std::min({
        step.result.transferred,
        destination.size(),
        step.bytes.size(),
    });
    std::ranges::copy_n(
        step.bytes.begin(),
        static_cast<std::ptrdiff_t>(copied),
        destination.begin());
    return step.result;
}

void ScriptedSocket::close() noexcept {
    state_->closed = true;
    ++state_->close_count;
}

SocketIoResult ScriptedSocket::unexpected_call(const std::string_view operation) {
    if (state_->failure.empty()) {
        state_->failure = "unexpected scripted socket ";
        state_->failure.append(operation);
    }
    return {
        .status = SocketIoStatus::Error,
        .detail = state_->failure,
    };
}

std::unique_ptr<ITcpSocket> make_scripted_socket(
    const std::shared_ptr<ScriptState>& state) {
    return std::make_unique<ScriptedSocket>(state);
}

}  // namespace kairosboot::transport::test
