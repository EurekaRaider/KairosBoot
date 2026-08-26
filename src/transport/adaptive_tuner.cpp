#include "src/transport/adaptive_tuner.hpp"

#include <algorithm>
#include <stdexcept>

namespace kairosboot::transport {

namespace {

[[nodiscard]] std::size_t doubled_clamped(const std::size_t value,
                                          const std::size_t maximum) noexcept {
    if (value >= maximum || value > maximum - value) {
        return maximum;
    }
    return std::min(maximum, value + value);
}

}  // namespace

AdaptiveTransferTuner::AdaptiveTransferTuner(const TuningBounds bounds) : bounds_(bounds) {
    if (bounds_.minimum_chunk == 0 || bounds_.minimum_depth == 0 ||
        bounds_.minimum_chunk > bounds_.maximum_chunk ||
        bounds_.minimum_depth > bounds_.maximum_depth) {
        throw std::invalid_argument("invalid adaptive transfer tuning bounds");
    }
    reset();
}

TuningDecision AdaptiveTransferTuner::current() const noexcept { return decision_; }

std::expected<TuningDecision, TuningSampleError> AdaptiveTransferTuner::observe(
    const TuningSample& sample) noexcept {
    if (sample.completed_bytes == 0 && !sample.transport_error) {
        return std::unexpected(TuningSampleError::empty_sample);
    }
    if (sample.elapsed <= std::chrono::nanoseconds::zero() && !sample.transport_error) {
        return std::unexpected(TuningSampleError::non_positive_duration);
    }

    if (sample.transport_error) {
        apply_multiplicative_decrease(true);
        throughput_ewma_ = 0.0;
        consecutive_improvements_ = 0;
        return decision_;
    }

    const auto measured = throughput(sample);
    if (sample.memory_pressure) {
        apply_multiplicative_decrease(decision_.depth == bounds_.minimum_depth);
        throughput_ewma_ = measured;
        consecutive_improvements_ = 0;
        return decision_;
    }

    if (throughput_ewma_ == 0.0) {
        throughput_ewma_ = measured;
        return decision_;
    }

    const auto relative_change = measured / throughput_ewma_ - 1.0;
    if (relative_change >= 0.03) {
        ++consecutive_improvements_;
        increase_pipeline();
        if (consecutive_improvements_ >= 2) {
            decision_.chunk_size = doubled_clamped(decision_.chunk_size,
                                                   bounds_.maximum_chunk);
            consecutive_improvements_ = 0;
        }
    } else if (relative_change <= -0.075) {
        apply_multiplicative_decrease(true);
        consecutive_improvements_ = 0;
    } else if (sample.queue_starved) {
        increase_pipeline();
        consecutive_improvements_ = 0;
    } else {
        consecutive_improvements_ = 0;
    }

    throughput_ewma_ = throughput_ewma_ * 0.75 + measured * 0.25;
    return decision_;
}

void AdaptiveTransferTuner::reset() noexcept {
    decision_.chunk_size = std::clamp(kDefaultTunedChunk,
                                      bounds_.minimum_chunk,
                                      bounds_.maximum_chunk);
    decision_.depth = std::clamp(kDefaultTunedDepth,
                                 bounds_.minimum_depth,
                                 bounds_.maximum_depth);
    throughput_ewma_ = 0.0;
    consecutive_improvements_ = 0;
}

void AdaptiveTransferTuner::apply_multiplicative_decrease(
    const bool reduce_chunk) noexcept {
    decision_.depth = std::max(bounds_.minimum_depth, decision_.depth / 2);
    if (reduce_chunk) {
        decision_.chunk_size = std::max(bounds_.minimum_chunk, decision_.chunk_size / 2);
    }
}

void AdaptiveTransferTuner::increase_pipeline() noexcept {
    const auto increment = std::max<std::size_t>(1, decision_.depth / 4);
    if (decision_.depth >= bounds_.maximum_depth ||
        increment > bounds_.maximum_depth - decision_.depth) {
        decision_.depth = bounds_.maximum_depth;
    } else {
        decision_.depth += increment;
    }
}

double AdaptiveTransferTuner::throughput(const TuningSample& sample) const noexcept {
    const auto seconds = std::chrono::duration<double>(sample.elapsed).count();
    return static_cast<double>(sample.completed_bytes) / seconds;
}

}  // namespace kairosboot::transport
