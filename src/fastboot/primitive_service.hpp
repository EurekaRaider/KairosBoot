// SPDX-License-Identifier: MIT
#pragma once

#include "src/protocol/fastboot_protocol.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace kairosboot::fastboot {

enum class PrimitiveOperation : std::uint8_t {
    GetVar,
    Download,
    Flash,
    Erase,
    Reboot,
    ContinueBoot,
    Oem,
};

enum class PrimitiveErrorCode : std::uint8_t {
    InvalidArgument,
    Busy,
    Closed,
    Poisoned,
    Cancelled,
    Timeout,
    Disconnected,
    TransportIo,
    ProtocolViolation,
    DeviceFail,
};

enum class RebootTarget : std::uint8_t {
    System,
    Bootloader,
    Recovery,
    Fastboot,
};

struct PrimitiveReply final {
    protocol::Response terminal;
    std::vector<protocol::Response> informational;
    protocol::ProtocolPhase phase{protocol::ProtocolPhase::FinalResponse};
    protocol::TransferCertainty outbound_certainty{
        protocol::TransferCertainty::FullyTransferred};
};

struct PrimitiveError final {
    PrimitiveErrorCode code{PrimitiveErrorCode::ProtocolViolation};
    PrimitiveOperation operation{PrimitiveOperation::GetVar};
    protocol::ProtocolPhase phase{protocol::ProtocolPhase::Validation};
    std::string message;
    std::string device_message;
    std::vector<protocol::Response> informational;
    protocol::TransportStatus transport_status{protocol::TransportStatus::Ok};
    protocol::TransferCertainty transport_certainty{
        protocol::TransferCertainty::NotTransferred};
    // For Download this describes the image payload. For other primitives it
    // describes the exact command bytes named by operation.
    protocol::TransferCertainty outbound_certainty{
        protocol::TransferCertainty::NotTransferred};
    int native_code{0};
    bool session_poisoned{false};
};

struct DownloadAndFlashResult final {
    PrimitiveReply download;
    PrimitiveReply flash;
};

[[nodiscard]] std::expected<void, PrimitiveError> validate_download_size(
    std::uint64_t size);

// Internal operation layer over one already-selected transport session. It
// intentionally excludes device discovery, image planning and public ABI
// concerns. Calls are serialized by FastbootSession.
class PrimitiveService final {
public:
    explicit PrimitiveService(protocol::FastbootSession& session) noexcept;

    [[nodiscard]] std::expected<PrimitiveReply, PrimitiveError> getvar(
        std::string_view key);
    [[nodiscard]] std::expected<PrimitiveReply, PrimitiveError> download(
        std::span<const std::byte> bytes);
    [[nodiscard]] std::expected<PrimitiveReply, PrimitiveError> flash_downloaded(
        std::string_view partition);
    [[nodiscard]] std::expected<DownloadAndFlashResult, PrimitiveError>
    download_and_flash(
        std::string_view partition,
        std::span<const std::byte> bytes);
    [[nodiscard]] std::expected<PrimitiveReply, PrimitiveError> erase(
        std::string_view partition);
    [[nodiscard]] std::expected<PrimitiveReply, PrimitiveError> reboot(
        RebootTarget target = RebootTarget::System);
    [[nodiscard]] std::expected<PrimitiveReply, PrimitiveError> continue_boot();
    [[nodiscard]] std::expected<PrimitiveReply, PrimitiveError> oem(
        std::string_view raw_suffix);

    void request_cancel() noexcept;

private:
    [[nodiscard]] std::expected<std::string, PrimitiveError> parameter_command(
        PrimitiveOperation operation,
        std::string_view prefix,
        std::string_view parameter) const;
    [[nodiscard]] std::expected<PrimitiveReply, PrimitiveError> command(
        PrimitiveOperation operation,
        std::string_view command_text,
        bool retire_on_success = false);
    [[nodiscard]] PrimitiveError protocol_error(
        PrimitiveOperation operation,
        const protocol::ProtocolError& error,
        bool download_semantics) const;
    [[nodiscard]] PrimitiveError device_fail(
        PrimitiveOperation operation,
        const protocol::CommandResult& result,
        bool download_semantics) const;

    protocol::FastbootSession& session_;
};

}  // namespace kairosboot::fastboot
