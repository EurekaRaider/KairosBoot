// SPDX-License-Identifier: MIT
#include "update_executor.hpp"

#include <exception>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <utility>

namespace kairosboot::fastboot {
namespace {

// Requirement behavior was verified against the frozen Platform-Tools 37.0.1
// source commit a3b721a32242006b59cb12bd62c9133632af3a2d, fastboot.cpp
// CheckRequirement/HandlePartitionExists/CheckRequirements (lines 817-965):
// https://android.googlesource.com/platform/system/core/+/a3b721a32242006b59cb12bd62c9133632af3a2d/fastboot/fastboot.cpp

struct PendingFailure final {
    UpdateExecutionErrorKind kind{UpdateExecutionErrorKind::InvalidPreparedPackage};
    std::optional<std::size_t> requirement_index{};
    std::optional<std::size_t> task_index{};
    std::optional<UpdateSourceLocation> location{};
    std::string name{};
    std::string message{};
    std::optional<UpdateDeviceError> device_error{};
    std::optional<std::string> secondary_observer_error{};
};

class ExecutionState final {
public:
    explicit ExecutionState(const UpdateExecutionObserver& observer)
        : observer_(observer) {}

    [[nodiscard]] std::expected<void, std::string> emit(UpdateExecutionEvent event) {
        report.trace.push_back(std::move(event));
        if (!observer_) {
            return {};
        }
        try {
            observer_(report.trace.back());
            return {};
        } catch (const std::exception& error) {
            return std::unexpected("update execution observer threw an exception: " +
                                   std::string(error.what()));
        } catch (...) {
            return std::unexpected(
                "update execution observer threw a non-standard exception");
        }
    }

