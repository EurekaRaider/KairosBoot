// SPDX-License-Identifier: MIT
#include "update_operation.hpp"

#include <exception>
#include <new>
#include <utility>

namespace kairosboot::fastboot {
namespace {

using Clock = std::chrono::steady_clock;

[[nodiscard]] UpdatePackageOperationError operation_error(
    const UpdatePackageOperationErrorKind kind,
    const UpdatePackageOperationStage stage,
    std::string message,
    const Clock::time_point deadline = {}) {
    return {
        .kind = kind,
        .stage = stage,
        .message = std::move(message),
        .deadline = deadline,
    };
}

[[nodiscard]] std::expected<Clock::time_point, UpdatePackageOperationError>
operation_deadline(
    const Clock::time_point started,
    const std::optional<Clock::duration> timeout) {
    if (!timeout) {
        return Clock::time_point::max();
    }
    if (*timeout < Clock::duration::zero()) {
        return std::unexpected(operation_error(
            UpdatePackageOperationErrorKind::InvalidTimeout,
            UpdatePackageOperationStage::Deadline,
            "update operation timeout must not be negative"));
    }
    const auto started_count = started.time_since_epoch().count();
    const auto timeout_count = timeout->count();
    const auto maximum_count = Clock::duration::max().count();
    if (started_count > maximum_count - timeout_count) {
        return std::unexpected(operation_error(
            UpdatePackageOperationErrorKind::InvalidTimeout,
            UpdatePackageOperationStage::Deadline,
            "update operation deadline overflows steady_clock"));
    }
    return Clock::time_point(Clock::duration(started_count + timeout_count));
}

[[nodiscard]] std::optional<UpdatePackageOperationError> interruption(
    const std::stop_token cancellation,
    const Clock::time_point deadline,
    const UpdatePackageOperationStage stage,
    const std::size_t completed_tasks = 0U,
    const std::size_t total_tasks = 0U) {
    if (cancellation.stop_requested()) {
        auto error = operation_error(
            UpdatePackageOperationErrorKind::Cancelled, stage,
            "update package operation was cancelled", deadline);
        error.completed_tasks = completed_tasks;
        error.total_tasks = total_tasks;
        return error;
    }
    if (Clock::now() >= deadline) {
        auto error = operation_error(
            UpdatePackageOperationErrorKind::TimedOut, stage,
            "update package operation deadline expired", deadline);
        error.completed_tasks = completed_tasks;
        error.total_tasks = total_tasks;
        return error;
    }
    return std::nullopt;
}

[[nodiscard]] bool preflight_was_cancelled(
    const UpdatePackagePreflightError& error) noexcept {
    return error.kind == UpdatePackagePreflightErrorKind::Cancelled ||
           (error.artifact_error &&
            error.artifact_error->kind ==
                image::ArtifactSourceErrorKind::Cancelled);
}

[[nodiscard]] bool preflight_timed_out(
    const UpdatePackagePreflightError& error) noexcept {
    return error.artifact_error &&
           error.artifact_error->kind == image::ArtifactSourceErrorKind::TimedOut;
}

[[nodiscard]] UpdatePackageOperationError preflight_failure(
    UpdatePackagePreflightError error,
    const Clock::time_point deadline) {
    auto kind = UpdatePackageOperationErrorKind::PreflightFailed;
    if (preflight_was_cancelled(error)) {
        kind = UpdatePackageOperationErrorKind::Cancelled;
    } else if (preflight_timed_out(error)) {
        kind = UpdatePackageOperationErrorKind::TimedOut;
    }
    auto result = operation_error(kind,
                                  UpdatePackageOperationStage::PackagePreflight,
                                  error.message, deadline);
    result.preflight_error = std::move(error);
    return result;
}

[[nodiscard]] bool trace_contains(
    const UpdateExecutionError& error,
    const UpdateExecutionEventKind kind) noexcept {
    for (const auto& item : error.trace) {
        if (item.kind == kind) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] UpdatePackageOperationStage execution_stage(
    const UpdateExecutionError& error,
    const std::size_t total_tasks) noexcept {
    if ((total_tasks == 0U &&
         trace_contains(error, UpdateExecutionEventKind::ValidationCompleted)) ||
        (total_tasks != 0U && error.completed_tasks == total_tasks)) {
        return UpdatePackageOperationStage::Completion;
    }
    if (trace_contains(error, UpdateExecutionEventKind::TaskStarted)) {
        return UpdatePackageOperationStage::TaskExecution;
    }
    if (error.task_index) {
        return UpdatePackageOperationStage::TaskPreparation;
    }
    if (trace_contains(error, UpdateExecutionEventKind::ValidationCompleted)) {
        return UpdatePackageOperationStage::TaskPreparation;
    }
    return UpdatePackageOperationStage::DeviceValidation;
}

[[nodiscard]] UpdatePackageOperationError execution_failure(
    UpdateExecutionError error,
    const Clock::time_point deadline,
    const std::size_t total_tasks) {
    auto kind = UpdatePackageOperationErrorKind::ExecutionFailed;
    if (error.kind == UpdateExecutionErrorKind::Cancelled) {
        kind = UpdatePackageOperationErrorKind::Cancelled;
    } else if (error.kind == UpdateExecutionErrorKind::TimedOut) {
        kind = UpdatePackageOperationErrorKind::TimedOut;
    }

    auto result = operation_error(
        kind, execution_stage(error, total_tasks), error.message, deadline);
    result.completed_tasks = error.completed_tasks;
    result.total_tasks = total_tasks;
    if (error.device_error) {
        result.completed_actions = error.device_error->completed_actions;
        result.total_actions = error.device_error->total_actions;
        result.task_certainty = error.device_error->task_certainty;
    }
    result.execution_error = std::move(error);
    return result;
}

}  // namespace

std::expected<UpdatePackageOperationReport, UpdatePackageOperationError>
run_update_package_operation(
    image::ArtifactSourceResolver& resolver,
    const std::filesystem::path& package_directory_or_zip,
    IUpdateDevice& device,
    const UpdatePackageOperationOptions& options,
    const std::stop_token cancellation) {
    const auto started = Clock::now();
    auto computed_deadline = operation_deadline(started, options.timeout);
    if (!computed_deadline) {
        return std::unexpected(std::move(computed_deadline.error()));
    }
    const auto deadline = *computed_deadline;

    if (auto stopped = interruption(
            cancellation, deadline,
            UpdatePackageOperationStage::PackagePreflight)) {
        return std::unexpected(std::move(*stopped));
    }

    auto active_stage = UpdatePackageOperationStage::PackagePreflight;
    try {
        auto prepared = preflight_update_package(
            resolver, package_directory_or_zip, options.wants_wipe,
            options.preflight_limits, deadline, cancellation);
        if (!prepared) {
            return std::unexpected(
                preflight_failure(std::move(prepared.error()), deadline));
        }

        const auto total_tasks = prepared->plan.tasks.size();
        if (auto stopped = interruption(
                cancellation, deadline,
                UpdatePackageOperationStage::PackagePreflight, 0U,
                total_tasks)) {
            return std::unexpected(std::move(*stopped));
        }

        active_stage = UpdatePackageOperationStage::DeviceValidation;
        const UpdateExecutorOptions executor_options{
            .known_partitions = options.known_partitions,
            .deadline = deadline,
            .observer = options.observer,
        };
        auto executed = execute_prepared_update(
            *prepared, device, executor_options, cancellation);
        if (!executed) {
            return std::unexpected(execution_failure(
                std::move(executed.error()), deadline, total_tasks));
        }

        active_stage = UpdatePackageOperationStage::Completion;
        if (auto stopped = interruption(
                cancellation, deadline,
                UpdatePackageOperationStage::Completion,
                executed->completed_tasks, total_tasks)) {
            return std::unexpected(std::move(*stopped));
        }
        return UpdatePackageOperationReport{
            .deadline = deadline,
            .execution = std::move(*executed),
        };
    } catch (const std::bad_alloc&) {
        return std::unexpected(operation_error(
            UpdatePackageOperationErrorKind::InternalFailure,
            active_stage,
            "memory allocation failed during update package operation",
            deadline));
    } catch (const std::exception& error) {
        return std::unexpected(operation_error(
            UpdatePackageOperationErrorKind::InternalFailure,
            active_stage,
            "update package operation failed with an exception: " +
                std::string(error.what()),
            deadline));
    } catch (...) {
        return std::unexpected(operation_error(
            UpdatePackageOperationErrorKind::InternalFailure,
            active_stage,
            "update package operation failed with a non-standard exception",
            deadline));
    }
}

std::expected<UpdatePackageOperationReport, UpdatePackageOperationError>
run_update_package_operation(
    image::ArtifactSourceResolver& resolver,
    const std::filesystem::path& package_directory_or_zip,
    PrimitiveService& service,
    const UpdatePackageOperationOptions& options,
    PrimitiveUpdateDeviceOptions device_options,
    const std::stop_token cancellation) {
    try {
        PrimitiveUpdateDevice device(service, std::move(device_options));
        return run_update_package_operation(
            resolver, package_directory_or_zip, device, options,
            cancellation);
    } catch (const std::bad_alloc&) {
        return std::unexpected(operation_error(
            UpdatePackageOperationErrorKind::InternalFailure,
            UpdatePackageOperationStage::PackagePreflight,
            "memory allocation failed while creating the update actor"));
    } catch (const std::exception& error) {
        return std::unexpected(operation_error(
            UpdatePackageOperationErrorKind::InternalFailure,
            UpdatePackageOperationStage::PackagePreflight,
            "update actor construction failed: " +
                std::string(error.what())));
    } catch (...) {
        return std::unexpected(operation_error(
            UpdatePackageOperationErrorKind::InternalFailure,
            UpdatePackageOperationStage::PackagePreflight,
            "update actor construction failed with a non-standard "
            "exception"));
    }
}

}  // namespace kairosboot::fastboot
