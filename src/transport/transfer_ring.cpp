#include "src/transport/transfer_ring.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>
#include <utility>

namespace kairosboot::transport {

namespace {

[[nodiscard]] std::chrono::nanoseconds telemetry_elapsed(
    const TransferTelemetryTimePoint started,
    const TransferTelemetryTimePoint finished) noexcept {
    if (finished <= started) {
        return {};
    }
    return std::chrono::duration_cast<std::chrono::nanoseconds>(finished - started);
}

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

TransferTelemetry::TransferTelemetry(const TransferTelemetryConfig config) noexcept
    : config_(config) {
    reset();
}

bool TransferTelemetry::enabled() const noexcept { return config_.enabled; }

void TransferTelemetry::reset() noexcept {
    if (!config_.enabled) {
        return;
    }
    snapshot_ = {};
    snapshot_.enabled = true;
}

TransferTelemetrySnapshot TransferTelemetry::snapshot() const noexcept {
    return snapshot_;
}

TransferTelemetryTimePoint TransferTelemetry::now() const noexcept {
    if (config_.clock.now != nullptr) {
        return config_.clock.now(config_.clock.context);
    }
    return std::chrono::steady_clock::now();
}

void TransferTelemetry::record_budget_wait(
    const TransferTelemetryTimePoint started,
    const TransferTelemetryTimePoint finished) noexcept {
    if (!config_.enabled) {
        return;
    }
    ++snapshot_.budget_wait_count;
    snapshot_.budget_wait_time += telemetry_elapsed(started, finished);
}

void TransferTelemetry::record_source_read(
    const std::size_t bytes,
    const bool succeeded,
    const TransferTelemetryTimePoint started,
    const TransferTelemetryTimePoint finished) noexcept {
    ++snapshot_.source_read_count;
    if (succeeded) {
        snapshot_.source_read_bytes += static_cast<std::uint64_t>(bytes);
    }
    snapshot_.source_read_time += telemetry_elapsed(started, finished);
}

void TransferTelemetry::record_budget_acquire(
    const bool acquired,
    const TransferTelemetryTimePoint started,
    const TransferTelemetryTimePoint finished) noexcept {
    ++snapshot_.budget_acquire_attempt_count;
    if (acquired) {
        ++snapshot_.budget_acquire_count;
    }
    snapshot_.budget_acquire_time += telemetry_elapsed(started, finished);
}

void TransferTelemetry::record_submit_attempt() noexcept {
    ++snapshot_.submit_attempt_count;
}

void TransferTelemetry::record_submit(const std::size_t bytes,
                                      const std::size_t current_in_flight) noexcept {
    ++snapshot_.submit_count;
    snapshot_.submitted_bytes += static_cast<std::uint64_t>(bytes);
    snapshot_.current_in_flight = current_in_flight;
    snapshot_.peak_in_flight =
        std::max(snapshot_.peak_in_flight, current_in_flight);
}

void TransferTelemetry::record_completion(
    const std::size_t bytes,
    const std::size_t current_in_flight,
    const std::uint64_t contiguous_watermark,
    const bool cancelled) noexcept {
    ++snapshot_.completion_count;
    snapshot_.completed_bytes += static_cast<std::uint64_t>(bytes);
    snapshot_.current_in_flight = current_in_flight;
    snapshot_.contiguous_watermark = contiguous_watermark;
    if (cancelled) {
        ++snapshot_.cancelled_completion_count;
    }
}

void TransferTelemetry::record_cancel() noexcept { ++snapshot_.cancel_count; }

void TransferTelemetry::record_backend_cancel() noexcept {
    ++snapshot_.backend_cancel_count;
}

void TransferTelemetry::record_error() noexcept { ++snapshot_.error_count; }

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

TransferPermit::TransferPermit(
    BufferLease buffer,
    const std::uint64_t token,
    std::shared_ptr<TransferPermitProvider> provider)
    : buffer_(std::move(buffer)), token_(token), provider_(std::move(provider)) {}

TransferPermit::TransferPermit(TransferPermit&& other) noexcept
    : buffer_(std::move(other.buffer_)),
      token_(std::exchange(other.token_, 0)),
      provider_(std::move(other.provider_)),
      accepted_(std::exchange(other.accepted_, false)) {}

TransferPermit& TransferPermit::operator=(TransferPermit&& other) noexcept {
    if (this != &other) {
        settle(accepted_ ? TransferPermitSettlement::partial_or_unknown
                         : TransferPermitSettlement::not_submitted);
        buffer_ = std::move(other.buffer_);
        token_ = std::exchange(other.token_, 0);
        provider_ = std::move(other.provider_);
        accepted_ = std::exchange(other.accepted_, false);
    }
    return *this;
}

TransferPermit::~TransferPermit() {
    settle(accepted_ ? TransferPermitSettlement::partial_or_unknown
                     : TransferPermitSettlement::not_submitted);
}

TransferPermit::operator bool() const noexcept {
    return static_cast<bool>(buffer_);
}

std::uint64_t TransferPermit::token() const noexcept { return token_; }

std::size_t TransferPermit::size() const noexcept { return buffer_.size(); }

std::span<std::byte> TransferPermit::bytes() noexcept { return buffer_.bytes(); }

std::span<const std::byte> TransferPermit::bytes() const noexcept {
    return buffer_.bytes();
}

std::shared_ptr<const void> TransferPermit::lifetime_token() const noexcept {
    return buffer_.lifetime_token();
}

void TransferPermit::settle(const TransferPermitSettlement result) noexcept {
    if (!buffer_) {
        provider_.reset();
        token_ = 0;
        accepted_ = false;
        return;
    }

    const auto bytes = buffer_.size();
    auto provider = std::move(provider_);
    const auto token = std::exchange(token_, 0);
    accepted_ = false;
    // Commit scheduler state before releasing storage. In particular, an
    // uncertain transfer retires its flow before the budget release can wake a
    // waiter; known-not-submitted and success similarly become visible before
    // capacity is advertised.
    if (provider != nullptr) {
        provider->settle(token, bytes, result);
    }
    buffer_ = {};
}

void TransferPermit::mark_submission_attempted() noexcept { accepted_ = true; }

TransferPermit TransferPermitProvider::make_permit(BufferLease buffer,
                                                   const std::uint64_t token) {
    return TransferPermit(std::move(buffer), token, shared_from_this());
}

TransferRing::TransferRing(TransferBackend& backend,
                           std::shared_ptr<BufferBudget> budget,
                           const TransferRingConfig config,
                           TransferTelemetry* const telemetry,
                           std::shared_ptr<TransferPermitProvider> permit_provider)
    : backend_(backend),
      budget_(std::move(budget)),
      permit_provider_(std::move(permit_provider)),
      config_(config),
      telemetry_(telemetry != nullptr && telemetry->enabled() ? telemetry : nullptr) {
    if (telemetry_ != nullptr) {
        telemetry_->reset();
    }
}

bool TransferRing::start(std::shared_ptr<TransferSource> source,
                         const bool logical_message_end) {
    if (state_ != TransferRingState::idle || source == nullptr ||
        (budget_ == nullptr && permit_provider_ == nullptr) ||
        config_.chunk_size == 0 || config_.depth == 0 ||
        (permit_provider_ == nullptr && config_.chunk_size > budget_->limit())) {
        if (state_ == TransferRingState::idle) {
            error_ = TransferError{TransferErrorKind::invalid_configuration,
                                   DeliveryCertainty::not_sent};
            state_ = TransferRingState::failed;
            if (telemetry_ != nullptr) {
                telemetry_->record_error();
            }
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
        const auto maximum_chunk = static_cast<std::size_t>(std::min<std::uint64_t>(
            remaining, static_cast<std::uint64_t>(config_.chunk_size)));
        std::optional<TransferPermit> permit;
        const auto observed_generation = permit_provider_ != nullptr
            ? permit_provider_->readiness_generation()
            : std::uint64_t{};
        if (telemetry_ != nullptr) {
            const auto started = telemetry_->now();
            if (permit_provider_ != nullptr) {
                permit = permit_provider_->try_acquire(maximum_chunk);
            } else {
                auto lease = budget_->try_acquire(maximum_chunk);
                if (lease.has_value()) {
                    permit = TransferPermit(std::move(*lease), 0, nullptr);
                }
            }
            const auto finished = telemetry_->now();
            telemetry_->record_budget_acquire(
                permit.has_value(), started, finished);
        } else if (permit_provider_ != nullptr) {
            permit = permit_provider_->try_acquire(maximum_chunk);
        } else {
            auto lease = budget_->try_acquire(maximum_chunk);
            if (lease.has_value()) {
                permit = TransferPermit(std::move(*lease), 0, nullptr);
            }
        }
        if (!permit.has_value()) {
            permit_wait_generation_ = observed_generation;
            break;
        }
        const auto chunk = permit->size();
        if (chunk == 0 || chunk > maximum_chunk) {
            permit->settle(TransferPermitSettlement::not_submitted);
            begin_failure(TransferErrorKind::invalid_configuration);
            break;
        }
        bool read_succeeded;
        if (telemetry_ != nullptr) {
            const auto started = telemetry_->now();
            read_succeeded = source_->read_exact(next_offset_, permit->bytes());
            const auto finished = telemetry_->now();
            telemetry_->record_source_read(
                chunk, read_succeeded, started, finished);
        } else {
            read_succeeded = source_->read_exact(next_offset_, permit->bytes());
        }
        if (!read_succeeded) {
            begin_failure(TransferErrorKind::source_read);
            break;
        }

        const auto id = next_id_++;
        const auto offset = next_offset_;
        auto [entry, inserted] = in_flight_.emplace(
            id, InFlight{offset, chunk, std::move(*permit)});
        if (!inserted) {
            begin_failure(TransferErrorKind::submit_io);
            break;
        }

        const TransferSubmission submission{
            id,
            offset,
            entry->second.permit.bytes(),
            entry->second.permit.lifetime_token(),
            logical_message_end_ && offset + chunk == total_bytes_,
        };
        if (telemetry_ != nullptr) {
            telemetry_->record_submit_attempt();
        }
        // Once submit() is entered, an exception cannot prove that the backend
        // sent nothing. A returned rejection is the only safe not-sent case.
        entry->second.permit.mark_submission_attempted();
        SubmitResult result;
        try {
            result = backend_.submit(submission);
        } catch (...) {
            // The backend may have accepted the transfer before throwing.
            // Keep its permit armed and in-flight until cancellation completion.
            next_offset_ += chunk;
            begin_failure(TransferErrorKind::submit_io);
            break;
        }
        if (result != SubmitResult::accepted) {
            auto rejected = std::move(entry->second.permit);
            in_flight_.erase(entry);
            rejected.settle(TransferPermitSettlement::not_submitted);
            begin_failure(submit_error_kind(result));
            break;
        }
        next_offset_ += chunk;
        if (telemetry_ != nullptr) {
            telemetry_->record_submit(chunk, in_flight_.size());
        }
        submitted = true;
    }

    settle_terminal_state();
    return submitted;
}

bool TransferRing::wait_for_permit_until(
    const std::chrono::steady_clock::time_point deadline) {
    if (state_ != TransferRingState::running || permit_provider_ == nullptr ||
        !in_flight_.empty()) {
        return false;
    }
    if (permit_provider_->wait_for_ready(permit_wait_generation_, deadline) !=
        TransferPermitWaitResult::ready) {
        return false;
    }
    return pump();
}

bool TransferRing::handle_completion(const TransferCompletion& completion) {
    const auto entry = in_flight_.find(completion.id);
    if (entry == in_flight_.end()) {
        return false;
    }

    const auto offset = entry->second.offset;
    const auto requested = entry->second.requested_bytes;
    auto permit = std::move(entry->second.permit);
    const auto valid_completed_bytes = completion.transferred_bytes <= requested
        ? completion.transferred_bytes
        : std::size_t{0};
    in_flight_.erase(entry);

    if (completion.transferred_bytes > requested) {
        permit.settle(TransferPermitSettlement::partial_or_unknown);
        begin_failure(TransferErrorKind::invalid_completion);
    } else if (completion.code == CompletionCode::success &&
               completion.transferred_bytes == requested) {
        permit.settle(TransferPermitSettlement::fully_transferred);
        record_success(offset, requested);
    } else if (completion.code == CompletionCode::success) {
        permit.settle(TransferPermitSettlement::partial_or_unknown);
        begin_failure(TransferErrorKind::partial_transfer);
    } else if (completion.code == CompletionCode::cancelled &&
               (state_ == TransferRingState::cancelling ||
                state_ == TransferRingState::draining_failure)) {
        // Expected while draining. An accepted transfer still has unknown delivery.
        permit.settle(TransferPermitSettlement::partial_or_unknown);
    } else {
        permit.settle(TransferPermitSettlement::partial_or_unknown);
        begin_failure(completion_error_kind(completion.code));
    }

    if (state_ == TransferRingState::running) {
        static_cast<void>(pump());
    }
    settle_terminal_state();
    if (telemetry_ != nullptr) {
        telemetry_->record_completion(
            valid_completed_bytes,
            in_flight_.size(),
            completion_watermark_,
            completion.code == CompletionCode::cancelled);
    }
    return true;
}

void TransferRing::cancel() noexcept {
    if (state_ != TransferRingState::running) {
        return;
    }
    if (telemetry_ != nullptr) {
        telemetry_->record_cancel();
    }
    error_ = TransferError{TransferErrorKind::user_cancelled, current_certainty()};
    state_ = TransferRingState::cancelling;
    if (permit_provider_ != nullptr) {
        permit_provider_->cancel_wait();
    }
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

TransferTelemetrySnapshot TransferRing::telemetry_snapshot() const noexcept {
    return telemetry_ != nullptr ? telemetry_->snapshot()
                                 : TransferTelemetrySnapshot{};
}

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
        if (telemetry_ != nullptr) {
            telemetry_->record_error();
        }
    }
    if (state_ == TransferRingState::running) {
        state_ = TransferRingState::draining_failure;
        if (permit_provider_ != nullptr) {
            permit_provider_->cancel_wait();
        }
        cancel_outstanding();
    }
    settle_terminal_state();
}

void TransferRing::cancel_outstanding() noexcept {
    for (const auto& [id, ignored] : in_flight_) {
        static_cast<void>(ignored);
        backend_.cancel(id);
        if (telemetry_ != nullptr) {
            telemetry_->record_backend_cancel();
        }
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
