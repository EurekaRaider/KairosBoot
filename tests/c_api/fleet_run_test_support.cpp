// SPDX-License-Identifier: MIT

#include "src/api/fleet_run.hpp"

#include <chrono>
#include <expected>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

enum class ScriptMode {
    Success = 0,
    Failure = 1,
    WaitForCancel = 2,
};

[[nodiscard]] kairosboot::fleet::ReportDeviceSpec report_spec() {
    return {
        .identifier = "usb:1-1",
        .serial = std::string{"SERIAL-01"},
        .usb_path = std::string{"usb:1-1"},
        .target = "product-a",
        .expected_product = "product_a",
        .observed_product = std::string{"product_a"},
        .steps = {
            kairosboot::fleet::ReportStepSpec{
                .operation = kairosboot::fleet::ReportOperation::Flash,
                .partition = std::string{"system"},
                .artifact = std::string{"system"},
                .slot = std::nullopt,
                .reboot_target = std::nullopt,
                .oem_command = std::nullopt,
                .bytes_total = 4096U,
            },
        },
    };
}

[[nodiscard]] kairosboot::fleet::FleetActorExecutionError cancelled_error(
    const std::size_t device_index) {
    return {
        .kind = kairosboot::fleet::FleetActorExecutionErrorKind::Cancelled,
        .message = "scripted fleet device cancelled",
        .device_index = device_index,
        .step_index = 0U,
        .completed_steps = 0U,
        .completed_data_bytes = 0U,
        .completed_child_tasks_in_step = 0U,
        .total_child_tasks_in_step = 1U,
        .device_error = std::nullopt,
    };
}

}  // namespace

extern "C" void kb_test_set_fleet_script(const int mode) {
    const auto script = static_cast<ScriptMode>(mode);
    kairosboot::api::set_fleet_run_prepare_factory(
        [script](const kairosboot::fleet::JobPlan&, const std::stop_token token)
            -> std::expected<kairosboot::api::FleetRunPrepared,
                             kairosboot::api::FleetRunPrepareError> {
            kairosboot::fleet::FleetCoordinatorDeviceExecutor executor =
                [script, token](
                    const std::size_t device_index,
                    const kairosboot::fastboot::UpdateOperationContext& context,
                    const kairosboot::fleet::FleetActorExecutionObserver& observer)
                -> std::expected<
                    kairosboot::fleet::FleetActorDeviceExecution,
                    kairosboot::fleet::FleetActorExecutionError> {
                if (observer) {
                    observer({
                        .kind = kairosboot::fleet::
                            FleetActorExecutionEventKind::DeviceStarted,
                        .device_index = device_index,
                        .step_index = std::nullopt,
                        .completed_steps = 0U,
                        .completed_data_bytes = 0U,
                    });
                }
                if (script == ScriptMode::WaitForCancel) {
                    while (!context.cancellation.stop_requested()) {
                        std::this_thread::sleep_for(
                            std::chrono::milliseconds{1});
                    }
                    return std::unexpected(cancelled_error(device_index));
                }
                if (context.cancellation.stop_requested() ||
                    token.stop_requested()) {
                    return std::unexpected(cancelled_error(device_index));
                }
                if (script == ScriptMode::Failure) {
                    return std::unexpected(
                        kairosboot::fleet::FleetActorExecutionError{
                            .kind = kairosboot::fleet::
                                FleetActorExecutionErrorKind::DeviceTaskFailed,
                            .message = "scripted device rejected flash",
                            .device_index = device_index,
                            .step_index = 0U,
                            .completed_steps = 0U,
                            .completed_data_bytes = 0U,
                            .completed_child_tasks_in_step = 0U,
                            .total_child_tasks_in_step = 1U,
                            .device_error = std::nullopt,
                        });
                }
                if (observer) {
                    observer({
                        .kind = kairosboot::fleet::
                            FleetActorExecutionEventKind::DeviceCompleted,
                        .device_index = device_index,
                        .step_index = 0U,
                        .completed_steps = 1U,
                        .completed_data_bytes = 4096U,
                    });
                }
                return kairosboot::fleet::FleetActorDeviceExecution{
                    .completed_steps = 1U,
                    .completed_data_bytes = 4096U,
                };
            };
            return kairosboot::api::FleetRunPrepared{
                .report_specs = {report_spec()},
                .executor = std::move(executor),
            };
        });
}
