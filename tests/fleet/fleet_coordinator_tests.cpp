// SPDX-License-Identifier: MIT

#include "src/fleet/fleet_coordinator.hpp"

#include <algorithm>
#include <atomic>
#include <barrier>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <stop_token>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace {

using kairosboot::fastboot::UpdateOperationContext;
using kairosboot::fleet::FleetActorDeviceExecution;
using kairosboot::fleet::FleetActorExecutionError;
using kairosboot::fleet::FleetActorExecutionErrorKind;
using kairosboot::fleet::FleetActorExecutionEvent;
using kairosboot::fleet::FleetActorExecutionEventKind;
using kairosboot::fleet::FleetCoordinator;
using kairosboot::fleet::FleetCoordinatorDeviceState;
using kairosboot::fleet::FleetCoordinatorErrorKind;
using kairosboot::fleet::FleetCoordinatorState;
using kairosboot::fleet::FleetCoordinatorTestOptions;
using kairosboot::fleet::ManifestDeviceFailurePolicy;
using kairosboot::fleet::ManifestPolicy;

[[noreturn]] void fail(const char* expression, const int line) {
    std::cerr << "CHECK failed at line " << line << ": " << expression << '\n';
    std::exit(1);
}

#define CHECK(expression) \
    do {                  \
        if (!(expression)) { \
            fail(#expression, __LINE__); \
        }                 \
    } while (false)

[[nodiscard]] ManifestPolicy policy(
    const std::uint32_t parallel,
    const ManifestDeviceFailurePolicy failure =
        ManifestDeviceFailurePolicy::Continue) {
    ManifestPolicy result;
    result.max_parallel_devices = parallel;
    result.on_device_failure = failure;
    return result;
}

[[nodiscard]] FleetActorExecutionError actor_error(
    const std::size_t index,
    const FleetActorExecutionErrorKind kind,
    std::string message = "scripted failure") {
    return FleetActorExecutionError{
        .kind = kind,
        .message = std::move(message),
        .device_index = index,
        .step_index = std::nullopt,
        .completed_steps = 0U,
        .completed_data_bytes = 0U,
        .completed_child_tasks_in_step = 0U,
        .total_child_tasks_in_step = 0U,
        .device_error = std::nullopt,
    };
}

struct ConcurrencyProbe final {
    std::mutex mutex;
    std::condition_variable condition;
    std::size_t active{};
    std::size_t peak{};
    std::size_t barrier_target{};
    bool released{};
    std::vector<std::size_t> calls;
};

[[nodiscard]] auto successful_executor(
    const std::shared_ptr<ConcurrencyProbe>& probe) {
    return [probe](const std::size_t index,
                   const UpdateOperationContext&,
                   const auto&)
        -> std::expected<FleetActorDeviceExecution,
                         FleetActorExecutionError> {
        std::unique_lock lock(probe->mutex);
        probe->calls[index] += 1U;
        ++probe->active;
        probe->peak = std::max(probe->peak, probe->active);
        if (probe->barrier_target != 0U) {
            if (probe->active >= probe->barrier_target) {
                probe->released = true;
                probe->condition.notify_all();
            } else {
                probe->condition.wait(lock, [&] { return probe->released; });
            }
        }
        --probe->active;
        return FleetActorDeviceExecution{
            .completed_steps = index + 1U,
            .completed_data_bytes = 100U + index,
        };
    };
}

void invalid_inputs_are_rejected_without_execution() {
    auto never = [](const std::size_t,
                    const UpdateOperationContext&,
                    const auto&)
        -> std::expected<FleetActorDeviceExecution,
                         FleetActorExecutionError> {
        std::abort();
    };

    auto empty = FleetCoordinator::create_for_testing(0U, policy(1U), never);
    CHECK(!empty);
    CHECK(empty.error().kind == FleetCoordinatorErrorKind::InvalidArgument);

    auto zero = FleetCoordinator::create_for_testing(1U, policy(0U), never);
    CHECK(!zero);
    CHECK(zero.error().kind == FleetCoordinatorErrorKind::InvalidArgument);

    auto too_large =
        FleetCoordinator::create_for_testing(1U, policy(257U), never);
    CHECK(!too_large);
    CHECK(too_large.error().kind ==
          FleetCoordinatorErrorKind::InvalidArgument);

    auto unknown_policy = policy(1U);
    unknown_policy.on_device_failure =
        static_cast<ManifestDeviceFailurePolicy>(255U);
    auto unknown =
        FleetCoordinator::create_for_testing(1U, unknown_policy, never);
    CHECK(!unknown);
    CHECK(unknown.error().kind ==
          FleetCoordinatorErrorKind::InvalidArgument);

    auto missing = FleetCoordinator::create_for_testing(
        1U, policy(1U), kairosboot::fleet::FleetCoordinatorDeviceExecutor{});
    CHECK(!missing);
    CHECK(missing.error().kind ==
          FleetCoordinatorErrorKind::InvalidArgument);
}

void two_devices_obey_limit_and_run_once() {
    auto probe = std::make_shared<ConcurrencyProbe>();
    probe->calls.resize(2U);
    auto coordinator = FleetCoordinator::create_for_testing(
        2U, policy(1U), successful_executor(probe));
    CHECK(coordinator);
    auto result = (*coordinator)->run();
    CHECK(result);
    CHECK(result->state == FleetCoordinatorState::Succeeded);
    CHECK(probe->peak == 1U);
    CHECK(probe->calls == std::vector<std::size_t>({1U, 1U}));
    CHECK(result->devices.size() == 2U);
    for (std::size_t index = 0U; index < result->devices.size(); ++index) {
        const auto& device = result->devices[index];
        CHECK(device.device_index == index);
        CHECK(device.state == FleetCoordinatorDeviceState::Succeeded);
        CHECK(device.execution.has_value());
        CHECK(!device.error.has_value());
        CHECK(device.execution->completed_steps == index + 1U);
    }

    auto second = (*coordinator)->run();
    CHECK(!second);
    CHECK(second.error().kind == FleetCoordinatorErrorKind::AlreadyRun);
    CHECK(probe->calls == std::vector<std::size_t>({1U, 1U}));
}

void exact_parallel_limit_is_not_silently_capped() {
    struct Case final {
        std::size_t device_count;
        std::uint32_t parallel;
        std::size_t expected_peak;
    };
    for (const auto test : {
             Case{
                 .device_count = 32U,
                 .parallel = 32U,
                 .expected_peak = 32U,
             },
             Case{
                 .device_count = 33U,
                 .parallel = 32U,
                 .expected_peak = 32U,
             },
             Case{
                 .device_count = 33U,
                 .parallel = 33U,
                 .expected_peak = 33U,
             },
         }) {
        auto probe = std::make_shared<ConcurrencyProbe>();
        probe->calls.resize(test.device_count);
        probe->barrier_target = test.expected_peak;
        auto coordinator = FleetCoordinator::create_for_testing(
            test.device_count, policy(test.parallel), successful_executor(probe));
        CHECK(coordinator);
        auto result = (*coordinator)->run();
        CHECK(result);
        CHECK(result->state == FleetCoordinatorState::Succeeded);
        CHECK(probe->peak == test.expected_peak);
        CHECK(std::ranges::all_of(probe->calls, [](const auto calls) {
            return calls == 1U;
        }));
        CHECK(result->devices.size() == test.device_count);
    }
}

void continue_policy_isolates_device_and_observer_failures() {
    std::vector<std::size_t> calls(4U);
    std::mutex mutex;
    const auto observer = [](const FleetActorExecutionEvent&) {
        throw std::runtime_error("observer rejected event");
    };
    auto executor = [&](const std::size_t index,
                        const UpdateOperationContext&,
                        const auto& actor_observer)
        -> std::expected<FleetActorDeviceExecution,
                         FleetActorExecutionError> {
        {
            std::lock_guard lock(mutex);
            ++calls[index];
        }
        if (index == 0U) {
            return std::unexpected(actor_error(
                index, FleetActorExecutionErrorKind::DeviceTaskFailed));
        }
        if (index == 1U) {
            try {
                actor_observer(FleetActorExecutionEvent{
                    .kind = FleetActorExecutionEventKind::DeviceStarted,
                    .device_index = index,
                    .step_index = std::nullopt,
                    .completed_steps = 0U,
                    .completed_data_bytes = 0U,
                });
            } catch (...) {
                return std::unexpected(actor_error(
                    index, FleetActorExecutionErrorKind::ObserverFailed,
                    "observer rejected event"));
            }
        }
        return FleetActorDeviceExecution{};
    };
    auto coordinator = FleetCoordinator::create_for_testing(
        4U, policy(2U), executor, observer);
    CHECK(coordinator);
    auto result = (*coordinator)->run();
    CHECK(result);
    CHECK(result->state == FleetCoordinatorState::PartiallyFailed);
    CHECK(calls == std::vector<std::size_t>({1U, 1U, 1U, 1U}));
    CHECK(result->devices[0].state == FleetCoordinatorDeviceState::Failed);
    CHECK(result->devices[0].error->kind ==
          FleetActorExecutionErrorKind::DeviceTaskFailed);
    CHECK(result->devices[1].state == FleetCoordinatorDeviceState::Failed);
    CHECK(result->devices[1].error->kind ==
          FleetActorExecutionErrorKind::ObserverFailed);
    CHECK(result->devices[2].state == FleetCoordinatorDeviceState::Succeeded);
    CHECK(result->devices[3].state == FleetCoordinatorDeviceState::Succeeded);
}

void stop_policy_skips_every_actor_not_yet_started() {
    std::vector<std::size_t> calls(4U);
    auto executor = [&](const std::size_t index,
                        const UpdateOperationContext&,
                        const auto&)
        -> std::expected<FleetActorDeviceExecution,
                         FleetActorExecutionError> {
        ++calls[index];
        return std::unexpected(actor_error(
            index, FleetActorExecutionErrorKind::DeviceTaskFailed));
    };
    auto coordinator = FleetCoordinator::create_for_testing(
        4U, policy(1U, ManifestDeviceFailurePolicy::Stop), executor);
    CHECK(coordinator);
    auto result = (*coordinator)->run();
    CHECK(result);
    CHECK(result->state == FleetCoordinatorState::Failed);
    CHECK(calls == std::vector<std::size_t>({1U, 0U, 0U, 0U}));
    CHECK(result->devices[0].state == FleetCoordinatorDeviceState::Failed);
    for (std::size_t index = 1U; index < 4U; ++index) {
        CHECK(result->devices[index].state ==
              FleetCoordinatorDeviceState::Skipped);
        CHECK(!result->devices[index].execution.has_value());
        CHECK(!result->devices[index].error.has_value());
    }
}

void stop_policy_drains_in_flight_actor_without_replacement() {
    struct StopState final {
        std::mutex mutex;
        std::condition_variable condition;
        bool second_started{};
        bool failure_worker_exited{};
        std::vector<std::size_t> calls = std::vector<std::size_t>(4U);
    } state;
    auto executor = [&](const std::size_t index,
                        const UpdateOperationContext&,
                        const auto&)
        -> std::expected<FleetActorDeviceExecution,
                         FleetActorExecutionError> {
        ++state.calls[index];
        if (index == 0U) {
            std::unique_lock lock(state.mutex);
            state.condition.wait(lock, [&] { return state.second_started; });
            return std::unexpected(actor_error(
                index, FleetActorExecutionErrorKind::DeviceTaskFailed));
        }
        if (index == 1U) {
            {
                std::lock_guard lock(state.mutex);
                state.second_started = true;
            }
            state.condition.notify_all();
            std::unique_lock lock(state.mutex);
            // With this actor blocked, only device 0's worker can exit. That
            // exit acknowledges that its failure and Stop policy were latched.
            state.condition.wait(lock, [&] {
                return state.failure_worker_exited;
            });
        }
        return FleetActorDeviceExecution{};
    };
    auto coordinator = FleetCoordinator::create_for_testing(
        4U,
        policy(2U, ManifestDeviceFailurePolicy::Stop),
        executor,
        {},
        FleetCoordinatorTestOptions{
            .thread_factory = [&state](auto worker) {
                return std::jthread(
                    [&state, worker = std::move(worker)]() mutable {
                        worker();
                        {
                            std::lock_guard lock(state.mutex);
                            state.failure_worker_exited = true;
                        }
                        state.condition.notify_all();
                    });
            },
        });
    CHECK(coordinator);
    auto result = (*coordinator)->run();
    CHECK(result);
    CHECK(result->state == FleetCoordinatorState::PartiallyFailed);
    CHECK(state.calls == std::vector<std::size_t>({1U, 1U, 0U, 0U}));
    CHECK(result->devices[0].state == FleetCoordinatorDeviceState::Failed);
    CHECK(result->devices[1].state == FleetCoordinatorDeviceState::Succeeded);
    CHECK(result->devices[2].state == FleetCoordinatorDeviceState::Skipped);
    CHECK(result->devices[3].state == FleetCoordinatorDeviceState::Skipped);
}

void observer_is_globally_serialized_across_devices() {
    constexpr std::size_t count = 8U;
    std::barrier ready(static_cast<std::ptrdiff_t>(count));
    std::atomic<std::size_t> observer_active{};
    std::atomic<std::size_t> observer_peak{};
    auto observer = [&](const FleetActorExecutionEvent&) {
        const auto active = observer_active.fetch_add(1U) + 1U;
        auto peak = observer_peak.load();
        while (peak < active &&
               !observer_peak.compare_exchange_weak(peak, active)) {
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        observer_active.fetch_sub(1U);
    };
    auto executor = [&](const std::size_t index,
                        const UpdateOperationContext&,
                        const auto& actor_observer)
        -> std::expected<FleetActorDeviceExecution,
                         FleetActorExecutionError> {
        ready.arrive_and_wait();
        actor_observer(FleetActorExecutionEvent{
            .kind = FleetActorExecutionEventKind::DeviceStarted,
            .device_index = index,
            .step_index = std::nullopt,
            .completed_steps = 0U,
            .completed_data_bytes = 0U,
        });
        return FleetActorDeviceExecution{};
    };
    auto coordinator = FleetCoordinator::create_for_testing(
        count, policy(static_cast<std::uint32_t>(count)), executor, observer);
    CHECK(coordinator);
    auto result = (*coordinator)->run();
    CHECK(result);
    CHECK(result->state == FleetCoordinatorState::Succeeded);
    CHECK(observer_peak.load() == 1U);
}

void observer_can_cancel_and_reenter_run_without_deadlock() {
    FleetCoordinator* coordinator_address{};
    std::optional<FleetCoordinatorErrorKind> reentrant_error;
    auto observer = [&](const FleetActorExecutionEvent&) {
        auto reentrant = coordinator_address->run();
        CHECK(!reentrant);
        reentrant_error = reentrant.error().kind;
        coordinator_address->request_cancel();
    };
    auto executor = [&](const std::size_t index,
                        const UpdateOperationContext& context,
                        const auto& actor_observer)
        -> std::expected<FleetActorDeviceExecution,
                         FleetActorExecutionError> {
        actor_observer(FleetActorExecutionEvent{
            .kind = FleetActorExecutionEventKind::DeviceStarted,
            .device_index = index,
            .step_index = std::nullopt,
            .completed_steps = 0U,
            .completed_data_bytes = 0U,
        });
        if (context.cancellation.stop_requested()) {
            return std::unexpected(actor_error(
                index, FleetActorExecutionErrorKind::Cancelled));
        }
        return FleetActorDeviceExecution{};
    };
    auto coordinator = FleetCoordinator::create_for_testing(
        1U, policy(1U), executor, observer);
    CHECK(coordinator);
    coordinator_address = coordinator->get();
    auto result = (*coordinator)->run();
    CHECK(result);
    CHECK(reentrant_error == FleetCoordinatorErrorKind::AlreadyRun);
    CHECK(result->state == FleetCoordinatorState::Cancelled);
    CHECK(result->devices[0].state == FleetCoordinatorDeviceState::Cancelled);
}

void cancellation_wakes_in_flight_work_and_drains_pending_devices() {
    struct CancelState final {
        std::mutex mutex;
        std::condition_variable condition;
        std::size_t started{};
        std::vector<std::size_t> calls = std::vector<std::size_t>(4U);
    } state;

    auto executor = [&](const std::size_t index,
                        const UpdateOperationContext& context,
                        const auto&)
        -> std::expected<FleetActorDeviceExecution,
                         FleetActorExecutionError> {
        std::unique_lock lock(state.mutex);
        ++state.calls[index];
        ++state.started;
        state.condition.notify_all();
        std::stop_callback wake(context.cancellation, [&] {
            state.condition.notify_all();
        });
        state.condition.wait(lock, [&] {
            return context.cancellation.stop_requested();
        });
        return std::unexpected(actor_error(
            index, FleetActorExecutionErrorKind::Cancelled, "job cancelled"));
    };

    auto coordinator = FleetCoordinator::create_for_testing(
        4U, policy(2U), executor);
    CHECK(coordinator);
    std::optional<kairosboot::fleet::FleetCoordinatorResult> result;
    std::optional<kairosboot::fleet::FleetCoordinatorError> error;
    std::thread runner([&] {
        auto completed = (*coordinator)->run();
        if (completed) {
            result = std::move(*completed);
        } else {
            error = std::move(completed.error());
        }
    });
    {
        std::unique_lock lock(state.mutex);
        state.condition.wait(lock, [&] { return state.started == 2U; });
    }
    (*coordinator)->request_cancel();
    runner.join();

    CHECK(result.has_value());
    CHECK(!error.has_value());
    CHECK(result->state == FleetCoordinatorState::Cancelled);
    CHECK(state.calls == std::vector<std::size_t>({1U, 1U, 0U, 0U}));
    CHECK(std::ranges::all_of(result->devices, [](const auto& device) {
        return device.state == FleetCoordinatorDeviceState::Cancelled &&
            device.error.has_value() &&
            device.error->kind == FleetActorExecutionErrorKind::Cancelled;
    }));
}

void cancellation_before_run_dispatches_no_actor() {
    std::vector<std::size_t> calls(3U);
    std::size_t thread_factory_calls{};
    auto executor = [&](const std::size_t index,
                        const UpdateOperationContext&,
                        const auto&) {
        ++calls[index];
        return std::expected<FleetActorDeviceExecution,
                             FleetActorExecutionError>{
            FleetActorDeviceExecution{}};
    };
    FleetCoordinatorTestOptions options;
    options.thread_factory = [&](kairosboot::fleet::FleetCoordinatorWorker)
        -> std::jthread {
        ++thread_factory_calls;
        throw std::system_error(std::make_error_code(
            std::errc::resource_unavailable_try_again));
    };
    auto coordinator = FleetCoordinator::create_for_testing(
        3U, policy(2U), executor, {}, std::move(options));
    CHECK(coordinator);
    (*coordinator)->request_cancel();
    auto result = (*coordinator)->run();
    CHECK(result);
    CHECK(result->state == FleetCoordinatorState::Cancelled);
    CHECK(thread_factory_calls == 0U);
    CHECK(calls == std::vector<std::size_t>({0U, 0U, 0U}));
    CHECK(std::ranges::all_of(result->devices, [](const auto& device) {
        return device.state == FleetCoordinatorDeviceState::Cancelled &&
            device.error.has_value();
    }));
}

void expired_deadline_dispatches_no_remaining_actor() {
    std::vector<std::size_t> calls(3U);
    std::size_t thread_factory_calls{};
    auto executor = [&](const std::size_t index,
                        const UpdateOperationContext&,
                        const auto&) {
        ++calls[index];
        return std::expected<FleetActorDeviceExecution,
                             FleetActorExecutionError>{
            FleetActorDeviceExecution{}};
    };
    FleetCoordinatorTestOptions options;
    options.thread_factory =
        [&](kairosboot::fleet::FleetCoordinatorWorker worker) {
            ++thread_factory_calls;
            return std::jthread(std::move(worker));
        };
    auto coordinator = FleetCoordinator::create_for_testing(
        3U, policy(2U), executor, {}, std::move(options));
    CHECK(coordinator);
    auto result = (*coordinator)->run(UpdateOperationContext{
        .cancellation = {},
        .deadline = std::chrono::steady_clock::now(),
    });
    CHECK(result);
    CHECK(result->state == FleetCoordinatorState::Failed);
    CHECK(thread_factory_calls == 0U);
    CHECK(calls == std::vector<std::size_t>({0U, 0U, 0U}));
    CHECK(std::ranges::all_of(result->devices, [](const auto& device) {
        return device.state == FleetCoordinatorDeviceState::Failed &&
            device.error.has_value() &&
            device.error->kind == FleetActorExecutionErrorKind::TimedOut;
    }));
}

void partial_thread_creation_failure_executes_no_actor() {
    std::atomic<std::size_t> actor_calls{};
    std::size_t thread_factory_calls{};
    auto executor = [&](const std::size_t,
                        const UpdateOperationContext&,
                        const auto&) {
        actor_calls.fetch_add(1U);
        return std::expected<FleetActorDeviceExecution,
                             FleetActorExecutionError>{
            FleetActorDeviceExecution{}};
    };
    FleetCoordinatorTestOptions options;
    options.thread_factory =
        [&](kairosboot::fleet::FleetCoordinatorWorker worker)
            -> std::jthread {
            ++thread_factory_calls;
            if (thread_factory_calls == 3U) {
                throw std::system_error(std::make_error_code(
                    std::errc::resource_unavailable_try_again));
            }
            return std::jthread(std::move(worker));
        };
    auto coordinator = FleetCoordinator::create_for_testing(
        4U, policy(4U), executor, {}, std::move(options));
    CHECK(coordinator);
    auto result = (*coordinator)->run();
    CHECK(!result);
    CHECK(result.error().kind ==
          FleetCoordinatorErrorKind::ResourceExhausted);
    CHECK(thread_factory_calls == 3U);
    CHECK(actor_calls.load() == 0U);
}

}  // namespace

int main() {
    invalid_inputs_are_rejected_without_execution();
    two_devices_obey_limit_and_run_once();
    exact_parallel_limit_is_not_silently_capped();
    continue_policy_isolates_device_and_observer_failures();
    stop_policy_skips_every_actor_not_yet_started();
    stop_policy_drains_in_flight_actor_without_replacement();
    observer_is_globally_serialized_across_devices();
    observer_can_cancel_and_reenter_run_without_deadlock();
    cancellation_wakes_in_flight_work_and_drains_pending_devices();
    cancellation_before_run_dispatches_no_actor();
    expired_deadline_dispatches_no_remaining_actor();
    partial_thread_creation_failure_executes_no_actor();
    return 0;
}
