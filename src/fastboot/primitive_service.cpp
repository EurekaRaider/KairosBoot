// SPDX-License-Identifier: MIT
#include "src/fastboot/primitive_service.hpp"
#include "src/fastboot/slot_planner.hpp"
#include "src/fastboot/update_executor.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <limits>
#include <stop_token>
#include <system_error>
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
        .device_message = {},
        .informational = {},
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

[[nodiscard]] bool is_partition_name_character(
    const unsigned char character) noexcept {
    const auto ascii_alphanumeric =
        (character >= 'a' && character <= 'z') ||
        (character >= 'A' && character <= 'Z') ||
        (character >= '0' && character <= '9');
    return ascii_alphanumeric || character == '_' || character == '-' ||
        character == '.';
}

[[nodiscard]] std::expected<std::string, PrimitiveError>
validated_parameter_command(
    const PrimitiveOperation operation,
    const std::string_view prefix,
    const std::string_view parameter) {
    if (parameter.empty()) {
        return std::unexpected(invalid_argument(
            operation, "Fastboot command parameter must not be empty"));
    }
    if (!is_printable_ascii(parameter)) {
        return std::unexpected(invalid_argument(
            operation, "Fastboot command parameter must contain printable ASCII only"));
    }
    if (prefix.size() > protocol::kDefaultMaxCommandBytes ||
        parameter.size() > protocol::kDefaultMaxCommandBytes - prefix.size()) {
        return std::unexpected(invalid_argument(
            operation, "Fastboot command exceeds the 4096-byte protocol limit"));
    }

    std::string result;
    result.reserve(prefix.size() + parameter.size());
    result.append(prefix);
    result.append(parameter);
    return result;
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
        case protocol::ProtocolPhase::DataRead:
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

[[nodiscard]] SlotError slot_error(
    const SlotErrorCode code,
    std::string message) {
    return {
        .code = code,
        .message = std::move(message),
        .query_error = std::nullopt,
    };
}

[[nodiscard]] SlotError slot_query_error(
    std::string context,
    PrimitiveError error) {
    const auto unsupported = error.code == PrimitiveErrorCode::DeviceFail;
    if (!error.device_message.empty()) {
        context.append(": ");
        context.append(error.device_message);
    }
    return {
        .code = unsupported ? SlotErrorCode::Unsupported
                            : SlotErrorCode::QueryFailed,
        .message = std::move(context),
        .query_error = std::move(error),
    };
}

[[nodiscard]] std::optional<SlotError> slot_interruption(
    const UpdateOperationContext& context,
    const std::string_view position) {
    if (context.cancellation.stop_requested()) {
        return slot_error(
            SlotErrorCode::Cancelled,
            "Fastboot slot query was cancelled " + std::string(position));
    }
    if (context.deadline &&
        std::chrono::steady_clock::now() >= *context.deadline) {
        return slot_error(
            SlotErrorCode::TimedOut,
            "Fastboot slot query deadline expired " + std::string(position));
    }
    return std::nullopt;
}

[[nodiscard]] bool is_valid_partition_name(
    const std::string_view partition) noexcept {
    if (partition.empty() ||
        partition.size() > protocol::kDefaultMaxCommandBytes - 16U) {
        return false;
    }
    return std::ranges::all_of(partition, is_partition_name_character);
}

[[nodiscard]] bool append_fetch_hex(
    std::string& command,
    const std::uint64_t value) {
    std::array<char, 16> digits{};
    const auto [end, error] = std::to_chars(
        digits.data(), digits.data() + digits.size(), value, 16);
    if (error != std::errc{}) {
        return false;
    }
    const auto length = static_cast<std::size_t>(end - digits.data());
    command.append(":0x");
    if (length < 8) {
        command.append(8 - length, '0');
    }
    command.append(digits.data(), length);
    return true;
}

[[nodiscard]] std::expected<std::string, PrimitiveError> fetch_command(
    const std::string_view partition,
    const FetchRange& range) {
    if (!is_valid_partition_name(partition)) {
        return std::unexpected(invalid_argument(
            PrimitiveOperation::Fetch,
            "Fastboot fetch partition name is invalid"));
    }
    if (range.size.has_value() && !range.offset.has_value()) {
        return std::unexpected(invalid_argument(
            PrimitiveOperation::Fetch,
            "Fastboot fetch size requires an explicit offset"));
    }
    constexpr auto maximum_range_value =
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    if ((range.offset && *range.offset > maximum_range_value) ||
        (range.size && *range.size > maximum_range_value)) {
        return std::unexpected(invalid_argument(
            PrimitiveOperation::Fetch,
            "Fastboot fetch offset and size must fit signed 64-bit values"));
    }

    std::string command("fetch:");
    command.append(partition);
    if ((range.offset && !append_fetch_hex(command, *range.offset)) ||
        (range.size && !append_fetch_hex(command, *range.size))) {
        return std::unexpected(invalid_argument(
            PrimitiveOperation::Fetch,
            "Fastboot fetch range could not be formatted"));
    }
    if (command.size() > protocol::kDefaultMaxCommandBytes) {
        return std::unexpected(invalid_argument(
            PrimitiveOperation::Fetch,
            "Fastboot fetch command exceeds the 4096-byte protocol limit"));
    }
    return command;
}

[[nodiscard]] std::expected<std::string, PrimitiveError>
logical_partition_command(
    const PrimitiveOperation operation,
    const std::string_view prefix,
    const std::string_view name,
    const std::optional<std::uint64_t> size) {
    if (name.empty()) {
        return std::unexpected(invalid_argument(
            operation, "Fastboot logical partition name must not be empty"));
    }
    if (!std::ranges::all_of(name, is_partition_name_character)) {
        return std::unexpected(invalid_argument(
            operation,
            "Fastboot logical partition name must use ASCII letters, digits, '.', '-' or '_'"));
    }

    std::array<char, std::numeric_limits<std::uint64_t>::digits10 + 1> digits{};
    std::string_view formatted_size;
    if (size.has_value()) {
        const auto [end, error] = std::to_chars(
            digits.data(), digits.data() + digits.size(), *size, 10);
        if (error != std::errc{}) {
            return std::unexpected(invalid_argument(
                operation,
                "Fastboot logical partition size could not be formatted"));
        }
        formatted_size = std::string_view{
            digits.data(), static_cast<std::size_t>(end - digits.data())};
    }

    const auto suffix_size = size.has_value() ? 1U + formatted_size.size() : 0U;
    const auto fixed_size = prefix.size() + suffix_size;
    if (fixed_size > protocol::kDefaultMaxCommandBytes ||
        name.size() > protocol::kDefaultMaxCommandBytes - fixed_size) {
        return std::unexpected(invalid_argument(
            operation,
            "Fastboot logical partition command exceeds the 4096-byte protocol limit"));
    }

    std::string result;
    result.reserve(fixed_size + name.size());
    result.append(prefix);
    result.append(name);
    if (size.has_value()) {
        result.push_back(':');
        result.append(formatted_size);
    }
    return result;
}

[[nodiscard]] std::expected<std::string, SlotError> normalize_slot_name(
    std::string_view value,
    const SlotErrorCode error_code,
    std::string context) {
    if (value.starts_with('_')) {
        value.remove_prefix(1);
    }
    if (value.size() != 1 || value.front() < 'a' || value.front() > 'z') {
        context.append(" must be one lowercase ASCII letter, optionally prefixed by '_'");
        return std::unexpected(slot_error(error_code, std::move(context)));
    }
    return std::string(value);
}

[[nodiscard]] std::expected<std::vector<std::string>, SlotError>
parse_legacy_slot_suffixes(const std::string_view payload) {
    std::vector<std::string> slots;
    std::size_t start = 0;
    while (start <= payload.size()) {
        const auto comma = payload.find(',', start);
        const auto end = comma == std::string_view::npos ? payload.size() : comma;
        const auto token = payload.substr(start, end - start);
        auto normalized = normalize_slot_name(
            token,
            SlotErrorCode::InvalidDeviceResponse,
            "Fastboot slot-suffixes entry");
        if (!normalized) {
            return std::unexpected(std::move(normalized.error()));
        }
        slots.push_back(std::move(*normalized));
        if (comma == std::string_view::npos) {
            break;
        }
        start = comma + 1;
    }

    if (slots.size() < 2) {
        return std::unexpected(slot_error(
            SlotErrorCode::Unsupported,
            "Fastboot slot-suffixes does not describe an A/B device"));
    }
    if (slots.size() > 26) {
        return std::unexpected(slot_error(
            SlotErrorCode::InvalidDeviceResponse,
            "Fastboot slot-suffixes contains more than 26 slots"));
    }

    std::ranges::sort(slots);
    if (std::adjacent_find(slots.begin(), slots.end()) != slots.end()) {
        return std::unexpected(slot_error(
            SlotErrorCode::Ambiguous,
            "Fastboot slot-suffixes contains duplicate slot names"));
    }
    return slots;
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

std::expected<std::string, PrimitiveError> validate_oem_command_suffix(
    const std::string_view raw_suffix) {
    return validated_parameter_command(
        PrimitiveOperation::Oem, "oem ", raw_suffix);
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

std::expected<PrimitiveReply, PrimitiveError> PrimitiveService::stage(
    const std::span<const std::byte> bytes) {
    if (auto size = validate_download_size(bytes.size()); !size) {
        auto error = std::move(size.error());
        error.operation = PrimitiveOperation::Stage;
        return std::unexpected(std::move(error));
    }
    return finish_download(session_.download(bytes), PrimitiveOperation::Stage);
}

std::expected<PrimitiveReply, PrimitiveError> PrimitiveService::stage_source(
    std::shared_ptr<protocol::ITransferSource> source,
    const protocol::TransferProgressObserver& observer) {
    if (auto valid = validate_download_source(source); !valid) {
        auto error = std::move(valid.error());
        error.operation = PrimitiveOperation::Stage;
        return std::unexpected(std::move(error));
    }
    return finish_download(
        session_.download_source(std::move(source), observer),
        PrimitiveOperation::Stage);
}

std::expected<PrimitiveReply, PrimitiveError> PrimitiveService::upload_to_sink(
    std::shared_ptr<protocol::ITransferSink> sink,
    const std::uint64_t maximum_bytes,
    const protocol::TransferProgressObserver& observer) {
    return finish_receive(
        PrimitiveOperation::Upload,
        session_.receive_to_sink(
            "upload", std::move(sink), maximum_bytes, observer));
}

std::expected<PrimitiveReply, PrimitiveError>
PrimitiveService::get_staged_to_sink(
    std::shared_ptr<protocol::ITransferSink> sink,
    const std::uint64_t maximum_bytes,
    const protocol::TransferProgressObserver& observer) {
    return upload_to_sink(std::move(sink), maximum_bytes, observer);
}

std::expected<PrimitiveReply, PrimitiveError> PrimitiveService::fetch_to_sink(
    const std::string_view partition,
    const FetchRange range,
    std::shared_ptr<protocol::ITransferSink> sink,
    const std::uint64_t maximum_bytes,
    const protocol::TransferProgressObserver& observer) {
    auto command_text = fetch_command(partition, range);
    if (!command_text) {
        return std::unexpected(std::move(command_text.error()));
    }
    return finish_receive(
        PrimitiveOperation::Fetch,
        session_.receive_to_sink(
            *command_text, std::move(sink), maximum_bytes, observer));
}

std::expected<PrimitiveReply, PrimitiveError> PrimitiveService::boot_downloaded() {
    return command(PrimitiveOperation::Boot, "boot", true);
}

std::expected<DownloadAndBootResult, PrimitiveError>
PrimitiveService::download_and_boot(const std::span<const std::byte> bytes) {
    auto downloaded = download(bytes);
    if (!downloaded) {
        return std::unexpected(std::move(downloaded.error()));
    }
    auto booted = boot_downloaded();
    if (!booted) {
        return std::unexpected(std::move(booted.error()));
    }
    return DownloadAndBootResult{
        .download = std::move(*downloaded),
        .boot = std::move(*booted),
    };
}

std::expected<DownloadAndBootResult, PrimitiveError>
PrimitiveService::download_and_boot_source(
    std::shared_ptr<protocol::ITransferSource> source,
    const protocol::TransferProgressObserver& observer) {
    auto downloaded = download_source(std::move(source), observer);
    if (!downloaded) {
        return std::unexpected(std::move(downloaded.error()));
    }
    auto booted = boot_downloaded();
    if (!booted) {
        return std::unexpected(std::move(booted.error()));
    }
    return DownloadAndBootResult{
        .download = std::move(*downloaded),
        .boot = std::move(*booted),
    };
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

std::expected<PrimitiveReply, PrimitiveError> PrimitiveService::set_active(
    const std::string_view slot) {
    auto command_text = parameter_command(
        PrimitiveOperation::SetActive, "set_active:", slot);
    if (!command_text) {
        return std::unexpected(std::move(command_text.error()));
    }
    return command(PrimitiveOperation::SetActive, *command_text);
}

std::expected<PrimitiveReply, PrimitiveError> PrimitiveService::flashing(
    const FlashingCommand flashing_command) {
    switch (flashing_command) {
        case FlashingCommand::Lock:
            return command(PrimitiveOperation::Flashing, "flashing lock");
        case FlashingCommand::Unlock:
            return command(PrimitiveOperation::Flashing, "flashing unlock");
        case FlashingCommand::LockCritical:
            return command(
                PrimitiveOperation::Flashing, "flashing lock_critical");
        case FlashingCommand::UnlockCritical:
            return command(
                PrimitiveOperation::Flashing, "flashing unlock_critical");
        case FlashingCommand::GetUnlockAbility:
            return command(
                PrimitiveOperation::Flashing,
                "flashing get_unlock_ability");
    }
    return std::unexpected(invalid_argument(
        PrimitiveOperation::Flashing, "Fastboot flashing command is invalid"));
}

std::expected<PrimitiveReply, PrimitiveError> PrimitiveService::gsi(
    const GsiCommand gsi_command) {
    switch (gsi_command) {
        case GsiCommand::Wipe:
            return command(PrimitiveOperation::Gsi, "gsi:wipe");
        case GsiCommand::Disable:
            return command(PrimitiveOperation::Gsi, "gsi:disable");
        case GsiCommand::Status:
            return command(PrimitiveOperation::Gsi, "gsi:status");
    }
    return std::unexpected(invalid_argument(
        PrimitiveOperation::Gsi, "Fastboot GSI command is invalid"));
}

std::expected<PrimitiveReply, PrimitiveError> PrimitiveService::snapshot_update(
    const SnapshotUpdateCommand snapshot_command) {
    switch (snapshot_command) {
        case SnapshotUpdateCommand::Cancel:
            return command(
                PrimitiveOperation::SnapshotUpdate,
                "snapshot-update:cancel");
        case SnapshotUpdateCommand::Merge:
            return command(
                PrimitiveOperation::SnapshotUpdate,
                "snapshot-update:merge");
    }
    return std::unexpected(invalid_argument(
        PrimitiveOperation::SnapshotUpdate,
        "Fastboot snapshot-update command is invalid"));
}

std::expected<PrimitiveReply, PrimitiveError>
PrimitiveService::create_logical_partition(
    const std::string_view name,
    const std::uint64_t size) {
    auto command_text = logical_partition_command(
        PrimitiveOperation::CreateLogicalPartition,
        "create-logical-partition:", name, size);
    if (!command_text) {
        return std::unexpected(std::move(command_text.error()));
    }
    return command(PrimitiveOperation::CreateLogicalPartition, *command_text);
}

std::expected<PrimitiveReply, PrimitiveError>
PrimitiveService::delete_logical_partition(const std::string_view name) {
    auto command_text = logical_partition_command(
        PrimitiveOperation::DeleteLogicalPartition,
        "delete-logical-partition:", name, std::nullopt);
    if (!command_text) {
        return std::unexpected(std::move(command_text.error()));
    }
    return command(PrimitiveOperation::DeleteLogicalPartition, *command_text);
}

std::expected<PrimitiveReply, PrimitiveError>
PrimitiveService::resize_logical_partition(
    const std::string_view name,
    const std::uint64_t size) {
    auto command_text = logical_partition_command(
        PrimitiveOperation::ResizeLogicalPartition,
        "resize-logical-partition:", name, size);
    if (!command_text) {
        return std::unexpected(std::move(command_text.error()));
    }
    return command(PrimitiveOperation::ResizeLogicalPartition, *command_text);
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
    auto command_text = validate_oem_command_suffix(raw_suffix);
    if (!command_text) {
        return std::unexpected(std::move(command_text.error()));
    }
    return command(PrimitiveOperation::Oem, *command_text);
}

std::expected<PrimitiveReply, PrimitiveError> PrimitiveService::raw_command(
    const std::string_view command_text) {
    auto validated = parameter_command(
        PrimitiveOperation::RawCommand, {}, command_text);
    if (!validated) {
        return std::unexpected(std::move(validated.error()));
    }
    return command(PrimitiveOperation::RawCommand, *validated);
}

void PrimitiveService::request_cancel() noexcept {
    session_.request_cancel();
}

std::expected<std::string, PrimitiveError> PrimitiveService::parameter_command(
    const PrimitiveOperation operation,
    const std::string_view prefix,
    const std::string_view parameter) const {
    return validated_parameter_command(operation, prefix, parameter);
}

std::expected<PrimitiveReply, PrimitiveError> PrimitiveService::command(
    const PrimitiveOperation operation,
    const std::string_view command_text,
    const bool retire_on_success) {
    auto result = session_.command(command_text);
    if (!result) {
        auto error = protocol_error(operation, result.error(), false);
        // FastbootSession::command accepts only terminal OKAY/FAIL after
        // informational responses, so a well-formed DATA response reaches this
        // path as UnexpectedResponse and has already poisoned the session.
        if (operation == PrimitiveOperation::RawCommand &&
            result.error().code == protocol::ProtocolErrorCode::UnexpectedResponse) {
            error.code = PrimitiveErrorCode::Unsupported;
            error.message =
                "Fastboot raw command returned DATA; raw DATA exchanges are not supported";
        }
        return std::unexpected(std::move(error));
    }
    if (!result->succeeded()) {
        return std::unexpected(device_fail(operation, *result, false));
    }

    PrimitiveReply reply{
        .terminal = std::move(result->terminal),
        .informational = std::move(result->informational),
        .phase = result->phase,
        .outbound_certainty = result->outbound_certainty,
        .inbound_expected = result->inbound_expected,
        .inbound_transferred = result->inbound_transferred,
        .inbound_certainty = result->inbound_certainty,
    };
    if (retire_on_success) {
        session_.close();
    }
    return reply;
}

std::expected<PrimitiveReply, PrimitiveError> PrimitiveService::finish_download(
    std::expected<protocol::CommandResult, protocol::ProtocolError> result,
    const PrimitiveOperation operation) {
    if (!result) {
        return std::unexpected(protocol_error(
            operation, result.error(), true));
    }
    if (!result->succeeded()) {
        return std::unexpected(device_fail(
            operation, *result, true));
    }
    return PrimitiveReply{
        .terminal = std::move(result->terminal),
        .informational = std::move(result->informational),
        .phase = result->phase,
        .outbound_certainty = protocol::TransferCertainty::FullyTransferred,
    };
}

std::expected<PrimitiveReply, PrimitiveError> PrimitiveService::finish_receive(
    const PrimitiveOperation operation,
    std::expected<protocol::CommandResult, protocol::ProtocolError> result) {
    if (!result) {
        return std::unexpected(protocol_error(
            operation, result.error(), false));
    }
    if (!result->succeeded()) {
        return std::unexpected(device_fail(
            operation, *result, false));
    }
    return PrimitiveReply{
        .terminal = std::move(result->terminal),
        .informational = std::move(result->informational),
        .phase = result->phase,
        .outbound_certainty = result->outbound_certainty,
        .inbound_expected = result->inbound_expected,
        .inbound_transferred = result->inbound_transferred,
        .inbound_certainty = result->inbound_certainty,
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
        .device_message = {},
        .informational = error.informational,
        .transport_status = error.transport_status,
        .transport_certainty = error.transfer_certainty,
        .outbound_certainty = download_semantics
            ? download_certainty(error.phase, error.outbound_certainty)
            : error.outbound_certainty,
        .inbound_expected = error.inbound_expected,
        .inbound_transferred = error.inbound_transferred,
        .inbound_certainty = error.inbound_certainty,
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
        .inbound_expected = result.inbound_expected,
        .inbound_transferred = result.inbound_transferred,
        .inbound_certainty = result.inbound_certainty,
        .native_code = 0,
        .session_poisoned = false,
    };
}

std::expected<SlotSelection, SlotError> parse_slot_selection(
    const std::string_view value) {
    if (value.empty()) {
        return SlotSelection{
            .kind = SlotSelectionKind::Current,
            .name = {},
        };
    }
    if (value == "other") {
        return SlotSelection{
            .kind = SlotSelectionKind::Other,
            .name = {},
        };
    }
    if (value == "all") {
        return SlotSelection{
            .kind = SlotSelectionKind::All,
            .name = {},
        };
    }

    auto normalized = normalize_slot_name(
        value,
        SlotErrorCode::InvalidArgument,
        "Requested Fastboot slot");
    if (!normalized) {
        return std::unexpected(std::move(normalized.error()));
    }
    return SlotSelection{
        .kind = SlotSelectionKind::Explicit,
        .name = std::move(*normalized),
    };
}

SlotPlanner::SlotPlanner(PrimitiveService& primitives) noexcept
    : primitives_(primitives) {}

std::expected<SlotTopology, SlotError> SlotPlanner::query_topology() {
    return query_topology(UpdateOperationContext{});
}

std::expected<PrimitiveReply, SlotError> SlotPlanner::query_variable(
    const std::string_view name,
    const UpdateOperationContext& context) {
    if (auto stopped = slot_interruption(context, "before getvar")) {
        return std::unexpected(std::move(*stopped));
    }

    std::atomic<bool> active{true};
    std::stop_callback cancel_active(context.cancellation, [this, &active] {
        if (active.exchange(false, std::memory_order_acq_rel)) {
            primitives_.request_cancel();
        }
    });
    auto reply = primitives_.getvar(name);
    active.store(false, std::memory_order_release);

    if (auto stopped = slot_interruption(context, "after getvar")) {
        return std::unexpected(std::move(*stopped));
    }
    if (!reply) {
        return std::unexpected(slot_query_error(
            "Failed to query Fastboot " + std::string(name),
            std::move(reply.error())));
    }
    return std::move(*reply);
}

std::expected<SlotTopology, SlotError> SlotPlanner::query_topology(
    const UpdateOperationContext& context) {
    auto count_reply = query_variable("slot-count", context);
    if (count_reply) {
        const auto& payload = count_reply->terminal.payload;
        unsigned int count = 0;
        const auto parsed = std::from_chars(
            payload.data(), payload.data() + payload.size(), count, 10);
        if (parsed.ec != std::errc{} || parsed.ptr != payload.data() + payload.size()) {
            return std::unexpected(slot_error(
                SlotErrorCode::InvalidDeviceResponse,
                "Fastboot slot-count is not a decimal integer"));
        }
        if (count < 2) {
            return std::unexpected(slot_error(
                SlotErrorCode::Unsupported,
                "Fastboot slot-count does not describe an A/B device"));
        }
        if (count > 26) {
            return std::unexpected(slot_error(
                SlotErrorCode::InvalidDeviceResponse,
                "Fastboot slot-count exceeds the supported a-z namespace"));
        }

        std::vector<std::string> slots;
        slots.reserve(count);
        for (unsigned int index = 0; index < count; ++index) {
            slots.emplace_back(1, static_cast<char>('a' + index));
        }
        return SlotTopology{
            .slots = std::move(slots),
            .source = SlotTopologySource::SlotCount,
        };
    }
    if (!count_reply.error().query_error ||
        count_reply.error().query_error->code != PrimitiveErrorCode::DeviceFail) {
        return std::unexpected(std::move(count_reply.error()));
    }

    // The frozen AOSP baseline (a3b721a32242006b59cb12bd62c9133632af3a2d)
    // uses slot-suffixes only as a legacy fallback when the modern slot-count
    // getvar is rejected. KairosBoot retains that query order, while rejecting
    // malformed metadata instead of inventing a topology.
    auto suffix_reply = query_variable("slot-suffixes", context);
    if (!suffix_reply) {
        auto error = std::move(suffix_reply.error());
        if (error.query_error &&
            error.query_error->code == PrimitiveErrorCode::DeviceFail) {
            error.message =
                "Device does not expose Fastboot slot-count or slot-suffixes";
        }
        return std::unexpected(std::move(error));
    }
    auto slots = parse_legacy_slot_suffixes(suffix_reply->terminal.payload);
    if (!slots) {
        return std::unexpected(std::move(slots.error()));
    }
    return SlotTopology{
        .slots = std::move(*slots),
        .source = SlotTopologySource::LegacySlotSuffixes,
    };
}

std::expected<bool, SlotError> SlotPlanner::query_has_slot(
    const std::string_view partition,
    const UpdateOperationContext& context) {
    std::string key("has-slot:");
    key.append(partition);
    auto reply = query_variable(key, context);
    if (!reply) {
        auto error = std::move(reply.error());
        if (error.query_error &&
            error.query_error->code == PrimitiveErrorCode::DeviceFail) {
            error.message = "Device does not expose Fastboot " + key;
        }
        return std::unexpected(std::move(error));
    }
    if (reply->terminal.payload == "yes") {
        return true;
    }
    if (reply->terminal.payload == "no") {
        return false;
    }
    return std::unexpected(slot_error(
        SlotErrorCode::InvalidDeviceResponse,
        "Fastboot " + key + " must return exactly 'yes' or 'no'"));
}

std::expected<std::string, SlotError> SlotPlanner::query_current_slot(
    const SlotTopology& topology,
    const UpdateOperationContext& context) {
    auto reply = query_variable("current-slot", context);
    if (!reply) {
        auto error = std::move(reply.error());
        if (error.query_error &&
            error.query_error->code == PrimitiveErrorCode::DeviceFail) {
            error.message = "Device does not expose Fastboot current-slot";
        }
        return std::unexpected(std::move(error));
    }
    auto current = normalize_slot_name(
        reply->terminal.payload,
        SlotErrorCode::InvalidDeviceResponse,
        "Fastboot current-slot");
    if (!current) {
        return std::unexpected(std::move(current.error()));
    }
    if (std::ranges::find(topology.slots, *current) == topology.slots.end()) {
        return std::unexpected(slot_error(
            SlotErrorCode::InvalidDeviceResponse,
            "Fastboot current-slot is absent from the discovered slot topology"));
    }
    return current;
}

std::expected<std::vector<std::string>, SlotError> SlotPlanner::resolve_slots(
    const SlotTopology& topology,
    const SlotSelection& selection,
    const bool allow_all,
    const UpdateOperationContext& context) {
    switch (selection.kind) {
        case SlotSelectionKind::Explicit:
            if (std::ranges::find(topology.slots, selection.name) ==
                topology.slots.end()) {
                return std::unexpected(slot_error(
                    SlotErrorCode::InvalidArgument,
                    "Requested Fastboot slot does not exist on the device"));
            }
            return std::vector<std::string>{selection.name};
        case SlotSelectionKind::All:
            if (!allow_all) {
                return std::unexpected(slot_error(
                    SlotErrorCode::InvalidArgument,
                    "Fastboot slot 'all' is not valid for a single-slot operation"));
            }
            return topology.slots;
        case SlotSelectionKind::Current: {
            auto current = query_current_slot(topology, context);
            if (!current) {
                return std::unexpected(std::move(current.error()));
            }
            return std::vector<std::string>{std::move(*current)};
        }
        case SlotSelectionKind::Other: {
            if (topology.slots.size() != 2) {
                return std::unexpected(slot_error(
                    SlotErrorCode::Ambiguous,
                    "Fastboot slot 'other' is unambiguous only on a two-slot device"));
            }
            auto current = query_current_slot(topology, context);
            if (!current) {
                return std::unexpected(std::move(current.error()));
            }
            const auto other = topology.slots.front() == *current
                ? topology.slots.back()
                : topology.slots.front();
            return std::vector<std::string>{other};
        }
    }
    return std::unexpected(slot_error(
        SlotErrorCode::InvalidArgument,
        "Fastboot slot selection kind is invalid"));
}

std::expected<std::string, SlotError> SlotPlanner::resolve_active_slot(
    const std::string_view requested_slot) {
    return resolve_active_slot(requested_slot, UpdateOperationContext{});
}

std::expected<std::string, SlotError> SlotPlanner::resolve_active_slot(
    const std::string_view requested_slot,
    const UpdateOperationContext& context) {
    auto selection = parse_slot_selection(requested_slot);
    if (!selection) {
        return std::unexpected(std::move(selection.error()));
    }
    if (selection->kind == SlotSelectionKind::All) {
        return std::unexpected(slot_error(
            SlotErrorCode::InvalidArgument,
            "Fastboot set_active cannot target every slot"));
    }
    auto topology = query_topology(context);
    if (!topology) {
        return std::unexpected(std::move(topology.error()));
    }
    auto slots = resolve_slots(*topology, *selection, false, context);
    if (!slots) {
        return std::unexpected(std::move(slots.error()));
    }
    return std::move(slots->front());
}

std::expected<PartitionSlotPlan, SlotError> SlotPlanner::plan_partition(
    const std::string_view partition,
    const std::string_view requested_slot) {
    return plan_partition(
        partition, requested_slot, UpdateOperationContext{});
}

std::expected<PartitionSlotPlan, SlotError> SlotPlanner::plan_partition(
    const std::string_view partition,
    const std::string_view requested_slot,
    const UpdateOperationContext& context) {
    if (!is_valid_partition_name(partition)) {
        return std::unexpected(slot_error(
            SlotErrorCode::InvalidArgument,
            "Fastboot partition name must use ASCII letters, digits, '.', '-' or '_'"));
    }
    auto selection = parse_slot_selection(requested_slot);
    if (!selection) {
        return std::unexpected(std::move(selection.error()));
    }

    auto has_slot = query_has_slot(partition, context);
    if (!has_slot) {
        return std::unexpected(std::move(has_slot.error()));
    }
    if (!*has_slot) {
        if (selection->kind != SlotSelectionKind::Current) {
            return std::unexpected(slot_error(
                SlotErrorCode::Unsupported,
                "An explicit Fastboot slot was requested for a non-slotted partition"));
        }
        return PartitionSlotPlan{
            .slotted = false,
            .partition_names = {std::string(partition)},
            .slots = {},
        };
    }

    auto topology = query_topology(context);
    if (!topology) {
        return std::unexpected(std::move(topology.error()));
    }
    auto slots = resolve_slots(*topology, *selection, true, context);
    if (!slots) {
        return std::unexpected(std::move(slots.error()));
    }

    std::vector<std::string> names;
    names.reserve(slots->size());
    for (const auto& slot : *slots) {
        std::string name(partition);
        name.push_back('_');
        name.append(slot);
        names.push_back(std::move(name));
    }
    return PartitionSlotPlan{
        .slotted = true,
        .partition_names = std::move(names),
        .slots = std::move(*slots),
    };
}

}  // namespace kairosboot::fastboot
