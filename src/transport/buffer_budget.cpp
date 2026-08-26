#include "src/transport/buffer_budget.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(__linux__)
#include <unistd.h>
#elif defined(__APPLE__)
#include <sys/sysctl.h>
#else
#error "KairosBoot supports process memory budgets on Windows, Linux, and macOS"
#endif

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

namespace {

#if defined(_WIN32)
[[nodiscard]] PhysicalMemoryResult query_platform_physical_memory() noexcept {
    MEMORYSTATUSEX status{};
    status.dwLength = static_cast<DWORD>(sizeof(status));
    if (GlobalMemoryStatusEx(&status) == FALSE) {
        return {
            .status = PhysicalMemoryStatus::query_failed,
            .bytes = 0,
            .native_code = static_cast<std::uint32_t>(GetLastError()),
        };
    }
    if (status.ullTotalPhys == 0) {
        return {
            .status = PhysicalMemoryStatus::query_failed,
            .bytes = 0,
            .native_code = static_cast<std::uint32_t>(ERROR_INVALID_DATA),
        };
    }
    return {
        .status = PhysicalMemoryStatus::measured,
        .bytes = static_cast<std::uint64_t>(status.ullTotalPhys),
        .native_code = 0,
    };
}
#elif defined(__linux__)
[[nodiscard]] constexpr bool multiply_overflows(
    const std::uint64_t left,
    const std::uint64_t right) noexcept {
    return left != 0 &&
           right > std::numeric_limits<std::uint64_t>::max() / left;
}

[[nodiscard]] PhysicalMemoryResult query_platform_physical_memory() noexcept {
    errno = 0;
    const auto pages = sysconf(_SC_PHYS_PAGES);
    const auto pages_error = errno;
    errno = 0;
    const auto page_size = sysconf(_SC_PAGESIZE);
    const auto page_size_error = errno;
    if (pages <= 0 || page_size <= 0) {
        const auto native_error = pages <= 0 ? pages_error : page_size_error;
        return {
            .status = PhysicalMemoryStatus::query_failed,
            .bytes = 0,
            .native_code = static_cast<std::uint32_t>(
                native_error == 0 ? EINVAL : native_error),
        };
    }

    const auto page_count = static_cast<std::uint64_t>(pages);
    const auto bytes_per_page = static_cast<std::uint64_t>(page_size);
    if (multiply_overflows(page_count, bytes_per_page)) {
        return {
            .status = PhysicalMemoryStatus::arithmetic_overflow,
            .bytes = 0,
            .native_code = static_cast<std::uint32_t>(EOVERFLOW),
        };
    }
    return {
        .status = PhysicalMemoryStatus::measured,
        .bytes = page_count * bytes_per_page,
        .native_code = 0,
    };
}
#elif defined(__APPLE__)
[[nodiscard]] PhysicalMemoryResult query_platform_physical_memory() noexcept {
    std::uint64_t physical_memory = 0;
    std::size_t result_size = sizeof(physical_memory);
    errno = 0;
    if (sysctlbyname("hw.memsize",
                     &physical_memory,
                     &result_size,
                     nullptr,
                     0) != 0 ||
        result_size != sizeof(physical_memory) || physical_memory == 0) {
        const auto native_error = errno;
        return {
            .status = PhysicalMemoryStatus::query_failed,
            .bytes = 0,
            .native_code = static_cast<std::uint32_t>(
                native_error == 0 ? EINVAL : native_error),
        };
    }
    return {
        .status = PhysicalMemoryStatus::measured,
        .bytes = physical_memory,
        .native_code = 0,
    };
}
#endif

[[nodiscard]] ProcessUsbBufferBudgetRegistry& process_budget_registry() {
    static ProcessUsbBufferBudgetRegistry registry;
    return registry;
}

}  // namespace

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

PhysicalMemoryResult query_physical_memory(void*) noexcept {
    return query_platform_physical_memory();
}

ProcessUsbBufferBudgetInfo resolve_process_usb_buffer_budget(
    const PhysicalMemoryQuery query,
    void* const user_data) noexcept {
    const auto result = query == nullptr
        ? PhysicalMemoryResult{
              .status = PhysicalMemoryStatus::query_failed,
              .bytes = 0,
              .native_code = 0,
          }
        : query(user_data);
    if (result.status == PhysicalMemoryStatus::measured && result.bytes != 0) {
        return {
            .limit_bytes = calculate_process_usb_buffer_budget(result.bytes),
            .physical_memory_bytes = result.bytes,
            .query_status = result.status,
            .native_code = result.native_code,
            .fallback_used = false,
        };
    }
    return {
        .limit_bytes = kProcessUsbBufferBudgetFallbackBytes,
        .physical_memory_bytes = 0,
        .query_status = result.status == PhysicalMemoryStatus::measured
            ? PhysicalMemoryStatus::query_failed
            : result.status,
        .native_code = result.native_code,
        .fallback_used = true,
    };
}

ProcessUsbBufferBudgetRegistry::ProcessUsbBufferBudgetRegistry(
    const PhysicalMemoryQuery query,
    void* const user_data)
    : info_(resolve_process_usb_buffer_budget(query, user_data)),
      budget_(std::make_shared<BufferBudget>(info_.limit_bytes)) {}

std::shared_ptr<BufferBudget> ProcessUsbBufferBudgetRegistry::budget() const noexcept {
    return budget_;
}

const ProcessUsbBufferBudgetInfo& ProcessUsbBufferBudgetRegistry::info() const noexcept {
    return info_;
}

std::shared_ptr<BufferBudget> process_usb_buffer_budget() {
    return process_budget_registry().budget();
}

const ProcessUsbBufferBudgetInfo& process_usb_buffer_budget_info() {
    return process_budget_registry().info();
}

}  // namespace kairosboot::transport
