// SPDX-License-Identifier: MIT
#include "src/api/operation_state.hpp"

#include <atomic>
#include <chrono>
#include <exception>
#include <future>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using kairosboot::api::CommandMessageKind;
using kairosboot::api::CommandMessagePayload;
using kairosboot::api::CommandResultPayload;
using kairosboot::api::OperationErrorPayload;
using kairosboot::api::OperationOutcome;
using kairosboot::api::OperationPhase;
using kairosboot::api::OperationState;
using kairosboot::api::OperationWaitResult;

#define CHECK(condition)                                                         \
    do {                                                                         \
        if (!(condition)) {                                                       \
            throw std::runtime_error(                                             \
                std::string("check failed at line ") + std::to_string(__LINE__) + \
                ": " #condition);                                                \
        }                                                                        \
    } while (false)

void created_running_succeeded_and_wait_deadlines() {
    std::promise<void> entered;
    auto entered_future = entered.get_future();
    std::promise<void> release;
    auto release_future = release.get_future().share();

    OperationState operation([&](OperationState::TaskContext&) {
        entered.set_value();
        release_future.wait();
        return OperationOutcome::succeeded();
    });

    CHECK(operation.phase() == OperationPhase::Created);
    CHECK(operation.start());
    entered_future.wait();
    CHECK(operation.phase() == OperationPhase::Running);
    CHECK(operation.status() == KB_E_BUSY);
    CHECK(operation.wait_for(0ms) == OperationWaitResult::Timeout);
    CHECK(operation.wait_for(10ms) == OperationWaitResult::Timeout);

    release.set_value();
    operation.wait();
    CHECK(operation.wait_for(0ms) == OperationWaitResult::Terminal);
    CHECK(operation.phase() == OperationPhase::Succeeded);
    CHECK(operation.status() == KB_OK);
    CHECK(!operation.error().has_value());
    CHECK(!operation.start());
}

void cancellation_is_idempotent_and_publishes_after_drain() {
    std::promise<void> task_entered;
    auto task_entered_future = task_entered.get_future();
    std::promise<void> hook_entered;
    auto hook_entered_future = hook_entered.get_future();
    std::promise<void> release_hook;
    auto release_hook_future = release_hook.get_future().share();
    std::promise<void> release_task;
    auto release_task_future = release_task.get_future().share();
    std::atomic<unsigned int> hook_calls{0};
    std::atomic<bool> hook_observed_running{false};
    OperationState* operation_address = nullptr;

    OperationState operation([&](OperationState::TaskContext& context) {
        auto registration = context.register_cancellation_hook([&] {
            hook_calls.fetch_add(1, std::memory_order_relaxed);
            hook_observed_running.store(
                operation_address->phase() == OperationPhase::Running &&
                    operation_address->cancel_requested(),
                std::memory_order_release);
            hook_entered.set_value();
            release_hook_future.wait();
        });
        CHECK(registration.active());
        task_entered.set_value();
        while (!context.cancel_requested()) {
            std::this_thread::yield();
        }
        release_task_future.wait();
        return OperationOutcome::cancelled({
            KB_E_CANCELLED,
            "cancelled after transport drain",
            17,
            KB_TRANSFER_PARTIAL_OR_UNKNOWN,
            {},
        });
    });
    operation_address = &operation;

    CHECK(operation.start());
    task_entered_future.wait();

    std::thread first_cancel([&] { operation.cancel(); });
    hook_entered_future.wait();
    std::vector<std::thread> duplicate_cancellers;
    for (int index = 0; index < 4; ++index) {
        duplicate_cancellers.emplace_back([&] { operation.cancel(); });
    }
    for (auto& canceller : duplicate_cancellers) {
        canceller.join();
    }

    CHECK(operation.phase() == OperationPhase::Running);
    CHECK(operation.wait_for(0ms) == OperationWaitResult::Timeout);
    CHECK(hook_calls.load(std::memory_order_relaxed) == 1);

    std::atomic<unsigned int> completed_waiters{0};
    std::vector<std::thread> waiters;
    for (int index = 0; index < 4; ++index) {
        waiters.emplace_back([&] {
            operation.wait();
            completed_waiters.fetch_add(1, std::memory_order_relaxed);
        });
    }

    release_task.set_value();
    CHECK(operation.wait_for(10ms) == OperationWaitResult::Timeout);
    CHECK(operation.phase() == OperationPhase::Running);

    release_hook.set_value();
    first_cancel.join();
    CHECK(hook_observed_running.load(std::memory_order_acquire));
    for (auto& waiter : waiters) {
        waiter.join();
    }
    CHECK(completed_waiters.load(std::memory_order_relaxed) == 4);
    CHECK(operation.phase() == OperationPhase::Cancelled);
    const auto error = operation.error();
    CHECK(error.has_value());
    CHECK(error->status == KB_E_CANCELLED);
    CHECK(operation.status() == KB_E_CANCELLED);
    CHECK(error->message == "cancelled after transport drain");
    CHECK(error->native_code == 17);
    CHECK(error->transfer_state == KB_TRANSFER_PARTIAL_OR_UNKNOWN);
}

void cancel_before_start_is_terminal_and_skips_task() {
    std::atomic<bool> invoked{false};
    OperationState operation([&](OperationState::TaskContext&) {
        invoked.store(true, std::memory_order_release);
        return OperationOutcome::succeeded();
    });

    operation.cancel();
    operation.cancel();
    CHECK(operation.cancel_requested());
    CHECK(operation.phase() == OperationPhase::Cancelled);
    CHECK(operation.wait_for(0ms) == OperationWaitResult::Terminal);
    CHECK(!operation.start());
    CHECK(!invoked.load(std::memory_order_acquire));
    const auto error = operation.error();
    CHECK(error.has_value());
    CHECK(error->status == KB_E_CANCELLED);
}

void failure_payload_is_synchronized_and_immutable() {
    const OperationErrorPayload expected{
        KB_E_IO,
        "short USB transfer",
        -7,
        KB_TRANSFER_PARTIAL_OR_UNKNOWN,
        "usb:2-3",
    };
    OperationState operation([&](OperationState::TaskContext&) {
        return OperationOutcome::failed(expected);
    });

    CHECK(operation.start());
    operation.wait();
    CHECK(operation.phase() == OperationPhase::Failed);
    const auto first = operation.error();
    const auto second = operation.error();
    CHECK(first.has_value());
    CHECK(second.has_value());
    CHECK(*first == expected);
    CHECK(*second == expected);
    CHECK(operation.status() == KB_E_IO);
    CHECK(!operation.command_result());
}

void successful_command_result_is_shared_and_immutable() {
    auto expected = std::make_shared<const CommandResultPayload>(
        CommandResultPayload{
            .terminal_payload = "product_a",
            .messages = {
                CommandMessagePayload{CommandMessageKind::Info, "probing"},
                CommandMessagePayload{CommandMessageKind::Text, "ready"},
            },
        });
    std::weak_ptr<const CommandResultPayload> weak = expected;
    OperationState operation(
        [expected](OperationState::TaskContext&) {
            return OperationOutcome::succeeded(expected);
        });
    expected.reset();

    CHECK(operation.start());
    operation.wait();
    const auto first = operation.command_result();
    const auto second = operation.command_result();
    CHECK(first);
    CHECK(second);
    CHECK(first == second);
    CHECK(first->terminal_payload == "product_a");
    CHECK(first->messages.size() == 2);
    const CommandMessagePayload expected_info{CommandMessageKind::Info, "probing"};
    const CommandMessagePayload expected_text{CommandMessageKind::Text, "ready"};
    CHECK(first->messages[0] == expected_info);
    CHECK(first->messages[1] == expected_text);
    CHECK(!weak.expired());
}

void successful_task_is_cancelled_when_cancel_wins_publication() {
    std::promise<void> entered;
    auto entered_future = entered.get_future();
    std::promise<void> finish;
    auto finish_future = finish.get_future().share();
    OperationState operation([&](OperationState::TaskContext&) {
        entered.set_value();
        finish_future.wait();
        return OperationOutcome::succeeded();
    });

    CHECK(operation.start());
    entered_future.wait();
    operation.cancel();
    finish.set_value();
    operation.wait();
    CHECK(operation.phase() == OperationPhase::Cancelled);
    CHECK(operation.error()->status == KB_E_CANCELLED);
    CHECK(operation.error()->transfer_state == KB_TRANSFER_FULLY_TRANSFERRED);
    CHECK(!operation.command_result());
}

void task_payload_outlives_initiating_reference() {
    struct Payload final {
        explicit Payload(std::atomic<bool>& destroyed_value)
            : destroyed(destroyed_value) {}
        ~Payload() { destroyed.store(true, std::memory_order_release); }
        std::atomic<bool>& destroyed;
    };

    std::atomic<bool> destroyed{false};
    std::promise<void> entered;
    auto entered_future = entered.get_future();
    std::promise<void> finish;
    auto finish_future = finish.get_future().share();
    auto payload = std::make_shared<Payload>(destroyed);
    std::weak_ptr<Payload> weak_payload = payload;

    {
        OperationState operation(
            [payload, &entered, finish_future](OperationState::TaskContext&) {
                CHECK(!payload->destroyed.load(std::memory_order_acquire));
                entered.set_value();
                finish_future.wait();
                return OperationOutcome::succeeded();
            });
        payload.reset();
        CHECK(operation.start());
        entered_future.wait();
        CHECK(!weak_payload.expired());
        finish.set_value();
        operation.wait();
        CHECK(operation.phase() == OperationPhase::Succeeded);
    }

    CHECK(weak_payload.expired());
    CHECK(destroyed.load(std::memory_order_acquire));
}

void destructor_cancels_joins_and_leaves_no_callback() {
    std::promise<void> task_entered;
    auto task_entered_future = task_entered.get_future();
    std::atomic<unsigned int> hook_calls{0};
    std::atomic<bool> task_finished{false};

    {
        auto operation = std::make_unique<OperationState>(
            [&](OperationState::TaskContext& context) {
                auto registration = context.register_cancellation_hook([&] {
                    hook_calls.fetch_add(1, std::memory_order_relaxed);
                });
                CHECK(registration.active());
                task_entered.set_value();
                while (!context.cancellation_token().stop_requested()) {
                    std::this_thread::yield();
                }
                task_finished.store(true, std::memory_order_release);
                return OperationOutcome::cancelled();
            });
        CHECK(operation->start());
        task_entered_future.wait();
    }

    CHECK(task_finished.load(std::memory_order_acquire));
    CHECK(hook_calls.load(std::memory_order_relaxed) == 1);
}

void external_release_waits_for_running_callback() {
    std::promise<void> callback_entered;
    auto callback_entered_future = callback_entered.get_future();
    std::promise<void> release_callback;
    auto release_callback_future = release_callback.get_future().share();
    std::atomic<unsigned int> callback_calls{0};
    std::atomic<bool> task_finished{false};

    auto operation = std::make_unique<OperationState>(
        [&](OperationState::TaskContext& context) {
            callback_calls.fetch_add(1, std::memory_order_relaxed);
            callback_entered.set_value();
            release_callback_future.wait();
            task_finished.store(true, std::memory_order_release);
            return context.cancel_requested()
                ? OperationOutcome::cancelled()
                : OperationOutcome::succeeded();
        });
    CHECK(operation->start());
    callback_entered_future.wait();

    auto release = std::async(std::launch::async, [&] {
        operation.reset();
    });
    CHECK(release.wait_for(20ms) == std::future_status::timeout);
    CHECK(!task_finished.load(std::memory_order_acquire));

    release_callback.set_value();
    release.get();
    CHECK(task_finished.load(std::memory_order_acquire));
    CHECK(callback_calls.load(std::memory_order_relaxed) == 1);
}

void thrown_task_becomes_internal_failure() {
    OperationState operation([](OperationState::TaskContext&) -> OperationOutcome {
        throw std::runtime_error("task failure");
    });
    CHECK(operation.start());
    operation.wait();
    CHECK(operation.phase() == OperationPhase::Failed);
    const auto error = operation.error();
    CHECK(error.has_value());
    CHECK(error->status == KB_E_INTERNAL);
    CHECK(error->message == "task failure");
}

}  // namespace

