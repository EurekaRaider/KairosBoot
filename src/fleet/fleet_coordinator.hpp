// SPDX-License-Identifier: MIT
#pragma once

#include "src/fleet/device_actor.hpp"
#include "src/fleet/manifest.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace kairosboot::fleet {

inline constexpr std::uint32_t kFleetCoordinatorMaximumParallelDevices = 256U;

enum class FleetCoordinatorErrorKind : std::uint8_t {
    InvalidArgument,
    AlreadyRun,
    ResourceExhausted,
    UnexpectedFailure,
};

struct FleetCoordinatorError final {
    FleetCoordinatorErrorKind kind{FleetCoordinatorErrorKind::UnexpectedFailure};
    std::string message;
};

enum class FleetCoordinatorDeviceState : std::uint8_t {
    Pending,
    Running,
    Succeeded,
    Failed,
    Cancelled,
    Skipped,
};

struct FleetCoordinatorDeviceResult final {
    std::size_t device_index{};
    FleetCoordinatorDeviceState state{FleetCoordinatorDeviceState::Pending};
    std::optional<FleetActorDeviceExecution> execution;
    std::optional<FleetActorExecutionError> error;
};

enum class FleetCoordinatorState : std::uint8_t {
    Succeeded,
    PartiallyFailed,
    Failed,
    Cancelled,
};

struct FleetCoordinatorResult final {
    FleetCoordinatorState state{FleetCoordinatorState::Failed};
    std::vector<FleetCoordinatorDeviceResult> devices;
};

using FleetCoordinatorDeviceExecutor = std::function<
    std::expected<FleetActorDeviceExecution, FleetActorExecutionError>(
        std::size_t,
        const fastboot::UpdateOperationContext&,
        const FleetActorExecutionObserver&)>;

using FleetCoordinatorWorker = std::function<void()>;
using FleetCoordinatorThreadFactory =
    std::function<std::jthread(FleetCoordinatorWorker)>;

struct FleetCoordinatorTestOptions final {
    // Deterministic thread-creation failure seam. Production always leaves
    // this empty and constructs std::jthread directly.
    FleetCoordinatorThreadFactory thread_factory;
};

// Owns one already-prepared destructive gate. run() is blocking and may be
// called exactly once. Every worker is joinable and is drained before run()
// returns or the coordinator is destroyed.
class FleetCoordinator final {
public:
    FleetCoordinator(const FleetCoordinator&) = delete;
    FleetCoordinator& operator=(const FleetCoordinator&) = delete;
    FleetCoordinator(FleetCoordinator&&) = delete;
    FleetCoordinator& operator=(FleetCoordinator&&) = delete;
    ~FleetCoordinator();

    [[nodiscard]] static std::expected<std::unique_ptr<FleetCoordinator>,
                                       FleetCoordinatorError>
    create(PreparedFleetActorBatch&& batch,
           const ManifestPolicy& policy,
           FleetActorExecutionObserver observer = {});

    // Internal deterministic test seam. Production callers consume a sealed
    // PreparedFleetActorBatch through create().
    [[nodiscard]] static std::expected<std::unique_ptr<FleetCoordinator>,
                                       FleetCoordinatorError>
    create_for_testing(std::size_t device_count,
                       const ManifestPolicy& policy,
                       FleetCoordinatorDeviceExecutor executor,
                       FleetActorExecutionObserver observer = {},
                       FleetCoordinatorTestOptions options = {});

    [[nodiscard]] std::expected<FleetCoordinatorResult,
                                FleetCoordinatorError>
    run(const fastboot::UpdateOperationContext& context = {});

    void request_cancel() noexcept;

private:
    FleetCoordinator(std::size_t device_count,
                     ManifestPolicy policy,
                     FleetCoordinatorDeviceExecutor executor,
                     FleetActorExecutionObserver observer,
                     FleetCoordinatorTestOptions options);

    void worker_loop(const fastboot::UpdateOperationContext& context) noexcept;
    void join_workers() noexcept;

    struct Implementation;
    std::unique_ptr<Implementation> implementation_;
};

}  // namespace kairosboot::fleet
