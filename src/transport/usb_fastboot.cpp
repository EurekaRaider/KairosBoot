// SPDX-License-Identifier: MIT
#include "src/transport/usb_fastboot.hpp"

#include <algorithm>
#include <chrono>
#include <limits>
#include <new>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace kairosboot::transport {
namespace {

using Clock = std::chrono::steady_clock;

class SpanTransferSource final : public TransferSource {
public:
    explicit SpanTransferSource(const std::span<const std::byte> bytes) : bytes_(bytes) {}

    [[nodiscard]] std::uint64_t size() const noexcept override {
        return static_cast<std::uint64_t>(bytes_.size());
    }

    [[nodiscard]] bool read_exact(
        const std::uint64_t offset,
        const std::span<std::byte> destination) noexcept override {
        if (offset > bytes_.size()) {
            return false;
        }
        const auto start = static_cast<std::size_t>(offset);
        if (destination.size() > bytes_.size() - start) {
            return false;
        }
        std::copy_n(bytes_.begin() + static_cast<std::ptrdiff_t>(start),
                    destination.size(),
                    destination.begin());
        return true;
    }

private:
    std::span<const std::byte> bytes_;
};

[[nodiscard]] Clock::time_point deadline_after(
    const std::chrono::milliseconds timeout) noexcept {
    const auto now = Clock::now();
    if (timeout == std::chrono::milliseconds::max()) {
        return Clock::time_point::max();
    }
    if (timeout <= std::chrono::milliseconds::zero()) {
        return now;
    }
    const auto room = std::chrono::duration_cast<std::chrono::milliseconds>(
        Clock::time_point::max() - now);
    return timeout >= room ? Clock::time_point::max() : now + timeout;
}

[[nodiscard]] std::chrono::milliseconds remaining_until(
    const Clock::time_point deadline) noexcept {
    if (deadline == Clock::time_point::max()) {
        return std::chrono::milliseconds::max();
    }
    const auto now = Clock::now();
    if (now >= deadline) {
        return std::chrono::milliseconds::zero();
    }
    return std::chrono::ceil<std::chrono::milliseconds>(deadline - now);
}

[[nodiscard]] protocol::TransferCertainty transfer_certainty(
    const DeliveryCertainty certainty) noexcept {
    switch (certainty) {
        case DeliveryCertainty::not_sent:
            return protocol::TransferCertainty::NotTransferred;
        case DeliveryCertainty::fully_transferred:
            return protocol::TransferCertainty::FullyTransferred;
        case DeliveryCertainty::partial_or_unknown:
            return protocol::TransferCertainty::PartialOrUnknown;
    }
    return protocol::TransferCertainty::PartialOrUnknown;
}

[[nodiscard]] protocol::TransportStatus transport_status(
    const TransferErrorKind kind) noexcept {
    switch (kind) {
        case TransferErrorKind::submit_no_device:
        case TransferErrorKind::completion_no_device:
            return protocol::TransportStatus::Disconnected;
        case TransferErrorKind::completion_timeout:
            return protocol::TransportStatus::Timeout;
        case TransferErrorKind::user_cancelled:
            return protocol::TransportStatus::Cancelled;
        case TransferErrorKind::invalid_configuration:
        case TransferErrorKind::source_read:
        case TransferErrorKind::submit_resource_exhausted:
        case TransferErrorKind::submit_io:
        case TransferErrorKind::partial_transfer:
        case TransferErrorKind::completion_stall:
        case TransferErrorKind::completion_io:
        case TransferErrorKind::unexpected_cancellation:
        case TransferErrorKind::invalid_completion:
            return protocol::TransportStatus::IoError;
    }
    return protocol::TransportStatus::IoError;
}

[[nodiscard]] int transfer_native_error(const TransferErrorKind kind) noexcept {
    switch (kind) {
        case TransferErrorKind::submit_no_device:
        case TransferErrorKind::completion_no_device:
            return LIBUSB_ERROR_NO_DEVICE;
        case TransferErrorKind::submit_resource_exhausted:
            return LIBUSB_ERROR_NO_MEM;
        case TransferErrorKind::completion_timeout:
            return LIBUSB_ERROR_TIMEOUT;
        case TransferErrorKind::completion_stall:
            return LIBUSB_ERROR_PIPE;
        case TransferErrorKind::submit_io:
        case TransferErrorKind::completion_io:
            return LIBUSB_ERROR_IO;
        case TransferErrorKind::invalid_configuration:
        case TransferErrorKind::source_read:
        case TransferErrorKind::partial_transfer:
        case TransferErrorKind::unexpected_cancellation:
        case TransferErrorKind::invalid_completion:
        case TransferErrorKind::user_cancelled:
            return 0;
    }
    return 0;
}

[[nodiscard]] std::string_view transfer_error_detail(
    const TransferErrorKind kind) noexcept {
    switch (kind) {
        case TransferErrorKind::invalid_configuration:
            return "invalid USB transfer-ring configuration";
        case TransferErrorKind::source_read:
            return "USB transfer source could not provide a complete chunk";
        case TransferErrorKind::submit_no_device:
            return "USB device disconnected before transfer submission";
        case TransferErrorKind::submit_resource_exhausted:
            return "USB transfer submission exhausted host resources";
        case TransferErrorKind::submit_io:
            return "USB transfer submission failed";
        case TransferErrorKind::partial_transfer:
            return "USB bulk OUT completed with a short transfer";
        case TransferErrorKind::completion_no_device:
            return "USB device disconnected during bulk OUT";
        case TransferErrorKind::completion_timeout:
            return "USB bulk OUT timed out";
        case TransferErrorKind::completion_stall:
            return "USB bulk OUT endpoint stalled";
        case TransferErrorKind::completion_io:
            return "USB bulk OUT failed";
        case TransferErrorKind::unexpected_cancellation:
            return "USB bulk OUT was cancelled unexpectedly";
        case TransferErrorKind::invalid_completion:
            return "USB backend returned an invalid completion length";
        case TransferErrorKind::user_cancelled:
            return "USB bulk OUT was cancelled";
    }
    return "USB bulk OUT failed";
}

[[nodiscard]] protocol::TransferResult ring_failure(
    const TransferRing& ring,
    const protocol::TransportStatus fallback_status,
    const std::string_view fallback_detail) {
    const auto error = ring.error();
    const auto watermark = ring.completion_watermark();
    const auto transferred = static_cast<std::size_t>(std::min<std::uint64_t>(
        watermark, std::numeric_limits<std::size_t>::max()));
    if (!error.has_value()) {
        return {
            .status = fallback_status,
            .transferred = transferred,
            .certainty = watermark == 0
                ? protocol::TransferCertainty::NotTransferred
                : protocol::TransferCertainty::PartialOrUnknown,
            .truncated = false,
            .detail = std::string(fallback_detail),
            .native_code = 0,
        };
    }
    return {
        .status = transport_status(error->kind),
        .transferred = transferred,
        .certainty = transfer_certainty(error->certainty),
        .truncated = false,
        .detail = std::string(transfer_error_detail(error->kind)),
        .native_code = transfer_native_error(error->kind),
    };
}

[[nodiscard]] std::size_t transferred_size(
    const std::uint64_t bytes) noexcept {
    return static_cast<std::size_t>(std::min<std::uint64_t>(
        bytes, std::numeric_limits<std::size_t>::max()));
}

[[nodiscard]] protocol::TransferResult classify_requested_cancellation(
    protocol::TransferResult failure,
    const bool cancellation_requested) {
    if (cancellation_requested) {
        failure.status = protocol::TransportStatus::Cancelled;
        failure.detail = "Fastboot USB transfer was cancelled";
        failure.native_code = 0;
    }
    return failure;
}

[[nodiscard]] protocol::TransferResult read_result(
    const LibusbBulkOutBackend::ReadResult& result) {
    using ReadCode = LibusbBulkOutBackend::ReadCode;
    protocol::TransportStatus status = protocol::TransportStatus::IoError;
    std::string_view detail = "USB bulk IN failed";
    int native_error = LIBUSB_ERROR_IO;
    switch (result.code) {
        case ReadCode::success:
            status = protocol::TransportStatus::Ok;
            detail = {};
            native_error = 0;
            break;
        case ReadCode::timeout:
            status = protocol::TransportStatus::Timeout;
            detail = "USB bulk IN timed out";
            native_error = LIBUSB_ERROR_TIMEOUT;
            break;
        case ReadCode::cancelled:
            status = protocol::TransportStatus::Cancelled;
            detail = "USB bulk IN was cancelled";
            native_error = 0;
            break;
        case ReadCode::no_device:
            status = protocol::TransportStatus::Disconnected;
            detail = "USB device disconnected during bulk IN";
            native_error = LIBUSB_ERROR_NO_DEVICE;
            break;
        case ReadCode::stall:
            detail = "USB bulk IN endpoint stalled";
            native_error = LIBUSB_ERROR_PIPE;
            break;
        case ReadCode::overflow:
            detail = "Fastboot USB inbound transfer exceeded the destination";
            native_error = LIBUSB_ERROR_OVERFLOW;
            break;
        case ReadCode::resource_exhausted:
            detail = "USB bulk IN exhausted host resources";
            native_error = LIBUSB_ERROR_NO_MEM;
            break;
        case ReadCode::io_error:
            break;
        case ReadCode::closed:
            status = protocol::TransportStatus::Disconnected;
            detail = "Fastboot USB transport is closed";
            native_error = 0;
            break;
    }
    return {
        .status = status,
        .transferred = result.transferred,
        .certainty = result.code == ReadCode::success
            ? protocol::TransferCertainty::FullyTransferred
            : result.transferred == 0 && result.code != ReadCode::overflow
                ? protocol::TransferCertainty::NotTransferred
                : protocol::TransferCertainty::PartialOrUnknown,
        .truncated = result.truncated,
        .detail = std::string(detail),
        .native_code = native_error,
    };
}

}  // namespace

