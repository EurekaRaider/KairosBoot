// SPDX-License-Identifier: MIT
#include "scripted_transport.hpp"

#include <algorithm>
#include <utility>

namespace kairosboot::protocol::test {

std::vector<std::byte> to_bytes(const std::string_view value) {
    std::vector<std::byte> bytes;
    bytes.reserve(value.size());
    for (const unsigned char character : value) {
        bytes.push_back(static_cast<std::byte>(character));
    }
    return bytes;
}

void ScriptedTransport::expect_write(
    const std::string_view expected_call,
    const std::optional<std::size_t> reported_transferred,
    const TransportStatus status,
    const TransferCertainty certainty,
    const int native_code,
    std::string detail) {
    const auto bytes = to_bytes(expected_call);
    expect_write(
        bytes,
        reported_transferred,
        status,
        certainty,
        native_code,
        std::move(detail));
}

void ScriptedTransport::expect_write(
    const std::span<const std::byte> expected_call,
    const std::optional<std::size_t> reported_transferred,
    const TransportStatus status,
    const TransferCertainty certainty,
    const int native_code,
    std::string detail) {
    steps_.push_back(WriteStep{
        .expected_call = {expected_call.begin(), expected_call.end()},
        .reported_transferred = reported_transferred.value_or(expected_call.size()),
        .status = status,
        .certainty = certainty,
        .detail = std::move(detail),
        .native_code = native_code,
    });
}

void ScriptedTransport::respond(
    const std::string_view response,
    const TransportStatus status,
    const TransferCertainty certainty,
    const bool truncated,
    const std::optional<std::size_t> reported_transferred,
    const int native_code,
    std::string detail) {
    steps_.push_back(ReadStep{
        .response = to_bytes(response),
        .reported_transferred = reported_transferred,
        .status = status,
        .certainty = certainty,
        .truncated = truncated,
        .detail = std::move(detail),
        .native_code = native_code,
    });
}

void ScriptedTransport::expect_source_write(
    const std::span<const std::byte> expected_payload,
    std::vector<SourceRead> reads,
    const std::optional<std::size_t> reported_transferred,
    const TransportStatus status,
    const TransferCertainty certainty,
    const int native_code,
    std::string detail) {
    steps_.push_back(SourceWriteStep{
        .expected_payload = {expected_payload.begin(), expected_payload.end()},
        .reads = std::move(reads),
        .reported_transferred = reported_transferred.value_or(
            expected_payload.size()),
        .status = status,
        .certainty = certainty,
        .detail = std::move(detail),
        .native_code = native_code,
    });
}

TransferResult ScriptedTransport::write(
    const std::span<const std::byte> bytes,
    std::chrono::milliseconds /*timeout*/) {
    if (steps_.empty() || !std::holds_alternative<WriteStep>(steps_.front())) {
        return unexpected_call("write");
    }

    auto step = std::get<WriteStep>(std::move(steps_.front()));
    steps_.pop_front();
    if (!std::ranges::equal(bytes, step.expected_call) && failure_.empty()) {
        failure_ = "write call did not match the scripted bytes";
    }

    const auto accepted = std::min(step.reported_transferred, bytes.size());
    accepted_bytes_.insert(
        accepted_bytes_.end(), bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(accepted));
    return {
        .status = step.status,
        .transferred = step.reported_transferred,
        .certainty = step.certainty,
        .truncated = step.truncated,
        .detail = std::move(step.detail),
        .native_code = step.native_code,
    };
}

TransferResult ScriptedTransport::read(
    const std::span<std::byte> destination,
    std::chrono::milliseconds /*timeout*/) {
    if (steps_.empty() || !std::holds_alternative<ReadStep>(steps_.front())) {
        return unexpected_call("read");
    }

    auto step = std::get<ReadStep>(std::move(steps_.front()));
    steps_.pop_front();
    const auto copied = std::min(destination.size(), step.response.size());
    std::ranges::copy_n(step.response.begin(), copied, destination.begin());
    const auto reported = step.reported_transferred.value_or(step.response.size());
    return {
        .status = step.status,
        .transferred = reported,
        .certainty = step.certainty,
        .truncated = step.truncated || step.response.size() > destination.size(),
        .detail = std::move(step.detail),
        .native_code = step.native_code,
    };
}

TransferResult ScriptedTransport::read_data(
    const std::span<std::byte> destination,
    const std::chrono::milliseconds timeout) {
    return read(destination, timeout);
}

TransferResult ScriptedTransport::write_source(
    const std::shared_ptr<ITransferSource> source,
    std::chrono::milliseconds /*timeout*/,
    const TransferProgressObserver& observer) {
    if (steps_.empty() ||
        !std::holds_alternative<SourceWriteStep>(steps_.front())) {
        return unexpected_call("source write");
    }

    auto step = std::get<SourceWriteStep>(std::move(steps_.front()));
    steps_.pop_front();
    const auto configured_result = [&] {
        return TransferResult{
            .status = step.status,
            .transferred = step.reported_transferred,
            .certainty = step.certainty,
            .detail = step.detail,
            .native_code = step.native_code,
        };
    };
    if (source == nullptr || source->size() != step.expected_payload.size()) {
        if (failure_.empty()) {
            failure_ = "source write size did not match the script";
        }
        return {
            .status = TransportStatus::IoError,
            .certainty = TransferCertainty::NotTransferred,
            .detail = failure_,
        };
    }

    std::vector<std::byte> observed(step.expected_payload.size());
    std::vector<unsigned char> covered(step.expected_payload.size(), 0);
    std::uint64_t last_progress = 0;
    for (const auto& read : step.reads) {
        if (read.offset > step.expected_payload.size() ||
            read.size > step.expected_payload.size() -
                static_cast<std::size_t>(read.offset) ||
            read.progress_watermark < last_progress ||
            read.progress_watermark > step.expected_payload.size()) {
            if (failure_.empty()) {
                failure_ = "source write read plan is invalid";
            }
            return {
                .status = TransportStatus::IoError,
                .certainty = TransferCertainty::NotTransferred,
                .detail = failure_,
            };
        }

        std::vector<std::byte> buffer(read.size);
        if (!source->read_exact(read.offset, buffer)) {
            if (step.status == TransportStatus::Ok && failure_.empty()) {
                failure_ = "source read failed unexpectedly";
            }
            if (step.status != TransportStatus::Ok) {
                return configured_result();
            }
            return {
                .status = TransportStatus::IoError,
                .certainty = TransferCertainty::NotTransferred,
                .detail = failure_,
            };
        }

        const auto offset = static_cast<std::size_t>(read.offset);
        std::ranges::copy(buffer, observed.begin() +
                                     static_cast<std::ptrdiff_t>(offset));
        std::fill_n(
            covered.begin() + static_cast<std::ptrdiff_t>(offset),
            read.size,
            static_cast<unsigned char>(1));

        if (read.progress_watermark > last_progress) {
            auto action = TransferProgressAction::continue_transfer;
            if (observer) {
                try {
                    action = observer(
                        read.progress_watermark, step.expected_payload.size());
                } catch (...) {
                    return {
                        .status = TransportStatus::IoError,
                        .transferred = static_cast<std::size_t>(last_progress),
                        .certainty = last_progress == 0
                            ? TransferCertainty::NotTransferred
                            : TransferCertainty::PartialOrUnknown,
                        .detail = "scripted progress observer failed",
                    };
                }
            }
            last_progress = read.progress_watermark;
            if (action == TransferProgressAction::cancel) {
                const auto accepted = static_cast<std::size_t>(last_progress);
                accepted_bytes_.insert(
                    accepted_bytes_.end(),
                    step.expected_payload.begin(),
                    step.expected_payload.begin() +
                        static_cast<std::ptrdiff_t>(accepted));
                return {
                    .status = TransportStatus::Cancelled,
                    .transferred = accepted,
                    .certainty = accepted == 0
                        ? TransferCertainty::NotTransferred
                        : accepted == step.expected_payload.size()
                            ? TransferCertainty::FullyTransferred
                            : TransferCertainty::PartialOrUnknown,
                    .detail = "scripted source write was cancelled by progress observer",
                    .native_code = step.native_code,
                };
            }
        }
    }

    if (!std::ranges::all_of(covered, [](const unsigned char value) {
            return value != 0;
        }) || observed != step.expected_payload) {
        if (failure_.empty()) {
            failure_ = "source write payload did not match the script";
        }
        return {
            .status = TransportStatus::IoError,
            .certainty = TransferCertainty::NotTransferred,
            .detail = failure_,
        };
    }

    const auto accepted = std::min(
        step.reported_transferred, step.expected_payload.size());
    accepted_bytes_.insert(
        accepted_bytes_.end(),
        step.expected_payload.begin(),
        step.expected_payload.begin() + static_cast<std::ptrdiff_t>(accepted));
    return configured_result();
}

void ScriptedTransport::request_cancel() noexcept {
    cancellation_requested_.store(true, std::memory_order_release);
}

void ScriptedTransport::close() noexcept {
    closed_ = true;
}

bool ScriptedTransport::complete() const noexcept {
    return steps_.empty() && failure_.empty();
}

const std::string& ScriptedTransport::failure() const noexcept {
    return failure_;
}

const std::vector<std::byte>& ScriptedTransport::accepted_bytes() const noexcept {
    return accepted_bytes_;
}

bool ScriptedTransport::closed() const noexcept {
    return closed_;
}

bool ScriptedTransport::cancellation_requested() const noexcept {
    return cancellation_requested_.load(std::memory_order_acquire);
}

TransferResult ScriptedTransport::unexpected_call(const std::string_view operation) {
    if (failure_.empty()) {
        failure_ = "unexpected scripted transport ";
        failure_.append(operation);
    }
    return {
        .status = TransportStatus::IoError,
        .transferred = 0,
        .certainty = TransferCertainty::NotTransferred,
        .detail = failure_,
    };
}

}  // namespace kairosboot::protocol::test
