// SPDX-License-Identifier: MIT

#include "src/api/fleet_run.hpp"

#include "src/api/error_handle.hpp"
#include "src/api/error_mapping.hpp"
#include "src/api/operation_state.hpp"
#include "src/fleet/artifact_preflight.hpp"
#include "src/fleet/manifest.hpp"
#include "src/kairosboot_internal.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace {

using kairosboot::api::OperationErrorPayload;
using kairosboot::api::OperationOutcome;
using kairosboot::fleet::FleetCoordinatorDeviceState;
using kairosboot::fleet::FleetCoordinatorResult;
using kairosboot::fleet::FleetCoordinatorState;
using kairosboot::fleet::JobReport;
using kairosboot::fleet::JobReportBuilder;
using kairosboot::fleet::ReportError;
using kairosboot::fleet::ReportSkipReason;

struct JobSharedState final {
    mutable std::mutex mutex;
    std::shared_ptr<const std::string> report_json;
    std::stop_source cancellation;
};

std::mutex g_active_contexts_mutex;
std::unordered_set<const kb_context_t*> g_active_contexts;

}  // namespace

struct kb_job_report {
    explicit kb_job_report(std::shared_ptr<const std::string> value)
        : json(std::move(value)) {}

    std::shared_ptr<const std::string> json;
};

struct kb_job {
    kb_job(std::unique_ptr<kairosboot::api::OperationState> operation_state,
           std::shared_ptr<JobSharedState> result_state,
           const kb_context_t* owner_context)
        : state(std::move(operation_state)),
          result(std::move(result_state)),
          context(owner_context) {}

    ~kb_job() {
        if (result != nullptr) {
            static_cast<void>(result->cancellation.request_stop());
        }
        state.reset();
        std::scoped_lock lock(g_active_contexts_mutex);
        g_active_contexts.erase(context);
    }

    std::unique_ptr<kairosboot::api::OperationState> state;
    std::shared_ptr<JobSharedState> result;
    const kb_context_t* context{};
    mutable std::mutex error_mutex;
    mutable std::unique_ptr<kb_error> public_error;
};

namespace {

std::mutex g_prepare_factory_mutex;
kairosboot::api::FleetRunPrepareFactory g_prepare_factory;
kairosboot::api::FleetRunDeviceDependenciesFactory
    g_device_dependencies_factory;

void clear_error(kb_error_t** error) noexcept {
    if (error != nullptr) {
        *error = nullptr;
    }
}

kb_status_t fail(kb_error_t** error,
                 const kb_status_t status,
                 std::string message) noexcept {
    if (error != nullptr) {
        try {
            *error = new kb_error{
                .status = status,
                .message = std::move(message),
                .device_identifier = {},
                .native_code = 0,
                .transfer_state = KB_TRANSFER_NOT_SENT,
                .device_message = {},
                .command_messages = {},
                .inbound_expected = std::nullopt,
                .inbound_transferred = 0,
                .inbound_transfer_state = KB_TRANSFER_NOT_SENT,
                .session_poisoned = false,
            };
        } catch (...) {
            *error = nullptr;
        }
    }
    return status;
}

kb_status_t fail(kb_error_t** error,
                 const OperationErrorPayload& payload) noexcept {
    if (error != nullptr) {
        try {
            *error = new kb_error{
                .status = payload.status,
                .message = payload.message,
                .device_identifier = payload.device_identifier,
                .native_code = payload.native_code,
                .transfer_state = payload.transfer_state,
                .device_message = payload.device_message,
                .command_messages = payload.command_messages,
                .inbound_expected = payload.inbound_expected,
                .inbound_transferred = payload.inbound_transferred,
                .inbound_transfer_state = payload.inbound_transfer_state,
                .session_poisoned = payload.session_poisoned,
            };
        } catch (...) {
            *error = nullptr;
        }
    }
    return payload.status;
}

[[nodiscard]] OperationErrorPayload operation_error(
    const kb_status_t status,
    std::string message) {
    return {
        .status = status,
        .message = std::move(message),
        .native_code = 0,
        .transfer_state = KB_TRANSFER_NOT_SENT,
        .device_identifier = {},
        .device_message = {},
        .command_messages = {},
        .inbound_expected = std::nullopt,
        .inbound_transferred = 0,
        .inbound_transfer_state = KB_TRANSFER_NOT_SENT,
        .session_poisoned = false,
    };
}

[[nodiscard]] std::string utc_timestamp() {
    const std::time_t value = std::time(nullptr);
    std::tm parts{};
#if defined(_WIN32)
    if (gmtime_s(&parts, &value) != 0) {
        return "1970-01-01T00:00:00Z";
    }
#else
    if (gmtime_r(&value, &parts) == nullptr) {
        return "1970-01-01T00:00:00Z";
    }
#endif
    char buffer[32]{};
    const int written = std::snprintf(
        buffer,
        sizeof(buffer),
        "%04d-%02d-%02dT%02d:%02d:%02dZ",
        parts.tm_year + 1900,
        parts.tm_mon + 1,
        parts.tm_mday,
        parts.tm_hour,
        parts.tm_min,
        parts.tm_sec);
    return written > 0 ? std::string{buffer, static_cast<std::size_t>(written)}
                       : std::string{"1970-01-01T00:00:00Z"};
}

[[nodiscard]] bool valid_job_options(
    const kb_job_options_t* options) noexcept {
    return options == nullptr ||
           (options->struct_size >= KB_JOB_OPTIONS_V1_SIZE &&
            options->api_version == KB_API_VERSION);
}

[[nodiscard]] bool valid_utf8(const std::string_view value) noexcept {
    const auto* bytes =
        reinterpret_cast<const unsigned char*>(value.data());
    std::size_t index = 0;
    while (index < value.size()) {
        const unsigned char lead = bytes[index];
        if (lead <= 0x7FU) {
            ++index;
            continue;
        }
        std::size_t continuations = 0;
        unsigned char first_minimum = 0x80U;
        unsigned char first_maximum = 0xBFU;
        if (lead >= 0xC2U && lead <= 0xDFU) {
            continuations = 1;
        } else if (lead >= 0xE0U && lead <= 0xEFU) {
            continuations = 2;
            if (lead == 0xE0U) {
                first_minimum = 0xA0U;
            } else if (lead == 0xEDU) {
                first_maximum = 0x9FU;
            }
        } else if (lead >= 0xF0U && lead <= 0xF4U) {
            continuations = 3;
            if (lead == 0xF0U) {
                first_minimum = 0x90U;
            } else if (lead == 0xF4U) {
                first_maximum = 0x8FU;
            }
        } else {
            return false;
        }
        if (continuations > value.size() - index - 1) {
            return false;
        }
        const unsigned char first = bytes[index + 1];
        if (first < first_minimum || first > first_maximum) {
            return false;
        }
        for (std::size_t offset = 2; offset <= continuations; ++offset) {
            const unsigned char continuation = bytes[index + offset];
            if (continuation < 0x80U || continuation > 0xBFU) {
                return false;
            }
        }
        index += continuations + 1;
    }
    return true;
}

[[nodiscard]] std::filesystem::path utf8_path(
    const std::string_view value) {
#if defined(_WIN32)
    std::u8string converted;
    converted.reserve(value.size());
    for (const unsigned char byte : value) {
        converted.push_back(static_cast<char8_t>(byte));
    }
    return std::filesystem::path{converted};
#else
    return std::filesystem::path{value};
#endif
}

[[nodiscard]] kb_job_options_t job_options_or_default(
    const kb_job_options_t* options) noexcept {
    kb_job_options_t result{};
    result.struct_size = sizeof(result);
    result.api_version = KB_API_VERSION;
    result.timeout_ms = KB_WAIT_INFINITE;
    if (options != nullptr) {
        result.timeout_ms = options->timeout_ms;
        result.progress_callback = options->progress_callback;
        result.progress_user_data = options->progress_user_data;
    }
    return result;
}

[[nodiscard]] bool report_progress(const kb_job_options_t& options,
                                   const char* stage,
                                   const std::string_view device = {},
                                   const std::uint64_t completed = 0,
                                   const std::uint64_t total = 0) noexcept {
    if (options.progress_callback == nullptr) {
        return true;
    }
    const std::string identifier{device};
    const kb_progress_t progress{
        sizeof(kb_progress_t),
        KB_API_VERSION,
        completed,
        total,
        stage,
        identifier.c_str(),
    };
    try {
        return options.progress_callback(&progress, options.progress_user_data) !=
               KB_PROGRESS_CANCEL;
    } catch (...) {
        return false;
    }
}

[[nodiscard]] std::optional<std::chrono::steady_clock::time_point>
job_deadline(const kb_job_options_t& options) noexcept {
    if (options.timeout_ms == KB_WAIT_INFINITE) {
        return std::nullopt;
    }
    const auto now = std::chrono::steady_clock::now();
    const auto room = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::time_point::max() - now);
    const auto timeout = std::chrono::milliseconds{options.timeout_ms};
    return timeout >= room ? std::chrono::steady_clock::time_point::max()
                           : now + timeout;
}

