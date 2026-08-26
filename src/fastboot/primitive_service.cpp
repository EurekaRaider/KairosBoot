// SPDX-License-Identifier: MIT
#include "src/fastboot/primitive_service.hpp"

#include <limits>
#include <utility>

namespace kairosboot::fastboot {
namespace {

[[nodiscard]] PrimitiveError invalid_argument(
    const PrimitiveOperation operation,
    std::string message) {
    return {
        .code = PrimitiveErrorCode::InvalidArgument,
        .operation = operation,
        .phase = protocol::ProtocolPhase::Validation,
        .message = std::move(message),
        .transport_certainty = protocol::TransferCertainty::NotTransferred,
        .outbound_certainty = protocol::TransferCertainty::NotTransferred,
    };
}

[[nodiscard]] bool is_printable_ascii(const std::string_view value) noexcept {
    for (const unsigned char character : value) {
        if (character < 0x20U || character > 0x7EU) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::expected<void, PrimitiveError> validate_download_source(
    const std::shared_ptr<protocol::ITransferSource>& source) {
    if (source == nullptr) {
        return std::unexpected(invalid_argument(
            PrimitiveOperation::Download,
            "Fastboot download source must not be null"));
    }
    return validate_download_size(source->size());
}

[[nodiscard]] protocol::TransferCertainty download_certainty(
    const protocol::ProtocolPhase phase,
    const protocol::TransferCertainty phase_certainty) noexcept {
    switch (phase) {
        case protocol::ProtocolPhase::Validation:
        case protocol::ProtocolPhase::CommandWrite:
        case protocol::ProtocolPhase::InitialResponse:
            return protocol::TransferCertainty::NotTransferred;
        case protocol::ProtocolPhase::DataWrite:
            return phase_certainty;
        case protocol::ProtocolPhase::FinalResponse:
            return protocol::TransferCertainty::FullyTransferred;
    }
    return protocol::TransferCertainty::PartialOrUnknown;
}

[[nodiscard]] PrimitiveErrorCode primitive_error_code(
    const protocol::ProtocolErrorCode code) noexcept {
    using protocol::ProtocolErrorCode;
    switch (code) {
        case ProtocolErrorCode::InvalidArgument:
            return PrimitiveErrorCode::InvalidArgument;
        case ProtocolErrorCode::StreamingUnsupported:
            return PrimitiveErrorCode::Unsupported;
        case ProtocolErrorCode::Busy:
            return PrimitiveErrorCode::Busy;
        case ProtocolErrorCode::Closed:
            return PrimitiveErrorCode::Closed;
        case ProtocolErrorCode::Poisoned:
            return PrimitiveErrorCode::Poisoned;
        case ProtocolErrorCode::TransportCancelled:
            return PrimitiveErrorCode::Cancelled;
        case ProtocolErrorCode::TransportTimeout:
            return PrimitiveErrorCode::Timeout;
        case ProtocolErrorCode::TransportDisconnected:
            return PrimitiveErrorCode::Disconnected;
        case ProtocolErrorCode::TransportIo:
            return PrimitiveErrorCode::TransportIo;
        case ProtocolErrorCode::TransportContractViolation:
        case ProtocolErrorCode::ZeroProgress:
        case ProtocolErrorCode::MalformedResponse:
        case ProtocolErrorCode::UnexpectedResponse:
        case ProtocolErrorCode::DataLengthMismatch:
        case ProtocolErrorCode::TooManyInformationalResponses:
            return PrimitiveErrorCode::ProtocolViolation;
    }
    return PrimitiveErrorCode::ProtocolViolation;
}

}  // namespace

std::expected<void, PrimitiveError> validate_download_size(const std::uint64_t size) {
    if (size == 0) {
        return std::unexpected(invalid_argument(
            PrimitiveOperation::Download,
            "Fastboot download payload must not be empty"));
    }
    if (size > std::numeric_limits<std::uint32_t>::max()) {
        return std::unexpected(invalid_argument(
            PrimitiveOperation::Download,
            "Fastboot download payload exceeds the protocol's 32-bit size limit"));
    }
    return {};
}

PrimitiveService::PrimitiveService(protocol::FastbootSession& session) noexcept
    : session_(session) {}

std::expected<PrimitiveReply, PrimitiveError> PrimitiveService::getvar(
    const std::string_view key) {
    auto command_text = parameter_command(
        PrimitiveOperation::GetVar, "getvar:", key);
    if (!command_text) {
        return std::unexpected(std::move(command_text.error()));
    }
    return command(PrimitiveOperation::GetVar, *command_text);
}

std::expected<PrimitiveReply, PrimitiveError> PrimitiveService::download(
    const std::span<const std::byte> bytes) {
    if (auto size = validate_download_size(bytes.size()); !size) {
        return std::unexpected(std::move(size.error()));
    }

    return finish_download(session_.download(bytes));
}

std::expected<PrimitiveReply, PrimitiveError> PrimitiveService::download_source(
    std::shared_ptr<protocol::ITransferSource> source,
    const protocol::TransferProgressObserver& observer) {
    if (auto valid = validate_download_source(source); !valid) {
        return std::unexpected(std::move(valid.error()));
    }
    return finish_download(
        session_.download_source(std::move(source), observer));
}

std::expected<PrimitiveReply, PrimitiveError> PrimitiveService::flash_downloaded(
    const std::string_view partition) {
    auto command_text = parameter_command(
        PrimitiveOperation::Flash, "flash:", partition);
    if (!command_text) {
        return std::unexpected(std::move(command_text.error()));
    }
    return command(PrimitiveOperation::Flash, *command_text);
}

std::expected<DownloadAndFlashResult, PrimitiveError>
PrimitiveService::download_and_flash(
    const std::string_view partition,
    const std::span<const std::byte> bytes) {
    // Validate every local input before the device accepts a download.
    auto flash_command = parameter_command(
        PrimitiveOperation::Flash, "flash:", partition);
    if (!flash_command) {
        return std::unexpected(std::move(flash_command.error()));
    }
    if (auto size = validate_download_size(bytes.size()); !size) {
        return std::unexpected(std::move(size.error()));
    }

    auto downloaded = download(bytes);
    if (!downloaded) {
        return std::unexpected(std::move(downloaded.error()));
    }
    auto flashed = command(PrimitiveOperation::Flash, *flash_command);
    if (!flashed) {
        return std::unexpected(std::move(flashed.error()));
    }
    return DownloadAndFlashResult{
        .download = std::move(*downloaded),
        .flash = std::move(*flashed),
    };
}

std::expected<DownloadAndFlashResult, PrimitiveError>
PrimitiveService::download_and_flash_source(
    const std::string_view partition,
    std::shared_ptr<protocol::ITransferSource> source,
    const protocol::TransferProgressObserver& observer) {
    // Validate every local input before the device accepts a download.
    auto flash_command = parameter_command(
        PrimitiveOperation::Flash, "flash:", partition);
    if (!flash_command) {
        return std::unexpected(std::move(flash_command.error()));
    }
    if (auto valid = validate_download_source(source); !valid) {
        return std::unexpected(std::move(valid.error()));
    }

    auto downloaded = download_source(std::move(source), observer);
    if (!downloaded) {
        return std::unexpected(std::move(downloaded.error()));
    }
    auto flashed = command(PrimitiveOperation::Flash, *flash_command);
    if (!flashed) {
        return std::unexpected(std::move(flashed.error()));
    }
    return DownloadAndFlashResult{
        .download = std::move(*downloaded),
        .flash = std::move(*flashed),
    };
}

std::expected<PrimitiveReply, PrimitiveError> PrimitiveService::erase(
    const std::string_view partition) {
    auto command_text = parameter_command(
        PrimitiveOperation::Erase, "erase:", partition);
    if (!command_text) {
        return std::unexpected(std::move(command_text.error()));
    }
    return command(PrimitiveOperation::Erase, *command_text);
}

std::expected<PrimitiveReply, PrimitiveError> PrimitiveService::reboot(
    const RebootTarget target) {
    switch (target) {
        case RebootTarget::System:
            return command(PrimitiveOperation::Reboot, "reboot", true);
        case RebootTarget::Bootloader:
            return command(
                PrimitiveOperation::Reboot, "reboot-bootloader", true);
        case RebootTarget::Recovery:
            return command(
                PrimitiveOperation::Reboot, "reboot-recovery", true);
        case RebootTarget::Fastboot:
            return command(
                PrimitiveOperation::Reboot, "reboot-fastboot", true);
    }
    return std::unexpected(invalid_argument(
        PrimitiveOperation::Reboot, "Fastboot reboot target is invalid"));
}

std::expected<PrimitiveReply, PrimitiveError> PrimitiveService::continue_boot() {
    return command(PrimitiveOperation::ContinueBoot, "continue", true);
}

std::expected<PrimitiveReply, PrimitiveError> PrimitiveService::oem(
    const std::string_view raw_suffix) {
    auto command_text = parameter_command(
        PrimitiveOperation::Oem, "oem ", raw_suffix);
    if (!command_text) {
        return std::unexpected(std::move(command_text.error()));
    }
    return command(PrimitiveOperation::Oem, *command_text);
}

void PrimitiveService::request_cancel() noexcept {
    session_.request_cancel();
}

std::expected<std::string, PrimitiveError> PrimitiveService::parameter_command(
    const PrimitiveOperation operation,
    const std::string_view prefix,
    const std::string_view parameter) const {
    if (parameter.empty()) {
        return std::unexpected(invalid_argument(
            operation, "Fastboot command parameter must not be empty"));
    }
    if (!is_printable_ascii(parameter)) {
        return std::unexpected(invalid_argument(
            operation, "Fastboot command parameter must contain printable ASCII only"));
    }
    if (parameter.size() > protocol::kDefaultMaxCommandBytes - prefix.size()) {
        return std::unexpected(invalid_argument(
            operation, "Fastboot command exceeds the 4096-byte protocol limit"));
    }

    std::string result;
    result.reserve(prefix.size() + parameter.size());
    result.append(prefix);
    result.append(parameter);
    return result;
}

std::expected<PrimitiveReply, PrimitiveError> PrimitiveService::command(
    const PrimitiveOperation operation,
    const std::string_view command_text,
    const bool retire_on_success) {
    auto result = session_.command(command_text);
    if (!result) {
        return std::unexpected(protocol_error(operation, result.error(), false));
    }
    if (!result->succeeded()) {
        return std::unexpected(device_fail(operation, *result, false));
    }

    PrimitiveReply reply{
        .terminal = std::move(result->terminal),
        .informational = std::move(result->informational),
        .phase = result->phase,
        .outbound_certainty = result->outbound_certainty,
    };
    if (retire_on_success) {
        session_.close();
    }
    return reply;
}

std::expected<PrimitiveReply, PrimitiveError> PrimitiveService::finish_download(
    std::expected<protocol::CommandResult, protocol::ProtocolError> result) {
    if (!result) {
        return std::unexpected(protocol_error(
            PrimitiveOperation::Download, result.error(), true));
    }
    if (!result->succeeded()) {
        return std::unexpected(device_fail(
            PrimitiveOperation::Download, *result, true));
    }
    return PrimitiveReply{
        .terminal = std::move(result->terminal),
        .informational = std::move(result->informational),
        .phase = result->phase,
        .outbound_certainty = protocol::TransferCertainty::FullyTransferred,
    };
}

PrimitiveError PrimitiveService::protocol_error(
    const PrimitiveOperation operation,
    const protocol::ProtocolError& error,
    const bool download_semantics) const {
    return {
        .code = primitive_error_code(error.code),
        .operation = operation,
        .phase = error.phase,
        .message = error.message,
        .transport_status = error.transport_status,
        .transport_certainty = error.transfer_certainty,
        .outbound_certainty = download_semantics
            ? download_certainty(error.phase, error.outbound_certainty)
            : error.outbound_certainty,
        .native_code = error.native_code,
        .session_poisoned = session_.state() == protocol::SessionState::Poisoned,
    };
}

PrimitiveError PrimitiveService::device_fail(
    const PrimitiveOperation operation,
    const protocol::CommandResult& result,
    const bool download_semantics) const {
    return {
        .code = PrimitiveErrorCode::DeviceFail,
        .operation = operation,
        .phase = result.phase,
        .message = "Fastboot device rejected the command",
        .device_message = result.terminal.payload,
        .informational = result.informational,
        .transport_status = protocol::TransportStatus::Ok,
        .transport_certainty = protocol::TransferCertainty::FullyTransferred,
        .outbound_certainty = download_semantics
            ? download_certainty(result.phase, result.outbound_certainty)
            : result.outbound_certainty,
        .native_code = 0,
        .session_poisoned = false,
    };
}

}  // namespace kairosboot::fastboot
