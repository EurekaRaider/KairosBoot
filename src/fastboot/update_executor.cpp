// SPDX-License-Identifier: MIT
#include "update_executor.hpp"

#include <exception>
#include <map>
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
    std::optional<std::size_t> requirement_index;
    std::optional<std::size_t> task_index;
    std::optional<UpdateSourceLocation> location;
    std::string name;
    std::string message;
    std::optional<UpdateDeviceError> device_error;
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
        .validated_requirements = state.report.validated_requirements,
        .completed_tasks = state.report.completed_tasks,
        .trace = std::move(state.report.trace),
    };
}

[[nodiscard]] PendingFailure
cancelled(std::string message,
          const std::optional<std::size_t> requirement_index = std::nullopt,
          const std::optional<std::size_t> task_index = std::nullopt,
          const std::optional<UpdateSourceLocation> location = std::nullopt) {
    return {
        .kind = UpdateExecutionErrorKind::Cancelled,
        .requirement_index = requirement_index,
        .task_index = task_index,
        .location = location,
        .message = std::move(message),
    };
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
                          const std::stop_token cancellation) {
    if (prepared.requires_device_validation != !prepared.plan.requirements.empty()) {
        return std::unexpected(PendingFailure{
            .kind = UpdateExecutionErrorKind::InvalidPreparedPackage,
            .message = "prepared update device-validation flag is inconsistent",
        });
    }
    ArtifactMap artifacts;
    for (const auto& artifact : prepared.artifacts) {
        if (cancellation.stop_requested()) {
            return std::unexpected(
                cancelled("update execution was cancelled while validating artifacts"));
        }
        if (artifact.name.empty() || !artifact.resolved || !artifact.resolved->source ||
            artifact.resolved->logical_name != artifact.name ||
            artifact.artifact.transfer_source() != artifact.resolved->source) {
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
    for (std::size_t index = 0; index < prepared.plan.tasks.size(); ++index) {
        if (cancellation.stop_requested()) {
            return std::unexpected(
                cancelled("update execution was cancelled while validating tasks",
                          std::nullopt, index, prepared.plan.tasks[index].location));
        }
        const auto& task = prepared.plan.tasks[index];
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
        case UpdateTaskKind::UpdateSuper:
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

    for (const auto& [name, unused] : artifacts) {
        (void)unused;
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
              const std::stop_token cancellation) {
    if (cancellation.stop_requested()) {
        return std::unexpected(cancelled("update execution was cancelled before getvar",
                                         requirement_index, std::nullopt, location));
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

    std::expected<std::string, UpdateDeviceError> queried =
        std::unexpected(UpdateDeviceError{});
    try {
        queried = device.getvar(name, cancellation);
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
        const auto kind = queried.error().kind == UpdateDeviceErrorKind::Cancelled
                              ? UpdateExecutionErrorKind::Cancelled
                              : UpdateExecutionErrorKind::GetVarFailed;
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
    if (cancellation.stop_requested()) {
        return std::unexpected(cancelled("update execution was cancelled during getvar",
                                         requirement_index, std::nullopt, location));
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
                      ExecutionState& state, const std::stop_token cancellation) {
    std::map<std::string, std::string, std::less<>> cache;
    std::string current_product;
    if (!prepared.plan.requirements.empty()) {
        const auto& first = prepared.plan.requirements.front();
        auto product = getvar_cached(device, state, cache, "product", std::size_t{0},
                                     first.location, cancellation);
        if (!product) {
            return std::unexpected(std::move(product.error()));
        }
        current_product = std::move(*product);
    }

    for (std::size_t index = 0; index < prepared.plan.requirements.size(); ++index) {
        if (cancellation.stop_requested()) {
            return std::unexpected(cancelled(
                "update execution was cancelled during requirement validation", index,
                std::nullopt, prepared.plan.requirements[index].location));
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
                                       requirement.location, cancellation);
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
                                   requirement.location, cancellation);
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

[[nodiscard]] std::expected<void, UpdateDeviceError>
invoke_task(IUpdateDevice& device, const PlannedUpdateTask& task,
            const PreparedUpdateArtifact* artifact,
            const std::stop_token cancellation) {
    switch (task.kind) {
    case UpdateTaskKind::Flash:
        return device.flash(task, *artifact, cancellation);
    case UpdateTaskKind::Erase:
        return device.erase(task, cancellation);
    case UpdateTaskKind::Reboot:
        return device.reboot(task, cancellation);
    case UpdateTaskKind::UpdateSuper:
        return device.update_super(task, cancellation);
    default:
        return std::unexpected(UpdateDeviceError{
            .message = "unknown prepared update task kind",
        });
    }
}

}  // namespace

std::expected<UpdateExecutionReport, UpdateExecutionError>
execute_prepared_update(const PreparedUpdatePackage& prepared, IUpdateDevice& device,
                        const UpdateExecutorOptions& options,
                        const std::stop_token cancellation) {
    ExecutionState state(options.observer);
    try {
        const auto maximum_events = 4U + (prepared.plan.requirements.size() * 4U) +
                                    (prepared.plan.tasks.size() * 2U);
        state.report.trace.reserve(maximum_events);
        if (cancellation.stop_requested()) {
            return std::unexpected(finish_error(
                state, cancelled("update execution was cancelled before validation")));
        }
        if (auto failure =
                emit_checked(state, event(UpdateExecutionEventKind::ValidationStarted));
            failure) {
            return std::unexpected(finish_error(state, std::move(*failure)));
        }

        auto artifacts = validate_prepared_mapping(prepared, cancellation);
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
                                                  state, cancellation);
        if (!requirements) {
            return std::unexpected(
                finish_error(state, std::move(requirements.error())));
        }
        if (auto failure = emit_checked(
                state, event(UpdateExecutionEventKind::ValidationCompleted));
            failure) {
            return std::unexpected(finish_error(state, std::move(*failure)));
        }

        for (std::size_t index = 0; index < prepared.plan.tasks.size(); ++index) {
            const auto& task = prepared.plan.tasks[index];
            if (cancellation.stop_requested()) {
                return std::unexpected(finish_error(
                    state, cancelled("update execution was cancelled before a task",
                                     std::nullopt, index, task.location)));
            }
            if (auto failure = emit_checked(
                    state, event(UpdateExecutionEventKind::TaskStarted, std::nullopt,
                                 index, task.location, task_event_name(task)));
                failure) {
                return std::unexpected(finish_error(state, std::move(*failure)));
            }

            const PreparedUpdateArtifact* artifact = nullptr;
            if (task.kind == UpdateTaskKind::Flash) {
                artifact = artifacts->at(task.artifact);
            }
            std::expected<void, UpdateDeviceError> invoked =
                std::unexpected(UpdateDeviceError{});
            try {
                invoked = invoke_task(device, task, artifact, cancellation);
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
                const auto kind =
                    invoked.error().kind == UpdateDeviceErrorKind::Cancelled
                        ? UpdateExecutionErrorKind::Cancelled
                        : UpdateExecutionErrorKind::DeviceTaskFailed;
                const auto message =
                    "update device task failed: " + invoked.error().message;
                if (auto observer =
                        emit_checked(state, event(UpdateExecutionEventKind::TaskFailed,
                                                  std::nullopt, index, task.location,
                                                  task_event_name(task), {}, message));
                    observer) {
                    return std::unexpected(finish_error(state, std::move(*observer)));
                }
                return std::unexpected(
                    finish_error(state, PendingFailure{
                                            .kind = kind,
                                            .task_index = index,
                                            .location = task.location,
                                            .message = message,
                                            .device_error = std::move(invoked.error()),
                                        }));
            }

            ++state.report.completed_tasks;
            if (auto failure = emit_checked(
                    state, event(UpdateExecutionEventKind::TaskCompleted, std::nullopt,
                                 index, task.location, task_event_name(task)));
                failure) {
                return std::unexpected(finish_error(state, std::move(*failure)));
            }
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
