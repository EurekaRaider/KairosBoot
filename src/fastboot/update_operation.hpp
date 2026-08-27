// SPDX-License-Identifier: MIT
#pragma once

#include "src/fastboot/primitive_update_device.hpp"
#include "src/fastboot/update_executor.hpp"
#include "src/fastboot/update_package_preflight.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace kairosboot::fastboot {

enum class UpdatePackageOperationStage : std::uint8_t {
    Deadline,
    PackagePreflight,
    DeviceValidation,
    TaskPreparation,
    TaskExecution,
    Completion,
};

enum class UpdatePackageOperationErrorKind : std::uint8_t {
    InvalidTimeout,
    Cancelled,
    TimedOut,
    PreflightFailed,
    ExecutionFailed,
    InternalFailure,
};

struct UpdatePackageOperationOptions final {
    bool wants_wipe{};
    UpdatePackagePreflightLimits preflight_limits{};
    std::vector<std::string> known_partitions{};
    UpdateExecutionObserver observer{};

    // nullopt means infinite. A finite value is measured once from one
    // steady_clock sample. Negative values and time_point overflow fail before
    // package or device work starts.
    std::optional<std::chrono::steady_clock::duration> timeout{};
};

struct UpdatePackageOperationReport final {
    // time_point::max() represents an infinite operation budget. This is the
    // exact value passed to both package preflight and every device context.
    std::chrono::steady_clock::time_point deadline{};
    UpdateExecutionReport execution{};
};

struct UpdatePackageOperationError final {
    UpdatePackageOperationErrorKind kind{
        UpdatePackageOperationErrorKind::InternalFailure};
    UpdatePackageOperationStage stage{UpdatePackageOperationStage::Deadline};
    std::string message{};
    std::chrono::steady_clock::time_point deadline{};

    // These objects are retained without normalization so callers can inspect
    // the exact artifact, manifest, protocol and native diagnostics.
    std::optional<UpdatePackagePreflightError> preflight_error{};
    std::optional<UpdateExecutionError> execution_error{};

    std::size_t completed_tasks{};
    std::size_t total_tasks{};

    // Exact aggregate metadata for the current failed prepared task. These are
    // zero/NotTransferred when no task actor produced an UpdateDeviceError.
    std::size_t completed_actions{};
    std::size_t total_actions{};
    protocol::TransferCertainty task_certainty{
        protocol::TransferCertainty::NotTransferred};
};

// Performs transport-free package preflight first, then delegates the fully
// immutable result to the two-phase executor. One absolute deadline is created
// once and passed unchanged across every stage.
[[nodiscard]] std::expected<UpdatePackageOperationReport,
                            UpdatePackageOperationError>
run_update_package_operation(
    image::ArtifactSourceResolver& resolver,
    const std::filesystem::path& package_directory_or_zip,
    IUpdateDevice& device,
    const UpdatePackageOperationOptions& options = {},
    std::stop_token cancellation = {});

// Production convenience overload for one already-selected and serialized
// PrimitiveService session. The adapter is created before delegating, but the
// delegated call remains the sole owner of deadline computation.
[[nodiscard]] std::expected<UpdatePackageOperationReport,
                            UpdatePackageOperationError>
run_update_package_operation(
    image::ArtifactSourceResolver& resolver,
    const std::filesystem::path& package_directory_or_zip,
    PrimitiveService& service,
    const UpdatePackageOperationOptions& options = {},
    PrimitiveUpdateDeviceOptions device_options = {},
    std::stop_token cancellation = {});

}  // namespace kairosboot::fastboot
