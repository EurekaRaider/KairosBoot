// SPDX-License-Identifier: MIT
#pragma once

#include "src/protocol/transport_session.hpp"
#include "src/transport/libusb_runtime.hpp"

#include <atomic>
#include <expected>
#include <memory>
#include <mutex>

namespace kairosboot::transport {

struct UsbFastbootTransportOptions final {
    BulkOutOptions bulk_out{};
    TransferRingConfig data_ring{};
    std::shared_ptr<BufferBudget> buffer_budget;
};

// Internal protocol adapter. One instance exclusively owns one claimed USB
// interface. write()/read() are serialized; request_cancel()/close() may run
// from a different thread and use the runtime's bounded drain/quarantine path.
class UsbFastbootTransport final : public protocol::ITransportSession {
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

    [[nodiscard]] protocol::TransferResult read(
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
