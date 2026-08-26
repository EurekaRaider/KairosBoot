#include "src/fleet/controller_scheduler.hpp"
#include "src/transport/adaptive_tuner.hpp"
#include "src/transport/buffer_budget.hpp"
#include "src/transport/transfer_ring.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

using kairosboot::fleet::DeviceFlowSpec;
using kairosboot::fleet::WeightedControllerScheduler;
using kairosboot::transport::AdaptiveTransferTuner;
using kairosboot::transport::BufferBudget;
using kairosboot::transport::BufferBudgetReleaseObserver;
using kairosboot::transport::CompletionCode;
using kairosboot::transport::DeliveryCertainty;
using kairosboot::transport::MemoryTransferSource;
using kairosboot::transport::PhysicalMemoryResult;
using kairosboot::transport::PhysicalMemoryStatus;
using kairosboot::transport::ProcessUsbBufferBudgetRegistry;
using kairosboot::transport::SubmitResult;
using kairosboot::transport::TransferBackend;
using kairosboot::transport::TransferCompletion;
using kairosboot::transport::TransferErrorKind;
using kairosboot::transport::TransferId;
using kairosboot::transport::TransferRing;
using kairosboot::transport::TransferRingConfig;
using kairosboot::transport::TransferRingState;
using kairosboot::transport::TransferSubmission;
using kairosboot::transport::TuningSample;

class TestFailure final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

void check(const bool condition, const std::string_view expression, const int line) {
    if (!condition) {
        throw TestFailure(std::string("line ") + std::to_string(line) + ": " +
                          std::string(expression));
    }
}

#define KB_CHECK(expression) check((expression), #expression, __LINE__)

class ManualClock final {
public:
    using duration = std::chrono::nanoseconds;
    using time_point = std::chrono::time_point<ManualClock, duration>;

    [[nodiscard]] time_point now() const noexcept { return now_; }
    void advance(const duration amount) noexcept { now_ += amount; }

private:
    time_point now_{};
};

class FakeBackend final : public TransferBackend {
public:
    struct Pending final {
        std::uint64_t offset{};
        const std::byte* data{};
        std::size_t size{};
    };

    [[nodiscard]] SubmitResult submit(const TransferSubmission& submission) override {
        auto result = SubmitResult::accepted;
        if (!submit_results_.empty()) {
            result = submit_results_.front();
            submit_results_.pop_front();
        }
        if (result == SubmitResult::accepted) {
            order_.push_back(submission.id);
            pending_.emplace(submission.id,
                             Pending{submission.offset,
                                     submission.payload.data(),
                                     submission.payload.size()});
        }
        return result;
    }

    void cancel(const TransferId id) noexcept override { cancelled_.push_back(id); }

    void queue_submit_result(const SubmitResult result) { submit_results_.push_back(result); }

    [[nodiscard]] const std::vector<TransferId>& order() const noexcept { return order_; }

    [[nodiscard]] const Pending& pending(const TransferId id) const { return pending_.at(id); }

    [[nodiscard]] bool was_cancelled(const TransferId id) const {
        return std::ranges::find(cancelled_, id) != cancelled_.end();
    }

private:
    std::deque<SubmitResult> submit_results_;
    std::unordered_map<TransferId, Pending> pending_;
    std::vector<TransferId> order_;
    std::vector<TransferId> cancelled_;
};

[[nodiscard]] std::shared_ptr<MemoryTransferSource> make_source(const std::size_t size) {
    std::vector<std::byte> bytes(size);
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        bytes[index] = std::byte{static_cast<unsigned char>(index & 0xFFU)};
    }
    return std::make_shared<MemoryTransferSource>(std::move(bytes));
}

void test_buffer_lease_lifetime_and_limit() {
    const auto budget = std::make_shared<BufferBudget>(16);
    std::shared_ptr<const void> lifetime;
    {
        auto first = budget->try_acquire(10);
        KB_CHECK(first.has_value());
        KB_CHECK(budget->used() == 10);
        KB_CHECK(budget->available() == 6);
        KB_CHECK(!budget->try_acquire(7).has_value());

        auto moved = std::move(*first);
        KB_CHECK(static_cast<bool>(moved));
        KB_CHECK(moved.size() == 10);
        KB_CHECK(budget->used() == 10);
        lifetime = moved.lifetime_token();
    }
    KB_CHECK(budget->used() == 10);
    lifetime.reset();
    KB_CHECK(budget->used() == 0);
    KB_CHECK(budget->peak_used() == 10);
}

