// SPDX-License-Identifier: MIT
#pragma once

#include "src/fastboot/primitive_update_device.hpp"
#include "src/fleet/artifact_preflight.hpp"
#include "src/fleet/device_preflight.hpp"
#include "src/fleet/job_report.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace kairosboot::fleet {

inline constexpr std::uint64_t kFleetActorMaximumIJsonInteger =
    9'007'199'254'740'991ULL;

enum class FleetActorPrepareErrorKind : std::uint8_t {
    InvalidArgument,
    PlanDigestMismatch,
    InvalidPlan,
    MissingArtifact,
    SlotResolutionFailed,
    TaskPreparationFailed,
    IntegerOutOfRange,
    Cancelled,
    TimedOut,
    ResourceExhausted,
    UnexpectedFailure,
};

struct FleetActorPrepareError final {
    FleetActorPrepareErrorKind kind{
        FleetActorPrepareErrorKind::UnexpectedFailure};
    std::string message;
    std::optional<std::size_t> device_index;
    std::optional<std::size_t> step_index;
    std::optional<fastboot::UpdateDeviceError> device_error;
};

enum class FleetActorExecutionEventKind : std::uint8_t {
    DeviceStarted,
    StepStarted,
    StepCompleted,
    DeviceCompleted,
};

struct FleetActorExecutionEvent final {
    FleetActorExecutionEventKind kind{
        FleetActorExecutionEventKind::DeviceStarted};
    std::size_t device_index{};
    std::optional<std::size_t> step_index;
    std::size_t completed_steps{};
    std::uint64_t completed_data_bytes{};
};

using FleetActorExecutionObserver =
    std::function<void(const FleetActorExecutionEvent&)>;

enum class FleetActorExecutionErrorKind : std::uint8_t {
    InvalidArgument,
    Busy,
    AlreadyExecuted,
    Cancelled,
    TimedOut,
    DeviceTaskFailed,
    ObserverFailed,
    UnexpectedFailure,
};

struct FleetActorExecutionError final {
    FleetActorExecutionErrorKind kind{
        FleetActorExecutionErrorKind::UnexpectedFailure};
    std::string message;
    std::size_t device_index{};
    std::optional<std::size_t> step_index;
    std::size_t completed_steps{};
    std::uint64_t completed_data_bytes{};
    // Progress within the failed logical step. This is essential when one
    // manifest flash expands to multiple immutable child tasks (slot: all).
    std::size_t completed_child_tasks_in_step{};
    std::size_t total_child_tasks_in_step{};
    std::optional<fastboot::UpdateDeviceError> device_error;
};

struct FleetActorDeviceExecution final {
    std::size_t completed_steps{};
    std::uint64_t completed_data_bytes{};
};

class FleetDeviceActor final {
public:
    FleetDeviceActor(const FleetDeviceActor&) = delete;
    FleetDeviceActor& operator=(const FleetDeviceActor&) = delete;
    ~FleetDeviceActor();

private:
    struct PreparedStep;

    FleetDeviceActor(PreparedDeviceSession&& prepared,
                     const ManifestTarget& target);

    [[nodiscard]] std::expected<ReportDeviceSpec,
                                FleetActorPrepareError>
    prepare(const PreparedFleetArtifacts& artifacts,
            const fastboot::UpdateOperationContext& context,
            std::size_t device_index);

    [[nodiscard]] std::expected<FleetActorDeviceExecution,
                                FleetActorExecutionError>
    execute(std::size_t device_index,
            const fastboot::UpdateOperationContext& context,
            const FleetActorExecutionObserver& observer);

    void retire() noexcept;

    // Declaration order is a lifetime contract. Destruction runs steps,
    // update adapter and primitive service before the underlying session.
    std::unique_ptr<protocol::FastbootSession> session_;
    std::unique_ptr<fastboot::PrimitiveService> service_;
    std::unique_ptr<fastboot::PrimitiveUpdateDevice> update_;
    std::vector<PreparedStep> steps_;
    std::atomic<std::uint8_t> state_{0U};
    const ManifestTarget* target_{};

    friend class PreparedFleetActorBatch;
};

class PreparedFleetActorBatch final {
public:
    PreparedFleetActorBatch(const PreparedFleetActorBatch&) = delete;
    PreparedFleetActorBatch& operator=(const PreparedFleetActorBatch&) = delete;
    PreparedFleetActorBatch(PreparedFleetActorBatch&&) noexcept = default;
    PreparedFleetActorBatch& operator=(PreparedFleetActorBatch&&) noexcept =
        default;
    ~PreparedFleetActorBatch() = default;

    [[nodiscard]] std::span<const ReportDeviceSpec> report_specs()
        const noexcept;

    [[nodiscard]] std::expected<FleetActorDeviceExecution,
                                FleetActorExecutionError>
    execute_device(std::size_t device_index,
                   const fastboot::UpdateOperationContext& context = {},
                   const FleetActorExecutionObserver& observer = {});

private:
    PreparedFleetActorBatch(
        PreparedFleetArtifacts&& artifacts,
        std::vector<std::unique_ptr<FleetDeviceActor>>&& actors,
        std::vector<ReportDeviceSpec>&& report_specs) noexcept;

    [[nodiscard]] static std::expected<PreparedFleetActorBatch,
                                       FleetActorPrepareError>
    prepare(const JobPlan& plan,
            PreparedFleetArtifacts&& artifacts,
            PreparedDeviceBatchConsumption&& devices,
            const fastboot::UpdateOperationContext& context);

    // The sealed artifact owner outlives every task that aliases its sources.
    PreparedFleetArtifacts artifacts_;
    std::vector<std::unique_ptr<FleetDeviceActor>> actors_;
    std::vector<ReportDeviceSpec> report_specs_;

    friend std::expected<PreparedFleetActorBatch, FleetActorPrepareError>
    prepare_fleet_device_actors(
        const JobPlan&,
        PreparedFleetArtifacts&&,
        PreparedDeviceBatchConsumption&&,
        const fastboot::UpdateOperationContext&);
};

// Consumes the plan-bound device capability only after the JobPlan, artifact
// capability and device capability digests match. Every actor and every step
// is fully prepared before the returned destructive gate becomes executable.
[[nodiscard]] std::expected<PreparedFleetActorBatch, FleetActorPrepareError>
prepare_fleet_device_actors(
    const JobPlan& plan,
    PreparedFleetArtifacts&& artifacts,
    PreparedDeviceBatchConsumption&& devices,
    const fastboot::UpdateOperationContext& context = {});

}  // namespace kairosboot::fleet
