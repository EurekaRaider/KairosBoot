#pragma once

#include "src/protocol/transport_session.hpp"
#include "src/transport/buffer_budget.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace kairosboot::transport {

using TransferId = std::uint64_t;

enum class SubmitResult : std::uint8_t {
    accepted,
    no_device,
    resource_exhausted,
    io_error,
};

enum class CompletionCode : std::uint8_t {
    success,
    cancelled,
    no_device,
    timeout,
    stall,
    io_error,
};

enum class TransferErrorKind : std::uint8_t {
    invalid_configuration,
    source_read,
    submit_no_device,
    submit_resource_exhausted,
    submit_io,
    partial_transfer,
    completion_no_device,
    completion_timeout,
    completion_stall,
    completion_io,
    unexpected_cancellation,
    invalid_completion,
    user_cancelled,
};

enum class DeliveryCertainty : std::uint8_t {
    not_sent,
    partial_or_unknown,
    fully_transferred,
};

enum class TransferRingState : std::uint8_t {
    idle,
    running,
    cancelling,
    draining_failure,
    completed,
    cancelled,
    failed,
};

struct TransferSubmission final {
    TransferId id{};
    std::uint64_t offset{};
    std::span<const std::byte> payload;
    // Keeps payload storage alive through backend completion. Backends must
    // take their own copy when this token is empty.
    std::shared_ptr<const void> payload_lifetime;
    // True only when this transfer ends one complete protocol-level message.
    // Ordinary ring chunks leave this false; a caller may explicitly mark the
    // final chunk of the complete source as the logical message boundary.
    bool logical_message_end{false};
};

struct TransferCompletion final {
    TransferId id{};
    CompletionCode code{CompletionCode::io_error};
    std::size_t transferred_bytes{};
};

struct TransferError final {
    TransferErrorKind kind{TransferErrorKind::completion_io};
    DeliveryCertainty certainty{DeliveryCertainty::partial_or_unknown};
};

struct TransferRingConfig final {
    std::size_t chunk_size{1024U * 1024U};
    std::size_t depth{8};
};

enum class TransferPermitSettlement : std::uint8_t {
    fully_transferred,
    not_submitted,
    partial_or_unknown,
};

enum class TransferPermitWaitResult : std::uint8_t {
    ready,
    cancelled,
    timeout,
};

class TransferPermitProvider;

// Move-only ownership of one scheduler-granted buffer. An armed permit settles
// conservatively on destruction: before backend acceptance all bytes are known
// not submitted; after acceptance delivery is partial or unknown.
class TransferPermit final {
public:
    TransferPermit() = default;
    TransferPermit(const TransferPermit&) = delete;
    TransferPermit& operator=(const TransferPermit&) = delete;
    TransferPermit(TransferPermit&& other) noexcept;
    TransferPermit& operator=(TransferPermit&& other) noexcept;
    ~TransferPermit();

    [[nodiscard]] explicit operator bool() const noexcept;
    [[nodiscard]] std::uint64_t token() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] std::span<std::byte> bytes() noexcept;
    [[nodiscard]] std::span<const std::byte> bytes() const noexcept;
    [[nodiscard]] std::shared_ptr<const void> lifetime_token() const noexcept;

    void settle(TransferPermitSettlement result) noexcept;

private:
    friend class TransferPermitProvider;
    friend class TransferRing;

    TransferPermit(BufferLease buffer,
                   std::uint64_t token,
                   std::shared_ptr<TransferPermitProvider> provider);
    void mark_submission_attempted() noexcept;

    BufferLease buffer_;
    std::uint64_t token_{};
    std::shared_ptr<TransferPermitProvider> provider_;
    bool accepted_{};
};

