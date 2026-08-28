// SPDX-License-Identifier: MIT
#pragma once

#include "src/fleet/fleet_coordinator.hpp"
#include "src/fleet/job_plan.hpp"
#include "src/fleet/job_report.hpp"

#include <kairosboot/kairosboot.h>

#include <expected>
#include <functional>
#include <stop_token>
#include <string>
#include <vector>

namespace kairosboot::api {

// Narrow preparation seam between the stable C job lifecycle and the
// platform-specific device/session factory. It deliberately hands the C layer
// only already-verified report identities and coordinator executors. Until the
// production USB integration installs a factory, real execution fails closed
// with KB_E_NOT_SUPPORTED after manifest validation and planning.
struct FleetRunPrepared final {
    std::vector<fleet::ReportDeviceSpec> report_specs;
    fleet::FleetCoordinatorDeviceExecutor executor;
};

struct FleetRunPrepareError final {
    kb_status_t status{KB_E_INTERNAL};
    std::string message;
};

using FleetRunPrepareFactory = std::function<
    std::expected<FleetRunPrepared, FleetRunPrepareError>(
        const fleet::JobPlan&, std::stop_token)>;

// Internal integration/test hook. The callable is snapshotted per job, so a
// later replacement cannot change an operation already in flight. Passing an
// empty callable restores the fail-closed production default.
void set_fleet_run_prepare_factory(FleetRunPrepareFactory factory);

}  // namespace kairosboot::api
