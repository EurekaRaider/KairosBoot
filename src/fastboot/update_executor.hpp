// SPDX-License-Identifier: MIT
#pragma once

#include "src/fastboot/update_package_preflight.hpp"
#include "src/protocol/fastboot_protocol.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace kairosboot::fastboot {

enum class UpdateDeviceErrorKind : std::uint8_t {
    Failed,
    Cancelled,
    TimedOut,
};

struct UpdateDeviceError final {
    UpdateDeviceErrorKind kind{UpdateDeviceErrorKind::Failed};
    protocol::ProtocolPhase phase{protocol::ProtocolPhase::Validation};
    std::string message;
    std::string device_message;
    std::vector<protocol::Response> informational;
    protocol::TransportStatus transport_status{protocol::TransportStatus::Ok};
    protocol::TransferCertainty transport_certainty{
        protocol::TransferCertainty::NotTransferred};
    protocol::TransferCertainty outbound_certainty{
        protocol::TransferCertainty::NotTransferred};
    std::optional<std::uint64_t> inbound_expected{};
    std::uint64_t inbound_transferred{};
    protocol::TransferCertainty inbound_certainty{
        protocol::TransferCertainty::NotTransferred};
    bool session_poisoned{};
    bool session_closed{};
    int native_code{};
};

struct UpdateOperationContext final {
    std::stop_token cancellation{};
    std::optional<std::chrono::steady_clock::time_point> deadline{};
};

// A fully bound ordinary flash input. The shared immutable source is retained
// by the execute token, so execution never has to resolve or reopen the package.
struct UpdateFlashArtifactInput final {
    std::string logical_name;
    std::shared_ptr<const image::IImageSource> source;
    image::FlashArtifactMetadata metadata;
    image::Sha256Digest sha256{};
};

// Reserved binding for a future dedicated super-image builder. It deliberately
// lives outside PreparedUpdatePackage so package preflight remains transport-
// and device-independent.
struct UpdateSuperArtifactInput final {
    std::string logical_name;
    std::shared_ptr<const image::IImageSource> source;
    image::Sha256Digest sha256{};
};

struct UpdateDeviceTaskInput final {
    PlannedUpdateTask task;
    std::optional<UpdateFlashArtifactInput> flash_artifact;
    std::optional<UpdateSuperArtifactInput> super_artifact;
};

// Immutable execute capability returned only after the adapter has validated
// the partition, slot/reboot mode, dynamic-super requirements and device
// capabilities for this exact bound input.
class IPreparedDeviceTask {
public:
    IPreparedDeviceTask() = default;
    IPreparedDeviceTask(const IPreparedDeviceTask&) = delete;
    IPreparedDeviceTask& operator=(const IPreparedDeviceTask&) = delete;
    virtual ~IPreparedDeviceTask() = default;

    [[nodiscard]] virtual std::expected<void, UpdateDeviceError>
    execute(const UpdateOperationContext& context) const = 0;
};

// Minimal transport-independent actor. Production adapters retain ownership of
// dynamic partition, super, AVB, slot and protocol details. prepare_task must
// not issue destructive commands; a successful token owns every host-side
// input needed by execute().
class IUpdateDevice {
public:
    IUpdateDevice() = default;
    IUpdateDevice(const IUpdateDevice&) = delete;
    IUpdateDevice& operator=(const IUpdateDevice&) = delete;
    virtual ~IUpdateDevice() = default;

    [[nodiscard]] virtual std::expected<std::string, UpdateDeviceError>
    getvar(std::string_view name, const UpdateOperationContext& context) = 0;

    [[nodiscard]] virtual std::expected<std::unique_ptr<IPreparedDeviceTask>,
                                        UpdateDeviceError>
    prepare_task(UpdateDeviceTaskInput input,
                 const UpdateOperationContext& context) = 0;
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
    // One absolute package/job deadline, shared unchanged by validation,
    // getvar, preparation and execution. nullopt means no deadline.
    std::optional<std::chrono::steady_clock::time_point> deadline;
    // Optional prebuilt super input. Adapters that require it reject
    // update-super during preparation when it is absent.
    std::optional<UpdateSuperArtifactInput> super_artifact;
    UpdateExecutionObserver observer;
};

struct UpdateExecutionReport final {
    std::size_t validated_requirements{};
    std::size_t completed_tasks{};
    std::vector<UpdateExecutionEvent> trace;
};

enum class UpdateExecutionErrorKind : std::uint8_t {
    Cancelled,
    TimedOut,
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
    // A TaskFailed observer exception is diagnostic only when a device failure
    // already exists; it must never replace the primary device error.
    std::optional<std::string> secondary_observer_error;
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