// A non-blocking per-chunk scheduler seam. cancel_wait() must make any pending
// request ineligible and wake provider-owned waiters. The provider is retained
// by every live permit, so settlement remains valid through backend drain.
class TransferPermitProvider
    : public std::enable_shared_from_this<TransferPermitProvider> {
public:
    virtual ~TransferPermitProvider() = default;

    [[nodiscard]] virtual std::optional<TransferPermit> try_acquire(
        std::size_t maximum_bytes) = 0;
    [[nodiscard]] virtual std::uint64_t readiness_generation() const noexcept = 0;
    [[nodiscard]] virtual TransferPermitWaitResult wait_for_ready(
        std::uint64_t observed_generation,
        std::chrono::steady_clock::time_point deadline) = 0;
    virtual void cancel_wait() noexcept = 0;

protected:
    [[nodiscard]] TransferPermit make_permit(BufferLease buffer,
                                             std::uint64_t token);
    virtual void settle(std::uint64_t token,
                        std::size_t bytes,
                        TransferPermitSettlement result) noexcept = 0;

private:
    friend class TransferPermit;
};

// Internal late-binding capability for transports opened during fleet
// preflight. Exact per-device DATA bytes are known only after task preparation,
// so the scheduler provider is attached at the destructive barrier rather than
// guessed while the USB interface is opened.
class ITransferPermitConfigurableTransport {
public:
    virtual ~ITransferPermitConfigurableTransport() = default;

    [[nodiscard]] virtual bool configure_transfer_permits(
        std::shared_ptr<TransferPermitProvider> provider,
        TransferRingConfig config) noexcept = 0;
};

using TransferTelemetryTimePoint = std::chrono::steady_clock::time_point;
using TransferTelemetryNow =
    TransferTelemetryTimePoint (*)(void* context) noexcept;

struct TransferTelemetryClock final {
    // A null function selects std::chrono::steady_clock::now(). The context is
    // only used by deterministic internal tests.
    TransferTelemetryNow now{};
    void* context{};
};

struct TransferTelemetryConfig final {
    bool enabled{false};
    TransferTelemetryClock clock{};
};

// Internal, single-executor telemetry. Disabled instances remain all-zero and
// are omitted from the ring hot path. Values describe one TransferRing run;
// taking a snapshot does not allocate or invoke a callback. completed_bytes is
// the valid byte count reported by recognized completions, while
// contiguous_watermark only advances across complete successful segments.
struct TransferTelemetrySnapshot final {
    bool enabled{false};
    std::uint64_t source_read_count{};
    std::uint64_t source_read_bytes{};
    std::chrono::nanoseconds source_read_time{};
    std::uint64_t budget_acquire_attempt_count{};
    std::uint64_t budget_acquire_count{};
    std::chrono::nanoseconds budget_acquire_time{};
    // Failed non-blocking acquisitions are not waits. These fields only cover
    // actual budget waits performed by the USB DATA loop.
    std::uint64_t budget_wait_count{};
    std::chrono::nanoseconds budget_wait_time{};
    std::uint64_t submit_attempt_count{};
    std::uint64_t submit_count{};
    std::uint64_t submitted_bytes{};
    std::uint64_t completion_count{};
    std::uint64_t completed_bytes{};
    std::size_t current_in_flight{};
    std::size_t peak_in_flight{};
    std::uint64_t contiguous_watermark{};
    std::uint64_t cancel_count{};
    std::uint64_t backend_cancel_count{};
    std::uint64_t cancelled_completion_count{};
    std::uint64_t error_count{};
};

class TransferTelemetry final {
public:
    explicit TransferTelemetry(TransferTelemetryConfig config = {}) noexcept;

    [[nodiscard]] bool enabled() const noexcept;
    void reset() noexcept;
    [[nodiscard]] TransferTelemetrySnapshot snapshot() const noexcept;

    [[nodiscard]] TransferTelemetryTimePoint now() const noexcept;
    void record_budget_wait(TransferTelemetryTimePoint started,
                            TransferTelemetryTimePoint finished) noexcept;

private:
    friend class TransferRing;

    void record_source_read(std::size_t bytes,
                            bool succeeded,
                            TransferTelemetryTimePoint started,
                            TransferTelemetryTimePoint finished) noexcept;
    void record_budget_acquire(bool acquired,
                               TransferTelemetryTimePoint started,
                               TransferTelemetryTimePoint finished) noexcept;
    void record_submit_attempt() noexcept;
    void record_submit(std::size_t bytes, std::size_t current_in_flight) noexcept;
    void record_completion(std::size_t bytes,
                           std::size_t current_in_flight,
                           std::uint64_t contiguous_watermark,
                           bool cancelled) noexcept;
    void record_cancel() noexcept;
    void record_backend_cancel() noexcept;
    void record_error() noexcept;

