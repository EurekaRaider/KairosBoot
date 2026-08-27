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

inline constexpr std::string_view kSuperEmptyName{"super_empty.img"};
inline constexpr std::string_view kUpdateSuperPrefix{"update-super:"};
inline constexpr std::string_view kUpdateSuperWipeSuffix{":wipe"};

[[nodiscard]] bool valid_super_partition_name(
    const std::string_view value) noexcept {
    if (value.empty() ||
        kUpdateSuperPrefix.size() + value.size() +
                kUpdateSuperWipeSuffix.size() >
            protocol::kDefaultMaxCommandBytes) {
        return false;
    }
    return std::ranges::all_of(value, [](const unsigned char character) {
        const bool ascii_alphanumeric =
            (character >= 'a' && character <= 'z') ||
            (character >= 'A' && character <= 'Z') ||
            (character >= '0' && character <= '9');
        return ascii_alphanumeric || character == '_' || character == '-' ||
               character == '.';
    });
}

[[nodiscard]] bool complete_super_binding(
    const PreparedSuperArtifact& binding) noexcept {
    return complete_flash_binding(
        UpdateFlashArtifactInput{
            .resolved = binding.resolved(),
            .artifact = binding.artifact(),
        },
        kSuperEmptyName);
}

[[nodiscard]] std::expected<void, UpdateDeviceError>
require_userspace_fastboot(
    PrimitiveService& service,
    const UpdateOperationContext& context,
    const std::string_view phase) {
    if (auto stopped = interruption(
            context,
            "update-super was interrupted before " + std::string(phase))) {
        return std::unexpected(std::move(*stopped));
    }

    std::atomic<bool> cancellation_forwarded{false};
    auto result = invoke_with_cancellation(
        service, context.cancellation, cancellation_forwarded,
        [&service] { return service.getvar("is-userspace"); });
    if (!result) {
        const bool device_did_not_report_mode =
            result.error().code == PrimitiveErrorCode::DeviceFail;
        auto mapped = map_primitive_update_error(
            std::move(result.error()), context);
        retain_quarantine_if_forwarded(mapped, cancellation_forwarded);
        if (device_did_not_report_mode &&
            mapped.kind == UpdateDeviceErrorKind::Failed &&
            !cancellation_forwarded.load(std::memory_order_acquire)) {
            mapped.kind = UpdateDeviceErrorKind::Unsupported;
            mapped.message =
                "update-super requires reboot-fastboot and verified "
                "physical-port reconnect when is-userspace is unavailable; "
                "this transaction supports only an already-open fastbootd "
                "session";
        }
        return std::unexpected(std::move(mapped));
    }
    if (auto stopped = interruption_after_reply(
            context,
            "update-super was interrupted after " + std::string(phase),
            *result)) {
        retain_quarantine_if_forwarded(*stopped, cancellation_forwarded);
        return std::unexpected(std::move(*stopped));
    }
    if (result->terminal.payload != "yes") {
        auto error = local_error(
            UpdateDeviceErrorKind::Unsupported,
            "update-super requires reboot-fastboot and verified physical-port "
            "reconnect when is-userspace is not 'yes'; this transaction "
            "supports only an already-open fastbootd session",
            result->phase, result->outbound_certainty);
        error.informational = std::move(result->informational);
        error.inbound_expected = result->inbound_expected;
        error.inbound_transferred = result->inbound_transferred;
        error.inbound_certainty = result->inbound_certainty;
        return std::unexpected(std::move(error));
    }
    return {};
}

[[nodiscard]] std::expected<std::string, UpdateDeviceError>
resolve_super_partition_name(
    PrimitiveService& service,
    const UpdateOperationContext& context) {
    if (auto stopped = interruption(
            context,
            "update-super was interrupted before super-partition-name")) {
        return std::unexpected(std::move(*stopped));
    }

    std::atomic<bool> cancellation_forwarded{false};
    auto result = invoke_with_cancellation(
        service, context.cancellation, cancellation_forwarded,
        [&service] { return service.getvar("super-partition-name"); });
    if (!result) {
        const bool device_fail =
            result.error().code == PrimitiveErrorCode::DeviceFail;
        const bool interrupted = context.cancellation.stop_requested() ||
            (context.deadline &&
             std::chrono::steady_clock::now() >= *context.deadline) ||
            cancellation_forwarded.load(std::memory_order_acquire);
        if (device_fail && !interrupted) {
            // Frozen AOSP 37.0.1 uses "super" only for a well-formed device
            // FAIL. Transport/protocol failures never select a fallback.
            return std::string{"super"};
        }
        auto mapped = map_primitive_update_error(
            std::move(result.error()), context);
        retain_quarantine_if_forwarded(mapped, cancellation_forwarded);
        return std::unexpected(std::move(mapped));
    }
    if (auto stopped = interruption_after_reply(
            context,
            "update-super was interrupted after super-partition-name",
            *result)) {
        retain_quarantine_if_forwarded(*stopped, cancellation_forwarded);
        return std::unexpected(std::move(*stopped));
    }
    if (!valid_super_partition_name(result->terminal.payload)) {
        auto error = local_error(
            UpdateDeviceErrorKind::Failed,
            "Fastboot super-partition-name is not one valid partition name",
            result->phase, result->outbound_certainty);
        error.informational = std::move(result->informational);
        error.inbound_expected = result->inbound_expected;
        error.inbound_transferred = result->inbound_transferred;
        error.inbound_certainty = result->inbound_certainty;
        return std::unexpected(std::move(error));
    }
    return std::move(result->terminal.payload);
}

