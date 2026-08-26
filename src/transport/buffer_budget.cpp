#include "src/transport/buffer_budget.hpp"

#include <algorithm>
#include <atomic>
#include <utility>
#include <vector>

namespace kairosboot::transport {

namespace detail {

struct BufferBudgetState final {
    BufferBudgetState(const std::size_t limit_bytes,
                      BufferBudgetReleaseObserver* release_observer_value)
        : limit(limit_bytes), release_observer(release_observer_value) {}

    const std::size_t limit;
    BufferBudgetReleaseObserver* const release_observer;
    std::atomic<std::size_t> used{0};
    std::atomic<std::size_t> peak{0};
};

struct BufferLeaseStorage final {
    BufferLeaseStorage(std::shared_ptr<BufferBudgetState> budget_state,
                       const std::size_t bytes)
        : budget(std::move(budget_state)), buffer(bytes), reserved_bytes(bytes) {}

    ~BufferLeaseStorage() {
        // A destructor body runs before member destruction. Release the vector
        // explicitly so another thread cannot observe returned budget while
        // the corresponding allocation is still live.
        {
            std::vector<std::byte> released;
            released.swap(buffer);
        }
        if (budget->release_observer != nullptr) {
            budget->release_observer->on_buffer_released();
        }
        budget->used.fetch_sub(reserved_bytes, std::memory_order_acq_rel);
    }

    std::shared_ptr<BufferBudgetState> budget;
    std::vector<std::byte> buffer;
    std::size_t reserved_bytes{};
};

}  // namespace detail

BufferLease::BufferLease(std::shared_ptr<detail::BufferLeaseStorage> storage)
    : storage_(std::move(storage)) {}

BufferLease::BufferLease(BufferLease&& other) noexcept
    : storage_(std::move(other.storage_)) {}

BufferLease& BufferLease::operator=(BufferLease&& other) noexcept {
    if (this != &other) {
        storage_ = std::move(other.storage_);
    }
    return *this;
}

BufferLease::~BufferLease() = default;

BufferLease::operator bool() const noexcept { return storage_ != nullptr; }

std::size_t BufferLease::size() const noexcept {
    return storage_ == nullptr ? 0 : storage_->buffer.size();
}

std::span<std::byte> BufferLease::bytes() noexcept {
    return storage_ == nullptr ? std::span<std::byte>{}
                               : std::span<std::byte>{storage_->buffer};
}

std::span<const std::byte> BufferLease::bytes() const noexcept {
    return storage_ == nullptr ? std::span<const std::byte>{}
                               : std::span<const std::byte>{storage_->buffer};
}

std::shared_ptr<const void> BufferLease::lifetime_token() const noexcept {
    return storage_;
}

BufferBudget::BufferBudget(const std::size_t limit_bytes,
                           BufferBudgetReleaseObserver* const release_observer)
    : state_(std::make_shared<detail::BufferBudgetState>(limit_bytes,
                                                         release_observer)) {}

std::optional<BufferLease> BufferBudget::try_acquire(const std::size_t bytes) const {
    if (bytes == 0) {
        return BufferLease{std::make_shared<detail::BufferLeaseStorage>(state_, 0)};
    }

    auto current = state_->used.load(std::memory_order_acquire);
    for (;;) {
        if (current > state_->limit || bytes > state_->limit - current) {
            return std::nullopt;
        }
        if (state_->used.compare_exchange_weak(current,
                                               current + bytes,
                                               std::memory_order_acq_rel,
                                               std::memory_order_acquire)) {
            break;
        }
    }

    auto peak = state_->peak.load(std::memory_order_relaxed);
    while (peak < current + bytes &&
           !state_->peak.compare_exchange_weak(peak,
                                               current + bytes,
                                               std::memory_order_relaxed,
                                               std::memory_order_relaxed)) {
    }

    try {
        auto storage = std::make_shared<detail::BufferLeaseStorage>(state_, bytes);
        return BufferLease{std::move(storage)};
    } catch (...) {
        state_->used.fetch_sub(bytes, std::memory_order_acq_rel);
        throw;
    }
}

std::size_t BufferBudget::limit() const noexcept { return state_->limit; }

std::size_t BufferBudget::used() const noexcept {
    return state_->used.load(std::memory_order_acquire);
}

std::size_t BufferBudget::available() const noexcept {
    const auto current = used();
    return current >= state_->limit ? 0 : state_->limit - current;
}

std::size_t BufferBudget::peak_used() const noexcept {
    return state_->peak.load(std::memory_order_relaxed);
}

}  // namespace kairosboot::transport
