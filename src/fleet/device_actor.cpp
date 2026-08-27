// SPDX-License-Identifier: MIT
#include "device_actor.hpp"

#include "src/fastboot/slot_planner.hpp"

#include <chrono>
#include <exception>
#include <limits>
#include <new>
#include <stop_token>
#include <utility>

namespace kairosboot::fleet {
namespace {

constexpr std::uint8_t kActorReady = 0U;
constexpr std::uint8_t kActorExecuting = 1U;
constexpr std::uint8_t kActorFinished = 2U;

[[nodiscard]] bool interrupted(
    const fastboot::UpdateOperationContext& context) noexcept {
    return context.cancellation.stop_requested() ||
        (context.deadline &&
         std::chrono::steady_clock::now() >= *context.deadline);
}

[[nodiscard]] FleetActorPrepareError interruption_prepare_error(
    const fastboot::UpdateOperationContext& context,
    std::string message,
    const std::optional<std::size_t> device_index = std::nullopt,
    const std::optional<std::size_t> step_index = std::nullopt) {
    return {
        .kind = context.cancellation.stop_requested()
            ? FleetActorPrepareErrorKind::Cancelled
            : FleetActorPrepareErrorKind::TimedOut,
        .message = std::move(message),
        .device_index = device_index,
        .step_index = step_index,
        .device_error = std::nullopt,
    };
}

[[nodiscard]] FleetActorPrepareError device_prepare_error(
    fastboot::UpdateDeviceError error,
    std::string message,
    const std::size_t device_index,
    const std::size_t step_index) {
    auto kind = FleetActorPrepareErrorKind::TaskPreparationFailed;
    if (error.kind == fastboot::UpdateDeviceErrorKind::Cancelled) {
        kind = FleetActorPrepareErrorKind::Cancelled;
    } else if (error.kind == fastboot::UpdateDeviceErrorKind::TimedOut) {
        kind = FleetActorPrepareErrorKind::TimedOut;
    }
    return {
        .kind = kind,
        .message = std::move(message),
        .device_index = device_index,
        .step_index = step_index,
        .device_error = std::move(error),
    };
}

[[nodiscard]] FleetActorPrepareError slot_prepare_error(
    fastboot::SlotError error,
    const fastboot::UpdateOperationContext& context,
    const std::size_t device_index,
    const std::size_t step_index) {
    auto kind = FleetActorPrepareErrorKind::SlotResolutionFailed;
    if (error.code == fastboot::SlotErrorCode::Cancelled ||
        context.cancellation.stop_requested()) {
        kind = FleetActorPrepareErrorKind::Cancelled;
    } else if (error.code == fastboot::SlotErrorCode::TimedOut ||
               (context.deadline &&
                std::chrono::steady_clock::now() >= *context.deadline)) {
        kind = FleetActorPrepareErrorKind::TimedOut;
    }

    std::optional<fastboot::UpdateDeviceError> nested;
    if (error.query_error) {
        nested = fastboot::map_primitive_update_error(
            std::move(*error.query_error), context);
    }
    return {
        .kind = kind,
        .message = std::move(error.message),
        .device_index = device_index,
        .step_index = step_index,
        .device_error = std::move(nested),
    };
}

[[nodiscard]] std::optional<std::string_view> flash_slot_name(
    const std::optional<ManifestFlashSlot> slot) noexcept {
    if (!slot || *slot == ManifestFlashSlot::Current) {
        return std::string_view{};
    }
    switch (*slot) {
    case ManifestFlashSlot::Other:
        return "other";
    case ManifestFlashSlot::All:
        return "all";
    case ManifestFlashSlot::A:
        return "a";
    case ManifestFlashSlot::B:
        return "b";
    case ManifestFlashSlot::Current:
        return std::string_view{};
    default:
        return std::nullopt;
    }
}

[[nodiscard]] std::optional<std::string_view> active_slot_name(
    const ManifestActiveSlot slot) noexcept {
    switch (slot) {
    case ManifestActiveSlot::A:
        return "a";
    case ManifestActiveSlot::B:
        return "b";
    case ManifestActiveSlot::Other:
        return "other";
    default:
        return std::nullopt;
    }
}

[[nodiscard]] std::optional<ReportSlot> report_flash_slot(
    const std::optional<ManifestFlashSlot> slot) noexcept {
    if (!slot) {
        return std::nullopt;
    }
    switch (*slot) {
    case ManifestFlashSlot::Current:
        return ReportSlot::Current;
    case ManifestFlashSlot::Other:
        return ReportSlot::Other;
    case ManifestFlashSlot::All:
        return ReportSlot::All;
    case ManifestFlashSlot::A:
        return ReportSlot::A;
    case ManifestFlashSlot::B:
        return ReportSlot::B;
    default:
        return std::nullopt;
    }
}

[[nodiscard]] std::optional<ReportSlot> report_active_slot(
    const ManifestActiveSlot slot) noexcept {
    switch (slot) {
    case ManifestActiveSlot::A:
        return ReportSlot::A;
    case ManifestActiveSlot::B:
        return ReportSlot::B;
    case ManifestActiveSlot::Other:
        return ReportSlot::Other;
    default:
        return std::nullopt;
    }
}

[[nodiscard]] std::optional<fastboot::RebootTarget> reboot_target(
    const ManifestRebootTarget target) noexcept {
    switch (target) {
    case ManifestRebootTarget::System:
        return fastboot::RebootTarget::System;
    case ManifestRebootTarget::Bootloader:
        return fastboot::RebootTarget::Bootloader;
    case ManifestRebootTarget::Recovery:
        return fastboot::RebootTarget::Recovery;
    case ManifestRebootTarget::Fastboot:
        return fastboot::RebootTarget::Fastboot;
    default:
        return std::nullopt;
    }
}

[[nodiscard]] std::optional<ReportRebootTarget> report_reboot_target(
    const ManifestRebootTarget target) noexcept {
    switch (target) {
    case ManifestRebootTarget::System:
        return ReportRebootTarget::System;
    case ManifestRebootTarget::Bootloader:
        return ReportRebootTarget::Bootloader;
    case ManifestRebootTarget::Recovery:
        return ReportRebootTarget::Recovery;
    case ManifestRebootTarget::Fastboot:
        return ReportRebootTarget::Fastboot;
    default:
        return std::nullopt;
    }
}

[[nodiscard]] bool checked_add_i_json(
    std::uint64_t& total,
    const std::uint64_t value) noexcept {
    if (value > kFleetActorMaximumIJsonInteger ||
        total > kFleetActorMaximumIJsonInteger - value) {
        return false;
    }
    total += value;
    return true;
}

[[nodiscard]] fastboot::UpdateDeviceError local_device_error(
    const fastboot::UpdateDeviceErrorKind kind,
    std::string message,
    const protocol::TransferCertainty certainty =
        protocol::TransferCertainty::NotTransferred,
    const bool closed = false) {
    return {
        .kind = kind,
        .phase = protocol::ProtocolPhase::Validation,
        .message = std::move(message),
        .device_message = {},
        .informational = {},
        .transport_status = protocol::TransportStatus::Ok,
        .transport_certainty = certainty,
        .outbound_certainty = certainty,
        .inbound_expected = std::nullopt,
        .inbound_transferred = 0U,
        .inbound_certainty = protocol::TransferCertainty::NotTransferred,
        .session_poisoned = false,
        .session_closed = closed,
        .native_code = 0,
        .task_certainty = protocol::TransferCertainty::NotTransferred,
        .completed_actions = 0U,
        .total_actions = 0U,
    };
}

void describe_single_action(
    fastboot::UpdateDeviceError& error,
    const bool command_completed) noexcept {
    error.completed_actions = command_completed ? 1U : 0U;
    error.total_actions = 1U;
    error.task_certainty = command_completed
        ? protocol::TransferCertainty::FullyTransferred
        : error.outbound_certainty;
}

class PreparedPrimitiveCommandTask final
    : public fastboot::IPreparedDeviceTask {
public:
    enum class Kind : std::uint8_t {
        SetActive,
        Reboot,
        Oem,
    };

    PreparedPrimitiveCommandTask(fastboot::PrimitiveService& service,
                                 const Kind kind,
                                 std::string parameter,
                                 const fastboot::RebootTarget reboot) noexcept
        : service_(&service),
          kind_(kind),
          parameter_(std::move(parameter)),
          reboot_(reboot) {}

    [[nodiscard]] std::uint64_t host_to_device_data_bytes()
        const noexcept override {
        return 0U;
    }

    [[nodiscard]] std::expected<void, fastboot::UpdateDeviceError>
    execute(const fastboot::UpdateOperationContext& context) const override {
        if (context.cancellation.stop_requested()) {
            auto error = local_device_error(
                fastboot::UpdateDeviceErrorKind::Cancelled,
                "fleet command was cancelled before it was sent");
            describe_single_action(error, false);
            return std::unexpected(std::move(error));
        }
        if (context.deadline &&
            std::chrono::steady_clock::now() >= *context.deadline) {
            auto error = local_device_error(
                fastboot::UpdateDeviceErrorKind::TimedOut,
                "fleet command deadline expired before it was sent");
            describe_single_action(error, false);
            return std::unexpected(std::move(error));
        }

        std::atomic<bool> cancellation_forwarded{false};
        std::expected<fastboot::PrimitiveReply, fastboot::PrimitiveError>
            result;
        {
            // The callback covers only the primitive call itself, matching
            // invoke_with_cancellation() in primitive_update_device.cpp. A
            // cancellation arriving while this block is active is forwarded
            // to the session and poisons it; one arriving after the block
            // is never forwarded and is left to the later
            // stop_requested()/deadline checks.
            std::stop_callback cancellation(
                context.cancellation, [this, &cancellation_forwarded] {
                    cancellation_forwarded.store(true,
                                                 std::memory_order_release);
                    service_->request_cancel();
                });
            result = [&]() {
                switch (kind_) {
                case Kind::SetActive:
                    return service_->set_active(parameter_);
                case Kind::Reboot:
                    return service_->reboot(reboot_);
                case Kind::Oem:
                    return service_->oem(parameter_);
                }
                return service_->raw_command({});
            }();
        }
        if (!result) {
            auto error = fastboot::map_primitive_update_error(
                std::move(result.error()), context);
            if (cancellation_forwarded.load(std::memory_order_acquire)) {
                error.session_poisoned = true;
            }
            describe_single_action(error, false);
            return std::unexpected(std::move(error));
        }

        if (context.cancellation.stop_requested()) {
            auto error = local_device_error(
                fastboot::UpdateDeviceErrorKind::Cancelled,
                "fleet command was cancelled after its response",
                result->outbound_certainty, kind_ == Kind::Reboot);
            error.informational = std::move(result->informational);
            error.inbound_expected = result->inbound_expected;
            error.inbound_transferred = result->inbound_transferred;
            error.inbound_certainty = result->inbound_certainty;
            error.phase = result->phase;
            error.session_poisoned =
                cancellation_forwarded.load(std::memory_order_acquire);
            describe_single_action(error, true);
            return std::unexpected(std::move(error));
        }
        if (context.deadline &&
            std::chrono::steady_clock::now() >= *context.deadline) {
            auto error = local_device_error(
                fastboot::UpdateDeviceErrorKind::TimedOut,
                "fleet command deadline expired after its response",
                result->outbound_certainty, kind_ == Kind::Reboot);
            error.informational = std::move(result->informational);
            error.inbound_expected = result->inbound_expected;
            error.inbound_transferred = result->inbound_transferred;
            error.inbound_certainty = result->inbound_certainty;
            error.phase = result->phase;
            describe_single_action(error, true);
            return std::unexpected(std::move(error));
        }
        return {};
    }

private:
    fastboot::PrimitiveService* service_{};
    Kind kind_{Kind::SetActive};
    std::string parameter_;
    fastboot::RebootTarget reboot_{fastboot::RebootTarget::System};
};

[[nodiscard]] FleetActorExecutionError execution_error(
    const FleetActorExecutionErrorKind kind,
    std::string message,
    const std::size_t device_index,
    const std::optional<std::size_t> step_index,
    const std::size_t completed_steps,
    const std::uint64_t completed_data_bytes,
    std::optional<fastboot::UpdateDeviceError> device_error = std::nullopt,
    const std::size_t completed_child_tasks_in_step = 0U,
    const std::size_t total_child_tasks_in_step = 0U) {
    return {
        .kind = kind,
        .message = std::move(message),
        .device_index = device_index,
        .step_index = step_index,
        .completed_steps = completed_steps,
        .completed_data_bytes = completed_data_bytes,
        .completed_child_tasks_in_step = completed_child_tasks_in_step,
        .total_child_tasks_in_step = total_child_tasks_in_step,
        .device_error = std::move(device_error),
    };
}

[[nodiscard]] FleetActorExecutionError task_execution_error(
    fastboot::UpdateDeviceError error,
    const std::size_t device_index,
    const std::size_t step_index,
    const std::size_t completed_steps,
    const std::uint64_t completed_data_bytes,
    const std::size_t completed_child_tasks_in_step,
    const std::size_t total_child_tasks_in_step) {
    auto kind = FleetActorExecutionErrorKind::DeviceTaskFailed;
    if (error.kind == fastboot::UpdateDeviceErrorKind::Cancelled) {
        kind = FleetActorExecutionErrorKind::Cancelled;
    } else if (error.kind == fastboot::UpdateDeviceErrorKind::TimedOut) {
        kind = FleetActorExecutionErrorKind::TimedOut;
    }
    auto message = error.message;
    return execution_error(kind, std::move(message), device_index, step_index,
                           completed_steps, completed_data_bytes,
                           std::move(error), completed_child_tasks_in_step,
                           total_child_tasks_in_step);
}

}  // namespace

struct FleetDeviceActor::PreparedStep final {
    ReportStepSpec report;
    std::vector<std::unique_ptr<fastboot::IPreparedDeviceTask>> tasks;
};

FleetDeviceActor::FleetDeviceActor(PreparedDeviceSession&& prepared,
                                   const ManifestTarget& target)
    : session_(std::move(prepared).take_session()),
      service_(std::make_unique<fastboot::PrimitiveService>(*session_)),
      update_(std::make_unique<fastboot::PrimitiveUpdateDevice>(*service_)),
      target_(&target) {}

FleetDeviceActor::~FleetDeviceActor() = default;

std::expected<ReportDeviceSpec, FleetActorPrepareError>
FleetDeviceActor::prepare(
    const PreparedFleetArtifacts& artifacts,
    const fastboot::UpdateOperationContext& context,
    const std::size_t device_index) {
    if (target_ == nullptr || session_ == nullptr || service_ == nullptr ||
        update_ == nullptr) {
        return std::unexpected(FleetActorPrepareError{
            .kind = FleetActorPrepareErrorKind::InvalidArgument,
            .message = "fleet actor does not own one complete device session",
            .device_index = device_index,
            .step_index = std::nullopt,
            .device_error = std::nullopt,
        });
    }

    const auto& target = *target_;
    ReportDeviceSpec report{
        .identifier = {},
        .serial = std::nullopt,
        .usb_path = std::nullopt,
        .target = target.name.value,
        .expected_product = target.expected_product.value,
        .observed_product = std::nullopt,
        .steps = {},
    };
    std::uint64_t device_data_bytes = 0U;
    steps_.reserve(target.steps.size());
    report.steps.reserve(target.steps.size());
    fastboot::SlotPlanner slots(*service_);

    for (std::size_t step_index = 0U; step_index < target.steps.size();
         ++step_index) {
        if (interrupted(context)) {
            return std::unexpected(interruption_prepare_error(
                context, "fleet actor preparation was interrupted",
                device_index, step_index));
        }

        const auto& manifest_step = target.steps[step_index];
        PreparedStep prepared;
        if (const auto* flash =
                std::get_if<ManifestFlashStep>(&manifest_step.payload)) {
            prepared.report.operation = ReportOperation::Flash;
            prepared.report.partition = flash->partition.value;
            prepared.report.artifact = flash->artifact.value;
            prepared.report.slot = report_flash_slot(flash->slot);
            const auto requested = flash_slot_name(flash->slot);
            if (!requested) {
                return std::unexpected(FleetActorPrepareError{
                    .kind = FleetActorPrepareErrorKind::InvalidPlan,
                    .message = "flash step contains an invalid slot",
                    .device_index = device_index,
                    .step_index = step_index,
                .device_error = std::nullopt,
                });
            }
            auto partition_plan = slots.plan_partition(
                flash->partition.value, *requested, context);
            if (!partition_plan) {
                return std::unexpected(slot_prepare_error(
                    std::move(partition_plan.error()), context, device_index,
                    step_index));
            }
            const auto* artifact = artifacts.find(flash->artifact.value);
            if (artifact == nullptr || artifact->source() == nullptr) {
                return std::unexpected(FleetActorPrepareError{
                    .kind = FleetActorPrepareErrorKind::MissingArtifact,
                    .message = "flash step does not have a sealed artifact",
                    .device_index = device_index,
                    .step_index = step_index,
                .device_error = std::nullopt,
                });
            }

            const auto owner = artifact->source();
            const auto flash_artifact =
                std::shared_ptr<const image::FlashArtifact>(
                    owner, &owner->flash_artifact());
            std::uint64_t step_bytes = 0U;
            prepared.tasks.reserve(partition_plan->partition_names.size());
            for (const auto& partition : partition_plan->partition_names) {
                fastboot::UpdateDeviceTaskInput input{
                    .task = {
                        .kind = fastboot::UpdateTaskKind::Flash,
                        .conditional_on_wipe = false,
                        .location = {},
                        .partition = partition,
                        .artifact = owner->resolved()->logical_name,
                        .slot = fastboot::PlannedSlot::Default,
                        .apply_vbmeta = false,
                        .reboot_target = fastboot::PlannedRebootTarget::System,
                    },
                    .flash_artifact = fastboot::UpdateFlashArtifactInput{
                        .resolved = owner->resolved(),
                        .artifact = flash_artifact,
                    },
                    .super_artifact = {},
                };
                auto task = update_->prepare_task(std::move(input), context);
                if (!task) {
                    return std::unexpected(device_prepare_error(
                        std::move(task.error()),
                        "unable to prepare fleet flash step", device_index,
                        step_index));
                }
                const auto bytes = (*task)->host_to_device_data_bytes();
                if (!checked_add_i_json(step_bytes, bytes)) {
                    return std::unexpected(FleetActorPrepareError{
                        .kind = FleetActorPrepareErrorKind::IntegerOutOfRange,
                        .message = "flash DATA byte total exceeds the I-JSON integer range",
                        .device_index = device_index,
                        .step_index = step_index,
                    .device_error = std::nullopt,
                    });
                }
                prepared.tasks.push_back(std::move(*task));
            }
            if (prepared.tasks.empty()) {
                return std::unexpected(FleetActorPrepareError{
                    .kind = FleetActorPrepareErrorKind::InvalidPlan,
                    .message = "slot planning produced no flash task",
                    .device_index = device_index,
                    .step_index = step_index,
                .device_error = std::nullopt,
                });
            }
            prepared.report.bytes_total = step_bytes;
        } else if (const auto* erase =
                       std::get_if<ManifestEraseStep>(&manifest_step.payload)) {
            prepared.report.operation = ReportOperation::Erase;
            prepared.report.partition = erase->partition.value;
            fastboot::UpdateDeviceTaskInput input{
                .task = {
                    .kind = fastboot::UpdateTaskKind::Erase,
                    .conditional_on_wipe = false,
                    .location = {},
                    .partition = erase->partition.value,
                    .artifact = {},
                    .slot = fastboot::PlannedSlot::Default,
                    .apply_vbmeta = false,
                    .reboot_target = fastboot::PlannedRebootTarget::System,
                },
                .flash_artifact = std::nullopt,
                .super_artifact = {},
            };
            auto task = update_->prepare_task(std::move(input), context);
            if (!task) {
                return std::unexpected(device_prepare_error(
                    std::move(task.error()),
                    "unable to prepare fleet erase step", device_index,
                    step_index));
            }
            prepared.tasks.push_back(std::move(*task));
        } else if (const auto* active =
                       std::get_if<ManifestSetActiveStep>(
                           &manifest_step.payload)) {
            prepared.report.operation = ReportOperation::SetActive;
            prepared.report.slot = report_active_slot(active->slot);
            const auto requested = active_slot_name(active->slot);
            if (!requested || !prepared.report.slot) {
                return std::unexpected(FleetActorPrepareError{
                    .kind = FleetActorPrepareErrorKind::InvalidPlan,
                    .message = "set-active step contains an invalid slot",
                    .device_index = device_index,
                    .step_index = step_index,
                .device_error = std::nullopt,
                });
            }
            auto resolved = slots.resolve_active_slot(*requested, context);
            if (!resolved) {
                return std::unexpected(slot_prepare_error(
                    std::move(resolved.error()), context, device_index,
                    step_index));
            }
            prepared.tasks.push_back(
                std::make_unique<PreparedPrimitiveCommandTask>(
                    *service_, PreparedPrimitiveCommandTask::Kind::SetActive,
                    std::move(*resolved), fastboot::RebootTarget::System));
        } else if (const auto* reboot =
                       std::get_if<ManifestRebootStep>(
                           &manifest_step.payload)) {
            if (step_index + 1U != target.steps.size()) {
                return std::unexpected(FleetActorPrepareError{
                    .kind = FleetActorPrepareErrorKind::InvalidPlan,
                    .message = "reboot must be the final step for a target",
                    .device_index = device_index,
                    .step_index = step_index,
                .device_error = std::nullopt,
                });
            }
            const auto resolved = reboot_target(reboot->target);
            prepared.report.reboot_target =
                report_reboot_target(reboot->target);
            if (!resolved || !prepared.report.reboot_target) {
                return std::unexpected(FleetActorPrepareError{
                    .kind = FleetActorPrepareErrorKind::InvalidPlan,
                    .message = "reboot step contains an invalid target",
                    .device_index = device_index,
                    .step_index = step_index,
                .device_error = std::nullopt,
                });
            }
            prepared.report.operation = ReportOperation::Reboot;
            prepared.tasks.push_back(
                std::make_unique<PreparedPrimitiveCommandTask>(
                    *service_, PreparedPrimitiveCommandTask::Kind::Reboot,
                    std::string{}, *resolved));
        } else if (const auto* oem =
                       std::get_if<ManifestOemStep>(&manifest_step.payload)) {
            auto command = fastboot::validate_oem_command_suffix(
                oem->command.value);
            if (!command) {
                return std::unexpected(device_prepare_error(
                    fastboot::map_primitive_update_error(
                        std::move(command.error()), context),
                    "unable to validate fleet OEM step", device_index,
                    step_index));
            }
            prepared.report.operation = ReportOperation::Oem;
            prepared.report.oem_command = oem->command.value;
            prepared.tasks.push_back(
                std::make_unique<PreparedPrimitiveCommandTask>(
                    *service_, PreparedPrimitiveCommandTask::Kind::Oem,
                    oem->command.value, fastboot::RebootTarget::System));
        } else {
            return std::unexpected(FleetActorPrepareError{
                .kind = FleetActorPrepareErrorKind::InvalidPlan,
                .message = "fleet target contains an unknown step kind",
                .device_index = device_index,
                .step_index = step_index,
            .device_error = std::nullopt,
            });
        }

        if (prepared.report.bytes_total &&
            !checked_add_i_json(
                device_data_bytes, *prepared.report.bytes_total)) {
            return std::unexpected(FleetActorPrepareError{
                .kind = FleetActorPrepareErrorKind::IntegerOutOfRange,
                .message = "device DATA byte total exceeds the I-JSON integer range",
                .device_index = device_index,
                .step_index = step_index,
            .device_error = std::nullopt,
            });
        }
        report.steps.push_back(prepared.report);
        steps_.push_back(std::move(prepared));
    }
    return report;
}

std::expected<FleetActorDeviceExecution, FleetActorExecutionError>
FleetDeviceActor::execute(
    const std::size_t device_index,
    const fastboot::UpdateOperationContext& context,
    const FleetActorExecutionObserver& observer) {
    std::uint8_t expected = kActorReady;
    if (!state_.compare_exchange_strong(
            expected, kActorExecuting, std::memory_order_acq_rel)) {
        return std::unexpected(execution_error(
            expected == kActorExecuting ? FleetActorExecutionErrorKind::Busy
                                        : FleetActorExecutionErrorKind::AlreadyExecuted,
            expected == kActorExecuting
                ? "fleet device actor is already executing"
                : "fleet device actor has already executed",
            device_index, std::nullopt, 0U, 0U));
    }

    std::size_t completed_steps = 0U;
    std::uint64_t completed_bytes = 0U;
    const auto finish = [this]() noexcept {
        retire();
        state_.store(kActorFinished, std::memory_order_release);
    };
    const auto notify = [&](const FleetActorExecutionEvent& event) {
        if (observer) {
            observer(event);
        }
    };

    try {
        notify({
            .kind = FleetActorExecutionEventKind::DeviceStarted,
            .device_index = device_index,
            .step_index = std::nullopt,
            .completed_steps = 0U,
            .completed_data_bytes = 0U,
        });
    } catch (...) {
        finish();
        return std::unexpected(execution_error(
            FleetActorExecutionErrorKind::ObserverFailed,
            "fleet execution observer threw before the first step",
            device_index, std::nullopt, completed_steps, completed_bytes));
    }

    for (std::size_t step_index = 0U; step_index < steps_.size();
         ++step_index) {
        if (context.cancellation.stop_requested() ||
            (context.deadline &&
             std::chrono::steady_clock::now() >= *context.deadline)) {
            const auto kind = context.cancellation.stop_requested()
                ? FleetActorExecutionErrorKind::Cancelled
                : FleetActorExecutionErrorKind::TimedOut;
            finish();
            return std::unexpected(execution_error(
                kind, "fleet device execution was interrupted before a step",
                device_index, step_index, completed_steps, completed_bytes));
        }

        try {
            notify({
                .kind = FleetActorExecutionEventKind::StepStarted,
                .device_index = device_index,
                .step_index = step_index,
                .completed_steps = completed_steps,
                .completed_data_bytes = completed_bytes,
            });
        } catch (...) {
            finish();
            return std::unexpected(execution_error(
                FleetActorExecutionErrorKind::ObserverFailed,
                "fleet execution observer threw before a step",
                device_index, step_index, completed_steps, completed_bytes));
        }

        auto& step = steps_[step_index];
        std::size_t completed_child_tasks = 0U;
        for (std::size_t child_index = 0U; child_index < step.tasks.size();
             ++child_index) {
            const auto& task = step.tasks[child_index];
            std::expected<void, fastboot::UpdateDeviceError> result;
            try {
                result = task->execute(context);
            } catch (const std::bad_alloc&) {
                finish();
                return std::unexpected(execution_error(
                    FleetActorExecutionErrorKind::UnexpectedFailure,
                    "fleet device task exhausted host memory",
                    device_index, step_index, completed_steps,
                    completed_bytes, std::nullopt, completed_child_tasks,
                    step.tasks.size()));
            } catch (const std::exception& error) {
                auto message = std::string{
                    "fleet device task threw an unexpected exception: "};
                message.append(error.what());
                finish();
                return std::unexpected(execution_error(
                    FleetActorExecutionErrorKind::UnexpectedFailure,
                    std::move(message), device_index, step_index,
                    completed_steps, completed_bytes, std::nullopt,
                    completed_child_tasks, step.tasks.size()));
            } catch (...) {
                finish();
                return std::unexpected(execution_error(
                    FleetActorExecutionErrorKind::UnexpectedFailure,
                    "fleet device task threw an unexpected exception",
                    device_index, step_index, completed_steps,
                    completed_bytes, std::nullopt, completed_child_tasks,
                    step.tasks.size()));
            }
            if (!result) {
                auto error = task_execution_error(
                    std::move(result.error()), device_index, step_index,
                    completed_steps, completed_bytes,
                    completed_child_tasks, step.tasks.size());
                finish();
                return std::unexpected(std::move(error));
            }
            const auto child_bytes = task->host_to_device_data_bytes();
            if (!checked_add_i_json(completed_bytes, child_bytes)) {
                finish();
                return std::unexpected(execution_error(
                    FleetActorExecutionErrorKind::UnexpectedFailure,
                    "prepared child DATA accounting exceeded the I-JSON integer range",
                    device_index, step_index, completed_steps,
                    completed_bytes, std::nullopt, completed_child_tasks,
                    step.tasks.size()));
            }
            ++completed_child_tasks;
        }

        ++completed_steps;
        try {
            notify({
                .kind = FleetActorExecutionEventKind::StepCompleted,
                .device_index = device_index,
                .step_index = step_index,
                .completed_steps = completed_steps,
                .completed_data_bytes = completed_bytes,
            });
        } catch (...) {
            finish();
            return std::unexpected(execution_error(
                FleetActorExecutionErrorKind::ObserverFailed,
                "fleet execution observer threw after a step",
                device_index, step_index, completed_steps, completed_bytes));
        }
    }

    try {
        notify({
            .kind = FleetActorExecutionEventKind::DeviceCompleted,
            .device_index = device_index,
            .step_index = std::nullopt,
            .completed_steps = completed_steps,
            .completed_data_bytes = completed_bytes,
        });
    } catch (...) {
        finish();
        return std::unexpected(execution_error(
            FleetActorExecutionErrorKind::ObserverFailed,
            "fleet execution observer threw after the final step",
            device_index, std::nullopt, completed_steps, completed_bytes));
    }

    finish();
    return FleetActorDeviceExecution{
        .completed_steps = completed_steps,
        .completed_data_bytes = completed_bytes,
    };
}

void FleetDeviceActor::retire() noexcept {
    steps_.clear();
    update_.reset();
    service_.reset();
    session_.reset();
}

PreparedFleetActorBatch::PreparedFleetActorBatch(
    PreparedFleetArtifacts&& artifacts,
    std::vector<std::unique_ptr<FleetDeviceActor>>&& actors,
    std::vector<ReportDeviceSpec>&& report_specs) noexcept
    : artifacts_(std::move(artifacts)),
      actors_(std::move(actors)),
      report_specs_(std::move(report_specs)) {}

std::span<const ReportDeviceSpec>
PreparedFleetActorBatch::report_specs() const noexcept {
    return report_specs_;
}

std::expected<FleetActorDeviceExecution, FleetActorExecutionError>
PreparedFleetActorBatch::execute_device(
    const std::size_t device_index,
    const fastboot::UpdateOperationContext& context,
    const FleetActorExecutionObserver& observer) {
    if (device_index >= actors_.size() || actors_[device_index] == nullptr) {
        return std::unexpected(execution_error(
            FleetActorExecutionErrorKind::InvalidArgument,
            "fleet device index is outside the prepared batch", device_index,
            std::nullopt, 0U, 0U));
    }
    return actors_[device_index]->execute(device_index, context, observer);
}

std::expected<PreparedFleetActorBatch, FleetActorPrepareError>
PreparedFleetActorBatch::prepare(
    const JobPlan& plan,
    PreparedFleetArtifacts&& artifacts,
    PreparedDeviceBatchConsumption&& devices,
    const fastboot::UpdateOperationContext& context) {
    // These checks deliberately precede take_sessions_for_actor(). A digest
    // mismatch therefore cannot unwrap or consume the device capability.
    if (plan.sha256() != artifacts.plan().sha256() ||
        plan.sha256() != devices.plan_sha256()) {
        return std::unexpected(FleetActorPrepareError{
            .kind = FleetActorPrepareErrorKind::PlanDigestMismatch,
            .message = "JobPlan, artifact and device capability digests do not match",
                .device_index = std::nullopt,
        .step_index = std::nullopt,
        .device_error = std::nullopt,
        });
    }
    if (interrupted(context)) {
        return std::unexpected(interruption_prepare_error(
            context, "fleet actor preparation was interrupted before consuming devices"));
    }
    if (devices.devices().empty()) {
        return std::unexpected(FleetActorPrepareError{
            .kind = FleetActorPrepareErrorKind::InvalidArgument,
            .message = "fleet actor preparation requires at least one device",
                .device_index = std::nullopt,
        .step_index = std::nullopt,
        .device_error = std::nullopt,
        });
    }
    for (std::size_t index = 0U; index < devices.devices().size(); ++index) {
        if (devices.devices()[index].target_index() >=
            plan.manifest().targets.size()) {
            return std::unexpected(FleetActorPrepareError{
                .kind = FleetActorPrepareErrorKind::InvalidPlan,
                .message = "prepared device references a missing target",
                .device_index = index,
                .step_index = std::nullopt,
                .device_error = std::nullopt,
            });
        }
    }

    try {
        auto sessions = std::move(devices).take_sessions_for_actor();
        std::vector<std::unique_ptr<FleetDeviceActor>> actors;
        std::vector<ReportDeviceSpec> specs;
        actors.reserve(sessions.size());
        specs.reserve(sessions.size());

        for (std::size_t index = 0U; index < sessions.size(); ++index) {
            if (interrupted(context)) {
                return std::unexpected(interruption_prepare_error(
                    context, "fleet actor preparation was interrupted",
                    index));
            }
            auto& prepared = sessions[index];
            const auto target_index = prepared.target_index();
            auto observed_product = std::string(prepared.observed_product());
            auto serial = prepared.usb_identity().serial;
            auto physical_port_path =
                prepared.usb_identity().physical_port_path;
            auto actor = std::unique_ptr<FleetDeviceActor>(
                new FleetDeviceActor(
                    std::move(prepared),
                    plan.manifest().targets[target_index]));
            auto spec = actor->prepare(artifacts, context, index);
            if (!spec) {
                return std::unexpected(std::move(spec.error()));
            }
            spec->identifier = physical_port_path;
            spec->serial = std::move(serial);
            spec->usb_path = std::move(physical_port_path);
            spec->observed_product = std::move(observed_product);
            actors.push_back(std::move(actor));
            specs.push_back(std::move(*spec));
        }
        return PreparedFleetActorBatch{
            std::move(artifacts), std::move(actors), std::move(specs)};
    } catch (const std::bad_alloc&) {
        return std::unexpected(FleetActorPrepareError{
            .kind = FleetActorPrepareErrorKind::ResourceExhausted,
            .message = "unable to allocate the prepared fleet actor batch",
                .device_index = std::nullopt,
        .step_index = std::nullopt,
        .device_error = std::nullopt,
        });
    } catch (const std::exception& error) {
        return std::unexpected(FleetActorPrepareError{
            .kind = FleetActorPrepareErrorKind::UnexpectedFailure,
            .message = std::string{"unexpected fleet actor preparation failure: "} +
                error.what(),
                .device_index = std::nullopt,
        .step_index = std::nullopt,
        .device_error = std::nullopt,
        });
    } catch (...) {
        return std::unexpected(FleetActorPrepareError{
            .kind = FleetActorPrepareErrorKind::UnexpectedFailure,
            .message = "unexpected fleet actor preparation failure",
                .device_index = std::nullopt,
        .step_index = std::nullopt,
        .device_error = std::nullopt,
        });
    }
}

std::expected<PreparedFleetActorBatch, FleetActorPrepareError>
prepare_fleet_device_actors(
    const JobPlan& plan,
    PreparedFleetArtifacts&& artifacts,
    PreparedDeviceBatchConsumption&& devices,
    const fastboot::UpdateOperationContext& context) {
    return PreparedFleetActorBatch::prepare(
        plan, std::move(artifacts), std::move(devices), context);
}

}  // namespace kairosboot::fleet
