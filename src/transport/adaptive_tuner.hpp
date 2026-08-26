#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>

namespace kairosboot::transport {

inline constexpr std::size_t kMinimumTunedChunk = 256U * 1024U;
inline constexpr std::size_t kMaximumTunedChunk = 4U * 1024U * 1024U;
inline constexpr std::size_t kDefaultTunedChunk = 1024U * 1024U;
inline constexpr std::size_t kMinimumTunedDepth = 2;
inline constexpr std::size_t kMaximumTunedDepth = 32;
inline constexpr std::size_t kDefaultTunedDepth = 8;

struct TuningBounds final {
    std::size_t minimum_chunk{kMinimumTunedChunk};
    std::size_t maximum_chunk{kMaximumTunedChunk};
    std::size_t minimum_depth{kMinimumTunedDepth};
    std::size_t maximum_depth{kMaximumTunedDepth};
};

struct TuningDecision final {
    std::size_t chunk_size{kDefaultTunedChunk};
    std::size_t depth{kDefaultTunedDepth};
};

struct TuningSample final {
    std::uint64_t completed_bytes{};
    std::chrono::nanoseconds elapsed{};
    bool queue_starved{false};
    bool memory_pressure{false};
    bool transport_error{false};
};

enum class TuningSampleError : std::uint8_t {
    empty_sample,
    non_positive_duration,
};

// A bounded AIMD controller. It deliberately changes only one small step per
// observation; benchmark code remains responsible for selecting stable windows.
class AdaptiveTransferTuner final {
public:
    explicit AdaptiveTransferTuner(TuningBounds bounds = {});

    [[nodiscard]] TuningDecision current() const noexcept;
    [[nodiscard]] std::expected<TuningDecision, TuningSampleError> observe(
        const TuningSample& sample) noexcept;
    void reset() noexcept;

private:
    void apply_multiplicative_decrease(bool reduce_chunk) noexcept;
    void increase_pipeline() noexcept;
    [[nodiscard]] double throughput(const TuningSample& sample) const noexcept;

    TuningBounds bounds_;
    TuningDecision decision_;
    double throughput_ewma_{0.0};
    std::size_t consecutive_improvements_{0};
};

}  // namespace kairosboot::transport
