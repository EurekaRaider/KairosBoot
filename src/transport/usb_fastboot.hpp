// SPDX-License-Identifier: MIT
#pragma once

#include "src/protocol/transport_session.hpp"
#include "src/transport/libusb_runtime.hpp"

#include <atomic>
#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>

namespace kairosboot::transport {

struct UsbFastbootTransportOptions final {
    BulkOutOptions bulk_out{};
    TransferRingConfig data_ring{};
    std::shared_ptr<BufferBudget> buffer_budget;
};

using TransferProgressAction = protocol::TransferProgressAction;
using TransferProgressObserver = protocol::TransferProgressObserver;

namespace detail {

[[nodiscard]] inline protocol::TransferCertainty observer_cancel_certainty(
    const TransferRing& ring) noexcept {
    if (ring.total_bytes() != 0 &&
        ring.completion_watermark() == ring.total_bytes()) {
        return protocol::TransferCertainty::FullyTransferred;
    }
    return ring.submitted_bytes() == 0
        ? protocol::TransferCertainty::NotTransferred
        : protocol::TransferCertainty::PartialOrUnknown;
}

}  // namespace detail

// Internal protocol adapter. One instance exclusively owns one claimed USB
// interface. write()/read() are serialized; request_cancel()/close() may run
// from a different thread and use the runtime's bounded drain/quarantine path.
class UsbFastbootTransport final : public protocol::ITransportSession,
                                   public protocol::IStreamingTransportSession {
public:
    [[nodiscard]] static std::expected<std::unique_ptr<UsbFastbootTransport>,
                                       LibusbRuntimeError>
    open(std::shared_ptr<LibusbRuntime> runtime,
         const UsbDeviceInfo& device,
         UsbFastbootTransportOptions options = {});

    ~UsbFastbootTransport() override;

    UsbFastbootTransport(const UsbFastbootTransport&) = delete;
    UsbFastbootTransport& operator=(const UsbFastbootTransport&) = delete;
    UsbFastbootTransport(UsbFastbootTransport&&) = delete;
    UsbFastbootTransport& operator=(UsbFastbootTransport&&) = delete;

    [[nodiscard]] protocol::TransferResult write(
        std::span<const std::byte> bytes,
        std::chrono::milliseconds timeout) override;

    // Streams source chunks through the bounded transfer ring. Progress is
    // observed synchronously by this call's owning thread after contiguous
    // completions advance; libusb event callbacks never invoke the observer.
    [[nodiscard]] protocol::TransferResult write_source(
        std::shared_ptr<TransferSource> source,
        std::chrono::milliseconds timeout,
        const TransferProgressObserver& observer = {}) override;

    [[nodiscard]] protocol::TransferResult read(
        std::span<std::byte> destination,
        std::chrono::milliseconds timeout) override;

    [[nodiscard]] protocol::TransferResult read_data(
        std::span<std::byte> destination,
        std::chrono::milliseconds timeout) override;

    void request_cancel() noexcept override;
    void cancel() noexcept;
    void close() noexcept override;
    [[nodiscard]] bool is_open() const noexcept;

private:
    UsbFastbootTransport(std::unique_ptr<LibusbBulkOutBackend> backend,
                         UsbFastbootTransportOptions options,
                         std::shared_ptr<BufferBudget> budget);

    void poison_and_stop() noexcept;

    std::unique_ptr<LibusbBulkOutBackend> backend_;
    UsbFastbootTransportOptions options_;
    std::shared_ptr<BufferBudget> budget_;
    std::atomic<bool> cancellation_requested_{false};
    std::atomic<bool> open_{true};
    std::mutex operation_mutex_;
};

}  // namespace kairosboot::transport
