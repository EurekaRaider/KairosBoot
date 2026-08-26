#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
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

inline constexpr std::uint64_t kProcessUsbBufferBudgetCapBytes =
    2ULL * 1024ULL * 1024ULL * 1024ULL;
inline constexpr std::size_t kProcessUsbBufferBudgetFallbackBytes =
    64ULL * 1024ULL * 1024ULL;

enum class PhysicalMemoryStatus : std::uint8_t {
    measured,
    query_failed,
    arithmetic_overflow,
};

struct PhysicalMemoryResult final {
    PhysicalMemoryStatus status{PhysicalMemoryStatus::query_failed};
    std::uint64_t bytes{};
    std::uint32_t native_code{};
};

using PhysicalMemoryQuery =
    PhysicalMemoryResult (*)(void* user_data) noexcept;

struct ProcessUsbBufferBudgetInfo final {
    std::size_t limit_bytes{};
    std::uint64_t physical_memory_bytes{};
    PhysicalMemoryStatus query_status{PhysicalMemoryStatus::query_failed};
    std::uint32_t native_code{};
    bool fallback_used{true};
};

[[nodiscard]] constexpr std::size_t calculate_process_usb_buffer_budget(
    const std::uint64_t physical_memory_bytes) noexcept {
    // Division expresses 20% without overflowing for UINT64_MAX inputs.
    const auto twenty_percent = physical_memory_bytes / 5U;
    const auto capped = twenty_percent < kProcessUsbBufferBudgetCapBytes
        ? twenty_percent
        : kProcessUsbBufferBudgetCapBytes;
    const auto representable = static_cast<std::uint64_t>(
        std::numeric_limits<std::size_t>::max());
    return static_cast<std::size_t>(capped < representable ? capped
                                                           : representable);
}

[[nodiscard]] PhysicalMemoryResult query_physical_memory(
    void* user_data) noexcept;

[[nodiscard]] ProcessUsbBufferBudgetInfo resolve_process_usb_buffer_budget(
    PhysicalMemoryQuery query,
    void* user_data = nullptr) noexcept;

// Owns one shared accounting budget. The process registry invokes the platform
// query exactly once; tests may inject a deterministic query into a local
// registry.
class ProcessUsbBufferBudgetRegistry final {
public:
    explicit ProcessUsbBufferBudgetRegistry(
        PhysicalMemoryQuery query = &query_physical_memory,
        void* user_data = nullptr);

    [[nodiscard]] std::shared_ptr<BufferBudget> budget() const noexcept;
    [[nodiscard]] const ProcessUsbBufferBudgetInfo& info() const noexcept;

private:
    ProcessUsbBufferBudgetInfo info_;
    std::shared_ptr<BufferBudget> budget_;
};

[[nodiscard]] std::shared_ptr<BufferBudget> process_usb_buffer_budget();
[[nodiscard]] const ProcessUsbBufferBudgetInfo& process_usb_buffer_budget_info();

}  // namespace kairosboot::transport