void quarantine_update_super(
    PrimitiveService& service,
    UpdateDeviceError& error) noexcept {
    // request_cancel is sticky in FastbootSession. The next attempted command
    // transitions a superficially Ready session to Poisoned, so callers cannot
    // reuse a session after a failed transaction or cancellation race.
    service.request_cancel();
    error.session_poisoned = true;
}

}  // namespace

VerifiedInitialSessionBinding::VerifiedInitialSessionBinding(
    std::unique_ptr<protocol::FastbootSession> session,
    std::unique_ptr<PrimitiveService> service,
    ReconnectTarget reconnect_target) noexcept
    : session_(std::move(session)),
      service_(std::move(service)),
      reconnect_target_(std::move(reconnect_target)) {}

std::expected<VerifiedInitialSessionBinding, UpdateDeviceError>
bind_initial_reconnect_session(
    OpenedReconnectSession opened,
    ReconnectTarget reconnect_target) {
    if (reconnect_target.previous_mode != FastbootUsbMode::Bootloader ||
        reconnect_target.required_mode != FastbootUsbMode::Fastbootd) {
        return std::unexpected(local_error(
            UpdateDeviceErrorKind::Failed,
            "initial USB session binding requires a bootloader-to-fastbootd "
            "mode direction"));
    }
    if (auto valid = validate_reconnect_request(reconnect_target); !valid) {
        auto error = local_error(
            UpdateDeviceErrorKind::Failed,
            "initial USB session binding has an invalid reconnect target: " +
                valid.error().message);
        error.native_code = valid.error().native_code;
        return std::unexpected(std::move(error));
    }
    if (opened.session == nullptr ||
        opened.session->state() != protocol::SessionState::Ready ||
        opened.outbound_certainty !=
            protocol::TransferCertainty::FullyTransferred) {
        return std::unexpected(local_error(
            UpdateDeviceErrorKind::Failed,
            "initial reconnect binding requires one ready, exclusively "
            "opened session with fully transferred identity probes"));
    }
    const auto& verified_identity = opened.verified_identity;
    if (verified_identity.physical_port != reconnect_target.physical_port ||
        verified_identity.usb_fingerprint !=
            reconnect_target.usb_fingerprint ||
        verified_identity.serial != reconnect_target.serial ||
        verified_identity.product != reconnect_target.product ||
        verified_identity.mode != reconnect_target.previous_mode) {
        return std::unexpected(local_error(
            UpdateDeviceErrorKind::Failed,
            "initial PrimitiveService is not factory-bound to the complete "
            "ReconnectTarget physical, descriptor, serial, product and mode "
            "identity"));
    }
    try {
        auto service = std::make_unique<PrimitiveService>(*opened.session);
        return VerifiedInitialSessionBinding(
            std::move(opened.session), std::move(service),
            std::move(reconnect_target));
    } catch (const std::bad_alloc&) {
        return std::unexpected(local_error(
            UpdateDeviceErrorKind::Failed,
            "unable to allocate the factory-bound initial service"));
    }
}

struct FastbootdSessionAccess final {
    PrimitiveService* service{};
    bool transitioned{};
};

// One strictly serialized update execution owns this state through shared
// prepared tokens. The initial PrimitiveService remains caller-owned. A
// successfully verified reconnect is adopted here together with its service,
// and only then becomes visible to later tokens.
class PrimitiveUpdateSessionActor final {
public:
    explicit PrimitiveUpdateSessionActor(PrimitiveService& service) noexcept
        : current_service_(&service) {}

    PrimitiveUpdateSessionActor(
        VerifiedInitialSessionBinding initial_binding,
        ReconnectCoordinator& reconnect_coordinator,
        ReconnectOptions reconnect_options) noexcept
        : current_service_(initial_binding.service_.get()),
          reconnect_coordinator_(&reconnect_coordinator),
          reconnect_target_(std::move(initial_binding.reconnect_target_)),
          reconnect_options_(reconnect_options),
          owned_session_(std::move(initial_binding.session_)),
          owned_service_(std::move(initial_binding.service_)) {}

    [[nodiscard]] PrimitiveService& current_service() const noexcept {
        return *current_service_;
    }

    [[nodiscard]] std::size_t generation() const noexcept {
        return generation_;
    }

    [[nodiscard]] bool reconnect_enabled() const noexcept {
        return reconnect_coordinator_ != nullptr;
    }

    [[nodiscard]] std::expected<void, UpdateDeviceError>
    validate_fastbootd_transition_configuration() const {
        if (!reconnect_enabled()) {
            return {};
        }
        if (reconnect_target_.previous_mode != FastbootUsbMode::Bootloader ||
            reconnect_target_.required_mode != FastbootUsbMode::Fastbootd) {
            return std::unexpected(local_error(
                UpdateDeviceErrorKind::Failed,
                "fastbootd transition requires a bootloader-to-fastbootd "
                "ReconnectTarget"));
        }
        if (auto valid = validate_reconnect_request(
                reconnect_target_, reconnect_options_);
            !valid) {
            auto error = local_error(
                UpdateDeviceErrorKind::Failed,
                "fastbootd transition has an invalid reconnect target or "
                "options: " + valid.error().message);
            error.native_code = valid.error().native_code;
            return std::unexpected(std::move(error));
        }
        return {};
    }

    [[nodiscard]] std::expected<void, UpdateDeviceError>
    validate_fastbootd_task_preparation(
        const UpdateOperationContext& context) const {
        if (auto valid = validate_fastbootd_transition_configuration();
            !valid) {
            return valid;
        }
        if (reconnect_enabled()) {
            return {};
        }
        return require_userspace_fastboot(
            current_service(), context, "the preparation mode check");
    }

