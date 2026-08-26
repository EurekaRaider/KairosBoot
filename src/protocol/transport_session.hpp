// SPDX-License-Identifier: MIT
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>

namespace kairosboot::protocol {

enum class TransportStatus : std::uint8_t {
    Ok,
    Timeout,
    Cancelled,
    Disconnected,
    IoError,
};

// Describes what the transport can prove about a failed write. Fastboot cannot
// safely retry a command or data chunk when the amount accepted by the device is
// unknown.
enum class TransferCertainty : std::uint8_t {
    NotTransferred,
    FullyTransferred,
    PartialOrUnknown,
};

struct TransferResult {
    TransportStatus status{TransportStatus::Ok};
    std::size_t transferred{0};
    TransferCertainty certainty{TransferCertainty::FullyTransferred};
    bool truncated{false};
    std::string detail;
    int native_code{0};
};

// Random-access source for one immutable logical transfer. size() must remain
// stable for the lifetime of an operation. read_exact() may be called with
// bounded chunks in any order and must fill the complete destination.
class ITransferSource {
public:
    virtual ~ITransferSource() = default;
    [[nodiscard]] virtual std::uint64_t size() const noexcept = 0;
    [[nodiscard]] virtual bool read_exact(
        std::uint64_t offset,
        std::span<std::byte> destination) noexcept = 0;
};

enum class TransferProgressAction : std::uint8_t {
    continue_transfer,
    cancel,
};

using TransferProgressObserver = std::function<TransferProgressAction(
    std::uint64_t completion_watermark,
    std::uint64_t total_bytes)>;

// Optional capability for transports that can stream an ITransferSource
// without materializing it in one contiguous host buffer. The observer runs on
// the serialized operation thread, never on a native transport event thread.
class IStreamingTransportSession {
public:
    virtual ~IStreamingTransportSession() = default;

    [[nodiscard]] virtual TransferResult write_source(
        std::shared_ptr<ITransferSource> source,
        std::chrono::milliseconds timeout,
        const TransferProgressObserver& observer = {}) = 0;
};

// Internal transport seam shared by USB, TCP and UDP adapters.
//
// write() has stream semantics: a successful short write may be continued with
// the remaining bytes. read() returns exactly one logical framed Fastboot
// response and must set truncated when that response did not fit in the supplied
// buffer. USB adapters consume/ignore transfer-level ZLPs instead of surfacing
// them here; a successful zero-byte logical response is therefore malformed.
class ITransportSession {
public:
    virtual ~ITransportSession() = default;

    [[nodiscard]] virtual TransferResult write(
        std::span<const std::byte> bytes,
        std::chrono::milliseconds timeout) = 0;

    [[nodiscard]] virtual TransferResult read(
        std::span<std::byte> destination,
        std::chrono::milliseconds timeout) = 0;

    // Thread-safe cancellation signal for the currently active operation. It
    // must not wait for the serialized read/write call to return.
    virtual void request_cancel() noexcept = 0;
    virtual void close() noexcept = 0;
};

}  // namespace kairosboot::protocol