class ReleaseObserver final : public BufferBudgetReleaseObserver {
public:
    void on_buffer_released() noexcept override {
        releases.fetch_add(1, std::memory_order_release);
    }

    std::atomic<std::size_t> releases{0};
};

void test_buffer_release_precedes_concurrent_budget_reuse() {
    ReleaseObserver observer;
    BufferBudget budget(1024, &observer);
    for (std::size_t iteration = 0; iteration < 64; ++iteration) {
        auto lease = budget.try_acquire(1024);
        KB_CHECK(lease.has_value());
        const auto expected_release = observer.releases.load(std::memory_order_acquire) + 1;
        std::atomic<bool> watcher_started{false};
        bool release_visible_when_charge_returned = false;
        std::thread watcher([&] {
            watcher_started.store(true, std::memory_order_release);
            while (budget.used() != 0) {
                std::this_thread::yield();
            }
            release_visible_when_charge_returned =
                observer.releases.load(std::memory_order_acquire) >= expected_release;
        });
        while (!watcher_started.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        lease.reset();
        watcher.join();
        KB_CHECK(release_visible_when_charge_returned);
    }
}

void test_transfer_ring_out_of_order_watermark() {
    FakeBackend backend;
    const auto budget = std::make_shared<BufferBudget>(12);
    TransferRing ring(backend, budget, TransferRingConfig{4, 3});

    KB_CHECK(ring.start(make_source(12)));
    KB_CHECK(backend.order() == std::vector<TransferId>({1, 2, 3}));
    KB_CHECK(budget->used() == 12);
    KB_CHECK(backend.pending(2).offset == 4);
    KB_CHECK(backend.pending(2).data[0] == std::byte{4});

    KB_CHECK(ring.handle_completion({2, CompletionCode::success, 4}));
    KB_CHECK(ring.completion_watermark() == 0);
    KB_CHECK(ring.completed_bytes() == 4);
    KB_CHECK(budget->used() == 8);

    KB_CHECK(ring.handle_completion({1, CompletionCode::success, 4}));
    KB_CHECK(ring.completion_watermark() == 8);
    KB_CHECK(ring.handle_completion({3, CompletionCode::success, 4}));
    KB_CHECK(ring.completion_watermark() == 12);
    KB_CHECK(ring.state() == TransferRingState::completed);
    KB_CHECK(budget->used() == 0);
    KB_CHECK(!ring.handle_completion({999, CompletionCode::success, 0}));
}

void test_transfer_ring_respects_shared_budget() {
    FakeBackend backend;
    const auto budget = std::make_shared<BufferBudget>(8);
    TransferRing ring(backend, budget, TransferRingConfig{4, 4});

    KB_CHECK(ring.start(make_source(12)));
    KB_CHECK(ring.in_flight() == 2);
    KB_CHECK(ring.submitted_bytes() == 8);
    KB_CHECK(ring.handle_completion({1, CompletionCode::success, 4}));
    KB_CHECK(ring.in_flight() == 2);
    KB_CHECK(ring.submitted_bytes() == 12);
    KB_CHECK(budget->used() == 8);
    KB_CHECK(budget->peak_used() <= budget->limit());

    KB_CHECK(ring.handle_completion({2, CompletionCode::success, 4}));
    KB_CHECK(ring.handle_completion({3, CompletionCode::success, 4}));
    KB_CHECK(ring.state() == TransferRingState::completed);
    KB_CHECK(budget->used() == 0);
}

void test_transfer_ring_partial_failure_cancels_and_drains() {
    FakeBackend backend;
    const auto budget = std::make_shared<BufferBudget>(12);
    TransferRing ring(backend, budget, TransferRingConfig{4, 3});

    KB_CHECK(ring.start(make_source(12)));
    KB_CHECK(ring.handle_completion({1, CompletionCode::success, 2}));
    KB_CHECK(ring.state() == TransferRingState::draining_failure);
    KB_CHECK(ring.error().has_value());
    KB_CHECK(ring.error()->kind == TransferErrorKind::partial_transfer);
    KB_CHECK(ring.error()->certainty == DeliveryCertainty::partial_or_unknown);
    KB_CHECK(backend.was_cancelled(2));
    KB_CHECK(backend.was_cancelled(3));

    KB_CHECK(ring.handle_completion({2, CompletionCode::cancelled, 0}));
    KB_CHECK(ring.handle_completion({3, CompletionCode::cancelled, 0}));
    KB_CHECK(ring.state() == TransferRingState::failed);
    KB_CHECK(budget->used() == 0);
}

void test_transfer_ring_no_device_classification() {
    FakeBackend backend;
    const auto budget = std::make_shared<BufferBudget>(8);
    TransferRing ring(backend, budget, TransferRingConfig{4, 2});

    KB_CHECK(ring.start(make_source(8)));
    KB_CHECK(ring.handle_completion({1, CompletionCode::no_device, 0}));
    KB_CHECK(ring.error().has_value());
    KB_CHECK(ring.error()->kind == TransferErrorKind::completion_no_device);
    KB_CHECK(ring.error()->certainty == DeliveryCertainty::partial_or_unknown);
    KB_CHECK(backend.was_cancelled(2));
    KB_CHECK(ring.handle_completion({2, CompletionCode::cancelled, 0}));
    KB_CHECK(ring.state() == TransferRingState::failed);
}

void test_transfer_ring_cancel_waits_for_drain() {
    FakeBackend backend;
    const auto budget = std::make_shared<BufferBudget>(8);
    TransferRing ring(backend, budget, TransferRingConfig{4, 2});

    KB_CHECK(ring.start(make_source(12)));
    ring.cancel();
    KB_CHECK(ring.state() == TransferRingState::cancelling);
    KB_CHECK(backend.was_cancelled(1));
    KB_CHECK(backend.was_cancelled(2));
    KB_CHECK(ring.handle_completion({2, CompletionCode::success, 4}));
    KB_CHECK(ring.state() == TransferRingState::cancelling);
    KB_CHECK(ring.handle_completion({1, CompletionCode::cancelled, 0}));
    KB_CHECK(ring.state() == TransferRingState::cancelled);
    KB_CHECK(ring.error()->kind == TransferErrorKind::user_cancelled);
    KB_CHECK(ring.error()->certainty == DeliveryCertainty::partial_or_unknown);
    KB_CHECK(ring.submitted_bytes() == 8);
    KB_CHECK(budget->used() == 0);
}

void test_tuner_defaults_bounds_and_deterministic_feedback() {
    AdaptiveTransferTuner tuner;
    KB_CHECK(tuner.current().chunk_size == kairosboot::transport::kDefaultTunedChunk);
    KB_CHECK(tuner.current().depth == kairosboot::transport::kDefaultTunedDepth);

    ManualClock clock;
    const auto first_start = clock.now();
    clock.advance(std::chrono::milliseconds(10));
    const auto first = tuner.observe(
        TuningSample{1000, clock.now() - first_start, false, false, false});
    KB_CHECK(first.has_value());
    KB_CHECK(first->depth == 8);

    const auto second_start = clock.now();
    clock.advance(std::chrono::milliseconds(10));
    const auto second = tuner.observe(
        TuningSample{1200, clock.now() - second_start, false, false, false});
    KB_CHECK(second.has_value());
    KB_CHECK(second->depth == 10);

    const auto third_start = clock.now();
    clock.advance(std::chrono::milliseconds(10));
    const auto third = tuner.observe(
        TuningSample{1400, clock.now() - third_start, false, false, false});
    KB_CHECK(third.has_value());
    KB_CHECK(third->depth == 12);
    KB_CHECK(third->chunk_size == 2U * 1024U * 1024U);

    const auto decreased = tuner.observe(TuningSample{0, {}, false, false, true});
    KB_CHECK(decreased.has_value());
    KB_CHECK(decreased->depth == 6);
    KB_CHECK(decreased->chunk_size == 1024U * 1024U);

    for (int iteration = 0; iteration < 16; ++iteration) {
        KB_CHECK(tuner.observe(TuningSample{0, {}, false, false, true}).has_value());
    }
    KB_CHECK(tuner.current().depth == kairosboot::transport::kMinimumTunedDepth);
    KB_CHECK(tuner.current().chunk_size == kairosboot::transport::kMinimumTunedChunk);

    AdaptiveTransferTuner invalid_sample_tuner;
    const auto empty = invalid_sample_tuner.observe(TuningSample{});
    KB_CHECK(!empty.has_value());
}

void test_global_memory_budget_formula() {
    using kairosboot::fleet::calculate_global_memory_budget;
    constexpr std::uint64_t gib = 1024ULL * 1024ULL * 1024ULL;
    KB_CHECK(calculate_global_memory_budget(10U * gib) == 2U * gib);
    KB_CHECK(calculate_global_memory_budget(5U * gib) == gib);
    KB_CHECK(calculate_global_memory_budget(4) == 0);
}

[[nodiscard]] PhysicalMemoryResult injected_physical_memory_query(
    void* const user_data) noexcept {
    return *static_cast<const PhysicalMemoryResult*>(user_data);
}

void test_process_usb_budget_formula_and_cap() {
    using kairosboot::transport::calculate_process_usb_buffer_budget;
    constexpr std::uint64_t gib = 1024ULL * 1024ULL * 1024ULL;
    static_assert(calculate_process_usb_buffer_budget(4) == 0);
    static_assert(calculate_process_usb_buffer_budget(5) == 1);
    static_assert(calculate_process_usb_buffer_budget(5U * gib) == gib);
    static_assert(calculate_process_usb_buffer_budget(10U * gib) == 2U * gib);

    KB_CHECK(calculate_process_usb_buffer_budget(0) == 0);
    KB_CHECK(calculate_process_usb_buffer_budget(5U * gib) == gib);
    KB_CHECK(calculate_process_usb_buffer_budget(10U * gib) == 2U * gib);
    KB_CHECK(calculate_process_usb_buffer_budget(20U * gib) == 2U * gib);
    KB_CHECK(calculate_process_usb_buffer_budget(
                 std::numeric_limits<std::uint64_t>::max()) ==
             2U * gib);
}

void test_process_usb_budget_resolution_and_failure_fallback() {
    using kairosboot::transport::kProcessUsbBufferBudgetFallbackBytes;
    using kairosboot::transport::resolve_process_usb_buffer_budget;
    constexpr std::uint64_t gib = 1024ULL * 1024ULL * 1024ULL;

    PhysicalMemoryResult measured{
        .status = PhysicalMemoryStatus::measured,
        .bytes = 5U * gib,
        .native_code = 0,
    };
    const auto resolved = resolve_process_usb_buffer_budget(
        &injected_physical_memory_query, &measured);
    KB_CHECK(resolved.limit_bytes == gib);
    KB_CHECK(resolved.physical_memory_bytes == 5U * gib);
    KB_CHECK(resolved.query_status == PhysicalMemoryStatus::measured);
    KB_CHECK(resolved.native_code == 0);
    KB_CHECK(!resolved.fallback_used);

    PhysicalMemoryResult failed{
        .status = PhysicalMemoryStatus::query_failed,
        .bytes = 0,
        .native_code = 17,
    };
    const auto failure = resolve_process_usb_buffer_budget(
        &injected_physical_memory_query, &failed);
    KB_CHECK(failure.limit_bytes == kProcessUsbBufferBudgetFallbackBytes);
    KB_CHECK(failure.physical_memory_bytes == 0);
    KB_CHECK(failure.query_status == PhysicalMemoryStatus::query_failed);
    KB_CHECK(failure.native_code == 17);
    KB_CHECK(failure.fallback_used);

    PhysicalMemoryResult overflow{
        .status = PhysicalMemoryStatus::arithmetic_overflow,
        .bytes = 0,
        .native_code = 75,
    };
    const auto overflow_failure = resolve_process_usb_buffer_budget(
        &injected_physical_memory_query, &overflow);
    KB_CHECK(overflow_failure.limit_bytes ==
             kProcessUsbBufferBudgetFallbackBytes);
    KB_CHECK(overflow_failure.query_status ==
             PhysicalMemoryStatus::arithmetic_overflow);
    KB_CHECK(overflow_failure.native_code == 75);
    KB_CHECK(overflow_failure.fallback_used);

    PhysicalMemoryResult invalid_measurement{
        .status = PhysicalMemoryStatus::measured,
        .bytes = 0,
        .native_code = 0,
    };
    const auto invalid = resolve_process_usb_buffer_budget(
        &injected_physical_memory_query, &invalid_measurement);
    KB_CHECK(invalid.limit_bytes == kProcessUsbBufferBudgetFallbackBytes);
    KB_CHECK(invalid.query_status == PhysicalMemoryStatus::query_failed);
    KB_CHECK(invalid.fallback_used);

    const auto missing_query = resolve_process_usb_buffer_budget(nullptr);
    KB_CHECK(missing_query.limit_bytes == kProcessUsbBufferBudgetFallbackBytes);
    KB_CHECK(missing_query.query_status == PhysicalMemoryStatus::query_failed);
    KB_CHECK(missing_query.fallback_used);
}

void test_process_usb_budget_registry_identity() {
    constexpr std::size_t expected_limit = 4096;
    PhysicalMemoryResult measured{
        .status = PhysicalMemoryStatus::measured,
        .bytes = 5U * expected_limit,
        .native_code = 0,
    };
    ProcessUsbBufferBudgetRegistry registry(
        &injected_physical_memory_query, &measured);
    const auto first = registry.budget();
    const auto second = registry.budget();
    KB_CHECK(first != nullptr);
    KB_CHECK(first.get() == second.get());
    KB_CHECK(first->limit() == expected_limit);
    KB_CHECK(registry.info().limit_bytes == expected_limit);
    KB_CHECK(!registry.info().fallback_used);

    const auto process_first =
        kairosboot::transport::process_usb_buffer_budget();
    const auto process_second =
        kairosboot::transport::process_usb_buffer_budget();
    KB_CHECK(process_first != nullptr);
    KB_CHECK(process_first.get() == process_second.get());
    KB_CHECK(process_first->limit() ==
             kairosboot::transport::process_usb_buffer_budget_info()
                 .limit_bytes);
}

void test_process_usb_budget_32_actor_concurrency_and_release() {
    constexpr std::size_t actor_count = 32;
    constexpr std::size_t iterations = 64;
    constexpr std::size_t budget_limit = 4096;
    constexpr std::size_t lease_size = 256;
    PhysicalMemoryResult measured{
        .status = PhysicalMemoryStatus::measured,
        .bytes = 5U * budget_limit,
        .native_code = 0,
    };
    ProcessUsbBufferBudgetRegistry registry(
        &injected_physical_memory_query, &measured);
    const auto budget = registry.budget();

    std::atomic<std::size_t> ready{0};
    std::atomic<std::size_t> completed{0};
    std::atomic<bool> start{false};
    std::atomic<bool> limit_violated{false};
    std::vector<std::thread> actors;
    actors.reserve(actor_count);
    for (std::size_t actor = 0; actor < actor_count; ++actor) {
        actors.emplace_back([&] {
            ready.fetch_add(1, std::memory_order_release);
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            for (std::size_t iteration = 0; iteration < iterations;
                 ++iteration) {
                std::optional<kairosboot::transport::BufferLease> lease;
                while (!(lease = budget->try_acquire(lease_size)).has_value()) {
                    std::this_thread::yield();
                }
                if (budget->used() > budget->limit()) {
                    limit_violated.store(true, std::memory_order_release);
                }
                std::this_thread::yield();
                lease.reset();
            }
            completed.fetch_add(1, std::memory_order_release);
        });
    }

    while (ready.load(std::memory_order_acquire) != actor_count) {
        std::this_thread::yield();
    }
    start.store(true, std::memory_order_release);
    for (auto& actor : actors) {
        actor.join();
    }

    KB_CHECK(completed.load(std::memory_order_acquire) == actor_count);
    KB_CHECK(!limit_violated.load(std::memory_order_acquire));
    KB_CHECK(budget->peak_used() <= budget->limit());
    KB_CHECK(budget->used() == 0);
    KB_CHECK(budget->available() == budget->limit());
}

void test_scheduler_weighted_fairness_without_starvation() {
    const auto budget = std::make_shared<BufferBudget>(1024);
    WeightedControllerScheduler scheduler(budget, 64);
    KB_CHECK(scheduler.add_flow(DeviceFlowSpec{"low", "controller-0", 1, 64U * 100U}));
    KB_CHECK(scheduler.add_flow(DeviceFlowSpec{"high", "controller-0", 3, 64U * 300U}));

    std::size_t low_count = 0;
    std::size_t high_count = 0;
    for (std::size_t index = 0; index < 160; ++index) {
        auto dispatch = scheduler.next(64);
        KB_CHECK(dispatch.has_value());
        if (dispatch->device_id() == "low") {
            ++low_count;
        } else if (dispatch->device_id() == "high") {
            ++high_count;
        } else {
            KB_CHECK(false);
        }
        KB_CHECK(scheduler.finish(std::move(*dispatch)));
    }
    KB_CHECK(low_count == 40);
    KB_CHECK(high_count == 120);

    const auto many_budget = std::make_shared<BufferBudget>(64);
    WeightedControllerScheduler many(many_budget, 64);
    for (std::size_t index = 0; index < 32; ++index) {
        KB_CHECK(many.add_flow(DeviceFlowSpec{"device-" + std::to_string(index),
                                              "controller-0",
                                              1,
                                              64}));
    }
    std::unordered_set<std::string> seen;
    for (std::size_t index = 0; index < 32; ++index) {
        auto dispatch = many.next(64);
        KB_CHECK(dispatch.has_value());
        seen.emplace(dispatch->device_id());
        KB_CHECK(many.finish(std::move(*dispatch)));
    }
    KB_CHECK(seen.size() == 32);
}

void test_scheduler_large_request_is_work_conserving_and_weighted() {
    constexpr std::size_t quantum = 64;
    constexpr std::size_t maximum = 4U * quantum;

    const auto solo_budget = std::make_shared<BufferBudget>(maximum);
    WeightedControllerScheduler solo(solo_budget, quantum);
    KB_CHECK(solo.add_flow(DeviceFlowSpec{"solo", "controller", 1, maximum}));
    auto immediate = solo.next(maximum);
    KB_CHECK(immediate.has_value());
    KB_CHECK(immediate->device_id() == "solo");
    KB_CHECK(immediate->bytes() == quantum);
    KB_CHECK(solo.finish(std::move(*immediate)));

    const auto weighted_budget = std::make_shared<BufferBudget>(maximum);
    WeightedControllerScheduler weighted(weighted_budget, quantum);
    KB_CHECK(weighted.add_flow(DeviceFlowSpec{"low", "controller", 1, 64U * quantum}));
    KB_CHECK(weighted.add_flow(DeviceFlowSpec{"high", "controller", 3, 192U * quantum}));

    std::uint64_t low_bytes = 0;
    std::uint64_t high_bytes = 0;
    std::size_t low_dispatches = 0;
    std::size_t high_dispatches = 0;
    for (std::size_t index = 0; index < 80; ++index) {
        auto dispatch = weighted.next(maximum);
        KB_CHECK(dispatch.has_value());
        if (dispatch->device_id() == "low") {
            low_bytes += dispatch->bytes();
            ++low_dispatches;
        } else if (dispatch->device_id() == "high") {
            high_bytes += dispatch->bytes();
            ++high_dispatches;
        } else {
            KB_CHECK(false);
        }
        KB_CHECK(weighted.finish(std::move(*dispatch)));
    }

    KB_CHECK(low_dispatches == 40);
    KB_CHECK(high_dispatches == 40);
    KB_CHECK(low_bytes > 0);
    KB_CHECK(high_bytes == 3U * low_bytes);
}

void test_scheduler_controller_fairness_and_memory_limit() {
    const auto budget = std::make_shared<BufferBudget>(128);
    WeightedControllerScheduler scheduler(budget, 64);
    KB_CHECK(scheduler.add_flow(DeviceFlowSpec{"a", "controller-a", 1, 640}));
    KB_CHECK(scheduler.add_flow(DeviceFlowSpec{"b", "controller-b", 1, 640}));

    auto first = scheduler.next(64);
    auto second = scheduler.next(64);
    KB_CHECK(first.has_value());
    KB_CHECK(second.has_value());
    KB_CHECK(first->controller_id() == "controller-a");
    KB_CHECK(second->controller_id() == "controller-b");
    KB_CHECK(budget->used() == 128);
    KB_CHECK(!scheduler.next(64).has_value());
    KB_CHECK(budget->peak_used() <= budget->limit());
    KB_CHECK(scheduler.outstanding_count() == 2);

    KB_CHECK(scheduler.finish(std::move(*first)));
    KB_CHECK(budget->used() == 64);
    auto third = scheduler.next(64);
    KB_CHECK(third.has_value());
    KB_CHECK(third->controller_id() == "controller-a");
    KB_CHECK(scheduler.finish(std::move(*second)));
    KB_CHECK(scheduler.finish(std::move(*third)));
    KB_CHECK(budget->used() == 0);
    KB_CHECK(scheduler.outstanding_count() == 0);
}

void test_scheduler_requeues_only_known_unsent_bytes() {
    const auto budget = std::make_shared<BufferBudget>(64);
    WeightedControllerScheduler scheduler(budget, 64);
    KB_CHECK(scheduler.add_flow(DeviceFlowSpec{"device", "controller", 1, 128}));
    KB_CHECK(!scheduler.add_flow(DeviceFlowSpec{"device", "other", 1, 128}));

    auto dispatch = scheduler.next(64);
    KB_CHECK(dispatch.has_value());
    KB_CHECK(scheduler.remaining("device") == 64);
    auto moved_dispatch = std::move(*dispatch);
    KB_CHECK(dispatch->token() == 0);
    KB_CHECK(scheduler.finish(std::move(moved_dispatch), 64));
    KB_CHECK(scheduler.remaining("device") == 128);
    KB_CHECK(budget->used() == 0);
}

struct TestCase final {
    std::string_view name;
    std::function<void()> run;
};

}  // namespace