[[nodiscard]] bool deadline_expired(
    const std::optional<std::chrono::steady_clock::time_point>& deadline)
    noexcept {
    return deadline && std::chrono::steady_clock::now() >= *deadline;
}

[[nodiscard]] kb_status_t manifest_status(
    const kairosboot::fleet::ManifestErrorKind kind) noexcept {
    using kairosboot::fleet::ManifestErrorKind;
    switch (kind) {
        case ManifestErrorKind::NotFound:
        case ManifestErrorKind::Io:
            return KB_E_IO;
        case ManifestErrorKind::Cancelled:
            return KB_E_CANCELLED;
        case ManifestErrorKind::TimedOut:
            return KB_E_TIMEOUT;
        case ManifestErrorKind::ResourceExhausted:
            return KB_E_OUT_OF_MEMORY;
        case ManifestErrorKind::UnexpectedFailure:
            return KB_E_INTERNAL;
        default:
            return KB_E_INVALID_ARGUMENT;
    }
}

[[nodiscard]] kb_status_t actor_error_status(
    const kairosboot::fleet::FleetActorExecutionError& error) noexcept {
    using kairosboot::fleet::FleetActorExecutionErrorKind;
    switch (error.kind) {
        case FleetActorExecutionErrorKind::InvalidArgument:
            return KB_E_INVALID_ARGUMENT;
        case FleetActorExecutionErrorKind::Busy:
        case FleetActorExecutionErrorKind::AlreadyExecuted:
            return KB_E_BUSY;
        case FleetActorExecutionErrorKind::Cancelled:
            return KB_E_CANCELLED;
        case FleetActorExecutionErrorKind::TimedOut:
            return KB_E_TIMEOUT;
        case FleetActorExecutionErrorKind::DeviceTaskFailed:
            return KB_E_DEVICE_FAIL;
        case FleetActorExecutionErrorKind::ObserverFailed:
        case FleetActorExecutionErrorKind::UnexpectedFailure:
            return KB_E_INTERNAL;
    }
    return KB_E_INTERNAL;
}

[[nodiscard]] kb_status_t artifact_preflight_status(
    const kairosboot::fleet::ArtifactPreflightErrorKind kind) noexcept {
    using kairosboot::fleet::ArtifactPreflightErrorKind;
    switch (kind) {
        case ArtifactPreflightErrorKind::Cancelled:
            return KB_E_CANCELLED;
        case ArtifactPreflightErrorKind::TimedOut:
            return KB_E_TIMEOUT;
        case ArtifactPreflightErrorKind::NotFound:
        case ArtifactPreflightErrorKind::Io:
            return KB_E_IO;
        case ArtifactPreflightErrorKind::ResourceExhausted:
            return KB_E_OUT_OF_MEMORY;
        case ArtifactPreflightErrorKind::UnexpectedFailure:
            return KB_E_INTERNAL;
        case ArtifactPreflightErrorKind::InvalidArgument:
        case ArtifactPreflightErrorKind::InvalidPlan:
        case ArtifactPreflightErrorKind::DuplicateArtifactId:
        case ArtifactPreflightErrorKind::ConflictingDeclaredDigest:
        case ArtifactPreflightErrorKind::UnsafePath:
        case ArtifactPreflightErrorKind::LimitExceeded:
        case ArtifactPreflightErrorKind::Integrity:
        case ArtifactPreflightErrorKind::HashMismatch:
        case ArtifactPreflightErrorKind::InvalidImage:
            return KB_E_INVALID_ARGUMENT;
    }
    return KB_E_INTERNAL;
}