    [[nodiscard]] std::expected<FastbootdSessionAccess, UpdateDeviceError>
    ensure_fastbootd(const UpdateOperationContext& context) {
        if (!reconnect_enabled()) {
            auto userspace = require_userspace_fastboot(
                current_service(), context, "the execution mode check");
            if (!userspace) {
                return std::unexpected(std::move(userspace.error()));
            }
            return FastbootdSessionAccess{
                .service = &current_service(),
                .transitioned = false,
            };
        }

        if (auto valid = validate_fastbootd_transition_configuration();
            !valid) {
            return std::unexpected(std::move(valid.error()));
        }
        if (unavailable_after_transition_) {
            auto error = local_error(
                UpdateDeviceErrorKind::Failed,
                "fastbootd transition actor cannot reuse a retired session",
                protocol::ProtocolPhase::Validation,
                protocol::TransferCertainty::NotTransferred,
                true);
            error.session_poisoned = true;
            return std::unexpected(std::move(error));
        }
        if (auto stopped = interruption(
                context,
                "fastbootd transition was interrupted before the mode query")) {
            return std::unexpected(std::move(*stopped));
        }

        auto& service = current_service();
        bool bootloader_mode = false;
        std::atomic<bool> query_cancel_forwarded{false};
        auto mode = invoke_with_cancellation(
            service, context.cancellation, query_cancel_forwarded,
            [&service] { return service.getvar("is-userspace"); });
        if (!mode) {
            const bool missing_mode_variable =
                mode.error().code == PrimitiveErrorCode::DeviceFail;
            auto mapped = map_primitive_update_error(
                std::move(mode.error()), context);
            retain_quarantine_if_forwarded(mapped, query_cancel_forwarded);
            if (!missing_mode_variable ||
                mapped.kind != UpdateDeviceErrorKind::Failed ||
                query_cancel_forwarded.load(std::memory_order_acquire)) {
                return std::unexpected(std::move(mapped));
            }
            // Frozen AOSP treats an unavailable is-userspace variable as
            // bootloader mode. A well-formed FAIL is the only error that may
            // select this non-destructive compatibility fallback.
            bootloader_mode = true;
        } else {
            if (auto stopped = interruption_after_reply(
                    context,
                    "fastbootd transition was interrupted after the mode query",
                    *mode)) {
                retain_quarantine_if_forwarded(*stopped,
                                                query_cancel_forwarded);
                return std::unexpected(std::move(*stopped));
            }
            if (mode->terminal.payload == "yes") {
                if (generation_ != 0U) {
                    return FastbootdSessionAccess{
                        .service = &service,
                        .transitioned = false,
                    };
                }
                auto error = local_error(
                    UpdateDeviceErrorKind::Failed,
                    "initial session mode no longer matches its "
                    "factory-bound bootloader identity",
                    mode->phase, mode->outbound_certainty);
                error.informational = std::move(mode->informational);
                error.inbound_expected = mode->inbound_expected;
                error.inbound_transferred = mode->inbound_transferred;
                error.inbound_certainty = mode->inbound_certainty;
                return std::unexpected(std::move(error));
            }
            if (mode->terminal.payload != "no") {
                auto error = local_error(
                    UpdateDeviceErrorKind::Failed,
                    "Fastboot is-userspace must be exactly 'yes' or 'no'",
                    mode->phase, mode->outbound_certainty);
                error.informational = std::move(mode->informational);
                return std::unexpected(std::move(error));
            }
            bootloader_mode = true;
        }

        if (!bootloader_mode) {
            return std::unexpected(local_error(
                UpdateDeviceErrorKind::Failed,
                "fastbootd transition reached an invalid mode state"));
        }

        auto product = verify_initial_identity_variable(
            service, "product", reconnect_target_.product, context);
        if (!product) {
            return std::unexpected(std::move(product.error()));
        }
        if (reconnect_target_.serial) {
            auto serial = verify_initial_identity_variable(
                service, "serialno", *reconnect_target_.serial, context);
            if (!serial) {
                return std::unexpected(std::move(serial.error()));
            }
        }
        if (auto stopped = interruption(
                context,
                "fastbootd transition was interrupted before reboot-fastboot")) {
            return std::unexpected(std::move(*stopped));
        }

        std::atomic<bool> reboot_cancel_forwarded{false};
        auto rebooted = invoke_with_cancellation(
            service, context.cancellation, reboot_cancel_forwarded,
            [&service] { return service.reboot(RebootTarget::Fastboot); });
        if (!rebooted) {
            const bool explicit_device_fail =
                rebooted.error().code == PrimitiveErrorCode::DeviceFail;
            auto mapped = map_primitive_update_error(
                std::move(rebooted.error()), context);
            retain_quarantine_if_forwarded(mapped,
                                            reboot_cancel_forwarded);
            // No discovery is allowed for any failed reboot result. In
            // particular, PartialOrUnknown cannot be converted into a scan or
            // guessed retry.
            if (mapped.session_closed || mapped.session_poisoned ||
                (!explicit_device_fail &&
                 mapped.outbound_certainty !=
                     protocol::TransferCertainty::NotTransferred)) {
                unavailable_after_transition_ = true;
                if (!mapped.session_closed) {
                    mapped.session_poisoned = true;
                }
            }
            describe_task_failure(
                mapped, 0U, 2U,
                mapped.outbound_certainty !=
                    protocol::TransferCertainty::NotTransferred);
            return std::unexpected(std::move(mapped));
        }
        unavailable_after_transition_ = true;
        if (rebooted->outbound_certainty !=
            protocol::TransferCertainty::FullyTransferred) {
            auto error = local_error(
                UpdateDeviceErrorKind::Failed,
                "reboot-fastboot succeeded without a fully transferred "
                "outbound command; reconnect was refused",
                rebooted->phase, rebooted->outbound_certainty, true);
            describe_task_failure(error, 0U, 2U, true);
            return std::unexpected(std::move(error));
        }
        if (auto stopped = interruption_after_reply(
                context,
                "fastbootd transition was interrupted after reboot-fastboot",
                *rebooted, true)) {
            retain_quarantine_if_forwarded(*stopped,
                                            reboot_cancel_forwarded);
            describe_task_failure(*stopped, 1U, 2U);
            return std::unexpected(std::move(*stopped));
        }

        auto target = reconnect_target_;
        target.preceding_operation_certainty = rebooted->outbound_certainty;
        const auto deadline = context.deadline.value_or(
            ReconnectTimePoint::max());
        auto reconnected = reconnect_coordinator_->reconnect(
            target, deadline, reconnect_options_, context.cancellation);
        if (!reconnected) {
            auto kind = UpdateDeviceErrorKind::Failed;
            if (context.cancellation.stop_requested() ||
                reconnected.error().code == ReconnectErrorCode::Cancelled) {
                kind = UpdateDeviceErrorKind::Cancelled;
            } else if (reconnected.error().code ==
                       ReconnectErrorCode::DeadlineExceeded) {
                kind = UpdateDeviceErrorKind::TimedOut;
            }
            auto error = local_error(
                kind,
                "verified fastbootd reconnect failed: " +
                    reconnected.error().message,
                protocol::ProtocolPhase::Validation,
                reconnected.error().reconnect_outbound_certainty,
                true);
            error.transport_certainty =
                reconnected.error().reconnect_outbound_certainty;
            error.native_code = reconnected.error().native_code;
            error.session_poisoned =
                reconnected.error().reconnect_outbound_certainty ==
                protocol::TransferCertainty::PartialOrUnknown;
            describe_task_failure(error, 1U, 2U);
            return std::unexpected(std::move(error));
        }

        const auto& identity = reconnected->identity;
        const bool serial_matches =
            !target.serial.has_value() || identity.serial == target.serial;
        if (identity.physical_port != target.physical_port ||
            identity.usb_fingerprint != target.usb_fingerprint ||
            !serial_matches || identity.product != target.product ||
            identity.mode != FastbootUsbMode::Fastbootd ||
            reconnected->session == nullptr ||
            reconnected->session->state() != protocol::SessionState::Ready) {
            auto error = local_error(
                UpdateDeviceErrorKind::Failed,
                "reconnect coordinator returned an unverified fastbootd "
                "session",
                protocol::ProtocolPhase::Validation,
                reconnected->outbound_certainty,
                true);
            error.session_poisoned =
                reconnected->outbound_certainty ==
                protocol::TransferCertainty::PartialOrUnknown;
            describe_task_failure(error, 1U, 2U);
            return std::unexpected(std::move(error));
        }
        if (context.cancellation.stop_requested()) {
            auto error = local_error(
                UpdateDeviceErrorKind::Cancelled,
                "fastbootd transition was cancelled before publishing the "
                "replacement session",
                protocol::ProtocolPhase::Validation,
                reconnected->outbound_certainty,
                true);
            describe_task_failure(error, 1U, 2U);
            return std::unexpected(std::move(error));
        }
        if (context.deadline &&
            std::chrono::steady_clock::now() >= *context.deadline) {
            auto error = local_error(
                UpdateDeviceErrorKind::TimedOut,
                "fastbootd transition deadline expired before publishing the "
                "replacement session",
                protocol::ProtocolPhase::Validation,
                reconnected->outbound_certainty,
                true);
            describe_task_failure(error, 1U, 2U);
            return std::unexpected(std::move(error));
        }
        if (generation_ == std::numeric_limits<std::size_t>::max()) {
            auto error = local_error(
                UpdateDeviceErrorKind::Failed,
                "fastbootd transition session generation overflowed",
                protocol::ProtocolPhase::Validation,
                protocol::TransferCertainty::NotTransferred,
                true);
            describe_task_failure(error, 1U, 2U);
            return std::unexpected(std::move(error));
        }

        try {
            auto replacement_session = std::move(reconnected->session);
            auto replacement_service =
                std::make_unique<PrimitiveService>(*replacement_session);
            retired_service_ = std::move(owned_service_);
            retired_session_ = std::move(owned_session_);
            owned_session_ = std::move(replacement_session);
            owned_service_ = std::move(replacement_service);
            current_service_ = owned_service_.get();
            ++generation_;
            unavailable_after_transition_ = false;
            return FastbootdSessionAccess{
                .service = current_service_,
                .transitioned = true,
            };
        } catch (const std::bad_alloc&) {
            auto error = local_error(
                UpdateDeviceErrorKind::Failed,
                "unable to allocate the replacement fastbootd service",
                protocol::ProtocolPhase::Validation,
                protocol::TransferCertainty::NotTransferred,
                true);
            describe_task_failure(error, 1U, 2U);
            return std::unexpected(std::move(error));
        }
    }

private:
    [[nodiscard]] static std::expected<void, UpdateDeviceError>
    verify_initial_identity_variable(
        PrimitiveService& service,
        const std::string_view name,
        const std::string_view expected,
        const UpdateOperationContext& context) {
        if (auto stopped = interruption(
                context,
                "fastbootd transition was interrupted before initial " +
                    std::string(name) + " verification")) {
            return std::unexpected(std::move(*stopped));
        }
        std::atomic<bool> cancellation_forwarded{false};
        auto reply = invoke_with_cancellation(
            service, context.cancellation, cancellation_forwarded,
            [&service, name] { return service.getvar(name); });
        if (!reply) {
            auto mapped = map_primitive_update_error(
                std::move(reply.error()), context);
            retain_quarantine_if_forwarded(mapped, cancellation_forwarded);
            return std::unexpected(std::move(mapped));
        }
        if (auto stopped = interruption_after_reply(
                context,
                "fastbootd transition was interrupted after initial " +
                    std::string(name) + " verification",
                *reply)) {
            retain_quarantine_if_forwarded(*stopped, cancellation_forwarded);
            return std::unexpected(std::move(*stopped));
        }
        if (reply->terminal.payload != expected) {
            auto error = local_error(
                UpdateDeviceErrorKind::Failed,
                "initial session " + std::string(name) +
                    " no longer matches its factory-bound USB identity",
                reply->phase, reply->outbound_certainty);
            error.informational = std::move(reply->informational);
            error.inbound_expected = reply->inbound_expected;
            error.inbound_transferred = reply->inbound_transferred;
            error.inbound_certainty = reply->inbound_certainty;
            return std::unexpected(std::move(error));
        }
        return {};
    }