int main() {
    const std::vector<TestCase> tests{
        {"buffer lease lifetime and limit", test_buffer_lease_lifetime_and_limit},
        {"buffer release before concurrent budget reuse", test_buffer_release_precedes_concurrent_budget_reuse},
        {"out-of-order completion watermark", test_transfer_ring_out_of_order_watermark},
        {"transfer ring shared budget", test_transfer_ring_respects_shared_budget},
        {"partial transfer cancel and drain", test_transfer_ring_partial_failure_cancels_and_drains},
        {"no-device error classification", test_transfer_ring_no_device_classification},
        {"explicit cancellation drain", test_transfer_ring_cancel_waits_for_drain},
        {"adaptive tuner deterministic feedback", test_tuner_defaults_bounds_and_deterministic_feedback},
        {"global memory budget formula", test_global_memory_budget_formula},
        {"process USB budget formula and cap", test_process_usb_budget_formula_and_cap},
        {"process USB budget failure fallback", test_process_usb_budget_resolution_and_failure_fallback},
        {"process USB budget registry identity", test_process_usb_budget_registry_identity},
        {"process USB budget 32 actor concurrency", test_process_usb_budget_32_actor_concurrency_and_release},
        {"weighted device fairness", test_scheduler_weighted_fairness_without_starvation},
        {"large request work conservation", test_scheduler_large_request_is_work_conserving_and_weighted},
        {"controller fairness and memory limit", test_scheduler_controller_fairness_and_memory_limit},
        {"known-unsent requeue", test_scheduler_requeues_only_known_unsent_bytes},
    };

    std::size_t failures = 0;
    for (const auto& test : tests) {
        try {
            test.run();
            std::cout << "[PASS] " << test.name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "[FAIL] " << test.name << ": " << error.what() << '\n';
        }
    }

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }
    std::cout << tests.size() << " test(s) passed\n";
    return 0;
}
