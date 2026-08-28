// SPDX-License-Identifier: MIT
#pragma once

#include "src/fastboot/primitive_service.hpp"
#include "src/protocol/file_transfer_sink.hpp"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace kairosboot::fastboot {

inline constexpr std::uint64_t kMaximumFileReceiveBytes =
    std::numeric_limits<std::uint32_t>::max();

enum class FileReceiveErrorCode : std::uint8_t {
    InvalidArgument,
    LimitExceeded,
    DestinationCreate,
    Protocol,
    DeviceFail,
    DestinationPublish,
};

// Structured error for one device-to-file transaction. Protocol failures retain
// the PrimitiveService metadata verbatim. Destination failures use the same
// fields so callers do not lose a successfully received terminal response when
// the final atomic publication fails.
struct FileReceiveError final {
    FileReceiveErrorCode code{FileReceiveErrorCode::Protocol};
    PrimitiveOperation operation{PrimitiveOperation::Upload};
    protocol::ProtocolPhase phase{protocol::ProtocolPhase::Validation};
    std::string message;
    std::string device_message;
    std::optional<PrimitiveErrorCode> primitive_code{};
    std::optional<protocol::FileTransferSinkErrorKind> file_code{};
    std::optional<protocol::Response> terminal{};
    std::vector<protocol::Response> informational;
    protocol::TransportStatus transport_status{protocol::TransportStatus::Ok};
    protocol::TransferCertainty transport_certainty{
        protocol::TransferCertainty::NotTransferred};
    protocol::TransferCertainty outbound_certainty{
        protocol::TransferCertainty::NotTransferred};
    std::optional<std::uint64_t> inbound_expected{};
    std::uint64_t inbound_transferred{0};
    protocol::TransferCertainty inbound_certainty{
        protocol::TransferCertainty::NotTransferred};
    int native_code{0};
    bool session_poisoned{false};
};

struct FileReceiveResult final {
    // Received payload bytes live only in the published file. The result owns
    // protocol metadata, never a second in-memory copy of the payload.
    PrimitiveReply reply;
    std::uint64_t bytes_published{0};
};

// Transactional file destination for Fastboot upload and fetch. A destination
// is published only after the protocol reports terminal OKAY and every byte
// announced by DATA has been committed to the private file. Calls are
// synchronous; observers are borrowed for the call and are never retained.
// maximum_bytes must be in [1, kMaximumFileReceiveBytes].
class FileReceiveService final {
public:
    explicit FileReceiveService(PrimitiveService& primitives) noexcept;

    [[nodiscard]] std::expected<FileReceiveResult, FileReceiveError> upload(
        const std::filesystem::path& destination,
        std::uint64_t maximum_bytes,
        const protocol::TransferProgressObserver& observer = {});

    [[nodiscard]] std::expected<FileReceiveResult, FileReceiveError>
    get_staged(
        const std::filesystem::path& destination,
        std::uint64_t maximum_bytes,
        const protocol::TransferProgressObserver& observer = {});

    [[nodiscard]] std::expected<FileReceiveResult, FileReceiveError> fetch(
        std::string_view partition,
        FetchRange range,
        const std::filesystem::path& destination,
        std::uint64_t maximum_bytes,
        const protocol::TransferProgressObserver& observer = {});

    void request_cancel() noexcept;

private:
    [[nodiscard]] std::expected<
        std::shared_ptr<protocol::FileTransferSink>, FileReceiveError>
    create_sink(
        PrimitiveOperation operation,
        const std::filesystem::path& destination) const;
    [[nodiscard]] std::expected<FileReceiveResult, FileReceiveError> finish(
        PrimitiveOperation operation,
        std::shared_ptr<protocol::FileTransferSink> sink,
        std::uint64_t maximum_bytes,
        std::expected<PrimitiveReply, PrimitiveError> reply) const;

    PrimitiveService& primitives_;
};

}  // namespace kairosboot::fastboot