std::expected<std::unique_ptr<UsbFastbootTransport>, LibusbRuntimeError>
UsbFastbootTransport::open(
    std::shared_ptr<LibusbRuntime> runtime,
    const UsbDeviceInfo& device,
    UsbFastbootTransportOptions options) {
    if (runtime == nullptr || options.data_ring.chunk_size == 0 ||
        options.data_ring.depth == 0 ||
        options.data_ring.chunk_size >
            std::numeric_limits<std::size_t>::max() / options.data_ring.depth) {
        return std::unexpected(
            LibusbRuntimeError{LibusbRuntimeErrorKind::invalid_device});
    }

    std::shared_ptr<BufferBudget> budget = options.buffer_budget;
    try {
        if (budget == nullptr) {
            budget = process_usb_buffer_budget();
        }
        if (options.data_ring.chunk_size > budget->limit()) {
            return std::unexpected(
                LibusbRuntimeError{LibusbRuntimeErrorKind::invalid_device});
        }
        auto backend = runtime->open_bulk_out(device, options.bulk_out);
        if (!backend.has_value()) {
            return std::unexpected(backend.error());
        }
        return std::unique_ptr<UsbFastbootTransport>(new UsbFastbootTransport(
            std::move(*backend), std::move(options), std::move(budget)));
    } catch (const std::bad_alloc&) {
        return std::unexpected(LibusbRuntimeError{
            LibusbRuntimeErrorKind::open_failed, LIBUSB_ERROR_NO_MEM});
    }
}

