#include "src/fleet/controller_scheduler.hpp"
#include "src/transport/adaptive_tuner.hpp"
#include "src/transport/buffer_budget.hpp"
#include "src/transport/transfer_ring.hpp"

#include <algorithm>
#include <atomic>
#include <barrier>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <exception>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace allocation_fault {

thread_local bool enabled = false;
thread_local std::size_t attempts = 0;

[[nodiscard]] void* allocate(const std::size_t size) {
    if (enabled) {
        ++attempts;
        throw std::bad_alloc{};
    }
    if (void* allocation = std::malloc(size == 0 ? 1 : size); allocation != nullptr) {
        return allocation;
    }
    throw std::bad_alloc{};
}

}  // namespace allocation_fault

void* operator new(const std::size_t size) {
    return allocation_fault::allocate(size);
}

void* operator new[](const std::size_t size) {
    return allocation_fault::allocate(size);
}

void operator delete(void* const allocation) noexcept {
    std::free(allocation);
}

void operator delete[](void* const allocation) noexcept {
    std::free(allocation);
}

void operator delete(void* const allocation, std::size_t) noexcept {
    std::free(allocation);
}

void operator delete[](void* const allocation, std::size_t) noexcept {
    std::free(allocation);
}

namespace {

using kairosboot::fleet::DeviceFlowSpec;
using kairosboot::fleet::WeightedControllerScheduler;
using kairosboot::transport::AdaptiveTransferTuner;
using kairosboot::transport::BufferBudget;
using kairosboot::transport::BufferBudgetAvailabilityObserver;
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
using kairosboot::transport::TransferPermit;
using kairosboot::transport::TransferPermitProvider;
using kairosboot::transport::TransferPermitSettlement;
using kairosboot::transport::TransferPermitWaitResult;
using kairosboot::transport::TransferRing;
using kairosboot::transport::TransferRingConfig;
using kairosboot::transport::TransferRingState;
using kairosboot::transport::TransferSubmission;
using kairosboot::transport::TransferTelemetry;
using kairosboot::transport::TransferTelemetryConfig;
using kairosboot::transport::TransferTelemetryTimePoint;
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

struct IncrementingTelemetryClock final {
    TransferTelemetryTimePoint current{};
    std::chrono::nanoseconds step{1};
    std::size_t calls{};
};

[[nodiscard]] TransferTelemetryTimePoint sample_telemetry_clock(
    void* const context) noexcept {
    auto& clock = *static_cast<IncrementingTelemetryClock*>(context);
    const auto sampled = clock.current;
    clock.current += clock.step;
    ++clock.calls;
    return sampled;
}

[[nodiscard]] TransferTelemetry make_telemetry(
    IncrementingTelemetryClock& clock,
    const bool enabled = true) noexcept {
    return TransferTelemetry(TransferTelemetryConfig{
        .enabled = enabled,
        .clock = {
            .now = &sample_telemetry_clock,
            .context = &clock,
        },
    });
}

class FakeBackend final : public TransferBackend {
public:
    struct Pending final {
        std::uint64_t offset{};
        const std::byte* data{};
        std::size_t size{};
    };

    FakeBackend() { cancelled_.reserve(32); }

    [[nodiscard]] SubmitResult submit(const TransferSubmission& submission) override {
        ++submit_attempts_;
        if (throw_on_submit_attempt_ != 0 &&
            submit_attempts_ == throw_on_submit_attempt_) {
            throw std::runtime_error("submit delivery is unknown");
        }
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
    void throw_on_submit(const std::size_t attempt = 1) noexcept {
        throw_on_submit_attempt_ = attempt;
    }

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
    std::size_t submit_attempts_{};
    std::size_t throw_on_submit_attempt_{};
};

class TestPermitProvider final : public TransferPermitProvider {
public:
    struct Settlement final {
        std::uint64_t token{};
        std::size_t bytes{};
        TransferPermitSettlement result{TransferPermitSettlement::partial_or_unknown};
    };

    explicit TestPermitProvider(std::shared_ptr<BufferBudget> budget)
        : budget_(std::move(budget)) {
        settlements_.reserve(32);
    }

    [[nodiscard]] std::optional<TransferPermit> try_acquire(
        const std::size_t maximum_bytes) override {
        if (maximum_bytes == 0) {
            return std::nullopt;
        }
        auto lease = budget_->try_acquire(maximum_bytes);
        if (!lease.has_value()) {
            return std::nullopt;
        }
        std::uint64_t token;
        {
            std::scoped_lock lock(mutex_);
            if (cancelled_) {
                return std::nullopt;
            }
            token = next_token_++;
        }
        return make_permit(std::move(*lease), token);
    }

    [[nodiscard]] std::uint64_t readiness_generation() const noexcept override {
        std::scoped_lock lock(mutex_);
        return generation_;
    }

    [[nodiscard]] kairosboot::transport::TransferPermitWaitResult wait_for_ready(
        const std::uint64_t observed_generation,
        const std::chrono::steady_clock::time_point deadline) override {
        std::unique_lock lock(mutex_);
        if (!changed_.wait_until(lock, deadline, [&] {
                return cancelled_ || generation_ != observed_generation;
            })) {
            return kairosboot::transport::TransferPermitWaitResult::timeout;
        }
        return cancelled_
            ? kairosboot::transport::TransferPermitWaitResult::cancelled
            : kairosboot::transport::TransferPermitWaitResult::ready;
    }

    void cancel_wait() noexcept override {
        std::scoped_lock lock(mutex_);
        if (!cancelled_) {
            cancelled_ = true;
            ++cancel_count_;
            ++generation_;
            changed_.notify_all();
        }
    }

    [[nodiscard]] const std::vector<Settlement>& settlements() const noexcept {
        return settlements_;
    }

    [[nodiscard]] std::size_t cancel_count() const noexcept { return cancel_count_; }

private:
    void settle(const std::uint64_t token,
                const std::size_t bytes,
                const TransferPermitSettlement result) noexcept override {
        std::scoped_lock lock(mutex_);
        settlements_.push_back(Settlement{token, bytes, result});
        ++generation_;
        changed_.notify_all();
    }

    std::shared_ptr<BufferBudget> budget_;
    std::vector<Settlement> settlements_;
    std::uint64_t next_token_{1};
    std::size_t cancel_count_{};
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::uint64_t generation_{1};
    bool cancelled_{};
};

class AllocationFailureScope final {
public:
    AllocationFailureScope() noexcept {
        allocation_fault::attempts = 0;
        allocation_fault::enabled = true;
    }

    ~AllocationFailureScope() { allocation_fault::enabled = false; }

