// SPDX-License-Identifier: MIT
#include "primitive_update_device.hpp"

#include "src/fastboot/variable_parser.hpp"
#include "src/transport/image_transfer_source.hpp"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <exception>
#include <functional>
#include <limits>
#include <new>
#include <stop_token>
#include <system_error>
#include <utility>
#include <vector>

namespace kairosboot::fastboot {
namespace {

// Compatibility boundary audited against frozen AOSP system/core commit
// a3b721a32242006b59cb12bd62c9133632af3a2d, fastboot/fastboot.cpp and
// fastboot/task.cpp. No AOSP implementation code is reproduced here.

[[nodiscard]] UpdateDeviceError local_error(
    const UpdateDeviceErrorKind kind,
    std::string message,
    const protocol::ProtocolPhase phase = protocol::ProtocolPhase::Validation,
    const protocol::TransferCertainty outbound =
        protocol::TransferCertainty::NotTransferred,
    const bool session_closed = false) {
    return {
        .kind = kind,
        .phase = phase,
        .message = std::move(message),
        .transport_status = protocol::TransportStatus::Ok,
        .transport_certainty = outbound,
        .outbound_certainty = outbound,
        .session_closed = session_closed,
    };
}

[[nodiscard]] std::optional<UpdateDeviceError> interruption(
    const UpdateOperationContext& context,
    std::string message,
    const protocol::ProtocolPhase phase = protocol::ProtocolPhase::Validation,
    const protocol::TransferCertainty outbound =
        protocol::TransferCertainty::NotTransferred,
    const bool session_closed = false) {
    if (context.cancellation.stop_requested()) {
        return local_error(UpdateDeviceErrorKind::Cancelled, std::move(message),
                           phase, outbound, session_closed);
    }
    if (context.deadline &&
        std::chrono::steady_clock::now() >= *context.deadline) {
        return local_error(UpdateDeviceErrorKind::TimedOut, std::move(message),
                           phase, outbound, session_closed);
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<UpdateDeviceError> interruption_after_reply(
    const UpdateOperationContext& context,
    std::string message,
    const PrimitiveReply& reply,
    const bool session_closed = false) {
    auto result = interruption(context, std::move(message), reply.phase,
                               reply.outbound_certainty, session_closed);
    if (!result) {
        return std::nullopt;
    }
    result->informational = reply.informational;
    result->inbound_expected = reply.inbound_expected;
    result->inbound_transferred = reply.inbound_transferred;
    result->inbound_certainty = reply.inbound_certainty;
    return result;
}

template <typename Callable>
[[nodiscard]] auto invoke_with_cancellation(
    PrimitiveService& service,
    const std::stop_token cancellation,
    std::atomic<bool>& cancellation_forwarded,
    Callable&& callable) {
    std::stop_callback cancel_active(cancellation, [&] {
        cancellation_forwarded.store(true, std::memory_order_release);
        service.request_cancel();
    });
    return std::invoke(std::forward<Callable>(callable));
}

void retain_quarantine_if_forwarded(
    UpdateDeviceError& error,
    const std::atomic<bool>& cancellation_forwarded) noexcept {
    if (cancellation_forwarded.load(std::memory_order_acquire)) {
        // FastbootSession cancellation is sticky. Even if a native operation
        // raced to a successful return, this session must be reconnected rather
        // than reused for a later command.
        error.session_poisoned = true;
    }
}

void describe_task_failure(
    UpdateDeviceError& error,
    const std::size_t completed_actions,
    const std::size_t total_actions,
    const bool current_action_started = false) noexcept {
    error.completed_actions = completed_actions;
    error.total_actions = total_actions;
    if (total_actions == 0U) {
        error.task_certainty = protocol::TransferCertainty::NotTransferred;
    } else if (completed_actions == total_actions) {
        error.task_certainty = protocol::TransferCertainty::FullyTransferred;
    } else if (completed_actions != 0U || current_action_started) {
        error.task_certainty = protocol::TransferCertainty::PartialOrUnknown;
    } else {
        // No earlier action succeeded. Preserve the current primitive's exact
        // outbound evidence as the aggregate evidence for this one action.
        error.task_certainty = error.outbound_certainty;
    }
}

[[nodiscard]] bool printable_parameter(
    const std::string_view value,
    const std::size_t prefix_size) noexcept {
    if (value.empty() ||
        prefix_size > protocol::kDefaultMaxCommandBytes ||
        value.size() > protocol::kDefaultMaxCommandBytes - prefix_size) {
        return false;
    }
    return std::ranges::all_of(value, [](const unsigned char character) {
        return character >= 0x20U && character <= 0x7EU;
    });
}

[[nodiscard]] std::optional<std::string> normalized_slot_name(
    std::string_view value) {
    if (value.starts_with('_')) {
        value.remove_prefix(1U);
    }
    if (value.size() != 1U || value.front() < 'a' || value.front() > 'z') {
        return std::nullopt;
    }
    return std::string(value);
}

[[nodiscard]] std::expected<std::vector<std::string>, UpdateDeviceError>
parse_legacy_slot_topology(const std::string_view payload) {
    std::vector<std::string> slots;
    std::size_t start = 0U;
    while (start <= payload.size()) {
        const auto comma = payload.find(',', start);
        const auto end =
            comma == std::string_view::npos ? payload.size() : comma;
        auto normalized = normalized_slot_name(payload.substr(start, end - start));
        if (!normalized) {
            return std::unexpected(local_error(
                UpdateDeviceErrorKind::Failed,
                "Fastboot slot-suffixes contains an invalid slot name",
                protocol::ProtocolPhase::FinalResponse,
                protocol::TransferCertainty::FullyTransferred));
        }
        slots.push_back(std::move(*normalized));
        if (comma == std::string_view::npos) {
            break;
        }
        start = comma + 1U;
    }
    if (slots.size() < 2U || slots.size() > 26U) {
        return std::unexpected(local_error(
            UpdateDeviceErrorKind::Failed,
            "Fastboot slot-suffixes must contain between 2 and 26 slots",
            protocol::ProtocolPhase::FinalResponse,
            protocol::TransferCertainty::FullyTransferred));
    }
    std::ranges::sort(slots);
    if (std::adjacent_find(slots.begin(), slots.end()) != slots.end()) {
        return std::unexpected(local_error(
            UpdateDeviceErrorKind::Failed,
            "Fastboot slot-suffixes contains duplicate slot names",
            protocol::ProtocolPhase::FinalResponse,
            protocol::TransferCertainty::FullyTransferred));
    }
    return slots;
}

[[nodiscard]] bool known_origin(
    const image::ArtifactSourceOrigin origin) noexcept {
    switch (origin) {
    case image::ArtifactSourceOrigin::DirectFile:
    case image::ArtifactSourceOrigin::DirectoryEntry:
    case image::ArtifactSourceOrigin::ZipEntry:
        return true;
    default:
        return false;
    }
}

[[nodiscard]] bool same_sparse_header(const image::SparseHeader& left,
                                      const image::SparseHeader& right) noexcept {
    return left.major_version == right.major_version &&
           left.minor_version == right.minor_version &&
           left.file_header_size == right.file_header_size &&
           left.chunk_header_size == right.chunk_header_size &&
           left.block_size == right.block_size &&
           left.total_blocks == right.total_blocks &&
           left.total_chunks == right.total_chunks &&
           left.image_checksum == right.image_checksum;
}

[[nodiscard]] bool complete_flash_binding(
    const UpdateFlashArtifactInput& binding,
    const std::string_view expected_name) noexcept {
    if (!binding.resolved || !binding.resolved->source || !binding.artifact ||
        expected_name.empty() ||
        binding.resolved->logical_name != expected_name ||
        !known_origin(binding.resolved->origin) ||
        binding.artifact->transfer_source() != binding.resolved->source) {
        return false;
    }

    const auto& metadata = binding.artifact->metadata();
    if (metadata.transfer_size == 0U ||
        metadata.transfer_size != binding.resolved->source->size()) {
        return false;
    }
    switch (metadata.kind) {
    case image::FlashArtifactKind::Raw:
        return binding.artifact->sparse_image() == nullptr &&
               !metadata.sparse_header.has_value() &&
               metadata.expanded_size == metadata.transfer_size;
    case image::FlashArtifactKind::AndroidSparse: {
        const auto* sparse = binding.artifact->sparse_image();
        return sparse != nullptr && metadata.sparse_header.has_value() &&
               metadata.expanded_size == sparse->output_size() &&
               same_sparse_header(*metadata.sparse_header, sparse->header());
    }
    default:
        return false;
    }
}

[[nodiscard]] std::optional<RebootTarget> reboot_target(
    const PlannedRebootTarget target) noexcept {
    switch (target) {
    case PlannedRebootTarget::System:
        return RebootTarget::System;
    case PlannedRebootTarget::Bootloader:
        return RebootTarget::Bootloader;
    case PlannedRebootTarget::Recovery:
        return RebootTarget::Recovery;
    case PlannedRebootTarget::Fastboot:
        return RebootTarget::Fastboot;
    default:
        return std::nullopt;
    }
}

struct PreparedFlash final {
    UpdateFlashArtifactInput binding{};
    std::shared_ptr<const image::SparseFlashPlan> plan{};
    std::vector<std::shared_ptr<protocol::ITransferSource>> sources{};
    std::string partition{};
    PrimitiveUpdateProgressObserver progress{};
};

class PreparedFlashTask final : public IPreparedDeviceTask {
public:
    PreparedFlashTask(PrimitiveService& service, PreparedFlash prepared) noexcept
        : service_(service), prepared_(std::move(prepared)) {}

    [[nodiscard]] std::expected<void, UpdateDeviceError>
    execute(const UpdateOperationContext& context) const override {
        const auto total_actions = prepared_.sources.size();
        std::size_t completed_actions = 0U;
        std::uint64_t completed_bytes = 0U;
        const auto task_error = [&](UpdateDeviceError error,
                                    const bool current_action_started = false) {
            describe_task_failure(error, completed_actions, total_actions,
                                  current_action_started);
            return error;
        };
        if (auto stopped = interruption(
                context, "flash was interrupted before sending an image")) {
            return std::unexpected(task_error(std::move(*stopped)));
        }

        const auto total_bytes = prepared_.plan->transfer_size();
        if (prepared_.progress) {
            try {
                if (prepared_.progress(PrimitiveUpdateProgress{
                        .part_count = prepared_.sources.size(),
                        .total_bytes = total_bytes,
                    }) == PrimitiveUpdateProgressAction::Cancel) {
                    return std::unexpected(task_error(local_error(
                        UpdateDeviceErrorKind::Cancelled,
                        "flash was cancelled by the progress observer before "
                        "sending an image")));
                }
            } catch (...) {
                return std::unexpected(task_error(local_error(
                    UpdateDeviceErrorKind::Failed,
                    "flash progress observer threw before sending an image")));
            }
        }
        if (auto stopped = interruption(
                context, "flash was interrupted after initial progress")) {
            return std::unexpected(task_error(std::move(*stopped)));
        }

        for (std::size_t index = 0; index < prepared_.sources.size(); ++index) {
            if (auto stopped = interruption(
                    context, "flash was interrupted before a transfer part")) {
                return std::unexpected(task_error(std::move(*stopped)));
            }

            bool observer_threw = false;
            bool observer_cancelled = false;
            bool deadline_expired = false;
            const auto part_size = prepared_.sources[index]->size();
            const protocol::TransferProgressObserver observer =
                [this, &context, &observer_threw, &observer_cancelled,
                 &deadline_expired, index, completed_bytes, total_bytes,
                 part_size](const std::uint64_t part_completed,
                            const std::uint64_t) {
                    if (context.cancellation.stop_requested()) {
                        observer_cancelled = true;
                        return protocol::TransferProgressAction::cancel;
                    }
                    if (context.deadline &&
                        std::chrono::steady_clock::now() >= *context.deadline) {
                        deadline_expired = true;
                        return protocol::TransferProgressAction::cancel;
                    }
                    if (prepared_.progress) {
                        try {
                            if (prepared_.progress(PrimitiveUpdateProgress{
                                    .part_index = index,
                                    .part_count = prepared_.sources.size(),
                                    .part_completed_bytes = part_completed,
                                    .part_total_bytes = part_size,
                                    .completed_bytes =
                                        completed_bytes + part_completed,
                                    .total_bytes = total_bytes,
                                }) == PrimitiveUpdateProgressAction::Cancel) {
                                observer_cancelled = true;
                                return protocol::TransferProgressAction::cancel;
                            }
                        } catch (...) {
                            observer_threw = true;
                            return protocol::TransferProgressAction::cancel;
                        }
                    }
                    if (context.cancellation.stop_requested()) {
                        observer_cancelled = true;
                        return protocol::TransferProgressAction::cancel;
                    }
                    if (context.deadline &&
                        std::chrono::steady_clock::now() >= *context.deadline) {
                        deadline_expired = true;
                        return protocol::TransferProgressAction::cancel;
                    }
                    return protocol::TransferProgressAction::continue_transfer;
                };

            std::atomic<bool> download_cancel_forwarded{false};
            auto downloaded = invoke_with_cancellation(
                service_, context.cancellation, download_cancel_forwarded,
                [this, &observer, index] {
                    return service_.download_source(prepared_.sources[index],
                                                    observer);
                });
            if (!downloaded) {
                auto mapped = map_primitive_update_error(
                    std::move(downloaded.error()), context);
                retain_quarantine_if_forwarded(
                    mapped, download_cancel_forwarded);
                if (observer_threw) {
                    mapped.kind = UpdateDeviceErrorKind::Failed;
                    mapped.message =
                        "flash progress observer threw during image transfer";
                } else if (deadline_expired &&
                           !context.cancellation.stop_requested()) {
                    mapped.kind = UpdateDeviceErrorKind::TimedOut;
                } else if (observer_cancelled) {
                    mapped.kind = UpdateDeviceErrorKind::Cancelled;
                }
                return std::unexpected(task_error(std::move(mapped)));
            }
            // DATA is staging, but flash:<partition> is destructive. Preserve
            // the absolute deadline/cancellation boundary between them rather
            // than hiding both protocol operations inside one helper call.
            if (auto stopped = interruption_after_reply(
                    context,
                    "flash was interrupted after DATA and before the flash "
                    "command",
                    *downloaded)) {
                retain_quarantine_if_forwarded(
                    *stopped, download_cancel_forwarded);
                return std::unexpected(
                    task_error(std::move(*stopped), true));
            }

            std::atomic<bool> flash_cancel_forwarded{false};
            auto flashed = invoke_with_cancellation(
                service_, context.cancellation, flash_cancel_forwarded,
                [this] {
                    return service_.flash_downloaded(prepared_.partition);
                });
            if (!flashed) {
                auto mapped = map_primitive_update_error(
                    std::move(flashed.error()), context);
                retain_quarantine_if_forwarded(mapped,
                                                flash_cancel_forwarded);
                return std::unexpected(task_error(std::move(mapped), true));
            }

            completed_bytes += part_size;
            ++completed_actions;
            if (auto stopped = interruption_after_reply(
                    context, "flash was interrupted after a transfer part",
                    *flashed)) {
                retain_quarantine_if_forwarded(*stopped,
                                                flash_cancel_forwarded);
                return std::unexpected(task_error(std::move(*stopped)));
            }
        }
        return {};
    }

private:
    PrimitiveService& service_;
    // Retains the complete immutable FlashArtifact/ResolvedArtifact binding as
    // well as the one prepare-time sparse plan and its bound transfer sources.
    PreparedFlash prepared_;
};

class PreparedEraseTask final : public IPreparedDeviceTask {
public:
    PreparedEraseTask(PrimitiveService& service, std::string partition) noexcept
        : service_(service), partition_(std::move(partition)) {}

    [[nodiscard]] std::expected<void, UpdateDeviceError>
    execute(const UpdateOperationContext& context) const override {
        if (auto stopped = interruption(
                context, "erase was interrupted before sending a command")) {
            describe_task_failure(*stopped, 0U, 1U);
            return std::unexpected(std::move(*stopped));
        }
        std::atomic<bool> cancellation_forwarded{false};
        auto result = invoke_with_cancellation(
            service_, context.cancellation, cancellation_forwarded,
            [this] { return service_.erase(partition_); });
        if (!result) {
            auto mapped = map_primitive_update_error(
                std::move(result.error()), context);
            retain_quarantine_if_forwarded(mapped, cancellation_forwarded);
            describe_task_failure(mapped, 0U, 1U);
            return std::unexpected(std::move(mapped));
        }
        if (auto stopped = interruption_after_reply(
                context, "erase was interrupted after the command", *result)) {
            retain_quarantine_if_forwarded(*stopped, cancellation_forwarded);
            describe_task_failure(*stopped, 1U, 1U);
            return std::unexpected(std::move(*stopped));
        }
        return {};
    }

private:
    PrimitiveService& service_;
    std::string partition_;
};

class PreparedRebootTask final : public IPreparedDeviceTask {
public:
    PreparedRebootTask(PrimitiveService& service,
                       const RebootTarget target) noexcept
        : service_(service), target_(target) {}

    [[nodiscard]] std::expected<void, UpdateDeviceError>
    execute(const UpdateOperationContext& context) const override {
        if (auto stopped = interruption(
                context, "reboot was interrupted before sending a command")) {
            describe_task_failure(*stopped, 0U, 1U);
            return std::unexpected(std::move(*stopped));
        }
        std::atomic<bool> cancellation_forwarded{false};
        auto result = invoke_with_cancellation(
            service_, context.cancellation, cancellation_forwarded,
            [this] { return service_.reboot(target_); });
        if (!result) {
            auto mapped = map_primitive_update_error(
                std::move(result.error()), context);
            retain_quarantine_if_forwarded(mapped, cancellation_forwarded);
            describe_task_failure(mapped, 0U, 1U);
            return std::unexpected(std::move(mapped));
        }
        if (auto stopped = interruption_after_reply(
                context, "reboot was interrupted after the command", *result,
                true)) {
            retain_quarantine_if_forwarded(*stopped, cancellation_forwarded);
            describe_task_failure(*stopped, 1U, 1U);
            return std::unexpected(std::move(*stopped));
        }
        return {};
    }

private:
    PrimitiveService& service_;
    RebootTarget target_;
};

}  // namespace

UpdateDeviceError map_primitive_update_error(
    PrimitiveError error,
    const UpdateOperationContext& context) {
    auto kind = UpdateDeviceErrorKind::Failed;
    if (error.code == PrimitiveErrorCode::Cancelled) {
        kind = UpdateDeviceErrorKind::Cancelled;
    } else if (error.code == PrimitiveErrorCode::Timeout) {
        kind = UpdateDeviceErrorKind::TimedOut;
    }
    if (context.cancellation.stop_requested()) {
        kind = UpdateDeviceErrorKind::Cancelled;
    } else if (context.deadline &&
               std::chrono::steady_clock::now() >= *context.deadline) {
        kind = UpdateDeviceErrorKind::TimedOut;
    }

    const bool session_closed = error.code == PrimitiveErrorCode::Closed;
    const bool session_poisoned =
        error.session_poisoned || error.code == PrimitiveErrorCode::Poisoned;
    return {
        .kind = kind,
        .phase = error.phase,
        .message = std::move(error.message),
        .device_message = std::move(error.device_message),
        .informational = std::move(error.informational),
        .transport_status = error.transport_status,
        .transport_certainty = error.transport_certainty,
        .outbound_certainty = error.outbound_certainty,
        .inbound_expected = error.inbound_expected,
        .inbound_transferred = error.inbound_transferred,
        .inbound_certainty = error.inbound_certainty,
        .session_poisoned = session_poisoned,
        .session_closed = session_closed,
        .native_code = error.native_code,
    };
}

PrimitiveUpdateDevice::PrimitiveUpdateDevice(
    PrimitiveService& service,
    PrimitiveUpdateDeviceOptions options) noexcept
    : service_(service), options_(std::move(options)) {}

std::expected<std::string, UpdateDeviceError> PrimitiveUpdateDevice::getvar(
    const std::string_view name,
    const UpdateOperationContext& context) {
    if (auto stopped = interruption(
            context, "getvar was interrupted before sending a command")) {
        return std::unexpected(std::move(*stopped));
    }

    std::atomic<bool> cancellation_forwarded{false};
    auto result = invoke_with_cancellation(
        service_, context.cancellation, cancellation_forwarded,
        [this, name] { return service_.getvar(name); });
    if (!result) {
        auto mapped = map_primitive_update_error(
            std::move(result.error()), context);
        retain_quarantine_if_forwarded(mapped, cancellation_forwarded);
        return std::unexpected(std::move(mapped));
    }
    if (result->terminal.kind != protocol::ResponseKind::Okay) {
        auto error = local_error(
            UpdateDeviceErrorKind::Failed,
            "getvar returned a non-OKAY terminal response", result->phase,
            result->outbound_certainty);
        error.informational = result->informational;
        retain_quarantine_if_forwarded(error, cancellation_forwarded);
        return std::unexpected(std::move(error));
    }
    if (result->terminal.payload.find('\0') != std::string::npos) {
        auto error = local_error(
            UpdateDeviceErrorKind::Failed,
            "getvar OKAY payload contains an embedded NUL", result->phase,
            result->outbound_certainty);
        error.informational = result->informational;
        retain_quarantine_if_forwarded(error, cancellation_forwarded);
        return std::unexpected(std::move(error));
    }
    if (auto stopped = interruption_after_reply(
            context, "getvar was interrupted after the command", *result)) {
        retain_quarantine_if_forwarded(*stopped, cancellation_forwarded);
        return std::unexpected(std::move(*stopped));
    }
    return std::move(result->terminal.payload);
}

std::expected<std::uint64_t, UpdateDeviceError>
PrimitiveUpdateDevice::maximum_download_size(
    const UpdateOperationContext& context) {
    if (maximum_download_size_) {
        return *maximum_download_size_;
    }
    auto value = getvar("max-download-size", context);
    if (!value) {
        return std::unexpected(std::move(value.error()));
    }
    auto parsed = parse_unsigned_variable(*value);
    if (!parsed || *parsed == 0U) {
        return std::unexpected(local_error(
            UpdateDeviceErrorKind::Failed,
            "max-download-size is not a positive complete unsigned decimal "
            "or 0x-prefixed hexadecimal value",
            protocol::ProtocolPhase::FinalResponse,
            protocol::TransferCertainty::FullyTransferred));
    }
    maximum_download_size_ = std::min<std::uint64_t>(
        *parsed, std::numeric_limits<std::uint32_t>::max());
    return *maximum_download_size_;
}

std::expected<std::string, UpdateDeviceError>
PrimitiveUpdateDevice::current_slot(
    const std::vector<std::string>& topology,
    const UpdateOperationContext& context) {
    if (auto stopped = interruption(
            context, "slot resolution was interrupted before current-slot")) {
        return std::unexpected(std::move(*stopped));
    }
    if (current_slot_) {
        return *current_slot_;
    }
    auto value = getvar("current-slot", context);
    if (!value) {
        return std::unexpected(std::move(value.error()));
    }
    auto normalized = normalized_slot_name(*value);
    if (!normalized) {
        return std::unexpected(local_error(
            UpdateDeviceErrorKind::Failed,
            "Fastboot current-slot must be one lowercase a-z slot name",
            protocol::ProtocolPhase::FinalResponse,
            protocol::TransferCertainty::FullyTransferred));
    }
    if (std::ranges::find(topology, *normalized) == topology.end()) {
        return std::unexpected(local_error(
            UpdateDeviceErrorKind::Failed,
            "Fastboot current-slot is absent from the discovered slot topology",
            protocol::ProtocolPhase::FinalResponse,
            protocol::TransferCertainty::FullyTransferred));
    }
    current_slot_ = std::move(*normalized);
    return *current_slot_;
}

std::expected<std::vector<std::string>, UpdateDeviceError>
PrimitiveUpdateDevice::slot_topology(
    const UpdateOperationContext& context) {
    if (auto stopped = interruption(
            context, "slot resolution was interrupted before topology query")) {
        return std::unexpected(std::move(*stopped));
    }
    if (slot_topology_) {
        return *slot_topology_;
    }

    std::atomic<bool> cancellation_forwarded{false};
    auto count_reply = invoke_with_cancellation(
        service_, context.cancellation, cancellation_forwarded,
        [this] { return service_.getvar("slot-count"); });
    if (!count_reply) {
        auto primitive = std::move(count_reply.error());
        const bool interrupted = context.cancellation.stop_requested() ||
            (context.deadline &&
             std::chrono::steady_clock::now() >= *context.deadline) ||
            cancellation_forwarded.load(std::memory_order_acquire);
        if (primitive.code != PrimitiveErrorCode::DeviceFail || interrupted) {
            auto mapped = map_primitive_update_error(std::move(primitive), context);
            retain_quarantine_if_forwarded(mapped, cancellation_forwarded);
            return std::unexpected(std::move(mapped));
        }

        // Frozen AOSP compatibility: only a well-formed device FAIL for the
        // modern query enables the legacy suffix-list fallback. Transport and
        // protocol errors never silently change discovery mechanisms.
        auto suffixes = getvar("slot-suffixes", context);
        if (!suffixes) {
            return std::unexpected(std::move(suffixes.error()));
        }
        auto parsed = parse_legacy_slot_topology(*suffixes);
        if (!parsed) {
            return std::unexpected(std::move(parsed.error()));
        }
        slot_topology_ = *parsed;
        return *slot_topology_;
    }
    if (auto stopped = interruption_after_reply(
            context, "slot resolution was interrupted after slot-count",
            *count_reply)) {
        retain_quarantine_if_forwarded(*stopped, cancellation_forwarded);
        return std::unexpected(std::move(*stopped));
    }

    const auto& value = count_reply->terminal.payload;
    unsigned int count = 0U;
    const auto parsed = std::from_chars(
        value.data(), value.data() + value.size(), count, 10);
    if (parsed.ec != std::errc{} ||
        parsed.ptr != value.data() + value.size() ||
        count < 2U || count > 26U) {
        return std::unexpected(local_error(
            UpdateDeviceErrorKind::Failed,
            "Fastboot slot-count must be a complete decimal integer from 2 to 26",
            protocol::ProtocolPhase::FinalResponse,
            protocol::TransferCertainty::FullyTransferred));
    }
    std::vector<std::string> topology;
    topology.reserve(count);
    for (unsigned int index = 0U; index < count; ++index) {
        topology.emplace_back(1U, static_cast<char>('a' + index));
    }
    slot_topology_ = std::move(topology);
    return *slot_topology_;
}

std::expected<bool, UpdateDeviceError>
PrimitiveUpdateDevice::partition_has_slot(
    const std::string_view partition,
    const UpdateOperationContext& context) {
    if (auto stopped = interruption(
            context, "slot resolution was interrupted before has-slot")) {
        return std::unexpected(std::move(*stopped));
    }
    if (const auto found = has_slot_.find(partition);
        found != has_slot_.end()) {
        return found->second;
    }
    std::string variable{"has-slot:"};
    variable.append(partition);
    auto value = getvar(variable, context);
    if (!value) {
        return std::unexpected(std::move(value.error()));
    }
    bool result = false;
    if (*value == "yes") {
        result = true;
    } else if (*value != "no") {
        return std::unexpected(local_error(
            UpdateDeviceErrorKind::Failed,
            "Fastboot has-slot value must be exactly 'yes' or 'no'",
            protocol::ProtocolPhase::FinalResponse,
            protocol::TransferCertainty::FullyTransferred));
    }
    has_slot_.emplace(partition, result);
    return result;
}

std::expected<std::string, UpdateDeviceError>
PrimitiveUpdateDevice::resolve_partition(
    const PlannedUpdateTask& task,
    const UpdateOperationContext& context) {
    if (auto stopped = interruption(
            context, "flash partition resolution was interrupted")) {
        return std::unexpected(std::move(*stopped));
    }
    if (task.slot == PlannedSlot::Default) {
        return task.partition;
    }
    if (task.slot != PlannedSlot::Other) {
        return std::unexpected(local_error(
            UpdateDeviceErrorKind::Failed,
            "flash task contains an invalid slot selection"));
    }

    const auto separator = task.partition.find(':');
    const auto base = std::string_view(task.partition).substr(0U, separator);
    if (base.empty()) {
        return std::unexpected(local_error(
            UpdateDeviceErrorKind::Failed,
            "slot-other partition has an empty base name"));
    }
    auto has_slot = partition_has_slot(base, context);
    if (!has_slot) {
        return std::unexpected(std::move(has_slot.error()));
    }
    if (!*has_slot) {
        return std::unexpected(local_error(
            UpdateDeviceErrorKind::Failed,
            "slot-other cannot target a non-slotted partition",
            protocol::ProtocolPhase::FinalResponse,
            protocol::TransferCertainty::FullyTransferred));
    }

    auto topology = slot_topology(context);
    if (!topology) {
        return std::unexpected(std::move(topology.error()));
    }
    if (topology->size() != 2U) {
        return std::unexpected(local_error(
            UpdateDeviceErrorKind::Failed,
            "Fastboot slot 'other' is unambiguous only on a two-slot device",
            protocol::ProtocolPhase::FinalResponse,
            protocol::TransferCertainty::FullyTransferred));
    }
    auto current = current_slot(*topology, context);
    if (!current) {
        return std::unexpected(std::move(current.error()));
    }

    const auto& other = topology->front() == *current
        ? topology->back()
        : topology->front();
    std::string resolved;
    resolved.reserve(task.partition.size() + 2U);
    resolved.append(base);
    resolved.push_back('_');
    resolved.append(other);
    if (separator != std::string::npos) {
        resolved.append(std::string_view(task.partition).substr(separator));
    }
    if (!printable_parameter(resolved, std::string_view{"flash:"}.size())) {
        return std::unexpected(local_error(
            UpdateDeviceErrorKind::Failed,
            "resolved slot-other partition exceeds Fastboot command limits"));
    }
    return resolved;
}

std::expected<std::unique_ptr<IPreparedDeviceTask>, UpdateDeviceError>
PrimitiveUpdateDevice::prepare_task(
    UpdateDeviceTaskInput input,
    const UpdateOperationContext& context) {
    if (auto stopped = interruption(
            context, "task preparation was interrupted before validation")) {
        return std::unexpected(std::move(*stopped));
    }

    try {
        switch (input.task.kind) {
        case UpdateTaskKind::Flash: {
            if (input.super_artifact || !input.flash_artifact ||
                input.task.reboot_target != PlannedRebootTarget::System ||
                !printable_parameter(
                    input.task.partition, std::string_view{"flash:"}.size()) ||
                input.task.artifact.empty() ||
                !complete_flash_binding(*input.flash_artifact,
                                        input.task.artifact)) {
                return std::unexpected(local_error(
                    UpdateDeviceErrorKind::Failed,
                    "flash task and complete immutable artifact binding are "
                    "inconsistent"));
            }
            if (input.task.apply_vbmeta) {
                return std::unexpected(local_error(
                    UpdateDeviceErrorKind::Failed,
                    "apply-vbmeta requires a pre-execution AVB transform that is "
                    "not available in the primitive update adapter"));
            }

            auto partition = resolve_partition(input.task, context);
            if (!partition) {
                return std::unexpected(std::move(partition.error()));
            }
            auto maximum = maximum_download_size(context);
            if (!maximum) {
                return std::unexpected(std::move(maximum.error()));
            }
            if (auto stopped = interruption(
                    context, "task preparation was interrupted before sparse "
                             "planning")) {
                return std::unexpected(std::move(*stopped));
            }

            auto plan = image::SparseFlashPlan::create(
                *input.flash_artifact->artifact, *maximum,
                options_.host_resparse_limit, context.cancellation);
            if (!plan) {
                const auto kind =
                    plan.error().kind ==
                            image::SparseFlashPlanErrorKind::Cancelled
                        ? UpdateDeviceErrorKind::Cancelled
                        : UpdateDeviceErrorKind::Failed;
                return std::unexpected(local_error(
                    kind, "unable to prepare sparse flash plan: " +
                              plan.error().message));
            }
            if (auto stopped = interruption(
                    context, "task preparation was interrupted after sparse "
                             "planning")) {
                return std::unexpected(std::move(*stopped));
            }

            std::vector<std::shared_ptr<protocol::ITransferSource>> sources;
            sources.reserve(plan->parts().size());
            for (const auto& part : plan->parts()) {
                if (auto stopped = interruption(
                        context, "task preparation was interrupted while binding "
                                 "transfer sources")) {
                    return std::unexpected(std::move(*stopped));
                }
                auto source = transport::ImageTransferSource::create(part.source);
                if (!source) {
                    return std::unexpected(local_error(
                        UpdateDeviceErrorKind::Failed,
                        "unable to bind flash transfer source: " +
                            source.error().message));
                }
                if ((*source)->size() > *maximum) {
                    return std::unexpected(local_error(
                        UpdateDeviceErrorKind::Failed,
                        "prepared flash transfer exceeds max-download-size"));
                }
                if (auto valid = validate_download_size((*source)->size()); !valid) {
                    return std::unexpected(map_primitive_update_error(
                        std::move(valid.error()), context));
                }
                sources.push_back(std::move(*source));
            }
            if (sources.empty()) {
                return std::unexpected(local_error(
                    UpdateDeviceErrorKind::Failed,
                    "prepared sparse flash plan contains no transfer source"));
            }

            auto retained_plan =
                std::make_shared<const image::SparseFlashPlan>(std::move(*plan));
            PreparedFlash prepared{
                .binding = std::move(*input.flash_artifact),
                .plan = std::move(retained_plan),
                .sources = std::move(sources),
                .partition = std::move(*partition),
                .progress = options_.progress,
            };
            std::unique_ptr<IPreparedDeviceTask> token =
                std::make_unique<PreparedFlashTask>(service_,
                                                    std::move(prepared));
            return token;
        }
        case UpdateTaskKind::Erase: {
            if (input.flash_artifact || input.super_artifact ||
                !input.task.artifact.empty() ||
                input.task.slot != PlannedSlot::Default ||
                input.task.apply_vbmeta ||
                input.task.reboot_target != PlannedRebootTarget::System ||
                !printable_parameter(
                    input.task.partition, std::string_view{"erase:"}.size())) {
                return std::unexpected(local_error(
                    UpdateDeviceErrorKind::Failed,
                    "erase task does not contain one valid partition"));
            }
            std::unique_ptr<IPreparedDeviceTask> token =
                std::make_unique<PreparedEraseTask>(service_,
                                                    input.task.partition);
            return token;
        }
        case UpdateTaskKind::Reboot: {
            if (input.flash_artifact || input.super_artifact ||
                !input.task.partition.empty() || !input.task.artifact.empty() ||
                input.task.slot != PlannedSlot::Default ||
                input.task.apply_vbmeta) {
                return std::unexpected(local_error(
                    UpdateDeviceErrorKind::Failed,
                    "reboot task contains incompatible flash fields"));
            }
            const auto target = reboot_target(input.task.reboot_target);
            if (!target) {
                return std::unexpected(local_error(
                    UpdateDeviceErrorKind::Failed,
                    "reboot task contains an invalid target"));
            }
            std::unique_ptr<IPreparedDeviceTask> token =
                std::make_unique<PreparedRebootTask>(service_, *target);
            return token;
        }
        case UpdateTaskKind::UpdateSuper:
            return std::unexpected(local_error(
                UpdateDeviceErrorKind::Failed,
                "update-super is unavailable until the dedicated immutable "
                "super_empty.img transaction adapter is integrated"));
        default:
            return std::unexpected(local_error(
                UpdateDeviceErrorKind::Failed,
                "update task contains an unknown task kind"));
        }
    } catch (const std::bad_alloc&) {
        return std::unexpected(local_error(
            UpdateDeviceErrorKind::Failed,
            "unable to allocate bounded update task preparation state"));
    } catch (const std::exception& error) {
        return std::unexpected(local_error(
            UpdateDeviceErrorKind::Failed,
            "update task preparation failed: " + std::string(error.what())));
    } catch (...) {
        return std::unexpected(local_error(
            UpdateDeviceErrorKind::Failed,
            "update task preparation failed with a non-standard exception"));
    }
}

}  // namespace kairosboot::fastboot