[[nodiscard]] kb_status_t device_preflight_status(
    const kairosboot::fleet::DevicePreflightError& error) noexcept {
    using kairosboot::fleet::DevicePreflightErrorKind;
    switch (error.kind) {
        case DevicePreflightErrorKind::Cancelled:
            return KB_E_CANCELLED;
        case DevicePreflightErrorKind::DeadlineExceeded:
            return KB_E_TIMEOUT;
        case DevicePreflightErrorKind::MissingSelectorDevice:
        case DevicePreflightErrorKind::MissingTargetDevice:
            return KB_E_NO_DEVICE;
        case DevicePreflightErrorKind::DuplicateSerial:
        case DevicePreflightErrorKind::DeviceMatchesMultipleTargets:
            return KB_E_AMBIGUOUS_DEVICE;
        case DevicePreflightErrorKind::ResourceExhausted:
            return KB_E_OUT_OF_MEMORY;
        case DevicePreflightErrorKind::ProbeFailed:
            return error.probe_error &&
                    error.probe_error->code ==
                        kairosboot::fleet::DevicePreflightProbeErrorCode::DeviceRejected
                ? KB_E_DEVICE_FAIL
                : KB_E_PROTOCOL;
        case DevicePreflightErrorKind::OpenFailed:
            if (error.open_error) {
                using kairosboot::fleet::DevicePreflightOpenErrorCode;
                switch (error.open_error->code) {
                    case DevicePreflightOpenErrorCode::NotFound:
                        return KB_E_NO_DEVICE;
                    case DevicePreflightOpenErrorCode::Busy:
                        return KB_E_BUSY;
                    case DevicePreflightOpenErrorCode::Cancelled:
                        return KB_E_CANCELLED;
                    case DevicePreflightOpenErrorCode::DeadlineExceeded:
                        return KB_E_TIMEOUT;
                    case DevicePreflightOpenErrorCode::ResourceExhausted:
                        return KB_E_OUT_OF_MEMORY;
                    case DevicePreflightOpenErrorCode::PermissionDenied:
                    case DevicePreflightOpenErrorCode::DriverUnavailable:
                    case DevicePreflightOpenErrorCode::TransportFailure:
                    case DevicePreflightOpenErrorCode::UnexpectedFailure:
                        return KB_E_IO;
                }
            }
            return KB_E_IO;
        case DevicePreflightErrorKind::UnexpectedFailure:
            return KB_E_INTERNAL;
        case DevicePreflightErrorKind::InvalidArgument:
        case DevicePreflightErrorKind::SnapshotLimitExceeded:
        case DevicePreflightErrorKind::UnreliableTopology:
        case DevicePreflightErrorKind::DuplicatePhysicalPath:
        case DevicePreflightErrorKind::OpenContractViolation:
        case DevicePreflightErrorKind::DeviceChangedDuringOpen:
        case DevicePreflightErrorKind::ProbeContractViolation:
        case DevicePreflightErrorKind::ProductMismatch:
            return KB_E_INVALID_ARGUMENT;
    }
    return KB_E_INTERNAL;
}

[[nodiscard]] kb_status_t actor_prepare_status(
    const kairosboot::fleet::FleetActorPrepareError& error) noexcept {
    using kairosboot::fleet::FleetActorPrepareErrorKind;
    switch (error.kind) {
        case FleetActorPrepareErrorKind::Cancelled:
            return KB_E_CANCELLED;
        case FleetActorPrepareErrorKind::TimedOut:
            return KB_E_TIMEOUT;
        case FleetActorPrepareErrorKind::ResourceExhausted:
            return KB_E_OUT_OF_MEMORY;
        case FleetActorPrepareErrorKind::UnexpectedFailure:
            return KB_E_INTERNAL;
        case FleetActorPrepareErrorKind::TaskPreparationFailed:
            if (error.device_error &&
                error.device_error->kind ==
                    kairosboot::fastboot::UpdateDeviceErrorKind::Unsupported) {
                return KB_E_NOT_SUPPORTED;
            }
            return KB_E_DEVICE_FAIL;
        case FleetActorPrepareErrorKind::InvalidArgument:
        case FleetActorPrepareErrorKind::PlanDigestMismatch:
        case FleetActorPrepareErrorKind::InvalidPlan:
        case FleetActorPrepareErrorKind::MissingArtifact:
        case FleetActorPrepareErrorKind::SlotResolutionFailed:
        case FleetActorPrepareErrorKind::IntegerOutOfRange:
            return KB_E_INVALID_ARGUMENT;
    }
    return KB_E_INTERNAL;
}

[[nodiscard]] std::string job_id_for_plan(
    const kairosboot::fleet::JobPlan* plan) {
    if (plan == nullptr) {
        return "job-unplanned";
    }
    const auto digest = plan->sha256_hex();
    return "job-" + std::string{digest.substr(0, 16)};
}

[[nodiscard]] bool publish_report(const std::shared_ptr<JobSharedState>& state,
                                  JobReport&& report) noexcept {
    try {
        auto json = std::make_shared<const std::string>(
            report.canonical_json());
        std::scoped_lock lock(state->mutex);
        state->report_json = std::move(json);
        return true;
    } catch (...) {
        return false;
    }
}