    TransferTelemetryConfig config_;
    TransferTelemetrySnapshot snapshot_;
};

class TransferBackend {
public:
    virtual ~TransferBackend() = default;

    // Accepted submissions must complete asynchronously through handle_completion().
    // Implementations must not invoke completion inline from submit().
    [[nodiscard]] virtual SubmitResult submit(const TransferSubmission& submission) = 0;
    // Cancellation only requests an asynchronous completion; it must not call
    // handle_completion() inline or otherwise mutate the owning ring.
    virtual void cancel(TransferId id) noexcept = 0;
};

using TransferSource = protocol::ITransferSource;

class MemoryTransferSource final : public TransferSource {
public:
    explicit MemoryTransferSource(std::vector<std::byte> bytes);

    [[nodiscard]] std::uint64_t size() const noexcept override;
    [[nodiscard]] bool read_exact(std::uint64_t offset,
                                  std::span<std::byte> destination) noexcept override;

private:
    std::vector<std::byte> bytes_;
};

// Single-executor transfer pipeline. All calls except BufferBudget inspection must be
// serialized by the owning device actor/event executor.
class TransferRing final {
public:
    // The optional internal telemetry sink must outlive the ring.
    TransferRing(TransferBackend& backend,
                 std::shared_ptr<BufferBudget> budget,
                 TransferRingConfig config = {},
                 TransferTelemetry* telemetry = nullptr,
                 std::shared_ptr<TransferPermitProvider> permit_provider = nullptr);

    [[nodiscard]] bool start(std::shared_ptr<TransferSource> source,
                             bool logical_message_end = false);
    [[nodiscard]] bool pump();
    // Event-driven retry for a zero-in-flight ring whose provider temporarily
    // denied a permit. Returns after one provider notification, cancellation,
    // or deadline; a ready notification immediately repumps on this executor.
    [[nodiscard]] bool wait_for_permit_until(
        std::chrono::steady_clock::time_point deadline);
    [[nodiscard]] bool handle_completion(const TransferCompletion& completion);
    void cancel() noexcept;

    [[nodiscard]] TransferRingState state() const noexcept;
    [[nodiscard]] std::optional<TransferError> error() const noexcept;
    [[nodiscard]] std::uint64_t total_bytes() const noexcept;
    [[nodiscard]] std::uint64_t submitted_bytes() const noexcept;
    [[nodiscard]] std::uint64_t completed_bytes() const noexcept;
    [[nodiscard]] std::uint64_t completion_watermark() const noexcept;
    [[nodiscard]] std::size_t in_flight() const noexcept;
    [[nodiscard]] TransferTelemetrySnapshot telemetry_snapshot() const noexcept;

private:
    struct InFlight final {
        std::uint64_t offset{};
        std::size_t requested_bytes{};
        TransferPermit permit;
    };

    [[nodiscard]] DeliveryCertainty current_certainty() const noexcept;
    void begin_failure(TransferErrorKind kind) noexcept;
    void cancel_outstanding() noexcept;
    void record_success(std::uint64_t offset, std::size_t bytes);
    void settle_terminal_state() noexcept;

    TransferBackend& backend_;
    std::shared_ptr<BufferBudget> budget_;
    std::shared_ptr<TransferPermitProvider> permit_provider_;
    TransferRingConfig config_;
    TransferTelemetry* telemetry_{};
    std::shared_ptr<TransferSource> source_;
    TransferRingState state_{TransferRingState::idle};
    std::optional<TransferError> error_;
    std::unordered_map<TransferId, InFlight> in_flight_;
    std::map<std::uint64_t, std::size_t> successful_segments_;
    TransferId next_id_{1};
    std::uint64_t total_bytes_{0};
    std::uint64_t next_offset_{0};
    std::uint64_t completed_bytes_{0};
    std::uint64_t completion_watermark_{0};
    bool logical_message_end_{false};
    std::uint64_t permit_wait_generation_{};
};

}  // namespace kairosboot::transport
