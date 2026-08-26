// SPDX-License-Identifier: MIT
#include "src/transport/sequential_streaming_transport.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <new>
#include <span>
#include <utility>
#include <vector>

namespace kairosboot::transport {
namespace {

class SequentialStreamingTransport final
    : public protocol::ITransportSession,
      public protocol::IStreamingTransportSession {
public:
    explicit SequentialStreamingTransport(
        std::unique_ptr<protocol::ITransportSession> transport)
        : transport_(std::move(transport)) {}

    [[nodiscard]] protocol::TransferResult write(
        std::span<const std::byte> bytes,
        std::chrono::milliseconds timeout) override {
        return transport_->write(bytes, timeout);
    }

    [[nodiscard]] protocol::TransferResult read(
        std::span<std::byte> destination,
        std::chrono::milliseconds timeout) override {
        return transport_->read(destination, timeout);
    }

    [[nodiscard]] protocol::TransferResult read_data(
        std::span<std::byte> destination,
        std::chrono::milliseconds timeout) override {
        return transport_->read_data(destination, timeout);
    }

    [[nodiscard]] protocol::TransferResult write_source(
        std::shared_ptr<protocol::ITransferSource> source,
        std::chrono::milliseconds timeout,
        const protocol::TransferProgressObserver& observer) override {
        if (source == nullptr) {
            return {
                .status = protocol::TransportStatus::IoError,
                .certainty = protocol::TransferCertainty::NotTransferred,
                .detail = "streaming source is null",
            };
        }
        constexpr std::size_t chunk_bytes = 1024U * 1024U;
        std::vector<std::byte> buffer;
        try {
            buffer.resize(static_cast<std::size_t>(
                std::min<std::uint64_t>(source->size(), chunk_bytes)));
        } catch (const std::bad_alloc&) {
            return {
                .status = protocol::TransportStatus::IoError,
                .certainty = protocol::TransferCertainty::NotTransferred,
                .detail = "streaming buffer allocation failed",
            };
        }
        const auto cancelled = [&observer, &source](
                                   std::uint64_t completed) noexcept {
            if (!observer) {
                return false;
            }
            try {
                return observer(completed, source->size()) ==
                    protocol::TransferProgressAction::cancel;
            } catch (...) {
                return true;
            }
        };

        std::uint64_t completed = 0;
        if (cancelled(completed)) {
            transport_->request_cancel();
            return {
                .status = protocol::TransportStatus::Cancelled,
                .transferred = 0,
                .certainty = protocol::TransferCertainty::NotTransferred,
                .detail = "streaming source cancelled before transfer",
            };
        }
        while (completed < source->size()) {
            const auto requested = static_cast<std::size_t>(
                std::min<std::uint64_t>(buffer.size(), source->size() - completed));
            auto chunk = std::span(buffer).first(requested);
            if (!source->read_exact(completed, chunk)) {
                return {
                    .status = protocol::TransportStatus::IoError,
                    .transferred = static_cast<std::size_t>(completed),
                    .certainty = completed == 0
                        ? protocol::TransferCertainty::NotTransferred
                        : protocol::TransferCertainty::PartialOrUnknown,
                    .detail = "streaming source read failed",
                };
            }
            std::size_t chunk_offset = 0;
            while (chunk_offset < chunk.size()) {
                const auto result = transport_->write(
                    chunk.subspan(chunk_offset), timeout);
                if (result.transferred > chunk.size() - chunk_offset) {
                    return {
                        .status = protocol::TransportStatus::IoError,
                        .transferred = static_cast<std::size_t>(completed) + chunk_offset,
                        .certainty = protocol::TransferCertainty::PartialOrUnknown,
                        .detail = "transport over-reported a streamed write",
                    };
                }
                chunk_offset += result.transferred;
                if (result.status != protocol::TransportStatus::Ok ||
                    result.certainty != protocol::TransferCertainty::FullyTransferred ||
                    result.transferred == 0 || result.truncated) {
                    const auto transferred =
                        static_cast<std::size_t>(completed) + chunk_offset;
                    auto failed = result;
                    failed.transferred = transferred;
                    if (transferred != source->size()) {
                        failed.certainty = transferred == 0
                            ? protocol::TransferCertainty::NotTransferred
                            : protocol::TransferCertainty::PartialOrUnknown;
                    }
                    return failed;
                }
            }
            completed += chunk.size();
            if (cancelled(completed)) {
                transport_->request_cancel();
                return {
                    .status = protocol::TransportStatus::Cancelled,
                    .transferred = static_cast<std::size_t>(completed),
                    .certainty = completed == source->size()
                        ? protocol::TransferCertainty::FullyTransferred
                        : protocol::TransferCertainty::PartialOrUnknown,
                    .detail = "streaming source cancelled",
                };
            }
        }
        return {
            .status = protocol::TransportStatus::Ok,
            .transferred = static_cast<std::size_t>(completed),
            .certainty = protocol::TransferCertainty::FullyTransferred,
        };
    }

    void request_cancel() noexcept override { transport_->request_cancel(); }
    void close() noexcept override { transport_->close(); }

private:
    std::unique_ptr<protocol::ITransportSession> transport_;
};

}  // namespace

std::unique_ptr<protocol::ITransportSession> make_sequential_streaming_transport(
    std::unique_ptr<protocol::ITransportSession> transport) {
    return std::make_unique<SequentialStreamingTransport>(std::move(transport));
}

}  // namespace kairosboot::transport
