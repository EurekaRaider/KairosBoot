#include "src/transport/transfer_ring.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

namespace kairosboot::transport {

namespace {

[[nodiscard]] TransferErrorKind submit_error_kind(const SubmitResult result) noexcept {
    switch (result) {
        case SubmitResult::no_device:
            return TransferErrorKind::submit_no_device;
        case SubmitResult::resource_exhausted:
            return TransferErrorKind::submit_resource_exhausted;
        case SubmitResult::io_error:
            return TransferErrorKind::submit_io;
        case SubmitResult::accepted:
            break;
    }
    return TransferErrorKind::submit_io;
}

[[nodiscard]] TransferErrorKind completion_error_kind(const CompletionCode code) noexcept {
    switch (code) {
        case CompletionCode::no_device:
            return TransferErrorKind::completion_no_device;
        case CompletionCode::timeout:
            return TransferErrorKind::completion_timeout;
        case CompletionCode::stall:
            return TransferErrorKind::completion_stall;
        case CompletionCode::io_error:
            return TransferErrorKind::completion_io;
        case CompletionCode::cancelled:
            return TransferErrorKind::unexpected_cancellation;
        case CompletionCode::success:
            break;
    }
    return TransferErrorKind::completion_io;
}

}  // namespace

MemoryTransferSource::MemoryTransferSource(std::vector<std::byte> bytes)
    : bytes_(std::move(bytes)) {}

std::uint64_t MemoryTransferSource::size() const noexcept {
    return static_cast<std::uint64_t>(bytes_.size());
}

bool MemoryTransferSource::read_exact(const std::uint64_t offset,
                                      const std::span<std::byte> destination) noexcept {
    if (offset > bytes_.size()) {
        return false;
    }
    const auto start = static_cast<std::size_t>(offset);
    if (destination.size() > bytes_.size() - start) {
        return false;
    }
    if (!destination.empty()) {
        std::memcpy(destination.data(), bytes_.data() + start, destination.size());
    }
    return true;
}

TransferRing::TransferRing(TransferBackend& backend,
                           std::shared_ptr<BufferBudget> budget,
                           const TransferRingConfig config)
    : backend_(backend), budget_(std::move(budget)), config_(config) {}

bool TransferRing::start(std::shared_ptr<TransferSource> source,
                         const bool logical_message_end) {
    if (state_ != TransferRingState::idle || source == nullptr || budget_ == nullptr ||
        config_.chunk_size == 0 || config_.depth == 0 ||
        config_.chunk_size > budget_->limit()) {
        if (state_ == TransferRingState::idle) {
            error_ = TransferError{TransferErrorKind::invalid_configuration,
                                   DeliveryCertainty::not_sent};
            state_ = TransferRingState::failed;
        }
        return false;
    }

    source_ = std::move(source);
    total_bytes_ = source_->size();
    logical_message_end_ = logical_message_end;
    state_ = TransferRingState::running;
    if (total_bytes_ == 0) {
        state_ = TransferRingState::completed;
        return true;
    }
    static_cast<void>(pump());
    return state_ == TransferRingState::running || state_ == TransferRingState::completed;
}

bool TransferRing::pump() {
    if (state_ != TransferRingState::running) {
        return false;
    }

    bool submitted = false;
    while (state_ == TransferRingState::running && in_flight_.size() < config_.depth &&
           next_offset_ < total_bytes_) {
        const auto remaining = total_bytes_ - next_offset_;
        const auto chunk = static_cast<std::size_t>(std::min<std::uint64_t>(
            remaining, static_cast<std::uint64_t>(config_.chunk_size)));
        auto lease = budget_->try_acquire(chunk);
        if (!lease.has_value()) {
            break;
        }
        if (!source_->read_exact(next_offset_, lease->bytes())) {
            begin_failure(TransferErrorKind::source_read);
            break;
        }

        const auto id = next_id_++;
        const auto offset = next_offset_;
        auto [entry, inserted] = in_flight_.emplace(
            id, InFlight{offset, chunk, std::move(*lease)});
        if (!inserted) {
            begin_failure(TransferErrorKind::submit_io);
            break;
        }

        const TransferSubmission submission{
            id,
            offset,
            entry->second.buffer.bytes(),
            entry->second.buffer.lifetime_token(),
            logical_message_end_ && offset + chunk == total_bytes_,
        };
        const auto result = backend_.submit(submission);
        if (result != SubmitResult::accepted) {
            in_flight_.erase(entry);
            begin_failure(submit_error_kind(result));
            break;
        }

        next_offset_ += chunk;
        submitted = true;
    }

    settle_terminal_state();
    return submitted;
}

bool TransferRing::handle_completion(const TransferCompletion& completion) {
    const auto entry = in_flight_.find(completion.id);
    if (entry == in_flight_.end()) {
        return false;
    }

    const auto offset = entry->second.offset;
    const auto requested = entry->second.requested_bytes;
    in_flight_.erase(entry);

    if (completion.transferred_bytes > requested) {
        begin_failure(TransferErrorKind::invalid_completion);
    } else if (completion.code == CompletionCode::success &&
               completion.transferred_bytes == requested) {
        record_success(offset, requested);
    } else if (completion.code == CompletionCode::success) {
        begin_failure(TransferErrorKind::partial_transfer);
    } else if (completion.code == CompletionCode::cancelled &&
               (state_ == TransferRingState::cancelling ||
                state_ == TransferRingState::draining_failure)) {
        // Expected while draining. An accepted transfer still has unknown delivery.
    } else {
        begin_failure(completion_error_kind(completion.code));
    }

    if (state_ == TransferRingState::running) {
        static_cast<void>(pump());
    }
    settle_terminal_state();
    return true;
}

void TransferRing::cancel() noexcept {
    if (state_ != TransferRingState::running) {
        return;
    }
    error_ = TransferError{TransferErrorKind::user_cancelled, current_certainty()};
    state_ = TransferRingState::cancelling;
    cancel_outstanding();
    settle_terminal_state();
}

TransferRingState TransferRing::state() const noexcept { return state_; }

std::optional<TransferError> TransferRing::error() const noexcept { return error_; }

std::uint64_t TransferRing::total_bytes() const noexcept { return total_bytes_; }

std::uint64_t TransferRing::submitted_bytes() const noexcept { return next_offset_; }

std::uint64_t TransferRing::completed_bytes() const noexcept { return completed_bytes_; }

std::uint64_t TransferRing::completion_watermark() const noexcept {
    return completion_watermark_;
}

std::size_t TransferRing::in_flight() const noexcept { return in_flight_.size(); }

DeliveryCertainty TransferRing::current_certainty() const noexcept {
    if (total_bytes_ != 0 && completion_watermark_ == total_bytes_) {
        return DeliveryCertainty::fully_transferred;
    }
    if (next_offset_ == 0) {
        return DeliveryCertainty::not_sent;
    }
    return DeliveryCertainty::partial_or_unknown;
}

void TransferRing::begin_failure(const TransferErrorKind kind) noexcept {
    if (!error_.has_value()) {
        error_ = TransferError{kind, current_certainty()};
    }
    if (state_ == TransferRingState::running) {
        state_ = TransferRingState::draining_failure;
        cancel_outstanding();
    }
    settle_terminal_state();
}

void TransferRing::cancel_outstanding() noexcept {
    for (const auto& [id, ignored] : in_flight_) {
        static_cast<void>(ignored);
        backend_.cancel(id);
    }
}

void TransferRing::record_success(const std::uint64_t offset, const std::size_t bytes) {
    successful_segments_.insert_or_assign(offset, bytes);
    completed_bytes_ += bytes;

    for (;;) {
        const auto segment = successful_segments_.find(completion_watermark_);
        if (segment == successful_segments_.end()) {
            break;
        }
        completion_watermark_ += segment->second;
        successful_segments_.erase(segment);
    }
}

void TransferRing::settle_terminal_state() noexcept {
    if (state_ == TransferRingState::running && next_offset_ == total_bytes_ &&
        in_flight_.empty()) {
        state_ = TransferRingState::completed;
        return;
    }
    if (state_ == TransferRingState::cancelling && in_flight_.empty()) {
        state_ = TransferRingState::cancelled;
        if (error_.has_value()) {
            error_->certainty = current_certainty();
        }
        return;
    }
    if (state_ == TransferRingState::draining_failure && in_flight_.empty()) {
        state_ = TransferRingState::failed;
        if (error_.has_value()) {
            error_->certainty = current_certainty();
        }
    }
}

}  // namespace kairosboot::transport
