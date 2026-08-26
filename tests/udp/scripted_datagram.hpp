// SPDX-License-Identifier: MIT
#pragma once

#include "udp_fastboot.hpp"

#include <deque>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace kairosboot::transport::test {

[[nodiscard]] std::vector<std::byte> udp_bytes(std::string_view value);

struct DatagramSendStep {
    std::vector<std::byte> expected_datagram;
    UdpPeer expected_peer;
    DatagramSendResult result;
};

struct DatagramReceiveStep {
    std::vector<std::byte> datagram;
    DatagramReceiveResult result;
    std::optional<std::size_t> expected_destination_size;
};

struct DatagramScriptState {
    using Step = std::variant<DatagramSendStep, DatagramReceiveStep>;

    std::deque<Step> steps;
    std::vector<std::vector<std::byte>> sent_datagrams;
    std::string failure;
    bool closed{false};
    std::size_t close_count{0};

    void expect_send(
        std::span<const std::byte> datagram,
        const UdpPeer& peer,
        DatagramIoStatus status = DatagramIoStatus::Ok,
        std::optional<std::size_t> reported_transferred = std::nullopt,
        std::string detail = {});

    void provide_receive(
        std::span<const std::byte> datagram,
        const UdpPeer& peer,
        DatagramIoStatus status = DatagramIoStatus::Ok,
        std::optional<std::size_t> reported_transferred = std::nullopt,
        std::optional<std::size_t> expected_destination_size = std::nullopt,
        std::string detail = {});

    void provide_timeout(const UdpPeer& peer);
    [[nodiscard]] bool complete() const noexcept;
};

class ScriptedDatagramSocket final : public IUdpSocket {
public:
    explicit ScriptedDatagramSocket(std::shared_ptr<DatagramScriptState> state);

    [[nodiscard]] DatagramSendResult send_datagram(
        std::span<const std::byte> datagram,
        const UdpPeer& peer,
        std::chrono::milliseconds timeout,
        UdpCancellationSignal cancellation) override;

    [[nodiscard]] DatagramReceiveResult receive_datagram(
        std::span<std::byte> destination,
        std::chrono::milliseconds timeout,
        UdpCancellationSignal cancellation) override;

    void close() noexcept override;

private:
    [[nodiscard]] DatagramSendResult unexpected_send(std::string_view detail);
    [[nodiscard]] DatagramReceiveResult unexpected_receive(std::string_view detail);

    std::shared_ptr<DatagramScriptState> state_;
};

[[nodiscard]] std::unique_ptr<IUdpSocket> make_scripted_datagram_socket(
    const std::shared_ptr<DatagramScriptState>& state);

}  // namespace kairosboot::transport::test
