// SPDX-License-Identifier: MIT

#include "src/fleet/fleet_coordinator.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <new>
#include <ranges>
#include <stop_token>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

namespace kairosboot::fleet {
namespace {

[[nodiscard]] FleetCoordinatorError invalid_argument(std::string message) {
    return FleetCoordinatorError{
        .kind = FleetCoordinatorErrorKind::InvalidArgument,
        .message = std::move(message),
    };
}

[[nodiscard]] FleetActorExecutionError synthetic_execution_error(
    const FleetActorExecutionErrorKind kind,
    std::string message,
    const std::size_t device_index) {
    return FleetActorExecutionError{
        .kind = kind,
        .message = std::move(message),
        .device_index = device_index,
        .step_index = std::nullopt,
        .completed_steps = 0U,
        .completed_data_bytes = 0U,
        .completed_child_tasks_in_step = 0U,
        .total_child_tasks_in_step = 0U,
        .device_error = std::nullopt,
    };
}

[[nodiscard]] FleetCoordinatorState derive_state(
    const std::vector<FleetCoordinatorDeviceResult>& devices,
    const bool job_cancelled) noexcept {
    if (job_cancelled) {
        return FleetCoordinatorState::Cancelled;
    }

    const auto succeeded = std::ranges::count_if(devices, [](const auto& item) {
        return item.state == FleetCoordinatorDeviceState::Succeeded;
    });
    if (succeeded == static_cast<std::ptrdiff_t>(devices.size())) {
        return FleetCoordinatorState::Succeeded;
    }
    return succeeded == 0 ? FleetCoordinatorState::Failed
                          : FleetCoordinatorState::PartiallyFailed;
}

}  // namespace

struct FleetCoordinator::Implementation final {
    std::size_t device_count{};
    ManifestPolicy policy;
    FleetCoordinatorDeviceExecutor executor;
    FleetActorExecutionObserver observer;
    FleetActorExecutionObserver serialized_observer;
    FleetCoordinatorThreadFactory thread_factory;