UsbFastbootTransport::UsbFastbootTransport(
    std::unique_ptr<LibusbBulkOutBackend> backend,
    UsbFastbootTransportOptions options,
    std::shared_ptr<BufferBudget> budget)
    : backend_(std::move(backend)),
      options_(std::move(options)),
      budget_(std::move(budget)) {}

UsbFastbootTransport::~UsbFastbootTransport() {
    close();
}

protocol::TransferResult UsbFastbootTransport::write(
    const std::span<const std::byte> bytes,
    const std::chrono::milliseconds timeout) {
    try {
        return write_source(
            std::make_shared<SpanTransferSource>(bytes), timeout);
    } catch (const std::bad_alloc&) {
        return {
            .status = protocol::TransportStatus::IoError,
            .transferred = 0,
            .certainty = protocol::TransferCertainty::NotTransferred,
            .truncated = false,
            .detail = "USB bulk OUT exhausted host resources before submission",
        };
    }
}

protocol::TransferResult UsbFastbootTransport::write_source(
    std::shared_ptr<TransferSource> source,
    const std::chrono::milliseconds timeout,
    const TransferProgressObserver& observer) {
    std::scoped_lock operation(operation_mutex_);
    if (!open_.load(std::memory_order_acquire)) {
        const auto cancelled =
            cancellation_requested_.load(std::memory_order_acquire);
        return {
            .status = cancelled ? protocol::TransportStatus::Cancelled
                                : protocol::TransportStatus::Disconnected,
            .transferred = 0,
            .certainty = protocol::TransferCertainty::NotTransferred,
            .truncated = false,
            .detail = cancelled ? "Fastboot USB transport was cancelled"
                                : "Fastboot USB transport is closed",
        };
    }
    if (source == nullptr) {
        return {
            .status = protocol::TransportStatus::IoError,
            .transferred = 0,
            .certainty = protocol::TransferCertainty::NotTransferred,
            .truncated = false,
            .detail = "USB transfer source is null",
        };
    }

    const auto total_bytes = source->size();
    if (total_bytes == 0) {
        return {
            .status = protocol::TransportStatus::Ok,
            .transferred = 0,
            .certainty = protocol::TransferCertainty::FullyTransferred,
            .truncated = false,
            .detail = {},
        };
    }
    const auto deadline = deadline_after(timeout);
    if (Clock::now() >= deadline) {
        auto failure = protocol::TransferResult{
            .status = protocol::TransportStatus::Timeout,
            .transferred = 0,
            .certainty = protocol::TransferCertainty::NotTransferred,
            .truncated = false,
            .detail = "USB bulk OUT deadline expired before submission",
        };
        poison_and_stop();
        return failure;
    }

    TransferRing ring(*backend_, budget_, options_.data_ring);
    try {
        if (!ring.start(std::move(source), true)) {
            auto failure = ring_failure(
                ring,
                protocol::TransportStatus::IoError,
                "USB transfer ring could not start");
            failure = classify_requested_cancellation(
                std::move(failure),
                cancellation_requested_.load(std::memory_order_acquire));
            poison_and_stop();
            return failure;
        }

        while (ring.state() == TransferRingState::running) {
            if (ring.in_flight() == 0) {
                static_cast<void>(ring.pump());
                if (ring.in_flight() == 0 &&
                    ring.state() == TransferRingState::running) {
                    if (Clock::now() >= deadline) {
                        ring.cancel();
                        auto failure = ring_failure(
                            ring,
                            protocol::TransportStatus::Timeout,
                            "USB bulk OUT timed out waiting for buffer budget");
                        failure.status = protocol::TransportStatus::Timeout;
                        failure = classify_requested_cancellation(
                            std::move(failure),
                            cancellation_requested_.load(
                                std::memory_order_acquire));
                        poison_and_stop();
                        return failure;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    continue;
                }
            }

            const auto waited = backend_->wait_for_completion(
                remaining_until(deadline));
            if (waited.code != LibusbBulkOutBackend::WaitCode::completion) {
                ring.cancel();
                auto status = protocol::TransportStatus::IoError;
                std::string_view detail = "USB bulk OUT backend stopped";
                if (waited.code == LibusbBulkOutBackend::WaitCode::timeout) {
                    status = protocol::TransportStatus::Timeout;
                    detail = "USB bulk OUT deadline expired";
                } else if (waited.code ==
                           LibusbBulkOutBackend::WaitCode::event_error) {
                    detail = "libusb event loop terminated during bulk OUT";
                }
                auto failure = ring_failure(ring, status, detail);
                failure.status = status;
                failure = classify_requested_cancellation(
                    std::move(failure),
                    cancellation_requested_.load(std::memory_order_acquire));
                poison_and_stop();
                return failure;
            }
            const auto previous_watermark = ring.completion_watermark();
            static_cast<void>(ring.handle_completion(waited.completion));
            const auto completion_watermark = ring.completion_watermark();
            if (observer && completion_watermark > previous_watermark) {
                TransferProgressAction action;
                try {
                    action = observer(completion_watermark, total_bytes);
                } catch (...) {
                    ring.cancel();
                    auto failure = ring_failure(
                        ring,
                        protocol::TransportStatus::IoError,
                        "USB transfer progress observer failed");
                    failure.status = protocol::TransportStatus::IoError;
                    failure.detail = "USB transfer progress observer failed";
                    poison_and_stop();
                    return failure;
                }
                if (action == TransferProgressAction::cancel) {
                    ring.cancel();
                    auto failure = protocol::TransferResult{
                        .status = protocol::TransportStatus::Cancelled,
                        .transferred = transferred_size(completion_watermark),
                        .certainty = detail::observer_cancel_certainty(ring),
                        .truncated = false,
                        .detail = "USB bulk OUT was cancelled by progress observer",
                        .native_code = 0,
                    };
                    poison_and_stop();
                    return failure;
                }
            }
        }

        if (ring.state() != TransferRingState::completed) {
            auto failure = ring_failure(
                ring,
                protocol::TransportStatus::IoError,
                "USB bulk OUT did not complete");
            failure = classify_requested_cancellation(
                std::move(failure),
                cancellation_requested_.load(std::memory_order_acquire));
            poison_and_stop();
            return failure;
        }
        return {
            .status = protocol::TransportStatus::Ok,
            .transferred = transferred_size(total_bytes),
            .certainty = protocol::TransferCertainty::FullyTransferred,
            .truncated = false,
            .detail = {},
        };
    } catch (const std::bad_alloc&) {
        poison_and_stop();
        auto failure = protocol::TransferResult{
            .status = protocol::TransportStatus::IoError,
            .transferred = transferred_size(ring.completion_watermark()),
            .certainty = ring.submitted_bytes() != 0
                ? protocol::TransferCertainty::PartialOrUnknown
                : protocol::TransferCertainty::NotTransferred,
            .truncated = false,
            .detail = "USB bulk OUT exhausted host resources",
        };
        return classify_requested_cancellation(
            std::move(failure),
            cancellation_requested_.load(std::memory_order_acquire));
    }
}

protocol::TransferResult UsbFastbootTransport::read(
    const std::span<std::byte> destination,
    const std::chrono::milliseconds timeout) {
    std::scoped_lock operation(operation_mutex_);
    if (!open_.load(std::memory_order_acquire)) {
        const auto cancelled =
            cancellation_requested_.load(std::memory_order_acquire);
        return {
            .status = cancelled ? protocol::TransportStatus::Cancelled
                                : protocol::TransportStatus::Disconnected,
            .transferred = 0,
            .certainty = protocol::TransferCertainty::NotTransferred,
            .truncated = false,
            .detail = cancelled ? "Fastboot USB transport was cancelled"
                                : "Fastboot USB transport is closed",
        };
    }
    auto result = read_result(backend_->read_logical_response(destination, timeout));
    if (result.status != protocol::TransportStatus::Ok || result.truncated) {
        poison_and_stop();
    }
    return result;
}

protocol::TransferResult UsbFastbootTransport::read_data(
    const std::span<std::byte> destination,
    const std::chrono::milliseconds timeout) {
    std::scoped_lock operation(operation_mutex_);
    if (!open_.load(std::memory_order_acquire)) {
        const auto cancelled =
            cancellation_requested_.load(std::memory_order_acquire);
        return {
            .status = cancelled ? protocol::TransportStatus::Cancelled
                                : protocol::TransportStatus::Disconnected,
            .transferred = 0,
            .certainty = protocol::TransferCertainty::NotTransferred,
            .truncated = false,
            .detail = cancelled ? "Fastboot USB transport was cancelled"
                                : "Fastboot USB transport is closed",
        };
    }
    auto result = read_result(backend_->read_data(destination, timeout));
    if (result.status != protocol::TransportStatus::Ok || result.truncated) {
        poison_and_stop();
    }
    return result;
}

void UsbFastbootTransport::request_cancel() noexcept {
    cancellation_requested_.store(true, std::memory_order_release);
    open_.store(false, std::memory_order_release);
    if (backend_ != nullptr) {
        backend_->request_stop();
    }
}

void UsbFastbootTransport::cancel() noexcept { request_cancel(); }

void UsbFastbootTransport::close() noexcept {
    poison_and_stop();
}

bool UsbFastbootTransport::is_open() const noexcept {
    return open_.load(std::memory_order_acquire);
}

void UsbFastbootTransport::poison_and_stop() noexcept {
    open_.store(false, std::memory_order_release);
    if (backend_ != nullptr) {
        backend_->stop();
    }
}

}  // namespace kairosboot::transport
