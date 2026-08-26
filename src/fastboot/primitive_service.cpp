// SPDX-License-Identifier: MIT
#include "src/fastboot/primitive_service.hpp"
#include "src/fastboot/slot_planner.hpp"

#include <algorithm>
#include <charconv>
#include <limits>
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

[[nodiscard]] bool is_valid_partition_name(
    const std::string_view partition) noexcept {
    if (partition.empty() ||
        partition.size() > protocol::kDefaultMaxCommandBytes - 16U) {
        return false;
    }
    return std::ranges::all_of(partition, [](const unsigned char character) {
        const auto ascii_alphanumeric =
            (character >= 'a' && character <= 'z') ||
            (character >= 'A' && character <= 'Z') ||
            (character >= '0' && character <= '9');
        return ascii_alphanumeric || character == '_' || character == '-' ||
            character == '.';
    });
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
        .device_message = {},
        .informational = {},
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
    auto count_reply = primitives_.getvar("slot-count");
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
    if (count_reply.error().code != PrimitiveErrorCode::DeviceFail) {
        return std::unexpected(slot_query_error(
            "Failed to query Fastboot slot-count",
            std::move(count_reply.error())));
    }

    // The frozen AOSP baseline (a3b721a32242006b59cb12bd62c9133632af3a2d)
    // uses slot-suffixes only as a legacy fallback when the modern slot-count
    // getvar is rejected. KairosBoot retains that query order, while rejecting
    // malformed metadata instead of inventing a topology.
    auto suffix_reply = primitives_.getvar("slot-suffixes");
    if (!suffix_reply) {
        return std::unexpected(slot_query_error(
            "Device does not expose Fastboot slot-count or slot-suffixes",
            std::move(suffix_reply.error())));
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
    const std::string_view partition) {
    std::string key("has-slot:");
    key.append(partition);
    auto reply = primitives_.getvar(key);
    if (!reply) {
        return std::unexpected(slot_query_error(
            "Device does not expose Fastboot " + key,
            std::move(reply.error())));
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
    const SlotTopology& topology) {
    auto reply = primitives_.getvar("current-slot");
    if (!reply) {
        return std::unexpected(slot_query_error(
            "Device does not expose Fastboot current-slot",
            std::move(reply.error())));
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
    const bool allow_all) {
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
            auto current = query_current_slot(topology);
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
            auto current = query_current_slot(topology);
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
    auto selection = parse_slot_selection(requested_slot);
    if (!selection) {
        return std::unexpected(std::move(selection.error()));
    }
    if (selection->kind == SlotSelectionKind::All) {
        return std::unexpected(slot_error(
            SlotErrorCode::InvalidArgument,
            "Fastboot set_active cannot target every slot"));
    }
    auto topology = query_topology();
    if (!topology) {
        return std::unexpected(std::move(topology.error()));
    }
    auto slots = resolve_slots(*topology, *selection, false);
    if (!slots) {
        return std::unexpected(std::move(slots.error()));
    }
    return std::move(slots->front());
}

std::expected<PartitionSlotPlan, SlotError> SlotPlanner::plan_partition(
    const std::string_view partition,
    const std::string_view requested_slot) {
    if (!is_valid_partition_name(partition)) {
        return std::unexpected(slot_error(
            SlotErrorCode::InvalidArgument,
            "Fastboot partition name must use ASCII letters, digits, '.', '-' or '_'"));
    }
    auto selection = parse_slot_selection(requested_slot);
    if (!selection) {
        return std::unexpected(std::move(selection.error()));
    }

    auto has_slot = query_has_slot(partition);
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

    auto topology = query_topology();
    if (!topology) {
        return std::unexpected(std::move(topology.error()));
    }
    auto slots = resolve_slots(*topology, *selection, true);
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