    std::mutex mutex;
    std::mutex observer_mutex;
    std::condition_variable start_condition;
    bool start_workers{};
    bool run_claimed{};
    bool policy_stopped{};
    bool deadline_expired{};
    std::size_t next_device{};
    std::stop_source cancellation;
    std::vector<std::jthread> workers;
    std::vector<FleetCoordinatorDeviceResult> results;
};

FleetCoordinator::FleetCoordinator(
    const std::size_t device_count,
    ManifestPolicy policy,
    FleetCoordinatorDeviceExecutor executor,
    FleetActorExecutionObserver observer,
    FleetCoordinatorTestOptions options)
    : implementation_(std::make_unique<Implementation>()) {
    implementation_->device_count = device_count;
    implementation_->policy = std::move(policy);
    implementation_->executor = std::move(executor);
    implementation_->observer = std::move(observer);
    implementation_->thread_factory = std::move(options.thread_factory);
    if (implementation_->observer) {
        auto* const state = implementation_.get();
        implementation_->serialized_observer =
            [state](const FleetActorExecutionEvent& event) {
                std::lock_guard lock(state->observer_mutex);
                state->observer(event);
            };
    }
    implementation_->results.reserve(device_count);
    for (std::size_t index = 0U; index < device_count; ++index) {
        implementation_->results.push_back(FleetCoordinatorDeviceResult{
            .device_index = index,
            .state = FleetCoordinatorDeviceState::Pending,
            .execution = std::nullopt,
            .error = std::nullopt,
        });
    }
}

FleetCoordinator::~FleetCoordinator() {
    request_cancel();
    join_workers();
}

std::expected<std::unique_ptr<FleetCoordinator>, FleetCoordinatorError>
FleetCoordinator::create(PreparedFleetActorBatch&& batch,
                         const ManifestPolicy& policy,
                         FleetActorExecutionObserver observer) {
    try {
        auto owned_batch = std::make_shared<PreparedFleetActorBatch>(
            std::move(batch));
        const auto device_count = owned_batch->report_specs().size();
        FleetCoordinatorDeviceExecutor executor =
            [owned_batch = std::move(owned_batch)](
                const std::size_t device_index,
                const fastboot::UpdateOperationContext& context,
                const FleetActorExecutionObserver& actor_observer) {
                return owned_batch->execute_device(
                    device_index, context, actor_observer);
            };
        return create_for_testing(
            device_count, policy, std::move(executor), std::move(observer), {});
    } catch (const std::bad_alloc&) {
        return std::unexpected(FleetCoordinatorError{
            .kind = FleetCoordinatorErrorKind::ResourceExhausted,
            .message = "unable to allocate the fleet coordinator",
        });
    } catch (const std::exception& error) {
        return std::unexpected(FleetCoordinatorError{
            .kind = FleetCoordinatorErrorKind::UnexpectedFailure,
            .message = std::string{"unexpected fleet coordinator creation failure: "} +
                error.what(),
        });
    } catch (...) {
        return std::unexpected(FleetCoordinatorError{
            .kind = FleetCoordinatorErrorKind::UnexpectedFailure,
            .message = "unexpected fleet coordinator creation failure",
        });
    }
}

std::expected<std::unique_ptr<FleetCoordinator>, FleetCoordinatorError>
FleetCoordinator::create_for_testing(
    const std::size_t device_count,
    const ManifestPolicy& policy,
    FleetCoordinatorDeviceExecutor executor,
    FleetActorExecutionObserver observer,
    FleetCoordinatorTestOptions options) {
    if (device_count == 0U) {
        return std::unexpected(invalid_argument(
            "fleet coordination requires at least one prepared device"));
    }
    if (policy.max_parallel_devices == 0U ||
        policy.max_parallel_devices > kFleetCoordinatorMaximumParallelDevices) {
        return std::unexpected(invalid_argument(
            "maxParallelDevices must be between 1 and 256"));
    }
    if (!executor) {
        return std::unexpected(invalid_argument(
            "fleet coordination requires a device executor"));
    }
    if (policy.on_device_failure != ManifestDeviceFailurePolicy::Continue &&
        policy.on_device_failure != ManifestDeviceFailurePolicy::Stop) {
        return std::unexpected(invalid_argument(
            "fleet coordination received an unknown device failure policy"));
    }

    try {
        return std::unique_ptr<FleetCoordinator>(new FleetCoordinator(
            device_count, policy, std::move(executor), std::move(observer),
            std::move(options)));
    } catch (const std::bad_alloc&) {
        return std::unexpected(FleetCoordinatorError{
            .kind = FleetCoordinatorErrorKind::ResourceExhausted,
            .message = "unable to allocate the fleet coordinator",
        });
    } catch (const std::exception& error) {
        return std::unexpected(FleetCoordinatorError{
            .kind = FleetCoordinatorErrorKind::UnexpectedFailure,
            .message = std::string{"unexpected fleet coordinator creation failure: "} +
                error.what(),
        });
    } catch (...) {
        return std::unexpected(FleetCoordinatorError{
            .kind = FleetCoordinatorErrorKind::UnexpectedFailure,
            .message = "unexpected fleet coordinator creation failure",
        });
    }
}

void FleetCoordinator::request_cancel() noexcept {
    if (!implementation_) {
        return;
    }
    implementation_->cancellation.request_stop();
    implementation_->start_condition.notify_all();
}

void FleetCoordinator::join_workers() noexcept {
    if (!implementation_) {
        return;
    }
    for (auto& worker : implementation_->workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    implementation_->workers.clear();
}

void FleetCoordinator::worker_loop(
    const fastboot::UpdateOperationContext& context) noexcept {
    auto& state = *implementation_;
    for (;;) {
        std::size_t device_index{};
        {
            std::unique_lock lock(state.mutex);
            state.start_condition.wait(lock, [&] {
                return state.start_workers ||
                    state.cancellation.stop_requested();
            });
            if (state.cancellation.stop_requested()) {
                return;
            }
            if (context.deadline &&
                std::chrono::steady_clock::now() >= *context.deadline) {
                state.deadline_expired = true;
                return;
            }
            if (state.policy_stopped ||
                state.next_device >= state.device_count) {
                return;
            }
            device_index = state.next_device++;
            state.results[device_index].state =
                FleetCoordinatorDeviceState::Running;
        }

        try {
            auto executed = state.executor(
                device_index, context, state.serialized_observer);
            std::lock_guard lock(state.mutex);
            auto& result = state.results[device_index];
            if (executed) {
                result.state = FleetCoordinatorDeviceState::Succeeded;
                result.execution = std::move(*executed);
            } else {
                result.state =
                    executed.error().kind ==
                            FleetActorExecutionErrorKind::Cancelled
                        ? FleetCoordinatorDeviceState::Cancelled
                        : FleetCoordinatorDeviceState::Failed;
                result.error = std::move(executed.error());
                if (state.policy.on_device_failure ==
                    ManifestDeviceFailurePolicy::Stop) {
                    state.policy_stopped = true;
                }
            }
        } catch (...) {
            // The worker boundary must never let an executor exception escape
            // the thread. Keep this fallback allocation-free so a bad_alloc
            // from the executor cannot recurse into std::terminate.
            std::lock_guard lock(state.mutex);
            auto& result = state.results[device_index];
            result.state = FleetCoordinatorDeviceState::Failed;
            result.error = synthetic_execution_error(
                FleetActorExecutionErrorKind::UnexpectedFailure, {},
                device_index);
            if (state.policy.on_device_failure ==
                ManifestDeviceFailurePolicy::Stop) {
                state.policy_stopped = true;
            }
        }
    }
}

std::expected<FleetCoordinatorResult, FleetCoordinatorError>
FleetCoordinator::run(const fastboot::UpdateOperationContext& context) {
    auto& state = *implementation_;
    {
        std::lock_guard lock(state.mutex);
        if (state.run_claimed) {
            return std::unexpected(FleetCoordinatorError{
                .kind = FleetCoordinatorErrorKind::AlreadyRun,
                .message = "fleet coordinator may run only once",
            });
        }
        state.run_claimed = true;
    }

    std::stop_callback external_cancel(context.cancellation, [this] {
        request_cancel();
    });
    const fastboot::UpdateOperationContext worker_context{
        .cancellation = state.cancellation.get_token(),
        .deadline = context.deadline,
    };
    if (state.cancellation.stop_requested()) {
        for (auto& result : state.results) {
            result.state = FleetCoordinatorDeviceState::Cancelled;
            result.error = synthetic_execution_error(
                FleetActorExecutionErrorKind::Cancelled,
                "fleet device was cancelled before execution",
                result.device_index);
        }
        return FleetCoordinatorResult{
            .state = FleetCoordinatorState::Cancelled,
            .devices = std::move(state.results),
        };
    }
    if (context.deadline &&
        std::chrono::steady_clock::now() >= *context.deadline) {
        state.deadline_expired = true;
        for (auto& result : state.results) {
            result.state = FleetCoordinatorDeviceState::Failed;
            result.error = synthetic_execution_error(
                FleetActorExecutionErrorKind::TimedOut,
                "fleet device deadline expired before execution",
                result.device_index);
        }
        return FleetCoordinatorResult{
            .state = FleetCoordinatorState::Failed,
            .devices = std::move(state.results),
        };
    }
    const auto worker_count = std::min<std::size_t>(
        state.device_count, state.policy.max_parallel_devices);

    try {
        state.workers.reserve(worker_count);
        for (std::size_t index = 0U; index < worker_count; ++index) {
            FleetCoordinatorWorker worker = [this, worker_context] {
                worker_loop(worker_context);
            };
            if (state.thread_factory) {
                state.workers.emplace_back(
                    state.thread_factory(std::move(worker)));
            } else {
                state.workers.emplace_back(std::move(worker));
            }
            if (!state.workers.back().joinable()) {
                throw std::system_error(
                    std::make_error_code(
                        std::errc::resource_unavailable_try_again),
                    "fleet thread factory returned a non-joinable worker");
            }
        }
    } catch (const std::system_error& error) {
        request_cancel();
        {
            std::lock_guard lock(state.mutex);
            state.start_workers = true;
        }
        state.start_condition.notify_all();
        join_workers();
        return std::unexpected(FleetCoordinatorError{
            .kind = FleetCoordinatorErrorKind::ResourceExhausted,
            .message = std::string{"unable to create fleet worker threads: "} +
                error.what(),
        });
    } catch (const std::bad_alloc&) {
        request_cancel();
        {
            std::lock_guard lock(state.mutex);
            state.start_workers = true;
        }
        state.start_condition.notify_all();
        join_workers();
        return std::unexpected(FleetCoordinatorError{
            .kind = FleetCoordinatorErrorKind::ResourceExhausted,
            .message = "unable to allocate fleet worker threads",
        });
    }

    {
        std::lock_guard lock(state.mutex);
        state.start_workers = true;
    }
    state.start_condition.notify_all();
    join_workers();

    const bool job_cancelled = state.cancellation.stop_requested();
    for (auto& result : state.results) {
        if (result.state != FleetCoordinatorDeviceState::Pending) {
            continue;
        }
        if (job_cancelled) {
            result.state = FleetCoordinatorDeviceState::Cancelled;
            result.error = synthetic_execution_error(
                FleetActorExecutionErrorKind::Cancelled,
                "fleet device was cancelled before execution",
                result.device_index);
        } else if (state.deadline_expired) {
            result.state = FleetCoordinatorDeviceState::Failed;
            result.error = synthetic_execution_error(
                FleetActorExecutionErrorKind::TimedOut,
                "fleet device deadline expired before execution",
                result.device_index);
        } else {
            result.state = FleetCoordinatorDeviceState::Skipped;
        }
    }

    const auto final_state = derive_state(state.results, job_cancelled);
    return FleetCoordinatorResult{
        .state = final_state,
        .devices = std::move(state.results),
    };
}

}  // namespace kairosboot::fleet