[[nodiscard]] OperationOutcome terminal_preflight_failure(
    const std::shared_ptr<JobSharedState>& shared,
    const kairosboot::fleet::JobPlan* plan,
    const kb_status_t status,
    std::string message) {
    const kairosboot::image::Sha256Digest empty_digest{};
    const auto& digest = plan == nullptr ? empty_digest : plan->sha256();
    auto builder = JobReportBuilder::create(
        job_id_for_plan(plan), digest, utc_timestamp(), {});
    if (!builder) {
        return OperationOutcome::failed(operation_error(
            KB_E_INTERNAL, "unable to create the fleet job failure report"));
    }

    const auto report_error = ReportError{
        .code = status,
        .message = message,
        .device_identifier = std::nullopt,
        .native_code = std::nullopt,
        .transfer_certainty = KB_TRANSFER_NOT_SENT,
    };
    if (status == KB_E_CANCELLED) {
        if (!builder->request_cancellation(report_error) ||
            !builder->finish_cancelled(utc_timestamp())) {
            return OperationOutcome::failed(operation_error(
                KB_E_INTERNAL, "unable to finalize the cancelled fleet report"));
        }
    } else if (!builder->finish_failed(utc_timestamp(), report_error)) {
        return OperationOutcome::failed(operation_error(
            KB_E_INTERNAL, "unable to finalize the fleet failure report"));
    }
    auto report = builder->terminal_snapshot();
    if (!report || !publish_report(shared, std::move(*report))) {
        return OperationOutcome::failed(operation_error(
            KB_E_OUT_OF_MEMORY, "unable to publish the fleet job report"));
    }
    auto payload = operation_error(status, std::move(message));
    return status == KB_E_CANCELLED
               ? OperationOutcome::cancelled(std::move(payload))
               : OperationOutcome::failed(std::move(payload));
}

[[nodiscard]] std::expected<JobReport, std::string> build_execution_report(
    const kairosboot::fleet::JobPlan& plan,
    std::vector<kairosboot::fleet::ReportDeviceSpec> specs,
    const FleetCoordinatorResult& result) {
    std::vector<std::size_t> step_counts;
    std::vector<std::vector<std::optional<std::uint64_t>>> step_totals;
    step_counts.reserve(specs.size());
    step_totals.reserve(specs.size());
    for (const auto& spec : specs) {
        step_counts.push_back(spec.steps.size());
        std::vector<std::optional<std::uint64_t>> totals;
        totals.reserve(spec.steps.size());
        for (const auto& step : spec.steps) {
            totals.push_back(step.bytes_total);
        }
        step_totals.push_back(std::move(totals));
    }

    auto builder = JobReportBuilder::create(
        job_id_for_plan(&plan), plan.sha256(), utc_timestamp(),
        std::move(specs));
    if (!builder) {
        return std::unexpected(builder.error().message);
    }

    for (const auto& device : result.devices) {
        if (device.device_index >= step_counts.size() ||
            step_counts[device.device_index] == 0) {
            return std::unexpected("coordinator returned an invalid device index");
        }
        const auto index = device.device_index;
        const auto steps = step_counts[index];
        if (device.state == FleetCoordinatorDeviceState::Skipped) {
            if (!builder->skip_pending_device(
                    index, utc_timestamp(), ReportSkipReason::PolicyStopped)) {
                return std::unexpected("unable to record a skipped fleet device");
            }
            continue;
        }
        if (device.state == FleetCoordinatorDeviceState::Cancelled) {
            continue;
        }

        std::size_t completed_steps = 0;
        if (device.execution) {
            completed_steps = device.execution->completed_steps;
        } else if (device.error) {
            completed_steps = device.error->completed_steps;
        }
        if (!builder->begin_first_step(index, utc_timestamp())) {
            return std::unexpected("unable to begin a fleet report device");
        }
        const auto advances = std::min(completed_steps, steps - 1);
        for (std::size_t step = 0; step < advances; ++step) {
            if (step_totals[index][step] &&
                !builder->update_flash_progress(
                    index, step, *step_totals[index][step])) {
                return std::unexpected(
                    "unable to complete fleet report byte progress");
            }
            if (!builder->advance_step(index,
                                       step,
                                       utc_timestamp(),
                                       utc_timestamp())) {
                return std::unexpected("unable to advance a fleet report step");
            }
        }

        if (device.state == FleetCoordinatorDeviceState::Succeeded) {
            for (std::size_t step = advances; step + 1 < steps; ++step) {
                if (step_totals[index][step] &&
                    !builder->update_flash_progress(
                        index, step, *step_totals[index][step])) {
                    return std::unexpected(
                        "unable to complete fleet report byte progress");
                }
                if (!builder->advance_step(index,
                                           step,
                                           utc_timestamp(),
                                           utc_timestamp())) {
                    return std::unexpected(
                        "unable to complete a fleet report step");
                }
            }
            if (step_totals[index][steps - 1] &&
                !builder->update_flash_progress(
                    index, steps - 1, *step_totals[index][steps - 1])) {
                return std::unexpected(
                    "unable to complete fleet report byte progress");
            }
            if (!builder->complete_device(index, steps - 1, utc_timestamp())) {
                return std::unexpected("unable to complete a fleet report device");
            }
            continue;
        }

        const auto failure_status = device.error
                                        ? actor_error_status(*device.error)
                                        : KB_E_INTERNAL;
        const auto failure_message =
            device.error && !device.error->message.empty()
                ? device.error->message
                : std::string{"fleet device execution failed"};
        const auto failure_step = device.error && device.error->step_index
                                      ? std::min(*device.error->step_index,
                                                 steps - 1)
                                      : std::min(completed_steps, steps - 1);
        if (!builder->fail_step(
                index,
                failure_step,
                utc_timestamp(),
                ReportError{
                    .code = failure_status,
                    .message = failure_message,
                    .device_identifier = std::nullopt,
                    .native_code = std::nullopt,
                    .transfer_certainty = KB_TRANSFER_NOT_SENT,
                },
                ReportSkipReason::FollowingStepFailure)) {
            return std::unexpected("unable to record a failed fleet device");
        }
    }

    if (result.state == FleetCoordinatorState::Cancelled) {
        const ReportError cancellation{
            .code = KB_E_CANCELLED,
            .message = "fleet job cancelled",
            .device_identifier = std::nullopt,
            .native_code = std::nullopt,
            .transfer_certainty = KB_TRANSFER_NOT_SENT,
        };
        if (!builder->request_cancellation(cancellation) ||
            !builder->finish_cancelled(utc_timestamp())) {
            return std::unexpected("unable to finalize the cancelled fleet report");
        }
    } else if (!builder->finish(utc_timestamp())) {
        return std::unexpected("unable to finalize the fleet execution report");
    }
    auto report = builder->terminal_snapshot();
    if (!report) {
        return std::unexpected(report.error().message);
    }
    return std::move(*report);
}