    PrimitiveService* current_service_{};
    ReconnectCoordinator* reconnect_coordinator_{};
    ReconnectTarget reconnect_target_{};
    ReconnectOptions reconnect_options_{};
    // Retain the one retired initial session until every prepared token and
    // scripted/HIL observer has finished. It is closed before publication and
    // can never again be selected by current_service_.
    std::unique_ptr<protocol::FastbootSession> retired_session_;
    std::unique_ptr<PrimitiveService> retired_service_;
    std::unique_ptr<protocol::FastbootSession> owned_session_;
    std::unique_ptr<PrimitiveService> owned_service_;
    std::size_t generation_{};
    bool unavailable_after_transition_{};
};

namespace {

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
    std::size_t session_generation{};
};

struct PreparedUpdateSuper final {
    std::shared_ptr<const PreparedSuperArtifact> binding;
    std::shared_ptr<protocol::ITransferSource> source;
    std::string command;
    bool resolve_transaction_at_execution{};
    PrimitiveUpdateProgressObserver progress;
};

class PreparedUpdateSuperTask final : public IPreparedDeviceTask {
public:
    PreparedUpdateSuperTask(
        std::shared_ptr<PrimitiveUpdateSessionActor> session_actor,
        PreparedUpdateSuper prepared) noexcept
        : session_actor_(std::move(session_actor)),
          prepared_(std::move(prepared)) {}