    UpdateExecutionReport report;

private:
    const UpdateExecutionObserver& observer_;
};

[[nodiscard]] UpdateExecutionEvent
event(const UpdateExecutionEventKind kind,
      const std::optional<std::size_t> requirement_index = std::nullopt,
      const std::optional<std::size_t> task_index = std::nullopt,
      const std::optional<UpdateSourceLocation> location = std::nullopt,
      std::string name = {}, std::string value = {}, std::string message = {}) {
    return {
        .kind = kind,
        .requirement_index = requirement_index,
        .task_index = task_index,
        .location = location,
        .name = std::move(name),
        .value = std::move(value),
        .message = std::move(message),
    };
}

[[nodiscard]] PendingFailure observer_failure(const UpdateExecutionEvent& emitted,
                                              std::string message) {
    return {
        .kind = UpdateExecutionErrorKind::ObserverFailed,
        .requirement_index = emitted.requirement_index,
        .task_index = emitted.task_index,
        .location = emitted.location,
        .name = emitted.name,
        .message = std::move(message),
    };
}

[[nodiscard]] std::optional<PendingFailure> emit_checked(ExecutionState& state,
                                                         UpdateExecutionEvent emitted) {
    auto result = state.emit(std::move(emitted));
    if (result) {
        return std::nullopt;
    }
    return observer_failure(state.report.trace.back(), std::move(result.error()));
}

[[nodiscard]] UpdateExecutionError finish_error(ExecutionState& state,
                                                PendingFailure failure) {
    return {
        .kind = failure.kind,
        .requirement_index = failure.requirement_index,
        .task_index = failure.task_index,
        .location = failure.location,
        .name = std::move(failure.name),
        .message = std::move(failure.message),
        .device_error = std::move(failure.device_error),
        .secondary_observer_error =
            std::move(failure.secondary_observer_error),
        .validated_requirements = state.report.validated_requirements,
        .completed_tasks = state.report.completed_tasks,
        .trace = std::move(state.report.trace),
    };
}

[[nodiscard]] PendingFailure
timed_out(std::string message,
          const std::optional<std::size_t> requirement_index = std::nullopt,
          const std::optional<std::size_t> task_index = std::nullopt,
          const std::optional<UpdateSourceLocation> location = std::nullopt) {
    return {
        .kind = UpdateExecutionErrorKind::TimedOut,
        .requirement_index = requirement_index,
        .task_index = task_index,
        .location = location,
        .message = std::move(message),
    };
}

[[nodiscard]] std::optional<PendingFailure> interruption(
    const UpdateOperationContext& context, std::string_view phase,
    const std::optional<std::size_t> requirement_index = std::nullopt,
    const std::optional<std::size_t> task_index = std::nullopt,
    const std::optional<UpdateSourceLocation> location = std::nullopt) {
    if (context.cancellation.stop_requested()) {
        return PendingFailure{
            .kind = UpdateExecutionErrorKind::Cancelled,
            .requirement_index = requirement_index,
            .task_index = task_index,
            .location = location,
            .message = "update execution was cancelled " + std::string(phase),
        };
    }
    if (context.deadline &&
        std::chrono::steady_clock::now() >= *context.deadline) {
        return timed_out("update execution deadline expired " + std::string(phase),
                         requirement_index, task_index, location);
    }
    return std::nullopt;
}

[[nodiscard]] UpdateExecutionErrorKind
device_failure_kind(const UpdateDeviceErrorKind kind,
                    const UpdateExecutionErrorKind fallback) noexcept {
    switch (kind) {
    case UpdateDeviceErrorKind::Cancelled:
        return UpdateExecutionErrorKind::Cancelled;
    case UpdateDeviceErrorKind::TimedOut:
        return UpdateExecutionErrorKind::TimedOut;
    case UpdateDeviceErrorKind::Failed:
    case UpdateDeviceErrorKind::Unsupported:
        return fallback;
    default:
        return fallback;
    }
}

[[nodiscard]] std::expected<std::size_t, PendingFailure>
maximum_trace_events(const std::size_t requirements, const std::size_t tasks,
                     const std::size_t maximum) {
    constexpr std::size_t kFixedEvents = 4U;
    // A requirement may add one product query, its own query, and its terminal
    // satisfied/skipped/failed event. Cache hits only reduce this bound.
    constexpr std::size_t kEventsPerRequirement = 5U;
    constexpr std::size_t kEventsPerTask = 2U;
    if (requirements >
        (std::numeric_limits<std::size_t>::max() - kFixedEvents) /
            kEventsPerRequirement) {
        return std::unexpected(PendingFailure{
            .kind = UpdateExecutionErrorKind::InvalidPreparedPackage,
            .message = "prepared update trace size overflows size_t",
        });
    }
    auto total = kFixedEvents + requirements * kEventsPerRequirement;
    if (tasks > (std::numeric_limits<std::size_t>::max() - total) /
                    kEventsPerTask) {
        return std::unexpected(PendingFailure{
            .kind = UpdateExecutionErrorKind::InvalidPreparedPackage,
            .message = "prepared update trace size overflows size_t",
        });
    }
    total += tasks * kEventsPerTask;
    if (total > maximum) {
        return std::unexpected(PendingFailure{
            .kind = UpdateExecutionErrorKind::InvalidPreparedPackage,
            .message = "prepared update trace exceeds vector capacity",
        });
    }
    return total;
}

[[nodiscard]] bool option_matches(const std::string_view option,
                                  const std::string_view value) noexcept {
    if (option == value) {
        return true;
    }
    return !option.empty() && option.back() == '*' &&
           value.starts_with(option.substr(0U, option.size() - 1U));
}

using ArtifactMap = std::map<std::string, const PreparedUpdateArtifact*, std::less<>>;
inline constexpr std::string_view kSuperEmptyName = "super_empty.img";

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

[[nodiscard]] bool flash_mapping_is_consistent(
    const std::shared_ptr<const image::ResolvedArtifact>& resolved,
    const std::shared_ptr<const image::FlashArtifact>& artifact,
    const std::string_view expected_name) noexcept {
    if (!resolved || !resolved->source || !artifact || expected_name.empty() ||
        resolved->logical_name != expected_name || !known_origin(resolved->origin) ||
        artifact->transfer_source() != resolved->source) {
        return false;
    }

    const auto& metadata = artifact->metadata();
    if (metadata.transfer_size != resolved->source->size()) {
        return false;
    }
    switch (metadata.kind) {
    case image::FlashArtifactKind::Raw:
        return artifact->sparse_image() == nullptr &&
               !metadata.sparse_header.has_value() &&
               metadata.expanded_size == metadata.transfer_size;
    case image::FlashArtifactKind::AndroidSparse: {
        const auto* sparse = artifact->sparse_image();
        return sparse != nullptr && metadata.sparse_header.has_value() &&
               metadata.expanded_size == sparse->output_size() &&
               same_sparse_header(*metadata.sparse_header, sparse->header());
    }
    default:
        return false;
    }
}

[[nodiscard]] std::string task_event_name(const PlannedUpdateTask& task) {
    switch (task.kind) {
    case UpdateTaskKind::Flash:
        return "flash:" + task.partition + ":" + task.artifact;
    case UpdateTaskKind::Erase:
        return "erase:" + task.partition;
    case UpdateTaskKind::Reboot:
        return "reboot:" + std::to_string(static_cast<unsigned>(task.reboot_target));
    case UpdateTaskKind::UpdateSuper:
        return "update-super";
    default:
        return "unknown";
    }
}

[[nodiscard]] std::expected<ArtifactMap, PendingFailure>
validate_prepared_mapping(const PreparedUpdatePackage& prepared,
                          const UpdateOperationContext& context) {
    if (prepared.requires_device_validation != !prepared.plan.requirements.empty()) {
        return std::unexpected(PendingFailure{
            .kind = UpdateExecutionErrorKind::InvalidPreparedPackage,
            .message = "prepared update device-validation flag is inconsistent",
        });
    }
    ArtifactMap artifacts;
    for (const auto& artifact : prepared.artifacts) {
        if (auto stopped = interruption(
                context, "while validating prepared artifacts")) {
            return std::unexpected(std::move(*stopped));
        }
        if (!flash_mapping_is_consistent(artifact.resolved, artifact.artifact,
                                         artifact.name)) {
            return std::unexpected(PendingFailure{
                .kind = UpdateExecutionErrorKind::InvalidPreparedPackage,
                .name = artifact.name,
                .message = "prepared update artifact has an inconsistent mapping",
            });
        }
        if (!artifacts.emplace(artifact.name, &artifact).second) {
            return std::unexpected(PendingFailure{
                .kind = UpdateExecutionErrorKind::InvalidPreparedPackage,
                .name = artifact.name,
                .message = "prepared update contains a duplicate artifact name",
            });
        }
    }

    std::set<std::string, std::less<>> referenced;
    bool has_update_super_task = false;
    for (std::size_t index = 0; index < prepared.plan.tasks.size(); ++index) {
        const auto& task = prepared.plan.tasks[index];
        if (auto stopped = interruption(context, "while validating prepared tasks",
                                        std::nullopt, index, task.location)) {
            return std::unexpected(std::move(*stopped));
        }
        switch (task.kind) {
        case UpdateTaskKind::Flash:
            if (task.partition.empty() || task.artifact.empty() ||
                !artifacts.contains(task.artifact)) {
                return std::unexpected(PendingFailure{
                    .kind = UpdateExecutionErrorKind::InvalidPreparedPackage,
                    .task_index = index,
                    .location = task.location,
                    .name = task.artifact,
                    .message = "flash task does not map to one prepared artifact",
                });
            }
            switch (task.slot) {
            case PlannedSlot::Default:
            case PlannedSlot::Other:
                break;
            default:
                return std::unexpected(PendingFailure{
                    .kind = UpdateExecutionErrorKind::InvalidPreparedPackage,
                    .task_index = index,
                    .location = task.location,
                    .name = task.partition,
                    .message = "flash task contains an unknown slot mode",
                });
            }
            referenced.insert(task.artifact);
            break;
        case UpdateTaskKind::Erase:
            if (task.partition.empty()) {
                return std::unexpected(PendingFailure{
                    .kind = UpdateExecutionErrorKind::InvalidPreparedPackage,
                    .task_index = index,
                    .location = task.location,
                    .message = "erase task has an empty partition",
                });
            }
            break;
        case UpdateTaskKind::Reboot:
            switch (task.reboot_target) {
            case PlannedRebootTarget::System:
            case PlannedRebootTarget::Bootloader:
            case PlannedRebootTarget::Recovery:
            case PlannedRebootTarget::Fastboot:
                break;
            default:
                return std::unexpected(PendingFailure{
                    .kind = UpdateExecutionErrorKind::InvalidPreparedPackage,
                    .task_index = index,
                    .location = task.location,
                    .message = "reboot task contains an unknown target",
                });
            }
            break;
        case UpdateTaskKind::UpdateSuper:
            has_update_super_task = true;
            break;
        default:
            return std::unexpected(PendingFailure{
                .kind = UpdateExecutionErrorKind::InvalidPreparedPackage,
                .task_index = index,
                .location = task.location,
                .message = "prepared update contains an unknown task kind",
            });
        }
    }

    const auto super_state_failure = [&]() {
        return PendingFailure{
            .kind = UpdateExecutionErrorKind::InvalidPreparedPackage,
            .name = std::string(kSuperEmptyName),
            .message =
                "prepared update-super state, task and artifact are inconsistent",
        };
    };
    switch (prepared.update_super_state) {
    case UpdateSuperPreparationState::NotRequired:
    case UpdateSuperPreparationState::SkippedNotFound:
        if (has_update_super_task || prepared.prepared_super_artifact) {
            return std::unexpected(super_state_failure());
        }
        break;
    case UpdateSuperPreparationState::Prepared:
        if (!has_update_super_task || !prepared.prepared_super_artifact ||
            !flash_mapping_is_consistent(
                prepared.prepared_super_artifact->resolved(),
                prepared.prepared_super_artifact->artifact(), kSuperEmptyName)) {
            return std::unexpected(super_state_failure());
        }
        if (const auto ordinary = artifacts.find(kSuperEmptyName);
            ordinary != artifacts.end() &&
            (ordinary->second->resolved !=
                 prepared.prepared_super_artifact->resolved() ||
             ordinary->second->artifact !=
                 prepared.prepared_super_artifact->artifact())) {
            return std::unexpected(PendingFailure{
                .kind = UpdateExecutionErrorKind::InvalidPreparedPackage,
                .name = std::string(kSuperEmptyName),
                .message =
                    "flash and update-super tasks do not share one immutable "
                    "preflight artifact",
            });
        }
        break;
    default:
        return std::unexpected(PendingFailure{
            .kind = UpdateExecutionErrorKind::InvalidPreparedPackage,
            .name = std::string(kSuperEmptyName),
            .message = "prepared update contains an unknown update-super state",
        });
    }

    for (const auto& [name, unused] : artifacts) {
        (void)unused;
        if (auto stopped = interruption(
                context, "while validating artifact references")) {
            return std::unexpected(std::move(*stopped));
        }
        if (!referenced.contains(name)) {
            return std::unexpected(PendingFailure{
                .kind = UpdateExecutionErrorKind::InvalidPreparedPackage,
                .name = name,
                .message = "prepared update contains an unreferenced artifact",
            });
        }
    }
    return artifacts;
}

[[nodiscard]] std::expected<std::string, PendingFailure>
getvar_cached(IUpdateDevice& device, ExecutionState& state,
              std::map<std::string, std::string, std::less<>>& cache,
              const std::string_view name,
              const std::optional<std::size_t> requirement_index,
              const std::optional<UpdateSourceLocation> location,
              const UpdateOperationContext& context) {
    if (auto stopped = interruption(context, "before getvar", requirement_index,
                                    std::nullopt, location)) {
        return std::unexpected(std::move(*stopped));
    }
    if (const auto found = cache.find(name); found != cache.end()) {
        if (auto failure =
                emit_checked(state, event(UpdateExecutionEventKind::GetVarCacheHit,
                                          requirement_index, std::nullopt, location,
                                          std::string(name), found->second));
            failure) {
            return std::unexpected(std::move(*failure));
        }
        return found->second;
    }

    if (auto failure = emit_checked(state, event(UpdateExecutionEventKind::GetVarQuery,
                                                 requirement_index, std::nullopt,
                                                 location, std::string(name)));
        failure) {
        return std::unexpected(std::move(*failure));
    }
    if (auto stopped = interruption(context, "after GetVarQuery",
                                    requirement_index, std::nullopt, location)) {
        return std::unexpected(std::move(*stopped));
    }

    std::expected<std::string, UpdateDeviceError> queried =
        std::unexpected(UpdateDeviceError{});
    try {
        queried = device.getvar(name, context);
    } catch (const std::exception& error) {
        return std::unexpected(PendingFailure{
            .kind = UpdateExecutionErrorKind::ActorException,
            .requirement_index = requirement_index,
            .location = location,
            .name = std::string(name),
            .message =
                "update device getvar threw an exception: " + std::string(error.what()),
        });
    } catch (...) {
        return std::unexpected(PendingFailure{
            .kind = UpdateExecutionErrorKind::ActorException,
            .requirement_index = requirement_index,
            .location = location,
            .name = std::string(name),
            .message = "update device getvar threw a non-standard exception",
        });
    }
    if (!queried) {
        const auto kind = device_failure_kind(queried.error().kind,
                                              UpdateExecutionErrorKind::GetVarFailed);
        return std::unexpected(PendingFailure{
            .kind = kind,
            .requirement_index = requirement_index,
            .location = location,
            .name = std::string(name),
            .message = "unable to query required device variable " + std::string(name) +
                       ": " + queried.error().message,
            .device_error = std::move(queried.error()),
        });
    }
    if (auto stopped = interruption(context, "during getvar", requirement_index,
                                    std::nullopt, location)) {
        return std::unexpected(std::move(*stopped));
    }

    auto [stored, inserted] = cache.emplace(std::string(name), std::move(*queried));
    (void)inserted;
    if (auto failure = emit_checked(
            state, event(UpdateExecutionEventKind::GetVarResult, requirement_index,
                         std::nullopt, location, stored->first, stored->second));
        failure) {
        return std::unexpected(std::move(*failure));
    }
    return stored->second;
}

[[nodiscard]] std::optional<PendingFailure> emit_requirement_failure(
    ExecutionState& state, const std::size_t index,
    const PlannedRequirement& requirement, std::string value, std::string message,
    const UpdateExecutionErrorKind kind = UpdateExecutionErrorKind::RequirementNotMet) {
    if (auto observer =
            emit_checked(state, event(UpdateExecutionEventKind::RequirementFailed,
                                      index, std::nullopt, requirement.location,
                                      requirement.variable, std::move(value), message));
        observer) {
        return observer;
    }
    return PendingFailure{
        .kind = kind,
        .requirement_index = index,
        .location = requirement.location,
        .name = requirement.variable,
        .message = std::move(message),
    };
}

[[nodiscard]] std::expected<void, PendingFailure>
validate_requirements(const PreparedUpdatePackage& prepared, IUpdateDevice& device,
                      const std::set<std::string, std::less<>>& known_partitions,
                      ExecutionState& state,
                      const UpdateOperationContext& context) {
    std::map<std::string, std::string, std::less<>> cache;
    std::string current_product;
    if (!prepared.plan.requirements.empty()) {
        const auto& first = prepared.plan.requirements.front();
        auto product = getvar_cached(device, state, cache, "product", std::size_t{0},
                                     first.location, context);
        if (!product) {
            return std::unexpected(std::move(product.error()));
        }
        current_product = std::move(*product);
    }

    for (std::size_t index = 0; index < prepared.plan.requirements.size(); ++index) {
        if (auto stopped = interruption(
                context, "during requirement validation", index, std::nullopt,
                prepared.plan.requirements[index].location)) {
            return std::unexpected(std::move(*stopped));
        }
        const auto& requirement = prepared.plan.requirements[index];
        if (requirement.variable.empty() || requirement.options.empty() ||
            (requirement.product && requirement.product->empty())) {
            return std::unexpected(PendingFailure{
                .kind = UpdateExecutionErrorKind::InvalidPreparedPackage,
                .requirement_index = index,
                .location = requirement.location,
                .name = requirement.variable,
                .message = "prepared update contains an invalid requirement",
            });
        }

        if (requirement.variable == "partition-exists") {
            const auto& partition = requirement.options.front();
            if (partition.empty()) {
                auto failure = emit_requirement_failure(
                    state, index, requirement, partition,
                    "required partition name is empty",
                    UpdateExecutionErrorKind::InvalidPreparedPackage);
                return std::unexpected(std::move(*failure));
            }
            const auto variable = "has-slot:" + partition;
            auto value = getvar_cached(device, state, cache, variable, index,
                                       requirement.location, context);
            if (!value) {
                return std::unexpected(std::move(value.error()));
            }
            if (*value != "yes" && *value != "no") {
                auto failure = emit_requirement_failure(
                    state, index, requirement, *value,
                    "device does not report the required partition");
                return std::unexpected(std::move(*failure));
            }
            if (!known_partitions.contains(partition)) {
                auto failure = emit_requirement_failure(
                    state, index, requirement, partition,
                    "required partition is not in the frozen host image table",
                    UpdateExecutionErrorKind::InvalidPreparedPackage);
                return std::unexpected(std::move(*failure));
            }
            ++state.report.validated_requirements;
            if (auto failure = emit_checked(
                    state, event(UpdateExecutionEventKind::RequirementSatisfied, index,
                                 std::nullopt, requirement.location,
                                 requirement.variable, *value));
                failure) {
                return std::unexpected(std::move(*failure));
            }
            continue;
        }

        if (requirement.product && *requirement.product != current_product) {
            ++state.report.validated_requirements;
            if (auto failure = emit_checked(
                    state, event(UpdateExecutionEventKind::RequirementSkipped, index,
                                 std::nullopt, requirement.location,
                                 requirement.variable, current_product,
                                 "requirement applies to a different product"));
                failure) {
                return std::unexpected(std::move(*failure));
            }
            continue;
        }

        auto value = getvar_cached(device, state, cache, requirement.variable, index,
                                   requirement.location, context);
        if (!value) {
            return std::unexpected(std::move(value.error()));
        }
        bool matched = false;
        for (const auto& option : requirement.options) {
            if (option_matches(option, *value)) {
                matched = true;
                break;
            }
        }
        bool satisfied = false;
        switch (requirement.action) {
        case RequirementAction::Require:
            satisfied = matched;
            break;
        case RequirementAction::Reject:
            satisfied = !matched;
            break;
        default:
            return std::unexpected(PendingFailure{
                .kind = UpdateExecutionErrorKind::InvalidPreparedPackage,
                .requirement_index = index,
                .location = requirement.location,
                .name = requirement.variable,
                .message = "prepared update contains an unknown requirement action",
            });
        }
        if (!satisfied) {
            auto failure = emit_requirement_failure(
                state, index, requirement, *value,
                requirement.action == RequirementAction::Reject
                    ? "device value is rejected by the update package"
                    : "device value does not satisfy the update package");
            return std::unexpected(std::move(*failure));
        }

        ++state.report.validated_requirements;
        if (auto failure = emit_checked(
                state, event(UpdateExecutionEventKind::RequirementSatisfied, index,
                             std::nullopt, requirement.location, requirement.variable,
                             *value));
            failure) {
            return std::unexpected(std::move(*failure));
        }
    }
    return {};
}

[[nodiscard]] UpdateDeviceTaskInput task_input(
    const PlannedUpdateTask& task, const ArtifactMap& artifacts,
    const std::shared_ptr<const PreparedSuperArtifact>& super_artifact) {
    UpdateDeviceTaskInput input{
        .task = task,
    };
    if (task.kind == UpdateTaskKind::Flash) {
        const auto* artifact = artifacts.at(task.artifact);
        input.flash_artifact = UpdateFlashArtifactInput{
            .resolved = artifact->resolved,
            .artifact = artifact->artifact,
        };
    } else if (task.kind == UpdateTaskKind::UpdateSuper && super_artifact) {
        input.super_artifact = super_artifact;
    }
    return input;
}

using PreparedDeviceTasks = std::vector<std::unique_ptr<IPreparedDeviceTask>>;

[[nodiscard]] std::expected<PreparedDeviceTasks, PendingFailure>
prepare_all_tasks(const PreparedUpdatePackage& prepared, IUpdateDevice& device,
                  const ArtifactMap& artifacts,
                  const std::shared_ptr<const PreparedSuperArtifact>& super_artifact,
                  const UpdateOperationContext& context) {
    PreparedDeviceTasks result;
    result.reserve(prepared.plan.tasks.size());
    for (std::size_t index = 0; index < prepared.plan.tasks.size(); ++index) {
        const auto& task = prepared.plan.tasks[index];
        if (auto stopped = interruption(context, "before task preparation",
                                        std::nullopt, index, task.location)) {
            return std::unexpected(std::move(*stopped));
        }

        std::expected<std::unique_ptr<IPreparedDeviceTask>, UpdateDeviceError>
            prepared_task = std::unexpected(UpdateDeviceError{});
        try {
            prepared_task = device.prepare_task(
                task_input(task, artifacts, super_artifact), context);
        } catch (const std::exception& error) {
            return std::unexpected(PendingFailure{
                .kind = UpdateExecutionErrorKind::ActorException,
                .task_index = index,
                .location = task.location,
                .message = "update device task preparation threw an exception: " +
                           std::string(error.what()),
            });
        } catch (...) {
            return std::unexpected(PendingFailure{
                .kind = UpdateExecutionErrorKind::ActorException,
                .task_index = index,
                .location = task.location,
                .message =
                    "update device task preparation threw a non-standard exception",
            });
        }
        if (!prepared_task) {
            const auto kind = device_failure_kind(
                prepared_task.error().kind,
                UpdateExecutionErrorKind::DeviceTaskFailed);
            auto message = "unable to prepare update device task: " +
                           prepared_task.error().message;
            return std::unexpected(PendingFailure{
                .kind = kind,
                .task_index = index,
                .location = task.location,
                .message = std::move(message),
                .device_error = std::move(prepared_task.error()),
            });
        }
        if (!*prepared_task) {
            return std::unexpected(PendingFailure{
                .kind = UpdateExecutionErrorKind::ActorException,
                .task_index = index,
                .location = task.location,
                .message = "update device returned an empty prepared task token",
            });
        }
        if (auto stopped = interruption(context, "during task preparation",
                                        std::nullopt, index, task.location)) {
            return std::unexpected(std::move(*stopped));
        }
        result.push_back(std::move(*prepared_task));
    }
    return result;
}

}  // namespace

std::expected<UpdateExecutionReport, UpdateExecutionError>
execute_prepared_update(const PreparedUpdatePackage& prepared, IUpdateDevice& device,
                        const UpdateExecutorOptions& options,
                        const std::stop_token cancellation) {
    ExecutionState state(options.observer);
    const UpdateOperationContext context{
        .cancellation = cancellation,
        .deadline = options.deadline,
    };
    try {
        auto maximum_events = maximum_trace_events(
            prepared.plan.requirements.size(), prepared.plan.tasks.size(),
            state.report.trace.max_size());
        if (!maximum_events) {
            return std::unexpected(
                finish_error(state, std::move(maximum_events.error())));
        }
        state.report.trace.reserve(*maximum_events);
        if (auto stopped = interruption(context, "before validation")) {
            return std::unexpected(finish_error(state, std::move(*stopped)));
        }
        if (auto failure =
                emit_checked(state, event(UpdateExecutionEventKind::ValidationStarted));
            failure) {
            return std::unexpected(finish_error(state, std::move(*failure)));
        }

        auto artifacts = validate_prepared_mapping(prepared, context);
        if (!artifacts) {
            return std::unexpected(finish_error(state, std::move(artifacts.error())));
        }
        if (auto failure = emit_checked(
                state, event(UpdateExecutionEventKind::PreparedPackageValidated));
            failure) {
            return std::unexpected(finish_error(state, std::move(*failure)));
        }

        std::set<std::string, std::less<>> known_partitions;
        for (const auto& partition : options.known_partitions) {
            if (auto stopped = interruption(
                    context, "while validating the host partition table")) {
                return std::unexpected(finish_error(state, std::move(*stopped)));
            }
            if (partition.empty() || !known_partitions.insert(partition).second) {
                return std::unexpected(finish_error(
                    state,
                    PendingFailure{
                        .kind = UpdateExecutionErrorKind::InvalidPreparedPackage,
                        .name = partition,
                        .message =
                            "executor known partition table is invalid or duplicated",
                    }));
            }
        }

        auto requirements = validate_requirements(prepared, device, known_partitions,
                                                  state, context);
        if (!requirements) {
            return std::unexpected(
                finish_error(state, std::move(requirements.error())));
        }

        // Phase one: bind and validate every exact device task. No token may
        // execute until every later task has also prepared successfully.
        auto prepared_tasks = prepare_all_tasks(prepared, device, *artifacts,
                                                prepared.prepared_super_artifact,
                                                context);
        if (!prepared_tasks) {
            return std::unexpected(
                finish_error(state, std::move(prepared_tasks.error())));
        }
        if (auto failure = emit_checked(
                state, event(UpdateExecutionEventKind::ValidationCompleted));
            failure) {
            return std::unexpected(finish_error(state, std::move(*failure)));
        }

        for (std::size_t index = 0; index < prepared.plan.tasks.size(); ++index) {
            const auto& task = prepared.plan.tasks[index];
            if (auto stopped = interruption(context, "before a task",
                                            std::nullopt, index, task.location)) {
                return std::unexpected(finish_error(state, std::move(*stopped)));
            }
            if (auto failure = emit_checked(
                    state, event(UpdateExecutionEventKind::TaskStarted, std::nullopt,
                                 index, task.location, task_event_name(task)));
                failure) {
                return std::unexpected(finish_error(state, std::move(*failure)));
            }
            // Observers run arbitrary user code. Cancellation or expiry raised
            // while TaskStarted was observed must stop before the token can
            // issue a destructive command.
            if (auto stopped = interruption(context, "after TaskStarted",
                                            std::nullopt, index, task.location)) {
                return std::unexpected(finish_error(state, std::move(*stopped)));
            }

            std::expected<void, UpdateDeviceError> invoked =
                std::unexpected(UpdateDeviceError{});
            try {
                invoked = (*prepared_tasks)[index]->execute(context);
            } catch (const std::exception& error) {
                return std::unexpected(finish_error(
                    state, PendingFailure{
                               .kind = UpdateExecutionErrorKind::ActorException,
                               .task_index = index,
                               .location = task.location,
                               .message = "update device task threw an exception: " +
                                          std::string(error.what()),
                           }));
            } catch (...) {
                return std::unexpected(finish_error(
                    state,
                    PendingFailure{
                        .kind = UpdateExecutionErrorKind::ActorException,
                        .task_index = index,
                        .location = task.location,
                        .message = "update device task threw a non-standard exception",
                    }));
            }
            if (!invoked) {
                auto device_error = std::move(invoked.error());
                const auto kind = device_failure_kind(
                    device_error.kind, UpdateExecutionErrorKind::DeviceTaskFailed);
                const auto message =
                    "update device task failed: " + device_error.message;
                PendingFailure primary{
                    .kind = kind,
                    .task_index = index,
                    .location = task.location,
                    .message = message,
                    .device_error = std::move(device_error),
                };
                if (auto observer =
                        emit_checked(state, event(UpdateExecutionEventKind::TaskFailed,
                                                  std::nullopt, index, task.location,
                                                  task_event_name(task), {}, message));
                    observer) {
                    primary.secondary_observer_error = std::move(observer->message);
                }
                return std::unexpected(finish_error(state, std::move(primary)));
            }

            ++state.report.completed_tasks;
            if (auto stopped = interruption(context, "during a task", std::nullopt,
                                            index, task.location)) {
                return std::unexpected(finish_error(state, std::move(*stopped)));
            }
            if (auto failure = emit_checked(
                    state, event(UpdateExecutionEventKind::TaskCompleted, std::nullopt,
                                 index, task.location, task_event_name(task)));
                failure) {
                return std::unexpected(finish_error(state, std::move(*failure)));
            }
        }

        if (auto stopped = interruption(context, "before completion")) {
            return std::unexpected(finish_error(state, std::move(*stopped)));
        }
        if (auto failure = emit_checked(
                state, event(UpdateExecutionEventKind::ExecutionCompleted));
            failure) {
            return std::unexpected(finish_error(state, std::move(*failure)));
        }
        return std::move(state.report);
    } catch (const std::exception& error) {
        return std::unexpected(finish_error(
            state, PendingFailure{
                       .kind = UpdateExecutionErrorKind::ActorException,
                       .message = "update executor failed with an exception: " +
                                  std::string(error.what()),
                   }));
    } catch (...) {
        return std::unexpected(finish_error(
            state,
            PendingFailure{
                .kind = UpdateExecutionErrorKind::ActorException,
                .message = "update executor failed with a non-standard exception",
            }));
    }
}

}  // namespace kairosboot::fastboot
