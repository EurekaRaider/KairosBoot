// SPDX-License-Identifier: MIT
#pragma once

#include "tcp_fastboot.hpp"

#include <cstddef>
#include <deque>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace kairosboot::transport::test {

[[nodiscard]] std::vector<std::byte> to_bytes(std::string_view value);

struct SendStep {
    std::vector<std::byte> expected_call;
    SocketIoResult result;
};

struct ReceiveStep {
    std::vector<std::byte> bytes;
    SocketIoResult result;
    std::optional<std::size_t> expected_destination_size;
};

struct ScriptState {
    using Step = std::variant<SendStep, ReceiveStep>;

    std::deque<Step> steps;
    std::vector<std::byte> accepted_bytes;
    std::string failure;
    bool closed{false};
    std::size_t close_count{0};

    void expect_send(
        std::span<const std::byte> expected_call,
        SocketIoStatus status = SocketIoStatus::Ok,
        std::optional<std::size_t> reported_transferred = std::nullopt,
        std::string detail = {});

    void expect_send(
        std::string_view expected_call,
        SocketIoStatus status = SocketIoStatus::Ok,
        std::optional<std::size_t> reported_transferred = std::nullopt,
        std::string detail = {});

    void provide_receive(
        std::span<const std::byte> bytes,
        SocketIoStatus status = SocketIoStatus::Ok,
        std::optional<std::size_t> reported_transferred = std::nullopt,
        std::optional<std::size_t> expected_destination_size = std::nullopt,
        std::string detail = {});

    void provide_receive(
        std::string_view bytes,
        SocketIoStatus status = SocketIoStatus::Ok,
        std::optional<std::size_t> reported_transferred = std::nullopt,
        std::optional<std::size_t> expected_destination_size = std::nullopt,
        std::string detail = {});

    [[nodiscard]] bool complete() const noexcept;
};

class ScriptedSocket final : public ITcpSocket {
public:
    explicit ScriptedSocket(std::shared_ptr<ScriptState> state);

    [[nodiscard]] SocketIoResult send_some(
        std::span<const std::byte> bytes,
        std::chrono::milliseconds timeout,
        CancellationSignal cancellation) override;

    [[nodiscard]] SocketIoResult receive_some(
        std::span<std::byte> destination,
        std::chrono::milliseconds timeout,
        CancellationSignal cancellation) override;

    void close() noexcept override;

private:
    [[nodiscard]] SocketIoResult unexpected_call(std::string_view operation);

    std::shared_ptr<ScriptState> state_;
};

[[nodiscard]] std::unique_ptr<ITcpSocket> make_scripted_socket(
    const std::shared_ptr<ScriptState>& state);

}  // namespace kairosboot::transport::test
