#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <span>

namespace kairosboot::transport {

namespace detail {
struct BufferBudgetState;
struct BufferLeaseStorage;
}

// Internal observability seam used to verify that storage is released before
// its accounting charge becomes available to another thread. The observer
// must outlive the budget and every lease acquired from it.
class BufferBudgetReleaseObserver {
public:
    virtual ~BufferBudgetReleaseObserver() = default;
    virtual void on_buffer_released() noexcept = 0;
};

// A move-only reservation whose storage and budget charge have the same lifetime.
class BufferLease final {
public:
    BufferLease() = default;
    BufferLease(const BufferLease&) = delete;
    BufferLease& operator=(const BufferLease&) = delete;
    BufferLease(BufferLease&& other) noexcept;
    BufferLease& operator=(BufferLease&& other) noexcept;
    ~BufferLease();

    [[nodiscard]] explicit operator bool() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] std::span<std::byte> bytes() noexcept;
    [[nodiscard]] std::span<const std::byte> bytes() const noexcept;
    [[nodiscard]] std::shared_ptr<const void> lifetime_token() const noexcept;

private:
    friend class BufferBudget;

    explicit BufferLease(std::shared_ptr<detail::BufferLeaseStorage> storage);

    std::shared_ptr<detail::BufferLeaseStorage> storage_;
};

// Thread-safe accounting shared by transfer rings and fleet dispatchers.
class BufferBudget final {
public:
    explicit BufferBudget(std::size_t limit_bytes,
                          BufferBudgetReleaseObserver* release_observer = nullptr);

    [[nodiscard]] std::optional<BufferLease> try_acquire(std::size_t bytes) const;
    [[nodiscard]] std::size_t limit() const noexcept;
    [[nodiscard]] std::size_t used() const noexcept;
    [[nodiscard]] std::size_t available() const noexcept;
    [[nodiscard]] std::size_t peak_used() const noexcept;

private:
    std::shared_ptr<detail::BufferBudgetState> state_;
};

}  // namespace kairosboot::transport