    [[nodiscard]] std::expected<void, UpdateDeviceError>
    execute(const UpdateOperationContext& context) const override {
        std::size_t total_actions = 1U;
        std::size_t completed_actions = 0U;
        const auto task_error = [&](UpdateDeviceError error,
                                    const bool current_action_started = false) {
            describe_task_failure(error, completed_actions, total_actions,
                                  current_action_started);
            return error;
        };

        if (auto stopped = interruption(
                context,
                "update-super was interrupted before the transaction")) {
            return std::unexpected(task_error(std::move(*stopped)));
        }
        // Preparation happens before any task executes. Resolve the current
        // service here so an earlier prepared reboot cannot make the mode proof
        // or service reference stale.
        auto fastbootd = session_actor_->ensure_fastbootd(context);
        if (!fastbootd) {
            auto error = std::move(fastbootd.error());
            if (error.total_actions == 0U) {
                error = task_error(std::move(error));
                // The exact query diagnostics remain above, but the one DATA +
                // update-super action has not started.
                error.task_certainty =
                    protocol::TransferCertainty::NotTransferred;
            }
            return std::unexpected(std::move(error));
        }
        auto& service = *fastbootd->service;
        if (fastbootd->transitioned) {
            completed_actions = 1U;
            total_actions = 2U;
        }

        std::string command = prepared_.command;
        if (prepared_.resolve_transaction_at_execution) {
            auto partition = resolve_super_partition_name(service, context);
            if (!partition) {
                return std::unexpected(
                    task_error(std::move(partition.error())));
            }

            std::atomic<bool> maximum_cancel_forwarded{false};
            auto maximum_reply = invoke_with_cancellation(
                service, context.cancellation, maximum_cancel_forwarded,
                [&service] { return service.getvar("max-download-size"); });
            if (!maximum_reply) {
                auto error = map_primitive_update_error(
                    std::move(maximum_reply.error()), context);
                retain_quarantine_if_forwarded(error,
                                                maximum_cancel_forwarded);
                return std::unexpected(task_error(std::move(error)));
            }
            if (auto stopped = interruption_after_reply(
                    context,
                    "update-super was interrupted after max-download-size",
                    *maximum_reply)) {
                retain_quarantine_if_forwarded(*stopped,
                                                maximum_cancel_forwarded);
                return std::unexpected(task_error(std::move(*stopped)));
            }
            auto maximum =
                parse_unsigned_variable(maximum_reply->terminal.payload);
            if (!maximum || *maximum == 0U) {
                return std::unexpected(task_error(local_error(
                    UpdateDeviceErrorKind::Failed,
                    "max-download-size is not a positive complete unsigned "
                    "decimal or 0x-prefixed hexadecimal value",
                    maximum_reply->phase,
                    maximum_reply->outbound_certainty)));
            }
            const auto wire_maximum = std::min<std::uint64_t>(
                *maximum, std::numeric_limits<std::uint32_t>::max());
            if (prepared_.source->size() > wire_maximum) {
                return std::unexpected(task_error(local_error(
                    UpdateDeviceErrorKind::Failed,
                    "prepared super_empty.img exceeds the current fastbootd "
                    "max-download-size; the immutable source will not be "
                    "reopened or reparsed")));
            }

            command.assign(kUpdateSuperPrefix);
            command.append(*partition);
            if (prepared_.binding->wants_wipe()) {
                command.append(kUpdateSuperWipeSuffix);
            }
        }

        const auto total_bytes = prepared_.source->size();
        if (prepared_.progress) {
            try {
                if (prepared_.progress(PrimitiveUpdateProgress{
                        .part_count = 1U,
                        .total_bytes = total_bytes,
                    }) == PrimitiveUpdateProgressAction::Cancel) {
                    return std::unexpected(task_error(local_error(
                        UpdateDeviceErrorKind::Cancelled,
                        "update-super was cancelled by the progress observer "
                        "before DATA")));
                }
            } catch (...) {
                return std::unexpected(task_error(local_error(
                    UpdateDeviceErrorKind::Failed,
                    "update-super progress observer threw before DATA")));
            }
        }
        if (auto stopped = interruption(
                context,
                "update-super was interrupted after initial progress")) {
            return std::unexpected(task_error(std::move(*stopped)));
        }

        bool observer_threw = false;
        bool observer_cancelled = false;
        bool deadline_expired = false;
        const protocol::TransferProgressObserver observer =
            [this, &context, &observer_threw, &observer_cancelled,
             &deadline_expired, total_bytes](
                const std::uint64_t completed,
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
                                .part_count = 1U,
                                .part_completed_bytes = completed,
                                .part_total_bytes = total_bytes,
                                .completed_bytes = completed,
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
            service, context.cancellation, download_cancel_forwarded,
            [this, &service, &observer] {
                return service.download_source(prepared_.source, observer);
            });
        if (!downloaded) {
            auto mapped = map_primitive_update_error(
                std::move(downloaded.error()), context);
            retain_quarantine_if_forwarded(mapped,
                                            download_cancel_forwarded);
            if (observer_threw) {
                mapped.kind = UpdateDeviceErrorKind::Failed;
                mapped.message =
                    "update-super progress observer threw during DATA";
            } else if (deadline_expired &&
                       !context.cancellation.stop_requested()) {
                mapped.kind = UpdateDeviceErrorKind::TimedOut;
            } else if (observer_cancelled) {
                mapped.kind = UpdateDeviceErrorKind::Cancelled;
            }
            const bool transaction_started =
                mapped.outbound_certainty !=
                protocol::TransferCertainty::NotTransferred;
            quarantine_update_super(service, mapped);
            return std::unexpected(task_error(
                std::move(mapped), transaction_started));
        }

        if (auto stopped = interruption_after_reply(
                context,
                "update-super was interrupted after DATA and before the "
                "update-super command",
                *downloaded)) {
            retain_quarantine_if_forwarded(*stopped,
                                            download_cancel_forwarded);
            quarantine_update_super(service, *stopped);
            return std::unexpected(task_error(std::move(*stopped), true));
        }

        std::atomic<bool> command_cancel_forwarded{false};
        auto updated = invoke_with_cancellation(
            service, context.cancellation, command_cancel_forwarded,
            [&service, &command] { return service.raw_command(command); });
        if (!updated) {
            auto mapped = map_primitive_update_error(
                std::move(updated.error()), context);
            retain_quarantine_if_forwarded(mapped,
                                            command_cancel_forwarded);
            quarantine_update_super(service, mapped);
            return std::unexpected(task_error(std::move(mapped), true));
        }

        ++completed_actions;
        if (auto stopped = interruption_after_reply(
                context,
                "update-super was interrupted after the update-super command",
                *updated)) {
            retain_quarantine_if_forwarded(*stopped,
                                            command_cancel_forwarded);
            quarantine_update_super(service, *stopped);
            return std::unexpected(task_error(std::move(*stopped)));
        }
        return {};
    }

private:
    std::shared_ptr<PrimitiveUpdateSessionActor> session_actor_;
    // Retains the immutable package snapshot and the one exact transfer source.
    // update-super consumes one DATA object; splitting it into independent
    // sparse flash parts would change the frozen AOSP protocol.
    PreparedUpdateSuper prepared_;
};

