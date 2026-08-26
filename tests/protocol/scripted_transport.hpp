// SPDX-License-Identifier: MIT
#pragma once

#include "transport_session.hpp"

#include <atomic>
#include <cstddef>
#include <deque>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace kairosboot::protocol::test {

[[nodiscard]] std::vector<std::byte> to_bytes(std::string_view value);

class ScriptedTransport final : public ITransportSession {
public:
    struct WriteStep {
        std::vector<std::byte> expected_call;
        std::size_t reported_transferred;
        TransportStatus status{TransportStatus::Ok};
        TransferCertainty certainty{TransferCertainty::FullyTransferred};
        bool truncated{false};
        std::string detail;
        int native_code{0};
    };

    struct ReadStep {
        std::vector<std::byte> response;
        std::optional<std::size_t> reported_transferred;
        TransportStatus status{TransportStatus::Ok};
        TransferCertainty certainty{TransferCertainty::FullyTransferred};
        bool truncated{false};
        std::string detail;
        int native_code{0};
    };

    void expect_write(
        std::string_view expected_call,
        std::optional<std::size_t> reported_transferred = std::nullopt,
        TransportStatus status = TransportStatus::Ok,
        TransferCertainty certainty = TransferCertainty::FullyTransferred,
        int native_code = 0,
        std::string detail = {});

    void expect_write(
        std::span<const std::byte> expected_call,
        std::optional<std::size_t> reported_transferred = std::nullopt,
        TransportStatus status = TransportStatus::Ok,
        TransferCertainty certainty = TransferCertainty::FullyTransferred,
        int native_code = 0,
        std::string detail = {});

    void respond(
        std::string_view response,
        TransportStatus status = TransportStatus::Ok,
        TransferCertainty certainty = TransferCertainty::FullyTransferred,
        bool truncated = false,
        std::optional<std::size_t> reported_transferred = std::nullopt,
        int native_code = 0,
        std::string detail = {});

    [[nodiscard]] TransferResult write(
        std::span<const std::byte> bytes,
        std::chrono::milliseconds timeout) override;

    [[nodiscard]] TransferResult read(
        std::span<std::byte> destination,
        std::chrono::milliseconds timeout) override;

    void request_cancel() noexcept override;
    void close() noexcept override;

    [[nodiscard]] bool complete() const noexcept;
    [[nodiscard]] const std::string& failure() const noexcept;
    [[nodiscard]] const std::vector<std::byte>& accepted_bytes() const noexcept;
    [[nodiscard]] bool closed() const noexcept;
    [[nodiscard]] bool cancellation_requested() const noexcept;

private:
    using Step = std::variant<WriteStep, ReadStep>;

    [[nodiscard]] TransferResult unexpected_call(std::string_view operation);

    std::deque<Step> steps_;
    std::vector<std::byte> accepted_bytes_;
    std::string failure_;
    bool closed_{false};
    std::atomic<bool> cancellation_requested_{false};
};

}  // namespace kairosboot::protocol::test