[[nodiscard]] kairosboot::api::FleetRunPrepareError prepare_error(
    const kb_status_t status,
    std::string message) {
    return {.status = status, .message = std::move(message)};
}

[[nodiscard]] std::expected<kairosboot::api::FleetRunDeviceDependencies,
                            kairosboot::api::FleetRunPrepareError>
production_device_dependencies(
    kb_context_t& context,
    const kairosboot::fleet::JobPlan& plan,
    const std::chrono::steady_clock::time_point deadline,
    const std::stop_token cancellation) {
    auto runtime = kairosboot::api::acquire_fleet_usb_runtime(context);
    if (!runtime) {
        return std::unexpected(prepare_error(
            runtime.error().status, runtime.error().message));
    }

    kairosboot::fastboot::LibusbReconnectAdapterOptions adapter_options;
    try {
        if (!plan.manifest().policy.memory_budget.automatic) {
            adapter_options.transport.buffer_budget =
                std::make_shared<kairosboot::transport::BufferBudget>(
                    static_cast<std::size_t>(
                        plan.manifest().policy.memory_budget.bytes));
        }
    } catch (const std::bad_alloc&) {
        return std::unexpected(prepare_error(
            KB_E_OUT_OF_MEMORY,
            "unable to allocate the fleet USB memory budget"));
    }
    auto execution_runtime =
        kairosboot::fleet::make_libusb_fleet_execution_runtime(
            *runtime, std::move(adapter_options));
    if (!execution_runtime) {
        return std::unexpected(prepare_error(
            actor_prepare_status(execution_runtime.error()),
            execution_runtime.error().message));
    }
    auto opener =
        kairosboot::fleet::make_libusb_device_preflight_session_opener(
            *runtime,
            execution_runtime->buffer_budget,
            execution_runtime->data_ring);
    if (!opener) {
        auto status = KB_E_IO;
        using kairosboot::fleet::DevicePreflightOpenErrorCode;
        switch (opener.error().code) {
            case DevicePreflightOpenErrorCode::Cancelled:
                status = KB_E_CANCELLED;
                break;
            case DevicePreflightOpenErrorCode::DeadlineExceeded:
                status = KB_E_TIMEOUT;
                break;
            case DevicePreflightOpenErrorCode::NotFound:
                status = KB_E_NO_DEVICE;
                break;
            case DevicePreflightOpenErrorCode::Busy:
                status = KB_E_BUSY;
                break;
            case DevicePreflightOpenErrorCode::ResourceExhausted:
                status = KB_E_OUT_OF_MEMORY;
                break;
            case DevicePreflightOpenErrorCode::PermissionDenied:
            case DevicePreflightOpenErrorCode::DriverUnavailable:
            case DevicePreflightOpenErrorCode::TransportFailure:
            case DevicePreflightOpenErrorCode::UnexpectedFailure:
                break;
        }
        return std::unexpected(prepare_error(status, opener.error().message));
    }

    kairosboot::transport::UsbInterfaceFilter filter;
    const auto vendor_id = kairosboot::api::fleet_usb_vendor_id(context);
    if (vendor_id != 0U) {
        filter.vendor_id = vendor_id;
    }
    filter.interface_class = 0xFFU;
    filter.interface_subclass = 0x42U;
    filter.interface_protocol = 0x03U;
    auto snapshot = (*runtime)->enumerate(filter, deadline, cancellation);
    if (!snapshot) {
        const auto error = kairosboot::api::normalize_public_error(
            snapshot.error(), {});
        return std::unexpected(prepare_error(error.status, error.message));
    }
    try {
        return kairosboot::api::FleetRunDeviceDependencies{
            .snapshot = std::move(*snapshot),
            .opener = std::move(*opener),
            .probe = std::make_unique<
                kairosboot::fleet::FastbootDevicePreflightProbe>(),
            .execution_runtime = std::move(*execution_runtime),
        };
    } catch (const std::bad_alloc&) {
        return std::unexpected(prepare_error(
            KB_E_OUT_OF_MEMORY,
            "unable to allocate fleet device preflight dependencies"));
    }
}

[[nodiscard]] std::expected<kairosboot::api::FleetRunPrepared,
                            kairosboot::api::FleetRunPrepareError>
