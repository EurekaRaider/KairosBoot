// SPDX-License-Identifier: MIT
#pragma once

#include "src/fastboot/update_package_preflight.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace kairosboot::fastboot {

enum class UpdateDeviceErrorKind : std::uint8_t {
    Failed,
    Cancelled,
};

struct UpdateDeviceError final {
    UpdateDeviceErrorKind kind{UpdateDeviceErrorKind::Failed};
    int native_code{};
    std::string message;
};

// Minimal transport-independent actor. Production adapters retain ownership of
// dynamic partition, super, AVB, slot and protocol details.
class IUpdateDevice {
public:
    IUpdateDevice() = default;
    IUpdateDevice(const IUpdateDevice&) = delete;
    IUpdateDevice& operator=(const IUpdateDevice&) = delete;
    virtual ~IUpdateDevice() = default;

    [[nodiscard]] virtual std::expected<std::string, UpdateDeviceError>
    getvar(std::string_view name, std::stop_token cancellation) = 0;

    [[nodiscard]] virtual std::expected<void, UpdateDeviceError>
    flash(const PlannedUpdateTask& task, const PreparedUpdateArtifact& artifact,
          std::stop_token cancellation) = 0;
    [[nodiscard]] virtual std::expected<void, UpdateDeviceError>
    erase(const PlannedUpdateTask& task, std::stop_token cancellation) = 0;
    [[nodiscard]] virtual std::expected<void, UpdateDeviceError>
    reboot(const PlannedUpdateTask& task, std::stop_token cancellation) = 0;
    [[nodiscard]] virtual std::expected<void, UpdateDeviceError>
    update_super(const PlannedUpdateTask& task, std::stop_token cancellation) = 0;
};

enum class UpdateExecutionEventKind : std::uint8_t {
    ValidationStarted,
    PreparedPackageValidated,
    GetVarQuery,
    GetVarResult,
    GetVarCacheHit,
    RequirementSatisfied,
    RequirementSkipped,
    RequirementFailed,
    ValidationCompleted,
    TaskStarted,
    TaskCompleted,
    TaskFailed,
    ExecutionCompleted,
};

struct UpdateExecutionEvent final {
    UpdateExecutionEventKind kind{UpdateExecutionEventKind::ValidationStarted};
    std::optional<std::size_t> requirement_index;
    std::optional<std::size_t> task_index;
    std::optional<UpdateSourceLocation> location;
    std::string name;
    std::string value;
    std::string message;
};

using UpdateExecutionObserver = std::function<void(const UpdateExecutionEvent&)>;

struct UpdateExecutorOptions final {
    // Mirrors the frozen AOSP host image table check used by
    // `require partition-exists=x`. The production planner supplies the exact
    // known partition set; the executor never guesses it from device state.
    std::vector<std::string> known_partitions;
    UpdateExecutionObserver observer;
};

struct UpdateExecutionReport final {
    std::size_t validated_requirements{};
    std::size_t completed_tasks{};
    std::vector<UpdateExecutionEvent> trace;
};

enum class UpdateExecutionErrorKind : std::uint8_t {
    Cancelled,
    InvalidPreparedPackage,
    GetVarFailed,
    RequirementNotMet,
    DeviceTaskFailed,
    ObserverFailed,
    ActorException,
};

struct UpdateExecutionError final {
    UpdateExecutionErrorKind kind{UpdateExecutionErrorKind::InvalidPreparedPackage};
    std::optional<std::size_t> requirement_index;
    std::optional<std::size_t> task_index;
    std::optional<UpdateSourceLocation> location;
    std::string name;
    std::string message;
    std::optional<UpdateDeviceError> device_error;
    std::size_t validated_requirements{};
    std::size_t completed_tasks{};
    std::vector<UpdateExecutionEvent> trace;
};

// Validates the complete prepared mapping and every device requirement before
// calling any destructive actor method. Successfully completed tasks are
// reported exactly; the executor never claims rollback after a later failure.
[[nodiscard]] std::expected<UpdateExecutionReport, UpdateExecutionError>
execute_prepared_update(const PreparedUpdatePackage& prepared, IUpdateDevice& device,
                        const UpdateExecutorOptions& options = {},
                        std::stop_token cancellation = {});

}  // namespace kairosboot::fastboot
