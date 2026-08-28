// SPDX-License-Identifier: MIT
#pragma once

#include "src/fleet/fleet_coordinator.hpp"
#include "src/fleet/job_plan.hpp"
#include "src/fleet/job_report.hpp"

#include <kairosboot/kairosboot.h>

#include <expected>
#include <functional>
#include <chrono>
#include <memory>
#include <stop_token>
#include <string>
#include <vector>

namespace kairosboot::api {

// Narrow scripted preparation seam between the stable C job lifecycle and the
// production device/session chain. Tests can replace the complete preparation
// result; production leaves this empty and builds an actor batch through the
// lower verified libusb path.
struct FleetRunPrepared final {
    std::vector<fleet::ReportDeviceSpec> report_specs;
    fleet::FleetCoordinatorDeviceExecutor executor;
    std::unique_ptr<fleet::PreparedFleetActorBatch> actor_batch;
};

struct FleetRunPrepareError final {
    kb_status_t status{KB_E_INTERNAL};
    std::string message;
};

using FleetRunPrepareFactory = std::function<
    std::expected<FleetRunPrepared, FleetRunPrepareError>(
        const fleet::JobPlan&, std::stop_token)>;

// Lower production dependency seam. Tests may script passive enumeration and
// live sessions while still exercising artifact preflight, device matching,
// plan-digest barriers, actor preparation and FleetCoordinator. Production
// leaves this empty and acquires the context-owned libusb runtime.
struct FleetRunDeviceDependencies final {
    std::vector<transport::UsbDeviceInfo> snapshot;
    std::unique_ptr<fleet::IDevicePreflightSessionOpener> opener;
    std::unique_ptr<fleet::IDevicePreflightProbe> probe;
    fleet::FleetExecutionRuntime execution_runtime;
};

using FleetRunDeviceDependenciesFactory = std::function<
    std::expected<FleetRunDeviceDependencies, FleetRunPrepareError>(
        kb_context_t&,
        const fleet::JobPlan&,
        std::chrono::steady_clock::time_point,
        std::stop_token)>;

// Internal integration/test hook. The callable is snapshotted per job, so a
// later replacement cannot change an operation already in flight. Passing an
// empty callable restores the fail-closed production default.
void set_fleet_run_prepare_factory(FleetRunPrepareFactory factory);

void set_fleet_run_device_dependencies_factory(
    FleetRunDeviceDependenciesFactory factory);

}  // namespace kairosboot::api