prepare_production_fleet_run(
    kb_context_t& context,
    const kairosboot::fleet::JobPlan& plan,
    const std::filesystem::path& manifest_path,
    const std::chrono::steady_clock::time_point deadline,
    const std::stop_token cancellation) {
    try {
        auto artifact_root = manifest_path.parent_path();
        if (artifact_root.empty()) {
            artifact_root = std::filesystem::path{"."};
        }
        kairosboot::fleet::ArtifactPreflightOptions artifact_options;
        artifact_options.deadline = deadline;
        artifact_options.cancellation = cancellation;
        auto artifacts = kairosboot::fleet::preflight_fleet_artifacts(
            plan, artifact_root, artifact_options);
        if (!artifacts) {
            return std::unexpected(prepare_error(
                artifact_preflight_status(artifacts.error().kind),
                artifacts.error().message));
        }

        kairosboot::api::FleetRunDeviceDependenciesFactory dependency_factory;
        {
            std::scoped_lock lock(g_prepare_factory_mutex);
            dependency_factory = g_device_dependencies_factory;
        }
        auto dependencies = dependency_factory
            ? dependency_factory(context, plan, deadline, cancellation)
            : production_device_dependencies(
                  context, plan, deadline, cancellation);
        if (!dependencies) {
            return std::unexpected(std::move(dependencies.error()));
        }
        if (dependencies->opener == nullptr || dependencies->probe == nullptr) {
            return std::unexpected(prepare_error(
                KB_E_INTERNAL,
                "fleet device dependencies are incomplete"));
        }

        auto devices = kairosboot::fleet::preflight_fleet_devices(
            plan,
            dependencies->snapshot,
            *dependencies->opener,
            *dependencies->probe,
            deadline,
            cancellation);
        if (!devices) {
            return std::unexpected(prepare_error(
                device_preflight_status(devices.error()),
                devices.error().message));
        }
        auto consumed = std::move(*devices).consume(plan.sha256());
        if (!consumed) {
            return std::unexpected(prepare_error(
                KB_E_INTERNAL,
                "prepared fleet device plan digest could not be consumed"));
        }
        const kairosboot::fastboot::UpdateOperationContext operation_context{
            .cancellation = cancellation,
            .deadline = deadline == std::chrono::steady_clock::time_point::max()
                ? std::nullopt
                : std::optional{deadline},
        };
        auto batch = kairosboot::fleet::prepare_fleet_device_actors(
            plan,
            std::move(*artifacts),
            std::move(*consumed),
            std::move(dependencies->execution_runtime),
            operation_context);
        if (!batch) {
            return std::unexpected(prepare_error(
                actor_prepare_status(batch.error()), batch.error().message));
        }

        std::vector<kairosboot::fleet::ReportDeviceSpec> report_specs;
        report_specs.assign(
            batch->report_specs().begin(), batch->report_specs().end());
        return kairosboot::api::FleetRunPrepared{
            .report_specs = std::move(report_specs),
            .executor = {},
            .actor_batch =
                std::make_unique<kairosboot::fleet::PreparedFleetActorBatch>(
                    std::move(*batch)),
        };
    } catch (const std::bad_alloc&) {
        return std::unexpected(prepare_error(
            KB_E_OUT_OF_MEMORY,
            "unable to allocate the prepared fleet execution"));
    } catch (const std::exception& error) {
        return std::unexpected(prepare_error(
            KB_E_INTERNAL,
            std::string{"unexpected fleet preparation failure: "} +
                error.what()));
    } catch (...) {
        return std::unexpected(prepare_error(
            KB_E_INTERNAL, "unexpected fleet preparation failure"));
    }
}

[[nodiscard]] OperationOutcome run_job_task(
    const std::shared_ptr<JobSharedState>& shared,
    kb_context_t& context,
    std::string file_path,
    const kb_job_options_t options,
    kairosboot::api::OperationState::TaskContext& task_context) {
    const auto deadline = job_deadline(options);
    const auto cancelled = [&] {
        return task_context.cancel_requested() ||
               shared->cancellation.stop_requested();
    };
    if (cancelled() || !report_progress(options, "validate")) {
        return terminal_preflight_failure(
            shared, nullptr, KB_E_CANCELLED, "fleet job cancelled");
    }
    if (deadline_expired(deadline)) {
        return terminal_preflight_failure(
            shared, nullptr, KB_E_TIMEOUT, "fleet job deadline expired");
    }

    kairosboot::fleet::ManifestParseOptions parse_options;
    parse_options.cancellation = shared->cancellation.get_token();
    if (deadline) {
        parse_options.deadline = *deadline;
    }
    auto manifest = kairosboot::fleet::load_fleet_manifest_file(
        utf8_path(file_path), parse_options);
    if (!manifest) {
        return terminal_preflight_failure(
            shared,
            nullptr,
            manifest_status(manifest.error().kind),
            file_path + ": " + manifest.error().message);
    }
    if (cancelled() || !report_progress(options, "plan")) {
        return terminal_preflight_failure(
            shared, nullptr, KB_E_CANCELLED, "fleet job cancelled");
    }
    auto plan = kairosboot::fleet::make_job_plan(std::move(*manifest));
    if (!plan) {
        return terminal_preflight_failure(
            shared, nullptr, KB_E_INVALID_ARGUMENT,
            "fleet job plan could not be constructed");
    }
    if (deadline_expired(deadline)) {
        return terminal_preflight_failure(
            shared, &*plan, KB_E_TIMEOUT, "fleet job deadline expired");
    }
    if (cancelled() || !report_progress(options, "prepare")) {
        return terminal_preflight_failure(
            shared, &*plan, KB_E_CANCELLED, "fleet job cancelled");
    }

    kairosboot::api::FleetRunPrepareFactory factory;
    {
        std::scoped_lock lock(g_prepare_factory_mutex);
        factory = g_prepare_factory;
    }
    auto prepared = factory
        ? factory(*plan, shared->cancellation.get_token())
        : prepare_production_fleet_run(
              context,
              *plan,
              utf8_path(file_path),
              deadline.value_or(
                  std::chrono::steady_clock::time_point::max()),
              shared->cancellation.get_token());
    if (!prepared) {
        return terminal_preflight_failure(
            shared, &*plan, prepared.error().status, prepared.error().message);
    }
    if (prepared->report_specs.empty() ||
        (!prepared->executor && prepared->actor_batch == nullptr)) {
        return terminal_preflight_failure(
            shared,
            &*plan,
            KB_E_INTERNAL,
            "fleet execution factory returned an incomplete prepared batch");
    }

    auto report_specs = std::move(prepared->report_specs);
    std::vector<std::string> identifiers;
    identifiers.reserve(report_specs.size());
    for (const auto& spec : report_specs) {
        identifiers.push_back(spec.identifier);
    }

    std::stop_source execution_cancel;
    std::stop_callback external_cancel(shared->cancellation.get_token(), [&] {
        static_cast<void>(execution_cancel.request_stop());
    });
    const auto observer = [&, options](
                              const kairosboot::fleet::FleetActorExecutionEvent&
                                  event) {
        const auto identifier = event.device_index < identifiers.size()
                                    ? std::string_view{identifiers[event.device_index]}
                                    : std::string_view{};
        if (!report_progress(options,
                             "execute",
                             identifier,
                             event.completed_data_bytes,
                             0)) {
            static_cast<void>(execution_cancel.request_stop());
        }
    };
    auto coordinator = prepared->actor_batch != nullptr
        ? kairosboot::fleet::FleetCoordinator::create(
              std::move(*prepared->actor_batch),
              plan->manifest().policy,
              observer)
        : kairosboot::fleet::FleetCoordinator::create_for_testing(
              report_specs.size(),
              plan->manifest().policy,
              std::move(prepared->executor),
              observer);
    if (!coordinator) {
        return terminal_preflight_failure(
            shared, &*plan, KB_E_INTERNAL, coordinator.error().message);
    }
    if (!report_progress(options, "execute")) {
        static_cast<void>(execution_cancel.request_stop());
    }
    const kairosboot::fastboot::UpdateOperationContext execution_context{
        .cancellation = execution_cancel.get_token(),
        .deadline = deadline,
    };
    auto result = (*coordinator)->run(execution_context);
    if (!result) {
        return terminal_preflight_failure(
            shared, &*plan, KB_E_INTERNAL, result.error().message);
    }
    if (result->state == FleetCoordinatorState::Succeeded &&
        (shared->cancellation.stop_requested() ||
         !report_progress(options, "complete"))) {
        result->state = FleetCoordinatorState::Cancelled;
    }
    auto report = build_execution_report(*plan, std::move(report_specs), *result);
    if (!report) {
        return OperationOutcome::failed(
            operation_error(KB_E_INTERNAL, std::move(report.error())));
    }
    if (!publish_report(shared, std::move(*report))) {
        return OperationOutcome::failed(operation_error(
            KB_E_OUT_OF_MEMORY, "unable to publish the fleet job report"));
    }

    if (result->state == FleetCoordinatorState::Succeeded) {
        return OperationOutcome::succeeded();
    }
    if (result->state == FleetCoordinatorState::Cancelled) {
        return OperationOutcome::cancelled(
            operation_error(KB_E_CANCELLED, "fleet job cancelled"));
    }
    for (const auto& device : result->devices) {
        if (device.error) {
            return OperationOutcome::failed(operation_error(
                actor_error_status(*device.error),
                device.error->message.empty()
                    ? std::string{"fleet device execution failed"}
                    : device.error->message));
        }
    }
    return OperationOutcome::failed(
        operation_error(KB_E_DEVICE_FAIL, "fleet job failed"));
}