class PreparedFlashTask final : public IPreparedDeviceTask {
public:
    PreparedFlashTask(
        std::shared_ptr<PrimitiveUpdateSessionActor> session_actor,
        PreparedFlash prepared) noexcept
        : session_actor_(std::move(session_actor)),
          prepared_(std::move(prepared)) {}

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

        auto& service = session_actor_->current_service();
        if (session_actor_->generation() != prepared_.session_generation) {
            std::atomic<bool> maximum_cancel_forwarded{false};
            auto maximum_reply = invoke_with_cancellation(
                service, context.cancellation, maximum_cancel_forwarded,
                [&service] { return service.getvar("max-download-size"); });
            if (!maximum_reply) {
                auto error = map_primitive_update_error(
                    std::move(maximum_reply.error()), context);
                retain_quarantine_if_forwarded(error,
                                                maximum_cancel_forwarded);
                return std::unexpected(task_error(std::move(error)));
            }
            if (auto stopped = interruption_after_reply(
                    context,
                    "flash was interrupted after validating the replacement "
                    "session download limit",
                    *maximum_reply)) {
                retain_quarantine_if_forwarded(*stopped,
                                                maximum_cancel_forwarded);
                return std::unexpected(task_error(std::move(*stopped)));
            }
            auto maximum =
                parse_unsigned_variable(maximum_reply->terminal.payload);
            if (!maximum || *maximum == 0U) {
                return std::unexpected(task_error(local_error(
                    UpdateDeviceErrorKind::Failed,
                    "replacement session max-download-size is invalid",
                    maximum_reply->phase,
                    maximum_reply->outbound_certainty)));
            }
            const auto wire_maximum = std::min<std::uint64_t>(
                *maximum, std::numeric_limits<std::uint32_t>::max());
            if (std::ranges::any_of(
                    prepared_.sources,
                    [wire_maximum](const auto& source) {
                        return source == nullptr ||
                            source->size() > wire_maximum;
                    })) {
                return std::unexpected(task_error(local_error(
                    UpdateDeviceErrorKind::Failed,
                    "prepared flash part exceeds the replacement session "
                    "max-download-size; immutable sparse sources will not be "
                    "reopened or replanned")));
            }
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
                service, context.cancellation, download_cancel_forwarded,
                [this, &service, &observer, index] {
                    return service.download_source(prepared_.sources[index],
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
                service, context.cancellation, flash_cancel_forwarded,
                [this, &service] {
                    return service.flash_downloaded(prepared_.partition);
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
    std::shared_ptr<PrimitiveUpdateSessionActor> session_actor_;
    // Retains the complete immutable FlashArtifact/ResolvedArtifact binding as
    // well as the one prepare-time sparse plan and its bound transfer sources.
    PreparedFlash prepared_;
};

class PreparedEraseTask final : public IPreparedDeviceTask {
public:
    PreparedEraseTask(
        std::shared_ptr<PrimitiveUpdateSessionActor> session_actor,
        std::string partition) noexcept
        : session_actor_(std::move(session_actor)),
          partition_(std::move(partition)) {}

    [[nodiscard]] std::expected<void, UpdateDeviceError>
    execute(const UpdateOperationContext& context) const override {
        if (auto stopped = interruption(
                context, "erase was interrupted before sending a command")) {
            describe_task_failure(*stopped, 0U, 1U);
            return std::unexpected(std::move(*stopped));
        }
        std::atomic<bool> cancellation_forwarded{false};
        auto& service = session_actor_->current_service();
        auto result = invoke_with_cancellation(
            service, context.cancellation, cancellation_forwarded,
            [this, &service] { return service.erase(partition_); });
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
    std::shared_ptr<PrimitiveUpdateSessionActor> session_actor_;
    std::string partition_;
};

class PreparedRebootTask final : public IPreparedDeviceTask {
public:
    PreparedRebootTask(std::shared_ptr<PrimitiveUpdateSessionActor> session_actor,
                       const RebootTarget target) noexcept
        : session_actor_(std::move(session_actor)), target_(target) {}

    [[nodiscard]] std::expected<void, UpdateDeviceError>
    execute(const UpdateOperationContext& context) const override {
        if (auto stopped = interruption(
                context, "reboot was interrupted before sending a command")) {
            describe_task_failure(*stopped, 0U, 1U);
            return std::unexpected(std::move(*stopped));
        }
        if (target_ == RebootTarget::Fastboot) {
            auto fastbootd = session_actor_->ensure_fastbootd(context);
            if (!fastbootd) {
                return std::unexpected(std::move(fastbootd.error()));
            }
            return {};
        }
        std::atomic<bool> cancellation_forwarded{false};
        auto& service = session_actor_->current_service();
        auto result = invoke_with_cancellation(
            service, context.cancellation, cancellation_forwarded,
            [this, &service] { return service.reboot(target_); });
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
    std::shared_ptr<PrimitiveUpdateSessionActor> session_actor_;
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
    } else if (error.code == PrimitiveErrorCode::Unsupported) {
        kind = UpdateDeviceErrorKind::Unsupported;
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
    PrimitiveUpdateDeviceOptions options)
    : session_actor_(
          std::make_shared<PrimitiveUpdateSessionActor>(service)),
      options_(std::move(options)) {}

PrimitiveUpdateDevice::PrimitiveUpdateDevice(
    ReconnectConstructionTag,
    VerifiedInitialSessionBinding initial_binding,
    ReconnectCoordinator& reconnect_coordinator,
    ReconnectOptions reconnect_options,
    PrimitiveUpdateDeviceOptions options)
    : session_actor_(std::make_shared<PrimitiveUpdateSessionActor>(
          std::move(initial_binding),
          reconnect_coordinator,
          reconnect_options)),
      options_(std::move(options)) {}

std::expected<std::unique_ptr<PrimitiveUpdateDevice>, UpdateDeviceError>
PrimitiveUpdateDevice::create_with_reconnect(
    VerifiedInitialSessionBinding initial_binding,
    ReconnectCoordinator& reconnect_coordinator,
    const ReconnectOptions reconnect_options,
    PrimitiveUpdateDeviceOptions options) {
    try {
        return std::unique_ptr<PrimitiveUpdateDevice>(
            new PrimitiveUpdateDevice(
                ReconnectConstructionTag{}, std::move(initial_binding),
                reconnect_coordinator, reconnect_options,
                std::move(options)));
    } catch (const std::bad_alloc&) {
        return std::unexpected(local_error(
            UpdateDeviceErrorKind::Failed,
            "unable to allocate the reconnect-capable update actor"));
    } catch (const std::exception& error) {
        return std::unexpected(local_error(
            UpdateDeviceErrorKind::Failed,
            "unable to construct the reconnect-capable update actor: " +
                std::string(error.what())));
    } catch (...) {
        return std::unexpected(local_error(
            UpdateDeviceErrorKind::Failed,
            "unable to construct the reconnect-capable update actor"));
    }
}

std::expected<std::string, UpdateDeviceError> PrimitiveUpdateDevice::getvar(
    const std::string_view name,
    const UpdateOperationContext& context) {
    if (auto stopped = interruption(
            context, "getvar was interrupted before sending a command")) {
        return std::unexpected(std::move(*stopped));
    }

    auto& service = session_actor_->current_service();
    std::atomic<bool> cancellation_forwarded{false};
    auto result = invoke_with_cancellation(
        service, context.cancellation, cancellation_forwarded,
        [&service, name] { return service.getvar(name); });
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

void PrimitiveUpdateDevice::synchronize_session_cache_generation() noexcept {
    const auto generation = session_actor_->generation();
    if (cache_generation_ == generation) {
        return;
    }
    maximum_download_size_.reset();
    current_slot_.reset();
    slot_topology_.reset();
    has_slot_.clear();
    cache_generation_ = generation;
}

std::expected<std::uint64_t, UpdateDeviceError>
PrimitiveUpdateDevice::maximum_download_size(
    const UpdateOperationContext& context) {
    synchronize_session_cache_generation();
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
    synchronize_session_cache_generation();
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
    synchronize_session_cache_generation();
    if (slot_topology_) {
        return *slot_topology_;
    }

    auto& service = session_actor_->current_service();
    std::atomic<bool> cancellation_forwarded{false};
    auto count_reply = invoke_with_cancellation(
        service, context.cancellation, cancellation_forwarded,
        [&service] { return service.getvar("slot-count"); });
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
    synchronize_session_cache_generation();
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
                    UpdateDeviceErrorKind::Unsupported,
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
                .session_generation = session_actor_->generation(),
            };
            std::unique_ptr<IPreparedDeviceTask> token =
                std::make_unique<PreparedFlashTask>(session_actor_,
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
                std::make_unique<PreparedEraseTask>(session_actor_,
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
            if (*target == RebootTarget::Fastboot) {
                auto valid =
                    session_actor_->validate_fastbootd_task_preparation(
                        context);
                if (!valid) {
                    return std::unexpected(std::move(valid.error()));
                }
            }
            std::unique_ptr<IPreparedDeviceTask> token =
                std::make_unique<PreparedRebootTask>(session_actor_, *target);
            return token;
        }
        case UpdateTaskKind::UpdateSuper: {
            if (input.flash_artifact || !input.super_artifact ||
                !input.task.partition.empty() || !input.task.artifact.empty() ||
                input.task.slot != PlannedSlot::Default ||
                input.task.apply_vbmeta ||
                input.task.reboot_target != PlannedRebootTarget::System ||
                !complete_super_binding(*input.super_artifact)) {
                return std::unexpected(local_error(
                    UpdateDeviceErrorKind::Failed,
                    "update-super task and complete immutable "
                    "super_empty.img binding are inconsistent"));
            }

            auto transition =
                session_actor_->validate_fastbootd_task_preparation(context);
            if (!transition) {
                return std::unexpected(std::move(transition.error()));
            }

            const auto& artifact = input.super_artifact->artifact();
            if (auto valid = validate_download_size(
                    artifact->metadata().transfer_size);
                !valid) {
                return std::unexpected(map_primitive_update_error(
                    std::move(valid.error()), context));
            }
            if (auto stopped = interruption(
                    context,
                    "update-super preparation was interrupted before binding "
                    "the transfer source")) {
                return std::unexpected(std::move(*stopped));
            }
            auto source = transport::ImageTransferSource::create(
                artifact->transfer_source());
            if (!source) {
                return std::unexpected(local_error(
                    UpdateDeviceErrorKind::Failed,
                    "unable to bind update-super transfer source: " +
                        source.error().message));
            }
            if ((*source)->size() != artifact->metadata().transfer_size) {
                return std::unexpected(local_error(
                    UpdateDeviceErrorKind::Failed,
                    "bound update-super transfer source changed size"));
            }

            std::string command;
            const bool resolve_transaction_at_execution =
                session_actor_->reconnect_enabled();
            if (!resolve_transaction_at_execution) {
                auto partition = resolve_super_partition_name(
                    session_actor_->current_service(), context);
                if (!partition) {
                    return std::unexpected(std::move(partition.error()));
                }
                auto maximum = maximum_download_size(context);
                if (!maximum) {
                    return std::unexpected(std::move(maximum.error()));
                }
                if (artifact->metadata().transfer_size > *maximum) {
                    return std::unexpected(local_error(
                        UpdateDeviceErrorKind::Failed,
                        "prepared super_empty.img exceeds max-download-size; "
                        "update-super consumes one exact DATA object and "
                        "cannot use independent sparse flash parts"));
                }
                command.assign(kUpdateSuperPrefix);
                command.append(*partition);
                if (input.super_artifact->wants_wipe()) {
                    command.append(kUpdateSuperWipeSuffix);
                }
            }
            if (auto stopped = interruption(
                    context,
                    "update-super preparation was interrupted after binding "
                    "the transaction")) {
                return std::unexpected(std::move(*stopped));
            }

            PreparedUpdateSuper prepared{
                .binding = std::move(input.super_artifact),
                .source = std::move(*source),
                .command = std::move(command),
                .resolve_transaction_at_execution =
                    resolve_transaction_at_execution,
                .progress = options_.progress,
            };
            std::unique_ptr<IPreparedDeviceTask> token =
                std::make_unique<PreparedUpdateSuperTask>(
                    session_actor_, std::move(prepared));
            return token;
        }
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
