#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace kairosboot::transport {

namespace detail {
struct BufferBudgetState;
}

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

private:
    friend class BufferBudget;

    BufferLease(std::shared_ptr<detail::BufferBudgetState> state, std::size_t bytes);
    void release() noexcept;

    std::shared_ptr<detail::BufferBudgetState> state_;
    std::vector<std::byte> storage_;
    std::size_t reserved_bytes_{0};
};

// Thread-safe accounting shared by transfer rings and fleet dispatchers.
class BufferBudget final {
public:
    explicit BufferBudget(std::size_t limit_bytes);

    [[nodiscard]] std::optional<BufferLease> try_acquire(std::size_t bytes) const;
    [[nodiscard]] std::size_t limit() const noexcept;
    [[nodiscard]] std::size_t used() const noexcept;
    [[nodiscard]] std::size_t available() const noexcept;
    [[nodiscard]] std::size_t peak_used() const noexcept;

private:
    std::shared_ptr<detail::BufferBudgetState> state_;
};

}  // namespace kairosboot::transport