[[nodiscard]] kb_operation_state_t public_state(
    const kairosboot::api::OperationPhase phase) noexcept {
    using kairosboot::api::OperationPhase;
    switch (phase) {
        case OperationPhase::Created:
            return KB_OPERATION_CREATED;
        case OperationPhase::Running:
            return KB_OPERATION_RUNNING;
        case OperationPhase::Succeeded:
            return KB_OPERATION_SUCCEEDED;
        case OperationPhase::Failed:
            return KB_OPERATION_FAILED;
        case OperationPhase::Cancelled:
            return KB_OPERATION_CANCELLED;
    }
    return KB_OPERATION_FAILED;
}

[[nodiscard]] const kb_error_t* materialize_error(
    const kb_job_t* job) noexcept {
    if (job == nullptr || job->state == nullptr) {
        return nullptr;
    }
    const auto payload = job->state->error();
    if (!payload) {
        return nullptr;
    }
    std::scoped_lock lock(job->error_mutex);
    if (job->public_error == nullptr) {
        try {
            job->public_error = std::make_unique<kb_error>(kb_error{
                .status = payload->status,
                .message = payload->message,
                .device_identifier = payload->device_identifier,
                .native_code = payload->native_code,
                .transfer_state = payload->transfer_state,
                .device_message = payload->device_message,
                .command_messages = payload->command_messages,
                .inbound_expected = payload->inbound_expected,
                .inbound_transferred = payload->inbound_transferred,
                .inbound_transfer_state = payload->inbound_transfer_state,
                .session_poisoned = payload->session_poisoned,
            });
        } catch (...) {
            return nullptr;
        }
    }
    return job->public_error.get();
}

}  // namespace

namespace kairosboot::api {

void set_fleet_run_prepare_factory(FleetRunPrepareFactory factory) {
    std::scoped_lock lock(g_prepare_factory_mutex);
    g_prepare_factory = std::move(factory);
}

void set_fleet_run_device_dependencies_factory(
    FleetRunDeviceDependenciesFactory factory) {
    std::scoped_lock lock(g_prepare_factory_mutex);
    g_device_dependencies_factory = std::move(factory);
}

}  // namespace kairosboot::api