    AllocationFailureScope(const AllocationFailureScope&) = delete;
    AllocationFailureScope& operator=(const AllocationFailureScope&) = delete;
};

[[nodiscard]] std::shared_ptr<MemoryTransferSource> make_source(const std::size_t size) {
    std::vector<std::byte> bytes(size);
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        bytes[index] = std::byte{static_cast<unsigned char>(index & 0xFFU)};
    }
    return std::make_shared<MemoryTransferSource>(std::move(bytes));
}

void test_transfer_telemetry_disabled_has_zero_clock_cost() {
    IncrementingTelemetryClock clock;
    auto telemetry = make_telemetry(clock, false);
    FakeBackend backend;
    const auto budget = std::make_shared<BufferBudget>(8);
    TransferRing ring(
        backend, budget, TransferRingConfig{4, 2}, &telemetry);

    KB_CHECK(ring.start(make_source(8)));
    KB_CHECK(ring.handle_completion({1, CompletionCode::success, 4}));
    KB_CHECK(ring.handle_completion({2, CompletionCode::success, 4}));
    KB_CHECK(clock.calls == 0);

    {
        AllocationFailureScope fault;
        const auto snapshot = ring.telemetry_snapshot();
        KB_CHECK(!snapshot.enabled);
        KB_CHECK(snapshot.source_read_count == 0);
        KB_CHECK(snapshot.budget_acquire_attempt_count == 0);
        KB_CHECK(snapshot.submit_count == 0);
        KB_CHECK(snapshot.completion_count == 0);
        KB_CHECK(snapshot.current_in_flight == 0);
        KB_CHECK(snapshot.peak_in_flight == 0);
        KB_CHECK(snapshot.cancel_count == 0);
        KB_CHECK(snapshot.error_count == 0);
    }
    KB_CHECK(allocation_fault::attempts == 0);
}

void test_transfer_telemetry_counts_and_out_of_order_watermark() {
    IncrementingTelemetryClock clock;
    auto telemetry = make_telemetry(clock);
    FakeBackend backend;
    const auto budget = std::make_shared<BufferBudget>(12);
    TransferRing ring(
        backend, budget, TransferRingConfig{4, 3}, &telemetry);

    KB_CHECK(ring.start(make_source(12)));
    auto snapshot = ring.telemetry_snapshot();
    KB_CHECK(snapshot.enabled);
    KB_CHECK(snapshot.source_read_count == 3);
    KB_CHECK(snapshot.source_read_bytes == 12);
    KB_CHECK(snapshot.source_read_time == std::chrono::nanoseconds(3));
    KB_CHECK(snapshot.budget_acquire_attempt_count == 3);
    KB_CHECK(snapshot.budget_acquire_count == 3);
    KB_CHECK(snapshot.budget_acquire_time == std::chrono::nanoseconds(3));
    KB_CHECK(snapshot.budget_wait_count == 0);
    KB_CHECK(snapshot.budget_wait_time == std::chrono::nanoseconds::zero());
    KB_CHECK(snapshot.submit_attempt_count == 3);
    KB_CHECK(snapshot.submit_count == 3);
    KB_CHECK(snapshot.submitted_bytes == 12);
    KB_CHECK(snapshot.completion_count == 0);
    KB_CHECK(snapshot.completed_bytes == 0);
    KB_CHECK(snapshot.current_in_flight == 3);
    KB_CHECK(snapshot.peak_in_flight == 3);
    KB_CHECK(snapshot.contiguous_watermark == 0);
    KB_CHECK(clock.calls == 12);

    KB_CHECK(ring.handle_completion({2, CompletionCode::success, 4}));
    snapshot = ring.telemetry_snapshot();
    KB_CHECK(snapshot.completion_count == 1);
    KB_CHECK(snapshot.completed_bytes == 4);
    KB_CHECK(snapshot.current_in_flight == 2);
    KB_CHECK(snapshot.contiguous_watermark == 0);

    KB_CHECK(ring.handle_completion({1, CompletionCode::success, 4}));
    snapshot = ring.telemetry_snapshot();
    KB_CHECK(snapshot.completion_count == 2);
    KB_CHECK(snapshot.completed_bytes == 8);
    KB_CHECK(snapshot.current_in_flight == 1);
    KB_CHECK(snapshot.contiguous_watermark == 8);

    KB_CHECK(ring.handle_completion({3, CompletionCode::success, 4}));
    snapshot = ring.telemetry_snapshot();
    KB_CHECK(snapshot.completion_count == 3);
    KB_CHECK(snapshot.completed_bytes == 12);
    KB_CHECK(snapshot.current_in_flight == 0);
    KB_CHECK(snapshot.peak_in_flight == 3);
    KB_CHECK(snapshot.contiguous_watermark == 12);
    KB_CHECK(snapshot.cancel_count == 0);
    KB_CHECK(snapshot.backend_cancel_count == 0);
    KB_CHECK(snapshot.cancelled_completion_count == 0);
    KB_CHECK(snapshot.error_count == 0);
}

void test_transfer_telemetry_budget_contention_does_not_invent_waits() {
    IncrementingTelemetryClock clock;
    auto telemetry = make_telemetry(clock);
    FakeBackend backend;
    const auto budget = std::make_shared<BufferBudget>(8);
    TransferRing ring(
        backend, budget, TransferRingConfig{4, 4}, &telemetry);

    KB_CHECK(ring.start(make_source(12)));
    auto snapshot = ring.telemetry_snapshot();
    KB_CHECK(snapshot.budget_acquire_attempt_count == 3);
    KB_CHECK(snapshot.budget_acquire_count == 2);
    KB_CHECK(snapshot.budget_wait_count == 0);
    KB_CHECK(snapshot.budget_wait_time == std::chrono::nanoseconds::zero());

    KB_CHECK(ring.handle_completion({1, CompletionCode::success, 4}));
    snapshot = ring.telemetry_snapshot();
    KB_CHECK(snapshot.budget_acquire_attempt_count == 4);
    KB_CHECK(snapshot.budget_acquire_count == 3);
    KB_CHECK(snapshot.budget_acquire_time == std::chrono::nanoseconds(4));
    KB_CHECK(snapshot.budget_wait_count == 0);
    KB_CHECK(snapshot.source_read_count == 3);
    KB_CHECK(snapshot.source_read_bytes == 12);

    KB_CHECK(ring.handle_completion({2, CompletionCode::success, 4}));
    KB_CHECK(ring.handle_completion({3, CompletionCode::success, 4}));
}

void test_transfer_telemetry_cancel_and_error_counts() {
    {
        IncrementingTelemetryClock clock;
        auto telemetry = make_telemetry(clock);
        FakeBackend backend;
        const auto budget = std::make_shared<BufferBudget>(12);
        TransferRing ring(
            backend, budget, TransferRingConfig{4, 3}, &telemetry);

        KB_CHECK(ring.start(make_source(12)));
        KB_CHECK(ring.handle_completion({1, CompletionCode::success, 2}));
        auto snapshot = ring.telemetry_snapshot();
        KB_CHECK(snapshot.completion_count == 1);
        KB_CHECK(snapshot.completed_bytes == 2);
        KB_CHECK(snapshot.contiguous_watermark == 0);
        KB_CHECK(snapshot.cancel_count == 0);
        KB_CHECK(snapshot.backend_cancel_count == 2);
        KB_CHECK(snapshot.cancelled_completion_count == 0);
        KB_CHECK(snapshot.error_count == 1);

        KB_CHECK(ring.handle_completion({2, CompletionCode::cancelled, 0}));
        KB_CHECK(ring.handle_completion({3, CompletionCode::cancelled, 0}));
        snapshot = ring.telemetry_snapshot();
        KB_CHECK(snapshot.completion_count == 3);
        KB_CHECK(snapshot.completed_bytes == 2);
        KB_CHECK(snapshot.current_in_flight == 0);
        KB_CHECK(snapshot.cancelled_completion_count == 2);
        KB_CHECK(snapshot.error_count == 1);
    }

    {
        IncrementingTelemetryClock clock;
        auto telemetry = make_telemetry(clock);
        FakeBackend backend;
        const auto budget = std::make_shared<BufferBudget>(8);
        TransferRing ring(
            backend, budget, TransferRingConfig{4, 2}, &telemetry);

        KB_CHECK(ring.start(make_source(8)));
        ring.cancel();
        auto snapshot = ring.telemetry_snapshot();
        KB_CHECK(snapshot.cancel_count == 1);
        KB_CHECK(snapshot.backend_cancel_count == 2);
        KB_CHECK(snapshot.error_count == 0);
        KB_CHECK(ring.handle_completion({2, CompletionCode::success, 4}));
        KB_CHECK(ring.handle_completion({1, CompletionCode::cancelled, 0}));
        snapshot = ring.telemetry_snapshot();
        KB_CHECK(snapshot.completion_count == 2);
        KB_CHECK(snapshot.completed_bytes == 4);
        KB_CHECK(snapshot.cancelled_completion_count == 1);
        KB_CHECK(snapshot.error_count == 0);
    }
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

class BlockingReleaseObserver final : public BufferBudgetReleaseObserver {
public:
    void on_buffer_released() noexcept override {
        std::unique_lock lock(mutex_);
        entered_ = true;
        changed_.notify_all();
        changed_.wait(lock, [this] { return release_allowed_; });
    }

    void wait_until_entered() {
        std::unique_lock lock(mutex_);
        changed_.wait(lock, [this] { return entered_; });
    }

    void allow_release() noexcept {
        std::scoped_lock lock(mutex_);
        release_allowed_ = true;
        changed_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    bool entered_{};
    bool release_allowed_{};
};

class CountingAvailabilityObserver final
    : public BufferBudgetAvailabilityObserver {
public:
    void on_buffer_budget_available() noexcept override {
        calls.fetch_add(1, std::memory_order_release);
    }

    std::atomic<std::size_t> calls{0};
};

class BlockingAvailabilityObserver final
    : public BufferBudgetAvailabilityObserver {
public:
    void on_buffer_budget_available() noexcept override {
        std::unique_lock lock(mutex_);
        if (blocked_once_) {
            return;
        }
        entered_ = true;
        changed_.notify_all();
        changed_.wait(lock, [this] { return release_allowed_; });
        blocked_once_ = true;
    }

    void wait_until_entered() {
        std::unique_lock lock(mutex_);
        changed_.wait(lock, [this] { return entered_; });
    }

    void allow_release() noexcept {
        std::scoped_lock lock(mutex_);
        release_allowed_ = true;
        changed_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    bool entered_{};
    bool release_allowed_{};
    bool blocked_once_{};
};

class ReentrantAvailabilityObserver final
    : public BufferBudgetAvailabilityObserver {
public:
    ReentrantAvailabilityObserver(
        BufferBudget& budget,
        std::shared_ptr<BufferBudgetAvailabilityObserver> additional_observer)
        : budget_(budget), additional_observer_(std::move(additional_observer)) {}

    void on_buffer_budget_available() noexcept override {
        calls.fetch_add(1, std::memory_order_release);
        if (reentered_.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        budget_.observe_availability(additional_observer_);
        auto nested = budget_.try_acquire(1);
        if (nested.has_value()) {
            nested.reset();
        }
    }

    std::atomic<std::size_t> calls{0};

private:
    BufferBudget& budget_;
    std::shared_ptr<BufferBudgetAvailabilityObserver> additional_observer_;
    std::atomic<bool> reentered_{false};
};

class DestructionReentrantAvailabilityObserver final
    : public BufferBudgetAvailabilityObserver {
public:
    DestructionReentrantAvailabilityObserver(
        BufferBudget& budget,
        std::shared_ptr<BufferBudgetAvailabilityObserver> replacement,
        std::shared_ptr<DestructionReentrantAvailabilityObserver>* owner,
        std::atomic<bool>& destroyed)
        : budget_(budget),
          replacement_(std::move(replacement)),
          owner_(owner),
          destroyed_(destroyed) {}

    ~DestructionReentrantAvailabilityObserver() override {
        budget_.observe_availability(replacement_);
        destroyed_.store(true, std::memory_order_release);
    }

    void on_buffer_budget_available() noexcept override { owner_->reset(); }

private:
    BufferBudget& budget_;
    std::shared_ptr<BufferBudgetAvailabilityObserver> replacement_;
    std::shared_ptr<DestructionReentrantAvailabilityObserver>* owner_;
    std::atomic<bool>& destroyed_;
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

void test_buffer_availability_observers_are_reentrant_and_destroyed_outside_lock() {
    {
        BufferBudget budget(1);
        const auto additional =
            std::make_shared<CountingAvailabilityObserver>();
        const auto reentrant = std::make_shared<ReentrantAvailabilityObserver>(
            budget, additional);
        budget.observe_availability(reentrant);

        auto lease = budget.try_acquire(1);
        KB_CHECK(lease.has_value());
        lease.reset();

        KB_CHECK(budget.used() == 0);
        KB_CHECK(reentrant->calls.load(std::memory_order_acquire) == 2);
        KB_CHECK(additional->calls.load(std::memory_order_acquire) == 1);
    }

    {
        BufferBudget budget(1);
        const auto replacement =
            std::make_shared<CountingAvailabilityObserver>();
        std::atomic<bool> destroyed{false};
        std::shared_ptr<DestructionReentrantAvailabilityObserver> observer;
        observer =
            std::make_shared<DestructionReentrantAvailabilityObserver>(
                budget, replacement, &observer, destroyed);
        budget.observe_availability(observer);

        auto first = budget.try_acquire(1);
        KB_CHECK(first.has_value());
        first.reset();
        KB_CHECK(observer == nullptr);
        KB_CHECK(destroyed.load(std::memory_order_acquire));

        auto second = budget.try_acquire(1);
        KB_CHECK(second.has_value());
        second.reset();
        KB_CHECK(replacement->calls.load(std::memory_order_acquire) == 1);
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

void test_transfer_ring_uses_permit_buffers_without_double_accounting() {
    FakeBackend backend;
    const auto budget = std::make_shared<BufferBudget>(32);
    WeightedControllerScheduler scheduler(budget, 4);
    KB_CHECK(scheduler.add_flow(DeviceFlowSpec{"device", "controller", 1, 32, 8}));
    auto provider = scheduler.make_permit_provider("device", 8);
    KB_CHECK(provider != nullptr);
    TransferRing ring(
        backend, budget, TransferRingConfig{4, 8}, nullptr, provider);

    KB_CHECK(ring.start(make_source(32)));
    KB_CHECK(ring.in_flight() == 8);
    KB_CHECK(budget->used() == 32);
    KB_CHECK(scheduler.outstanding_count() == 8);
    KB_CHECK(ring.handle_completion({8, CompletionCode::success, 4}));
    KB_CHECK(ring.completion_watermark() == 0);
    KB_CHECK(scheduler.outstanding_count() == 7);
    KB_CHECK(ring.handle_completion({1, CompletionCode::success, 4}));
    KB_CHECK(ring.completion_watermark() == 4);
    for (TransferId id = 2; id < 8; ++id) {
        KB_CHECK(ring.handle_completion({id, CompletionCode::success, 4}));
    }
    KB_CHECK(ring.state() == TransferRingState::completed);
    KB_CHECK(ring.completion_watermark() == 32);
    KB_CHECK(scheduler.remaining("device") == 0);
    KB_CHECK(scheduler.outstanding_count() == 0);
    KB_CHECK(budget->used() == 0);
    KB_CHECK(budget->peak_used() == 32);
}

void test_transfer_ring_permit_settlement_certainty() {
    {
        FakeBackend backend;
        backend.queue_submit_result(SubmitResult::resource_exhausted);
        const auto budget = std::make_shared<BufferBudget>(4);
        WeightedControllerScheduler scheduler(budget, 4);
        KB_CHECK(scheduler.add_flow(DeviceFlowSpec{"device", "controller", 1, 4}));
        auto provider = scheduler.make_permit_provider("device");
        TransferRing ring(
            backend, budget, TransferRingConfig{4, 1}, nullptr, provider);

        KB_CHECK(!ring.start(make_source(4)));
        KB_CHECK(ring.state() == TransferRingState::failed);
        KB_CHECK(scheduler.remaining("device") == 4);
        KB_CHECK(scheduler.outstanding_count() == 0);
        KB_CHECK(budget->used() == 0);
    }

    {
        FakeBackend backend;
        const auto budget = std::make_shared<BufferBudget>(8);
        WeightedControllerScheduler scheduler(budget, 4);
        KB_CHECK(scheduler.add_flow(DeviceFlowSpec{"device", "controller", 1, 12, 2}));
        auto provider = scheduler.make_permit_provider("device", 2);
        TransferRing ring(
            backend, budget, TransferRingConfig{4, 2}, nullptr, provider);

        KB_CHECK(ring.start(make_source(12)));
        KB_CHECK(ring.in_flight() == 2);
        KB_CHECK(ring.handle_completion({1, CompletionCode::success, 2}));
        KB_CHECK(ring.state() == TransferRingState::draining_failure);
        KB_CHECK(backend.was_cancelled(2));
        KB_CHECK(ring.handle_completion({2, CompletionCode::cancelled, 0}));
        KB_CHECK(ring.state() == TransferRingState::failed);
        KB_CHECK(scheduler.remaining("device") == 4);
        KB_CHECK(scheduler.outstanding_count() == 0);
        KB_CHECK(!scheduler.next(4).has_value());
        KB_CHECK(budget->used() == 0);
    }

    {
        FakeBackend backend;
        backend.throw_on_submit(3);
        const auto budget = std::make_shared<BufferBudget>(12);
        WeightedControllerScheduler scheduler(budget, 4);
        KB_CHECK(scheduler.add_flow(DeviceFlowSpec{"device", "controller", 1, 12, 3}));
        auto provider = scheduler.make_permit_provider("device", 3);
        TransferRing ring(
            backend, budget, TransferRingConfig{4, 3}, nullptr, provider);
        KB_CHECK(!ring.start(make_source(12)));
        KB_CHECK(ring.state() == TransferRingState::draining_failure);
        KB_CHECK(ring.error().has_value());
        KB_CHECK(ring.error()->kind == TransferErrorKind::submit_io);
        KB_CHECK(ring.error()->certainty == DeliveryCertainty::partial_or_unknown);
        KB_CHECK(backend.was_cancelled(1));
        KB_CHECK(backend.was_cancelled(2));
        KB_CHECK(backend.was_cancelled(3));
        KB_CHECK(scheduler.remaining("device") == 0);
        KB_CHECK(scheduler.outstanding_count() == 3);
        KB_CHECK(budget->used() == 12);
        KB_CHECK(ring.handle_completion({3, CompletionCode::cancelled, 0}));
        KB_CHECK(scheduler.outstanding_count() == 2);
        KB_CHECK(ring.handle_completion({2, CompletionCode::success, 4}));
        KB_CHECK(scheduler.outstanding_count() == 1);
        KB_CHECK(ring.handle_completion({1, CompletionCode::cancelled, 0}));
        KB_CHECK(ring.state() == TransferRingState::failed);
        KB_CHECK(scheduler.outstanding_count() == 0);
        KB_CHECK(budget->used() == 0);
    }
}

void test_transfer_ring_permit_out_of_order_watermark_and_cancel_drain() {
    {
        FakeBackend backend;
        const auto budget = std::make_shared<BufferBudget>(12);
        const auto provider = std::make_shared<TestPermitProvider>(budget);
        TransferRing ring(
            backend, nullptr, TransferRingConfig{4, 3}, nullptr, provider);

        KB_CHECK(ring.start(make_source(12)));
        KB_CHECK(ring.handle_completion({2, CompletionCode::success, 4}));
        KB_CHECK(ring.completion_watermark() == 0);
        KB_CHECK(ring.handle_completion({1, CompletionCode::success, 4}));
        KB_CHECK(ring.completion_watermark() == 8);
        KB_CHECK(ring.handle_completion({3, CompletionCode::success, 4}));
        KB_CHECK(ring.completion_watermark() == 12);
        KB_CHECK(provider->settlements().size() == 3);
        for (const auto& settlement : provider->settlements()) {
            KB_CHECK(settlement.result == TransferPermitSettlement::fully_transferred);
        }
        KB_CHECK(budget->used() == 0);
    }

    {
        FakeBackend backend;
        const auto budget = std::make_shared<BufferBudget>(8);
        const auto provider = std::make_shared<TestPermitProvider>(budget);
        TransferRing ring(
            backend, nullptr, TransferRingConfig{4, 2}, nullptr, provider);

        KB_CHECK(ring.start(make_source(8)));
        ring.cancel();
        KB_CHECK(provider->cancel_count() == 1);
        KB_CHECK(ring.handle_completion({2, CompletionCode::cancelled, 0}));
        KB_CHECK(ring.handle_completion({1, CompletionCode::cancelled, 0}));
        KB_CHECK(ring.state() == TransferRingState::cancelled);
        KB_CHECK(provider->settlements().size() == 2);
        for (const auto& settlement : provider->settlements()) {
            KB_CHECK(settlement.result == TransferPermitSettlement::partial_or_unknown);
        }
        KB_CHECK(budget->used() == 0);
    }

    {
        FakeBackend backend;
        const auto budget = std::make_shared<BufferBudget>(8);
        WeightedControllerScheduler scheduler(budget, 4);
        KB_CHECK(scheduler.add_flow(DeviceFlowSpec{"device", "controller", 1, 8, 2}));
        auto provider = scheduler.make_permit_provider("device", 2);
        TransferRing ring(
            backend, nullptr, TransferRingConfig{4, 2}, nullptr, provider);

        KB_CHECK(ring.start(make_source(8)));
        KB_CHECK(ring.in_flight() == 2);
        KB_CHECK(scheduler.outstanding_count() == 2);
        ring.cancel();
        KB_CHECK(!provider->try_acquire(4).has_value());
        KB_CHECK(ring.handle_completion({2, CompletionCode::cancelled, 0}));
        KB_CHECK(ring.handle_completion({1, CompletionCode::cancelled, 0}));
        KB_CHECK(ring.state() == TransferRingState::cancelled);
        KB_CHECK(scheduler.remaining("device") == 0);
        KB_CHECK(scheduler.outstanding_count() == 0);
        KB_CHECK(budget->used() == 0);
    }

    {
        FakeBackend waiting_backend;
        const auto budget = std::make_shared<BufferBudget>(4);
        WeightedControllerScheduler scheduler(budget, 4);
        KB_CHECK(scheduler.add_flow(DeviceFlowSpec{"holder", "controller-a", 1, 4}));
        KB_CHECK(scheduler.add_flow(DeviceFlowSpec{"waiting", "controller-b", 1, 4}));
        auto holder_provider = scheduler.make_permit_provider("holder");
        auto waiting_provider = scheduler.make_permit_provider("waiting");
        auto holder = holder_provider->try_acquire(4);
        KB_CHECK(holder.has_value());
        TransferRing ring(
            waiting_backend,
            nullptr,
            TransferRingConfig{4, 1},
            nullptr,
            waiting_provider);

        KB_CHECK(ring.start(make_source(4)));
        KB_CHECK(ring.state() == TransferRingState::running);
        KB_CHECK(ring.in_flight() == 0);
        std::atomic<bool> waiter_started{false};
        bool woke_and_submitted = false;
        std::thread waiter([&] {
            waiter_started.store(true, std::memory_order_release);
            woke_and_submitted = ring.wait_for_permit_until(
                std::chrono::steady_clock::now() + std::chrono::seconds(2));
        });
        while (!waiter_started.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        holder->settle(TransferPermitSettlement::fully_transferred);
        waiter.join();
        KB_CHECK(woke_and_submitted);
        KB_CHECK(ring.in_flight() == 1);
        KB_CHECK(ring.handle_completion({1, CompletionCode::success, 4}));
        KB_CHECK(ring.state() == TransferRingState::completed);
        KB_CHECK(scheduler.outstanding_count() == 0);
        KB_CHECK(budget->used() == 0);
    }

    {
        FakeBackend default_backend;
        FakeBackend provider_backend;
        const auto budget = std::make_shared<BufferBudget>(4);
        WeightedControllerScheduler scheduler(budget, 4);
        KB_CHECK(scheduler.add_flow(
            DeviceFlowSpec{"provider", "controller", 1, 4}));
        auto provider = scheduler.make_permit_provider("provider");
        TransferRing default_ring(
            default_backend, budget, TransferRingConfig{4, 1});
        TransferRing provider_ring(
            provider_backend,
            nullptr,
            TransferRingConfig{4, 1},
            nullptr,
            provider);

        KB_CHECK(default_ring.start(make_source(4)));
        KB_CHECK(provider_ring.start(make_source(4)));
        KB_CHECK(default_ring.in_flight() == 1);
        KB_CHECK(provider_ring.in_flight() == 0);
        bool provider_submitted = false;
        std::thread waiter([&] {
            provider_submitted = provider_ring.wait_for_permit_until(
                std::chrono::steady_clock::now() + std::chrono::seconds(2));
        });
        KB_CHECK(default_ring.handle_completion(
            {1, CompletionCode::success, 4}));
        waiter.join();
        KB_CHECK(provider_submitted);
        KB_CHECK(provider_ring.in_flight() == 1);
        KB_CHECK(provider_ring.handle_completion(
            {1, CompletionCode::success, 4}));
        KB_CHECK(provider_ring.state() == TransferRingState::completed);
        KB_CHECK(scheduler.outstanding_count() == 0);
        KB_CHECK(budget->used() == 0);
    }
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
    KB_CHECK(ring.completion_watermark() == 0);
    KB_CHECK(ring.submitted_bytes() == 8);
    KB_CHECK(ring.in_flight() == 2);
    ring.cancel();
    KB_CHECK(ring.state() == TransferRingState::cancelling);
    KB_CHECK(ring.error()->certainty == DeliveryCertainty::partial_or_unknown);
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

void test_transfer_ring_cancel_and_failure_drain_do_not_allocate() {
    {
        FakeBackend backend;
        const auto budget = std::make_shared<BufferBudget>(8);
        TransferRing ring(backend, budget, TransferRingConfig{4, 2});

        KB_CHECK(ring.start(make_source(8)));
        {
            AllocationFailureScope fault;
            ring.cancel();
            KB_CHECK(ring.handle_completion({2, CompletionCode::cancelled, 0}));
            KB_CHECK(ring.handle_completion({1, CompletionCode::cancelled, 0}));
        }
        KB_CHECK(allocation_fault::attempts == 0);
        KB_CHECK(ring.state() == TransferRingState::cancelled);
        KB_CHECK(ring.error()->certainty == DeliveryCertainty::partial_or_unknown);
        KB_CHECK(budget->used() == 0);
    }

    {
        FakeBackend backend;
        const auto budget = std::make_shared<BufferBudget>(12);
        TransferRing ring(backend, budget, TransferRingConfig{4, 3});

        KB_CHECK(ring.start(make_source(12)));
        {
            AllocationFailureScope fault;
            KB_CHECK(ring.handle_completion({1, CompletionCode::success, 2}));
            KB_CHECK(ring.handle_completion({2, CompletionCode::cancelled, 0}));
            KB_CHECK(ring.handle_completion({3, CompletionCode::cancelled, 0}));
        }
        KB_CHECK(allocation_fault::attempts == 0);
        KB_CHECK(ring.state() == TransferRingState::failed);
        KB_CHECK(ring.error()->kind == TransferErrorKind::partial_transfer);
        KB_CHECK(ring.error()->certainty == DeliveryCertainty::partial_or_unknown);
        KB_CHECK(budget->used() == 0);
    }
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
    auto retried = scheduler.next(64);
    KB_CHECK(retried.has_value());
    KB_CHECK(scheduler.finish(std::move(*retried)));
    KB_CHECK(budget->used() == 0);
}

void test_scheduler_partial_unknown_retires_legacy_flow() {
    const auto budget = std::make_shared<BufferBudget>(64);
    WeightedControllerScheduler scheduler(budget, 64);
    KB_CHECK(scheduler.add_flow(DeviceFlowSpec{"device", "controller", 1, 128}));
    auto dispatch = scheduler.next(64);
    KB_CHECK(dispatch.has_value());
    KB_CHECK(scheduler.finish(
        std::move(*dispatch),
        kairosboot::fleet::FleetDispatchSettlement::partial_or_unknown));
    KB_CHECK(scheduler.remaining("device") == 64);
    KB_CHECK(scheduler.outstanding_count() == 0);
    KB_CHECK(!scheduler.next(64).has_value());
    KB_CHECK(budget->used() == 0);
}

void test_scheduler_partial_unknown_settles_before_budget_wake() {
    constexpr std::size_t chunk = 64;
    {
        const auto budget = std::make_shared<BufferBudget>(2U * chunk);
        WeightedControllerScheduler scheduler(budget, chunk);
        KB_CHECK(scheduler.add_flow(
            DeviceFlowSpec{"provider", "controller", 1, 3U * chunk, 3}));
        auto provider = scheduler.make_permit_provider("provider", 3);
        const auto blocker = std::make_shared<BlockingAvailabilityObserver>();
        budget->observe_availability(blocker);

        auto first = provider->try_acquire(chunk);
        auto second = provider->try_acquire(chunk);
        KB_CHECK(first.has_value());
        KB_CHECK(second.has_value());
        const auto observed = provider->readiness_generation();
        KB_CHECK(!provider->try_acquire(chunk).has_value());

        std::atomic<bool> waiter_started{false};
        TransferPermitWaitResult wait_result = TransferPermitWaitResult::timeout;
        std::optional<TransferPermit> unexpected;
        std::thread waiter([&] {
            waiter_started.store(true, std::memory_order_release);
            wait_result = provider->wait_for_ready(
                observed,
                std::chrono::steady_clock::now() + std::chrono::seconds(2));
            if (wait_result == TransferPermitWaitResult::ready) {
                unexpected = provider->try_acquire(chunk);
            }
        });
        while (!waiter_started.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        std::thread settler([&] {
            first->settle(TransferPermitSettlement::partial_or_unknown);
        });
        blocker->wait_until_entered();
        waiter.join();
        blocker->allow_release();
        settler.join();

        KB_CHECK(wait_result == TransferPermitWaitResult::cancelled);
        KB_CHECK(!unexpected.has_value());
        KB_CHECK(!provider->try_acquire(chunk).has_value());
        second->settle(TransferPermitSettlement::fully_transferred);
        if (unexpected.has_value()) {
            unexpected->settle(TransferPermitSettlement::partial_or_unknown);
        }
        KB_CHECK(scheduler.remaining("provider") == chunk);
        KB_CHECK(scheduler.outstanding_count() == 0);
        KB_CHECK(budget->used() == 0);
    }

    {
        const auto budget = std::make_shared<BufferBudget>(2U * chunk);
        WeightedControllerScheduler scheduler(budget, chunk);
        KB_CHECK(scheduler.add_flow(
            DeviceFlowSpec{"legacy", "controller", 1, 3U * chunk, 3}));
        const auto blocker = std::make_shared<BlockingAvailabilityObserver>();
        budget->observe_availability(blocker);

        auto first = scheduler.next(chunk);
        auto second = scheduler.next(chunk);
        KB_CHECK(first.has_value());
        KB_CHECK(second.has_value());
        KB_CHECK(!scheduler.next(chunk).has_value());
        bool finished = false;
        std::thread settler([&] {
            finished = scheduler.finish(
                std::move(*first),
                kairosboot::fleet::FleetDispatchSettlement::partial_or_unknown);
        });
        blocker->wait_until_entered();
        auto unexpected = scheduler.next(chunk);
        blocker->allow_release();
        settler.join();

        KB_CHECK(finished);
        KB_CHECK(!unexpected.has_value());
        KB_CHECK(!scheduler.next(chunk).has_value());
        KB_CHECK(scheduler.finish(std::move(*second)));
        if (unexpected.has_value()) {
            KB_CHECK(scheduler.finish(
                std::move(*unexpected),
                kairosboot::fleet::FleetDispatchSettlement::partial_or_unknown));
        }
        KB_CHECK(scheduler.remaining("legacy") == chunk);
        KB_CHECK(scheduler.outstanding_count() == 0);
        KB_CHECK(budget->used() == 0);
    }
}

void test_scheduler_skips_blocked_and_retired_flows() {
    const auto budget = std::make_shared<BufferBudget>(128);
    WeightedControllerScheduler scheduler(budget, 64);
    KB_CHECK(scheduler.add_flow(DeviceFlowSpec{"a", "controller", 1, 128}));
    KB_CHECK(scheduler.add_flow(DeviceFlowSpec{"b", "controller", 1, 128}));
    KB_CHECK(scheduler.add_flow(DeviceFlowSpec{"retired", "controller", 1, 128}));
    KB_CHECK(scheduler.retire_flow("retired"));

    auto first = scheduler.next(64);
    auto second = scheduler.next(64);
    KB_CHECK(first.has_value());
    KB_CHECK(second.has_value());
    KB_CHECK(first->device_id() == "a");
    KB_CHECK(second->device_id() == "b");
    KB_CHECK(!scheduler.next(64).has_value());
    KB_CHECK(scheduler.outstanding_count() == 2);
    KB_CHECK(scheduler.finish(std::move(*first)));
    auto third = scheduler.next(64);
    KB_CHECK(third.has_value());
    KB_CHECK(third->device_id() == "a");
    KB_CHECK(scheduler.finish(std::move(*second)));
    KB_CHECK(scheduler.finish(std::move(*third)));
    KB_CHECK(scheduler.outstanding_count() == 0);
    KB_CHECK(budget->used() == 0);
}

void test_scheduler_permit_broker_fairness_and_controller_progress() {
    {
        const auto budget = std::make_shared<BufferBudget>(64);
        WeightedControllerScheduler scheduler(budget, 64);
        KB_CHECK(scheduler.add_flow(
            DeviceFlowSpec{"cancelled", "controller", 1, 64}));
        auto provider = scheduler.make_permit_provider("cancelled");
        KB_CHECK(provider != nullptr);
        const auto observed = provider->readiness_generation();
        std::atomic<bool> waiter_started{false};
        TransferPermitWaitResult wait_result = TransferPermitWaitResult::timeout;
        std::thread waiter([&] {
            waiter_started.store(true, std::memory_order_release);
            wait_result = provider->wait_for_ready(
                observed,
                std::chrono::steady_clock::now() + std::chrono::seconds(2));
        });
        while (!waiter_started.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        provider->cancel_wait();
        waiter.join();
        KB_CHECK(wait_result == TransferPermitWaitResult::cancelled);
        KB_CHECK(!provider->try_acquire(64).has_value());
        KB_CHECK(scheduler.outstanding_count() == 0);
        KB_CHECK(budget->used() == 0);
    }

    {
        constexpr std::size_t quantum = 64;
        const auto budget = std::make_shared<BufferBudget>(4U * quantum);
        WeightedControllerScheduler scheduler(budget, quantum);
        KB_CHECK(scheduler.add_flow(
            DeviceFlowSpec{"low", "controller", 1, 4U * quantum}));
        KB_CHECK(scheduler.add_flow(
            DeviceFlowSpec{"high", "controller", 3, 12U * quantum}));
        auto low = scheduler.make_permit_provider("low");
        auto high = scheduler.make_permit_provider("high");
        std::size_t low_bytes = 0;
        std::size_t high_bytes = 0;
        for (std::size_t round = 0; round < 4; ++round) {
            auto low_permit = low->try_acquire(4U * quantum);
            auto high_permit = high->try_acquire(4U * quantum);
            KB_CHECK(low_permit.has_value());
            KB_CHECK(high_permit.has_value());
            low_bytes += low_permit->size();
            high_bytes += high_permit->size();
            low_permit->settle(TransferPermitSettlement::fully_transferred);
            high_permit->settle(TransferPermitSettlement::fully_transferred);
        }
        KB_CHECK(high_bytes == 3U * low_bytes);
        KB_CHECK(scheduler.outstanding_count() == 0);
        KB_CHECK(budget->used() == 0);
    }

    {
        constexpr std::size_t quantum = 64;
        const auto budget = std::make_shared<BufferBudget>(2U * quantum);
        WeightedControllerScheduler scheduler(budget, quantum);
        KB_CHECK(scheduler.add_flow(
            DeviceFlowSpec{"low", "controller", 1, 2U * quantum}));
        KB_CHECK(scheduler.add_flow(
            DeviceFlowSpec{"high", "controller", 3, 4U * quantum}));
        auto low = scheduler.make_permit_provider("low");
        auto high = scheduler.make_permit_provider("high");
        auto low_first = low->try_acquire(quantum);
        auto high_first = high->try_acquire(quantum);
        KB_CHECK(low_first.has_value());
        KB_CHECK(high_first.has_value());
        low_first->settle(TransferPermitSettlement::fully_transferred);
        high_first->settle(TransferPermitSettlement::fully_transferred);

        for (std::size_t chunk = 0; chunk < 3; ++chunk) {
            auto high_only = high->try_acquire(quantum);
            KB_CHECK(high_only.has_value());
            KB_CHECK(high_only->size() == quantum);
            high_only->settle(TransferPermitSettlement::fully_transferred);
        }
        KB_CHECK(scheduler.remaining("low") == quantum);
        KB_CHECK(scheduler.remaining("high") == 0);
        KB_CHECK(scheduler.outstanding_count() == 0);
        KB_CHECK(budget->used() == 0);
    }

    {
        BlockingReleaseObserver release_observer;
        const auto budget =
            std::make_shared<BufferBudget>(64, &release_observer);
        WeightedControllerScheduler scheduler(budget, 64);
        KB_CHECK(scheduler.add_flow(
            DeviceFlowSpec{"legacy", "controller-a", 1, 64}));
        KB_CHECK(scheduler.add_flow(
            DeviceFlowSpec{"provider", "controller-b", 1, 64}));
        auto dispatch = scheduler.next(64);
        auto provider = scheduler.make_permit_provider("provider");
        KB_CHECK(dispatch.has_value());
        KB_CHECK(provider != nullptr);
        const auto observed = provider->readiness_generation();
        KB_CHECK(!provider->try_acquire(64).has_value());
        bool finished = false;
        std::thread finisher([&] {
            finished = scheduler.finish(std::move(*dispatch));
        });
        release_observer.wait_until_entered();
        KB_CHECK(budget->used() == 64);
        KB_CHECK(provider->readiness_generation() == observed);
        release_observer.allow_release();
        finisher.join();
        KB_CHECK(finished);
        KB_CHECK(provider->wait_for_ready(
                     observed,
                     std::chrono::steady_clock::now() +
                         std::chrono::seconds(2)) ==
                 TransferPermitWaitResult::ready);
        auto permit = provider->try_acquire(64);
        KB_CHECK(permit.has_value());
        permit->settle(TransferPermitSettlement::fully_transferred);
        KB_CHECK(scheduler.outstanding_count() == 0);
        KB_CHECK(budget->used() == 0);
    }

    {
        BlockingReleaseObserver release_observer;
        const auto budget =
            std::make_shared<BufferBudget>(64, &release_observer);
        WeightedControllerScheduler scheduler(budget, 64);
        KB_CHECK(scheduler.add_flow(
            DeviceFlowSpec{"holder", "controller-a", 1, 64}));
        KB_CHECK(scheduler.add_flow(
            DeviceFlowSpec{"waiting", "controller-b", 1, 64}));
        auto holder_provider = scheduler.make_permit_provider("holder");
        auto waiting_provider = scheduler.make_permit_provider("waiting");
        auto holder = holder_provider->try_acquire(64);
        KB_CHECK(holder.has_value());
        const auto observed = waiting_provider->readiness_generation();
        KB_CHECK(!waiting_provider->try_acquire(64).has_value());
        std::thread finisher([&] {
            holder->settle(TransferPermitSettlement::fully_transferred);
        });
        release_observer.wait_until_entered();
        KB_CHECK(budget->used() == 64);
        KB_CHECK(waiting_provider->readiness_generation() == observed);
        release_observer.allow_release();
        finisher.join();
        KB_CHECK(waiting_provider->wait_for_ready(
                     observed,
                     std::chrono::steady_clock::now() +
                         std::chrono::seconds(2)) ==
                 TransferPermitWaitResult::ready);
        auto waiting = waiting_provider->try_acquire(64);
        KB_CHECK(waiting.has_value());
        waiting->settle(TransferPermitSettlement::fully_transferred);
        KB_CHECK(scheduler.outstanding_count() == 0);
        KB_CHECK(budget->used() == 0);
    }

    {
        BlockingReleaseObserver release_observer;
        const auto budget =
            std::make_shared<BufferBudget>(64, &release_observer);
        WeightedControllerScheduler scheduler(budget, 64);
        KB_CHECK(scheduler.add_flow(
            DeviceFlowSpec{"provider", "controller", 1, 64}));
        auto provider = scheduler.make_permit_provider("provider");
        auto external = budget->try_acquire(64);
        KB_CHECK(provider != nullptr);
        KB_CHECK(external.has_value());
        const auto observed = provider->readiness_generation();
        KB_CHECK(!provider->try_acquire(64).has_value());
        std::thread releaser([&] { external.reset(); });
        release_observer.wait_until_entered();
        KB_CHECK(budget->used() == 64);
        KB_CHECK(provider->readiness_generation() == observed);
        release_observer.allow_release();
        releaser.join();
        KB_CHECK(provider->wait_for_ready(
                     observed,
                     std::chrono::steady_clock::now() +
                         std::chrono::seconds(2)) ==
                 TransferPermitWaitResult::ready);
        auto permit = provider->try_acquire(64);
        KB_CHECK(permit.has_value());
        permit->settle(TransferPermitSettlement::fully_transferred);
        KB_CHECK(scheduler.outstanding_count() == 0);
        KB_CHECK(budget->used() == 0);
    }

    {
        const auto budget = std::make_shared<BufferBudget>(128);
        WeightedControllerScheduler scheduler(budget, 64);
        KB_CHECK(scheduler.add_flow(DeviceFlowSpec{"a", "controller", 1, 128}));
        KB_CHECK(scheduler.add_flow(DeviceFlowSpec{"b", "controller", 1, 64}));
        auto a = scheduler.make_permit_provider("a");
        auto b = scheduler.make_permit_provider("b");
        auto a_permit = a->try_acquire(64);
        KB_CHECK(a_permit.has_value());
        KB_CHECK(!a->try_acquire(64).has_value());
        auto b_permit = b->try_acquire(64);
        KB_CHECK(b_permit.has_value());
        KB_CHECK(scheduler.outstanding_count() == 2);
        a_permit->settle(TransferPermitSettlement::fully_transferred);
        b_permit->settle(TransferPermitSettlement::fully_transferred);
        KB_CHECK(budget->used() == 0);
    }

    {
        const auto budget = std::make_shared<BufferBudget>(128);
        WeightedControllerScheduler scheduler(budget, 64);
        KB_CHECK(scheduler.add_flow(DeviceFlowSpec{"a", "controller-a", 1, 64}));
        KB_CHECK(scheduler.add_flow(DeviceFlowSpec{"b", "controller-b", 1, 64}));
        auto a = scheduler.make_permit_provider("a");
        auto b = scheduler.make_permit_provider("b");
        auto a_permit = a->try_acquire(64);
        auto b_permit = b->try_acquire(64);
        KB_CHECK(a_permit.has_value());
        KB_CHECK(b_permit.has_value());
        KB_CHECK(scheduler.outstanding_count() == 2);
        a_permit->settle(TransferPermitSettlement::fully_transferred);
        b_permit->settle(TransferPermitSettlement::fully_transferred);
        KB_CHECK(budget->used() == 0);
    }

    {
        constexpr std::size_t actors = 32;
        constexpr std::size_t capacity = 8;
        const auto budget = std::make_shared<BufferBudget>(capacity * 64U);
        WeightedControllerScheduler scheduler(budget, 64);
        std::vector<std::shared_ptr<TransferPermitProvider>> providers;
        providers.reserve(actors);
        for (std::size_t index = 0; index < actors; ++index) {
            const auto device = "device-" + std::to_string(index);
            KB_CHECK(scheduler.add_flow(
                DeviceFlowSpec{device, "controller", 1, 64}));
            providers.push_back(scheduler.make_permit_provider(device));
            KB_CHECK(providers.back() != nullptr);
        }
        std::barrier start_gate(static_cast<std::ptrdiff_t>(actors + 1));
        std::mutex gate_mutex;
        std::condition_variable gate_changed;
        std::size_t acquired{};
        std::size_t waiting_actors{};
        bool release_first_wave{};
        std::atomic<std::size_t> completed{0};
        std::atomic<std::size_t> failures{0};
        std::vector<std::thread> threads;
        threads.reserve(actors);
        for (const auto& provider : providers) {
            threads.emplace_back([&, provider] {
                start_gate.arrive_and_wait();
                std::optional<TransferPermit> permit;
                bool reported_wait = false;
                const auto deadline =
                    std::chrono::steady_clock::now() + std::chrono::seconds(5);
                while (!permit.has_value()) {
                    const auto observed = provider->readiness_generation();
                    permit = provider->try_acquire(64);
                    if (!permit.has_value()) {
                        if (!reported_wait) {
                            std::scoped_lock lock(gate_mutex);
                            reported_wait = true;
                            ++waiting_actors;
                            gate_changed.notify_all();
                        }
                        if (provider->wait_for_ready(observed, deadline) !=
                            TransferPermitWaitResult::ready) {
                            failures.fetch_add(1, std::memory_order_release);
                            return;
                        }
                    }
                }
                {
                    std::unique_lock lock(gate_mutex);
                    ++acquired;
                    gate_changed.notify_all();
                    if (acquired <= capacity) {
                        gate_changed.wait(lock, [&] { return release_first_wave; });
                    }
                }
                permit->settle(TransferPermitSettlement::fully_transferred);
                completed.fetch_add(1, std::memory_order_release);
            });
        }
        start_gate.arrive_and_wait();
        {
            std::unique_lock lock(gate_mutex);
            gate_changed.wait(lock, [&] {
                return acquired == capacity &&
                    waiting_actors == actors - capacity;
            });
            KB_CHECK(scheduler.outstanding_count() == capacity);
            KB_CHECK(budget->used() == capacity * 64U);
            release_first_wave = true;
        }
        gate_changed.notify_all();
        for (auto& thread : threads) {
            thread.join();
        }
        KB_CHECK(failures.load(std::memory_order_acquire) == 0);
        KB_CHECK(completed.load(std::memory_order_acquire) == actors);
        KB_CHECK(waiting_actors == actors - capacity);
        KB_CHECK(scheduler.outstanding_count() == 0);
        KB_CHECK(budget->used() == 0);
    }
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
        {"buffer availability observer reentrancy",
         test_buffer_availability_observers_are_reentrant_and_destroyed_outside_lock},
        {"disabled transfer telemetry has zero clock cost",
         test_transfer_telemetry_disabled_has_zero_clock_cost},
        {"transfer telemetry counts and watermark",
         test_transfer_telemetry_counts_and_out_of_order_watermark},
        {"transfer telemetry does not invent budget waits",
         test_transfer_telemetry_budget_contention_does_not_invent_waits},
        {"transfer telemetry cancel and error counts",
         test_transfer_telemetry_cancel_and_error_counts},
        {"out-of-order completion watermark", test_transfer_ring_out_of_order_watermark},
        {"transfer ring shared budget", test_transfer_ring_respects_shared_budget},
        {"transfer ring permit buffers",
         test_transfer_ring_uses_permit_buffers_without_double_accounting},
        {"transfer ring permit certainty",
         test_transfer_ring_permit_settlement_certainty},
        {"transfer ring permit watermark and cancel drain",
         test_transfer_ring_permit_out_of_order_watermark_and_cancel_drain},
        {"partial transfer cancel and drain", test_transfer_ring_partial_failure_cancels_and_drains},
        {"no-device error classification", test_transfer_ring_no_device_classification},
        {"explicit cancellation drain", test_transfer_ring_cancel_waits_for_drain},
        {"cancel and failure drain do not allocate",
         test_transfer_ring_cancel_and_failure_drain_do_not_allocate},
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
        {"partial-unknown retires legacy flow",
         test_scheduler_partial_unknown_retires_legacy_flow},
        {"partial-unknown settles before budget wake",
         test_scheduler_partial_unknown_settles_before_budget_wake},
        {"blocked and retired scheduler flows",
         test_scheduler_skips_blocked_and_retired_flows},
        {"scheduler permit broker fairness",
         test_scheduler_permit_broker_fairness_and_controller_progress},
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
