#pragma once

#include "src/transport/buffer_budget.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace kairosboot::fleet {

inline constexpr std::uint64_t kDefaultGlobalMemoryCap = 2ULL * 1024ULL * 1024ULL * 1024ULL;

[[nodiscard]] constexpr std::uint64_t calculate_global_memory_budget(
    const std::uint64_t physical_memory_bytes) noexcept {
    const auto twenty_percent = physical_memory_bytes / 5U;
    return twenty_percent < kDefaultGlobalMemoryCap ? twenty_percent
                                                     : kDefaultGlobalMemoryCap;
}

struct DeviceFlowSpec final {
    std::string device_id;
    std::string controller_id;
    std::uint32_t weight{1};
    std::uint64_t bytes_remaining{};
};

class FleetDispatch final {
public:
    FleetDispatch(const FleetDispatch&) = delete;
    FleetDispatch& operator=(const FleetDispatch&) = delete;
    FleetDispatch(FleetDispatch&& other) noexcept;
    FleetDispatch& operator=(FleetDispatch&& other) noexcept = delete;
    ~FleetDispatch() = default;

    [[nodiscard]] std::uint64_t token() const noexcept;
    [[nodiscard]] std::string_view device_id() const noexcept;
    [[nodiscard]] std::string_view controller_id() const noexcept;
    [[nodiscard]] std::size_t bytes() const noexcept;
    [[nodiscard]] std::span<std::byte> buffer() noexcept;

private:
    friend class WeightedControllerScheduler;

    FleetDispatch(std::uint64_t token,
                  std::string device_id,
                  std::string controller_id,
                  std::size_t bytes,
                  transport::BufferLease memory);

    std::uint64_t token_{0};
    std::string device_id_;
    std::string controller_id_;
    std::size_t bytes_{0};
    transport::BufferLease memory_;
};

// Weighted deficit round-robin within each USB controller and round-robin
// across controllers. Calls are serialized by the fleet coordinator.
class WeightedControllerScheduler final {
public:
    explicit WeightedControllerScheduler(std::shared_ptr<transport::BufferBudget> budget,
                                         std::size_t quantum_bytes = 256U * 1024U);
    ~WeightedControllerScheduler();

    WeightedControllerScheduler(const WeightedControllerScheduler&) = delete;
    WeightedControllerScheduler& operator=(const WeightedControllerScheduler&) = delete;
    WeightedControllerScheduler(WeightedControllerScheduler&&) noexcept;
    WeightedControllerScheduler& operator=(WeightedControllerScheduler&&) noexcept;

    [[nodiscard]] bool add_flow(DeviceFlowSpec flow);
    [[nodiscard]] std::optional<FleetDispatch> next(std::size_t maximum_bytes);

    // Releases the memory reservation. bytes_not_sent is requeued only when the
    // caller can prove those bytes never reached the transport.
    [[nodiscard]] bool finish(FleetDispatch&& dispatch,
                              std::size_t bytes_not_sent = 0) noexcept;

    [[nodiscard]] std::uint64_t remaining(std::string_view device_id) const noexcept;
    [[nodiscard]] std::size_t flow_count() const noexcept;
    [[nodiscard]] std::size_t outstanding_count() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace kairosboot::fleet