extern "C" {

void KB_CALL kb_job_options_init(kb_job_options_t* options) {
    kb_job_options_init_sized(options, KB_JOB_OPTIONS_V1_SIZE);
}

void KB_CALL kb_job_options_init_sized(kb_job_options_t* options,
                                       const uint32_t struct_size) {
    kairosboot::api::detail::initialize_struct_header(options, struct_size);
    kairosboot::api::detail::initialize_field(
        options, struct_size, offsetof(kb_job_options_t, timeout_ms),
        uint32_t{KB_WAIT_INFINITE});
}

kb_status_t KB_CALL kb_run_job_file_async(
    kb_context_t* context,
    const char* file_path,
    const kb_job_options_t* options_or_null,
    kb_job_t** job,
    kb_error_t** error) {
    clear_error(error);
    if (job == nullptr) {
        return fail(error, KB_E_INVALID_ARGUMENT,
                    "fleet job output pointer must not be null");
    }
    *job = nullptr;
    if (context == nullptr) {
        return fail(error, KB_E_INVALID_ARGUMENT,
                    "context must not be null");
    }
    if (file_path == nullptr || file_path[0] == '\0') {
        return fail(error, KB_E_INVALID_ARGUMENT,
                    "fleet manifest path must not be empty");
    }
    if (!valid_utf8(file_path)) {
        return fail(error, KB_E_INVALID_ARGUMENT,
                    "fleet manifest path must be valid UTF-8");
    }
    if (!valid_job_options(options_or_null)) {
        return fail(error, KB_E_INVALID_ARGUMENT,
                    "job options have an incompatible size or API version");
    }

    {
        std::scoped_lock lock(g_active_contexts_mutex);
        if (!g_active_contexts.insert(context).second) {
            return fail(error, KB_E_BUSY,
                        "context already has an active fleet job");
        }
    }

    try {
        auto shared = std::make_shared<JobSharedState>();
        auto options = job_options_or_default(options_or_null);
        std::string path{file_path};
        auto task = [shared, context, options, path = std::move(path)](
                        kairosboot::api::OperationState::TaskContext&
                            task_context) mutable {
            struct ActiveContextRelease final {
                const kb_context_t* context;
                ~ActiveContextRelease() {
                    std::scoped_lock lock(g_active_contexts_mutex);
                    g_active_contexts.erase(context);
                }
            } release{context};
            return run_job_task(
                shared, *context, std::move(path), options, task_context);
        };
        auto operation =
            std::make_unique<kairosboot::api::OperationState>(std::move(task));
        auto result =
            std::make_unique<kb_job>(std::move(operation), shared, context);
        if (!result->state->start()) {
            std::scoped_lock lock(g_active_contexts_mutex);
            g_active_contexts.erase(context);
            return fail(error, KB_E_INTERNAL, "unable to start the fleet job");
        }
        *job = result.release();
        return KB_OK;
    } catch (const std::bad_alloc&) {
        std::scoped_lock lock(g_active_contexts_mutex);
        g_active_contexts.erase(context);
        return fail(error, KB_E_OUT_OF_MEMORY,
                    "unable to allocate the fleet job");
    } catch (...) {
        std::scoped_lock lock(g_active_contexts_mutex);
        g_active_contexts.erase(context);
        return fail(error, KB_E_INTERNAL, "unable to create the fleet job");
    }
}

kb_status_t KB_CALL kb_run_job_file(
    kb_context_t* context,
    const char* file_path,
    const kb_job_options_t* options_or_null,
    kb_job_report_t** report,
    kb_error_t** error) {
    clear_error(error);
    if (report == nullptr) {
        return fail(error, KB_E_INVALID_ARGUMENT,
                    "fleet report output pointer must not be null");
    }
    *report = nullptr;
    kb_job_t* job = nullptr;
    const auto started = kb_run_job_file_async(
        context, file_path, options_or_null, &job, error);
    if (started != KB_OK) {
        return started;
    }
    const auto waited = kb_job_wait(job, KB_WAIT_INFINITE);
    kb_error_t* report_error = nullptr;
    const auto extracted = kb_job_get_report(job, report, &report_error);
    if (report_error != nullptr) {
        kb_error_release(report_error);
    }
    if (waited != KB_OK) {
        const auto payload = job->state->error();
        if (payload) {
            static_cast<void>(fail(error, *payload));
        } else {
            static_cast<void>(fail(error, waited, "fleet job failed"));
        }
    } else if (extracted != KB_OK) {
        static_cast<void>(fail(error, extracted,
                               "fleet job report is unavailable"));
    }
    kb_job_release(job);
    return waited == KB_OK ? extracted : waited;
}

kb_status_t KB_CALL kb_job_wait(kb_job_t* job, const uint32_t timeout_ms) {
    if (job == nullptr || job->state == nullptr) {
        return KB_E_INVALID_ARGUMENT;
    }
    if (timeout_ms == KB_WAIT_INFINITE) {
        job->state->wait();
    } else if (job->state->wait_for(std::chrono::milliseconds{timeout_ms}) ==
               kairosboot::api::OperationWaitResult::Timeout) {
        return KB_E_TIMEOUT;
    }
    return job->state->status();
}

kb_status_t KB_CALL kb_job_cancel(kb_job_t* job) {
    if (job == nullptr || job->state == nullptr || job->result == nullptr) {
        return KB_E_INVALID_ARGUMENT;
    }
    static_cast<void>(job->result->cancellation.request_stop());
    return KB_OK;
}

kb_operation_state_t KB_CALL kb_job_state(const kb_job_t* job) {
    return job == nullptr || job->state == nullptr
               ? KB_OPERATION_FAILED
               : public_state(job->state->phase());
}

const kb_error_t* KB_CALL kb_job_error(const kb_job_t* job) {
    return materialize_error(job);
}

kb_status_t KB_CALL kb_job_get_report(const kb_job_t* job,
                                      kb_job_report_t** report,
                                      kb_error_t** error) {
    clear_error(error);
    if (report == nullptr) {
        return fail(error, KB_E_INVALID_ARGUMENT,
                    "fleet report output pointer must not be null");
    }
    *report = nullptr;
    if (job == nullptr || job->state == nullptr || job->result == nullptr) {
        return fail(error, KB_E_INVALID_ARGUMENT, "job must not be null");
    }
    const auto phase = job->state->phase();
    if (phase == kairosboot::api::OperationPhase::Created ||
        phase == kairosboot::api::OperationPhase::Running) {
        return fail(error, KB_E_BUSY, "fleet job has not completed");
    }
    try {
        std::shared_ptr<const std::string> json;
        {
            std::scoped_lock lock(job->result->mutex);
            json = job->result->report_json;
        }
        if (json == nullptr) {
            return fail(error, KB_E_INTERNAL,
                        "fleet job completed without a report");
        }
        auto result = std::make_unique<kb_job_report>(std::move(json));
        *report = result.release();
        return KB_OK;
    } catch (const std::bad_alloc&) {
        return fail(error, KB_E_OUT_OF_MEMORY,
                    "unable to allocate the fleet report handle");
    } catch (...) {
        return fail(error, KB_E_INTERNAL,
                    "unable to materialize the fleet report");
    }
}

void KB_CALL kb_job_release(kb_job_t* job) {
    delete job;
}

const char* KB_CALL kb_job_report_json(const kb_job_report_t* report,
                                       size_t* size) {
    if (size != nullptr) {
        *size = 0;
    }
    if (report == nullptr || report->json == nullptr) {
        return nullptr;
    }
    if (size != nullptr) {
        *size = report->json->size();
    }
    return report->json->c_str();
}

void KB_CALL kb_job_report_release(kb_job_report_t* report) {
    delete report;
}

}  // extern "C"
