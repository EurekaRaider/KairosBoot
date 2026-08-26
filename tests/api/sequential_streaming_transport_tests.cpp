// SPDX-License-Identifier: MIT
#include "src/transport/sequential_streaming_transport.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using kairosboot::protocol::IStreamingTransportSession;
using kairosboot::protocol::ITransferSource;
using kairosboot::protocol::ITransportSession;
using kairosboot::protocol::TransferCertainty;
using kairosboot::protocol::TransferProgressAction;
using kairosboot::protocol::TransferResult;
using kairosboot::protocol::TransportStatus;

#define CHECK(condition)                                                        \
  do {                                                                          \
    if (!(condition)) {                                                         \
      throw std::runtime_error(std::string("check failed at line ") +          \
                               std::to_string(__LINE__) + ": " #condition);    \
    }                                                                           \
  } while (false)

class BytesSource final : public ITransferSource {
public:
  explicit BytesSource(std::vector<std::byte> bytes)
      : bytes_(std::move(bytes)) {}

  [[nodiscard]] std::uint64_t size() const noexcept override {
    return bytes_.size();
  }

  [[nodiscard]] bool read_exact(
      const std::uint64_t offset,
      const std::span<std::byte> destination) noexcept override {
    if (offset > bytes_.size() ||
        destination.size() > bytes_.size() - static_cast<std::size_t>(offset)) {
      return false;
    }
    std::copy_n(
        bytes_.data() + static_cast<std::size_t>(offset), destination.size(),
        destination.data());
    return true;
  }

private:
  std::vector<std::byte> bytes_;
};

struct RecordingState final {
  std::vector<std::byte> bytes;
  std::size_t maximum_write{256U * 1024U};
  bool cancelled{false};
  bool closed{false};
};

class RecordingTransport final : public ITransportSession {
public:
  explicit RecordingTransport(std::shared_ptr<RecordingState> state)
      : state_(std::move(state)) {}

  [[nodiscard]] TransferResult write(
      const std::span<const std::byte> bytes,
      std::chrono::milliseconds) override {
    const auto transferred = std::min(bytes.size(), state_->maximum_write);
    state_->bytes.insert(
        state_->bytes.end(), bytes.begin(), bytes.begin() + transferred);
    return {
        .status = TransportStatus::Ok,
        .transferred = transferred,
        .certainty = TransferCertainty::FullyTransferred,
    };
  }

  [[nodiscard]] TransferResult read(
      std::span<std::byte>, std::chrono::milliseconds) override {
    return {.status = TransportStatus::IoError};
  }

  [[nodiscard]] TransferResult read_data(
      std::span<std::byte>, std::chrono::milliseconds) override {
    return {.status = TransportStatus::IoError};
  }

  void request_cancel() noexcept override { state_->cancelled = true; }
  void close() noexcept override { state_->closed = true; }

private:
  std::shared_ptr<RecordingState> state_;
};

std::vector<std::byte> patterned_bytes(const std::size_t size) {
  std::vector<std::byte> result(size);
  for (std::size_t index = 0; index < result.size(); ++index) {
    result[index] = std::byte{static_cast<unsigned char>(index % 251U)};
  }
  return result;
}

void streams_bounded_chunks_and_reports_watermarks() {
  const auto expected = patterned_bytes(2U * 1024U * 1024U + 17U);
  auto state = std::make_shared<RecordingState>();
  auto transport = kairosboot::transport::make_sequential_streaming_transport(
      std::make_unique<RecordingTransport>(state));
  auto* streaming = dynamic_cast<IStreamingTransportSession*>(transport.get());
  CHECK(streaming != nullptr);
  std::vector<std::uint64_t> watermarks;
  const auto result = streaming->write_source(
      std::make_shared<BytesSource>(expected), 1s,
      [&watermarks](const std::uint64_t completed, const std::uint64_t) {
        watermarks.push_back(completed);
        return TransferProgressAction::continue_transfer;
      });

  CHECK(result.status == TransportStatus::Ok);
  CHECK(result.certainty == TransferCertainty::FullyTransferred);
  CHECK(result.transferred == expected.size());
  CHECK(state->bytes == expected);
  CHECK(watermarks == std::vector<std::uint64_t>(
                          {0, 1024U * 1024U, 2U * 1024U * 1024U,
                           expected.size()}));
}

void cancellation_before_submit_is_certainly_not_sent() {
  auto state = std::make_shared<RecordingState>();
  auto transport = kairosboot::transport::make_sequential_streaming_transport(
      std::make_unique<RecordingTransport>(state));
  auto* streaming = dynamic_cast<IStreamingTransportSession*>(transport.get());
  const auto result = streaming->write_source(
      std::make_shared<BytesSource>(patterned_bytes(32)), 1s,
      [](const std::uint64_t, const std::uint64_t) {
        return TransferProgressAction::cancel;
      });
  CHECK(result.status == TransportStatus::Cancelled);
  CHECK(result.certainty == TransferCertainty::NotTransferred);
  CHECK(result.transferred == 0U);
  CHECK(state->bytes.empty());
  CHECK(state->cancelled);
}

void cancellation_after_one_chunk_reports_partial() {
  auto state = std::make_shared<RecordingState>();
  auto transport = kairosboot::transport::make_sequential_streaming_transport(
      std::make_unique<RecordingTransport>(state));
  auto* streaming = dynamic_cast<IStreamingTransportSession*>(transport.get());
  const auto result = streaming->write_source(
      std::make_shared<BytesSource>(patterned_bytes(2U * 1024U * 1024U)), 1s,
      [](const std::uint64_t completed, const std::uint64_t) {
        return completed >= 1024U * 1024U ? TransferProgressAction::cancel
                                          : TransferProgressAction::continue_transfer;
      });
  CHECK(result.status == TransportStatus::Cancelled);
  CHECK(result.certainty == TransferCertainty::PartialOrUnknown);
  CHECK(result.transferred == 1024U * 1024U);
  CHECK(state->bytes.size() == 1024U * 1024U);
  CHECK(state->cancelled);
}

}  // namespace

int main() {
  const std::vector<std::pair<std::string, void (*)()>> tests{
      {"bounded streaming progress", streams_bounded_chunks_and_reports_watermarks},
      {"pre-submit cancellation", cancellation_before_submit_is_certainly_not_sent},
      {"partial cancellation", cancellation_after_one_chunk_reports_partial},
  };
  int failures = 0;
  for (const auto& [name, test] : tests) {
    try {
      test();
      std::cout << "PASS: " << name << '\n';
    } catch (const std::exception& error) {
      ++failures;
      std::cerr << "FAIL: " << name << ": " << error.what() << '\n';
    }
  }
  return failures == 0 ? 0 : 1;
}
