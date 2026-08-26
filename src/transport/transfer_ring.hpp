#pragma once

#include "src/transport/buffer_budget.hpp"

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
    // Transfer-ring chunks deliberately leave this false: chunk boundaries
    // are not USB message boundaries and must never trigger per-chunk ZLPs.
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

class TransferBackend {
public:
    virtual ~TransferBackend() = default;

    // Accepted submissions must complete asynchronously through handle_completion().
    // Implementations must not invoke completion inline from submit().
    [[nodiscard]] virtual SubmitResult submit(const TransferSubmission& submission) = 0;
    virtual void cancel(TransferId id) noexcept = 0;
};

class TransferSource {
public:
    virtual ~TransferSource() = default;
    [[nodiscard]] virtual std::uint64_t size() const noexcept = 0;
    [[nodiscard]] virtual bool read_exact(std::uint64_t offset,
                                          std::span<std::byte> destination) noexcept = 0;
};

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
    TransferRing(TransferBackend& backend,
                 std::shared_ptr<BufferBudget> budget,
                 TransferRingConfig config = {});

    [[nodiscard]] bool start(std::shared_ptr<TransferSource> source);
    [[nodiscard]] bool pump();
    [[nodiscard]] bool handle_completion(const TransferCompletion& completion);
    void cancel() noexcept;

    [[nodiscard]] TransferRingState state() const noexcept;
    [[nodiscard]] std::optional<TransferError> error() const noexcept;
    [[nodiscard]] std::uint64_t total_bytes() const noexcept;
    [[nodiscard]] std::uint64_t submitted_bytes() const noexcept;
    [[nodiscard]] std::uint64_t completed_bytes() const noexcept;
    [[nodiscard]] std::uint64_t completion_watermark() const noexcept;
    [[nodiscard]] std::size_t in_flight() const noexcept;

private:
    struct InFlight final {
        std::uint64_t offset{};
        std::size_t requested_bytes{};
        BufferLease buffer;
    };

    [[nodiscard]] DeliveryCertainty current_certainty() const noexcept;
    void begin_failure(TransferErrorKind kind) noexcept;
    void cancel_outstanding() noexcept;
    void record_success(std::uint64_t offset, std::size_t bytes);
    void settle_terminal_state() noexcept;

    TransferBackend& backend_;
    std::shared_ptr<BufferBudget> budget_;
    TransferRingConfig config_;
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
};

}  // namespace kairosboot::transport