int main() {
    const std::vector<std::pair<std::string_view, void (*)()>> tests{
        {"created/running/succeeded and wait deadlines",
         created_running_succeeded_and_wait_deadlines},
        {"cancellation idempotence and drain ordering",
         cancellation_is_idempotent_and_publishes_after_drain},
        {"cancel before start", cancel_before_start_is_terminal_and_skips_task},
        {"failure payload is immutable", failure_payload_is_synchronized_and_immutable},
        {"successful command result is immutable",
         successful_command_result_is_shared_and_immutable},
        {"cancel wins terminal publication",
         successful_task_is_cancelled_when_cancel_wins_publication},
        {"task payload lifetime", task_payload_outlives_initiating_reference},
        {"destructor cancellation and join", destructor_cancels_joins_and_leaves_no_callback},
        {"external release waits for callback",
         external_release_waits_for_running_callback},
        {"task exception normalization", thrown_task_becomes_internal_failure},
    };

    std::size_t failures = 0;
    for (const auto& [name, test] : tests) {
        try {
            test();
            std::cout << "PASS: " << name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "FAIL: " << name << ": " << error.what() << '\n';
        }
    }

    if (failures != 0) {
        std::cerr << failures << " operation-state test(s) failed\n";
        return 1;
    }
    std::cout << tests.size() << " operation-state tests passed\n";
    return 0;
}
