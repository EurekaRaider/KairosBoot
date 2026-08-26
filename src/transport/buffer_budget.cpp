#include "src/transport/buffer_budget.hpp"

#include <algorithm>
#include <atomic>
#include <utility>

namespace kairosboot::transport {

namespace detail {

struct BufferBudgetState final {
    explicit BufferBudgetState(const std::size_t limit_bytes) : limit(limit_bytes) {}

    const std::size_t limit;
    std::atomic<std::size_t> used{0};
    std::atomic<std::size_t> peak{0};
};

}  // namespace detail

BufferLease::BufferLease(std::shared_ptr<detail::BufferBudgetState> state,
                         const std::size_t bytes)
    : state_(std::move(state)), storage_(bytes), reserved_bytes_(bytes) {}

BufferLease::BufferLease(BufferLease&& other) noexcept
    : state_(std::move(other.state_)),
      storage_(std::move(other.storage_)),
      reserved_bytes_(std::exchange(other.reserved_bytes_, 0)) {}

BufferLease& BufferLease::operator=(BufferLease&& other) noexcept {
    if (this != &other) {
        release();
        state_ = std::move(other.state_);
        storage_ = std::move(other.storage_);
        reserved_bytes_ = std::exchange(other.reserved_bytes_, 0);
    }
    return *this;
}

BufferLease::~BufferLease() { release(); }

BufferLease::operator bool() const noexcept { return state_ != nullptr; }

std::size_t BufferLease::size() const noexcept { return storage_.size(); }

std::span<std::byte> BufferLease::bytes() noexcept { return storage_; }

std::span<const std::byte> BufferLease::bytes() const noexcept { return storage_; }

void BufferLease::release() noexcept {
    if (state_ != nullptr) {
        state_->used.fetch_sub(reserved_bytes_, std::memory_order_acq_rel);
        state_.reset();
        reserved_bytes_ = 0;
        storage_.clear();
    }
}

BufferBudget::BufferBudget(const std::size_t limit_bytes)
    : state_(std::make_shared<detail::BufferBudgetState>(limit_bytes)) {}

std::optional<BufferLease> BufferBudget::try_acquire(const std::size_t bytes) const {
    if (bytes == 0) {
        return BufferLease{state_, 0};
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
        return BufferLease{state_, bytes};
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
