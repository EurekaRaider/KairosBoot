// SPDX-License-Identifier: MIT
#pragma once

#include "src/protocol/fastboot_protocol.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace kairosboot::fastboot {

enum class PrimitiveOperation : std::uint8_t {
    GetVar,
    Download,
    Stage,
    Boot,
    Upload,
    Fetch,
    Flash,
    Erase,
    SetActive,
    Reboot,
    ContinueBoot,
    Oem,
    RawCommand,
};

enum class PrimitiveErrorCode : std::uint8_t {
    InvalidArgument,
    Unsupported,
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
    std::optional<std::uint64_t> inbound_expected{};
    std::uint64_t inbound_transferred{0};
    protocol::TransferCertainty inbound_certainty{
        protocol::TransferCertainty::NotTransferred};
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
    std::optional<std::uint64_t> inbound_expected{};
    std::uint64_t inbound_transferred{0};
    protocol::TransferCertainty inbound_certainty{
        protocol::TransferCertainty::NotTransferred};
    int native_code{0};
    bool session_poisoned{false};
};

struct DownloadAndFlashResult final {
    PrimitiveReply download;
    PrimitiveReply flash;
};

struct DownloadAndBootResult final {
    PrimitiveReply download;
    PrimitiveReply boot;
};

struct FetchRange final {
    std::optional<std::uint64_t> offset;
    std::optional<std::uint64_t> size;
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
    [[nodiscard]] std::expected<PrimitiveReply, PrimitiveError> download_source(
        std::shared_ptr<protocol::ITransferSource> source,
        const protocol::TransferProgressObserver& observer = {});
    [[nodiscard]] std::expected<PrimitiveReply, PrimitiveError> stage(
        std::span<const std::byte> bytes);
    [[nodiscard]] std::expected<PrimitiveReply, PrimitiveError> stage_source(
        std::shared_ptr<protocol::ITransferSource> source,
        const protocol::TransferProgressObserver& observer = {});
    [[nodiscard]] std::expected<PrimitiveReply, PrimitiveError> upload_to_sink(
        std::shared_ptr<protocol::ITransferSink> sink,
        std::uint64_t maximum_bytes,
        const protocol::TransferProgressObserver& observer = {});
    [[nodiscard]] std::expected<PrimitiveReply, PrimitiveError>
    get_staged_to_sink(
        std::shared_ptr<protocol::ITransferSink> sink,
        std::uint64_t maximum_bytes,
        const protocol::TransferProgressObserver& observer = {});
    [[nodiscard]] std::expected<PrimitiveReply, PrimitiveError> fetch_to_sink(
        std::string_view partition,
        FetchRange range,
        std::shared_ptr<protocol::ITransferSink> sink,
        std::uint64_t maximum_bytes,
        const protocol::TransferProgressObserver& observer = {});
    [[nodiscard]] std::expected<PrimitiveReply, PrimitiveError> boot_downloaded();
    [[nodiscard]] std::expected<DownloadAndBootResult, PrimitiveError>
    download_and_boot(std::span<const std::byte> bytes);
    [[nodiscard]] std::expected<DownloadAndBootResult, PrimitiveError>
    download_and_boot_source(
        std::shared_ptr<protocol::ITransferSource> source,
        const protocol::TransferProgressObserver& observer = {});
    [[nodiscard]] std::expected<PrimitiveReply, PrimitiveError> flash_downloaded(
        std::string_view partition);
    [[nodiscard]] std::expected<DownloadAndFlashResult, PrimitiveError>
    download_and_flash(
        std::string_view partition,
        std::span<const std::byte> bytes);
    [[nodiscard]] std::expected<DownloadAndFlashResult, PrimitiveError>
    download_and_flash_source(
        std::string_view partition,
        std::shared_ptr<protocol::ITransferSource> source,
        const protocol::TransferProgressObserver& observer = {});
    [[nodiscard]] std::expected<PrimitiveReply, PrimitiveError> erase(
        std::string_view partition);
    [[nodiscard]] std::expected<PrimitiveReply, PrimitiveError> set_active(
        std::string_view slot);
    [[nodiscard]] std::expected<PrimitiveReply, PrimitiveError> reboot(
        RebootTarget target = RebootTarget::System);
    [[nodiscard]] std::expected<PrimitiveReply, PrimitiveError> continue_boot();
    [[nodiscard]] std::expected<PrimitiveReply, PrimitiveError> oem(
        std::string_view raw_suffix);
    [[nodiscard]] std::expected<PrimitiveReply, PrimitiveError> raw_command(
        std::string_view command_text);

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
    [[nodiscard]] std::expected<PrimitiveReply, PrimitiveError> finish_download(
        std::expected<protocol::CommandResult, protocol::ProtocolError> result,
        PrimitiveOperation operation = PrimitiveOperation::Download);
    [[nodiscard]] std::expected<PrimitiveReply, PrimitiveError> finish_receive(
        PrimitiveOperation operation,
        std::expected<protocol::CommandResult, protocol::ProtocolError> result);
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
