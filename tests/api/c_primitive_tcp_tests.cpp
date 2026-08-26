// SPDX-License-Identifier: MIT
#include <kairosboot/kairosboot.h>

#include "src/transport/tcp_fastboot.hpp"

#include <boost/asio.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using boost::asio::ip::tcp;

#define CHECK(condition)                                                        \
  do {                                                                          \
    if (!(condition)) {                                                         \
      throw std::runtime_error(std::string("check failed at line ") +          \
                               std::to_string(__LINE__) + ": " #condition);    \
    }                                                                           \
  } while (false)

std::vector<std::byte> read_frame(tcp::socket& socket) {
  std::array<std::byte, 8> header{};
  boost::asio::read(socket, boost::asio::buffer(header));
  const auto size = kairosboot::transport::decode_tcp_frame_length(header);
  CHECK(size <= 4U * 1024U * 1024U);
  std::vector<std::byte> payload(static_cast<std::size_t>(size));
  boost::asio::read(socket, boost::asio::buffer(payload));
  return payload;
}

void write_frame(tcp::socket& socket, const std::string& payload) {
  const auto header = kairosboot::transport::encode_tcp_frame_length(
      payload.size());
  boost::asio::write(socket, boost::asio::buffer(header));
  boost::asio::write(socket, boost::asio::buffer(payload));
}

std::string as_string(const std::span<const std::byte> bytes) {
  return std::string(
      reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

class ScriptedServer final {
public:
  ScriptedServer()
      : acceptor_(context_, tcp::endpoint(tcp::v4(), 0)),
        port_(acceptor_.local_endpoint().port()),
        worker_([this] { run(); }) {}

  ~ScriptedServer() {
    if (worker_.joinable()) {
      boost::system::error_code ignored;
      acceptor_.close(ignored);
      worker_.join();
    }
  }

  [[nodiscard]] std::uint16_t port() const noexcept { return port_; }

  void finish() {
    worker_.join();
    if (failure_ != nullptr) {
      std::rethrow_exception(failure_);
    }
  }

private:
  void handshake(tcp::socket& socket) {
    std::array<char, 4> handshake{};
    boost::asio::read(socket, boost::asio::buffer(handshake));
    CHECK((handshake == std::array<char, 4>{'F', 'B', '0', '1'}));
    boost::asio::write(socket, boost::asio::buffer(handshake));
  }

  tcp::socket accept() {
    tcp::socket socket(context_);
    acceptor_.accept(socket);
    handshake(socket);
    return socket;
  }

  void run() noexcept {
    try {
      {
        auto socket = accept();
        CHECK(as_string(read_frame(socket)) == "getvar:product");
        write_frame(socket, std::string("INFOone\0two", 11));
        write_frame(socket, "TEXThuman text");
        write_frame(socket, "OKAYproduct_a");
      }
      {
        auto socket = accept();
        CHECK(as_string(read_frame(socket)) == "erase:userdata");
        write_frame(socket, "INFOwarning");
        write_frame(socket, "FAILpartition locked");
      }
      {
        auto socket = accept();
        CHECK(as_string(read_frame(socket)) == "download:00000010");
        write_frame(socket, "DATA00000010");
        const auto payload = read_frame(socket);
        CHECK(payload.size() == 16U);
        for (std::size_t index = 0; index < payload.size(); ++index) {
          CHECK(payload[index] == std::byte{static_cast<unsigned char>(index)});
        }
        write_frame(socket, "OKAY");
      }
      {
        auto socket = accept();
        CHECK(as_string(read_frame(socket)) == "upload");
        write_frame(socket, "DATA00000005");
        write_frame(socket, std::string("a\0bcd", 5));
        write_frame(socket, "OKAYuploaded");
      }
      {
        auto socket = accept();
        CHECK(as_string(read_frame(socket)) ==
              "fetch:vendor:0x00000002:0x00000003");
        write_frame(socket, "INFOfetching");
        write_frame(socket, "DATA00000003");
        write_frame(socket, std::string("x\0y", 3));
        write_frame(socket, "OKAYfetched");
      }
      {
        auto socket = accept();
        CHECK(as_string(read_frame(socket)) == "upload");
        write_frame(socket, "DATA00000020");
      }
    } catch (...) {
      failure_ = std::current_exception();
    }
  }

  boost::asio::io_context context_;
  tcp::acceptor acceptor_;
  std::uint16_t port_;
  std::thread worker_;
  std::exception_ptr failure_;
};

kb_progress_action_t KB_CALL record_progress(
    const kb_progress_t* progress, void* user_data) {
  auto& watermarks = *static_cast<std::vector<std::uint64_t>*>(user_data);
  watermarks.push_back(progress->bytes_completed);
  return KB_PROGRESS_CONTINUE;
}

void run_contract() {
  ScriptedServer server;
  const auto selector =
      "tcp:127.0.0.1:" + std::to_string(server.port());
  kb_context_t* context = nullptr;
  kb_error_t* error = nullptr;
  CHECK(kb_context_create(nullptr, &context, &error) == KB_OK);
  CHECK(context != nullptr);
  CHECK(error == nullptr);

  kb_command_result_t* result = nullptr;
  CHECK(kb_getvar(context, selector.c_str(), "product", nullptr, &result,
                  &error) == KB_OK);
  CHECK(result != nullptr);
  CHECK(error == nullptr);
  size_t size = 0;
  const auto* terminal = kb_command_result_terminal_payload(result, &size);
  CHECK(std::string(reinterpret_cast<const char*>(terminal), size) ==
        "product_a");
  CHECK(kb_command_result_message_count(result) == 2U);
  const auto* first = kb_command_result_message_payload(result, 0, &size);
  CHECK(size == 7U);
  CHECK(std::memcmp(first, "one\0two", size) == 0);
  CHECK(kb_command_result_message_kind(result, 0) == KB_COMMAND_MESSAGE_INFO);
  CHECK(kb_command_result_message_kind(result, 1) == KB_COMMAND_MESSAGE_TEXT);
  CHECK(std::strcmp(kb_command_result_device_identifier(result),
                    selector.c_str()) == 0);
  kb_command_result_release(result);

  result = nullptr;
  CHECK(kb_erase(context, selector.c_str(), "userdata", nullptr, &result,
                 &error) == KB_E_DEVICE_FAIL);
  CHECK(result == nullptr);
  CHECK(error != nullptr);
  CHECK(kb_error_status(error) == KB_E_DEVICE_FAIL);
  const auto* device_message = kb_error_device_message(error, &size);
  CHECK(std::string(reinterpret_cast<const char*>(device_message), size) ==
        "partition locked");
  CHECK(kb_error_command_message_count(error) == 1U);
  const auto* warning = kb_error_command_message_payload(error, 0, &size);
  CHECK(std::string(reinterpret_cast<const char*>(warning), size) == "warning");
  CHECK(kb_error_session_poisoned(error) == 0);
  kb_error_release(error);
  error = nullptr;

  std::array<std::byte, 16> stage_data{};
  for (std::size_t index = 0; index < stage_data.size(); ++index) {
    stage_data[index] = std::byte{static_cast<unsigned char>(index)};
  }
  std::vector<std::uint64_t> watermarks;
  kb_command_options_t options;
  kb_command_options_init(&options);
  options.progress_callback = record_progress;
  options.progress_user_data = &watermarks;
  CHECK(kb_stage(context, selector.c_str(), stage_data.data(), stage_data.size(),
                 &options, &result, &error) == KB_OK);
  CHECK(result != nullptr);
  CHECK(error == nullptr);
  CHECK(watermarks == std::vector<std::uint64_t>({0, 16}));
  kb_command_result_release(result);

  result = nullptr;
  CHECK(kb_upload(context, selector.c_str(), nullptr, &result, &error) == KB_OK);
  CHECK(result != nullptr);
  const auto* upload = kb_command_result_data(result, &size);
  CHECK(size == 5U);
  CHECK(std::memcmp(upload, "a\0bcd", size) == 0);
  terminal = kb_command_result_terminal_payload(result, &size);
  CHECK(std::string(reinterpret_cast<const char*>(terminal), size) ==
        "uploaded");
  kb_command_result_release(result);

  result = nullptr;
  CHECK(kb_fetch(context, selector.c_str(), "vendor", 2, 3, nullptr, &result,
                 &error) == KB_OK);
  CHECK(result != nullptr);
  const auto* fetched = kb_command_result_data(result, &size);
  CHECK(size == 3U);
  CHECK(std::memcmp(fetched, "x\0y", size) == 0);
  CHECK(kb_command_result_message_count(result) == 1U);
  kb_command_result_release(result);

  options.maximum_receive_bytes = 16;
  result = nullptr;
  CHECK(kb_upload(context, selector.c_str(), &options, &result, &error) ==
        KB_E_PROTOCOL);
  CHECK(result == nullptr);
  CHECK(error != nullptr);
  CHECK(kb_error_inbound_expected_bytes(error) == 32);
  CHECK(kb_error_inbound_transferred_bytes(error) == 0);
  CHECK(kb_error_inbound_transfer_state(error) ==
        KB_TRANSFER_PARTIAL_OR_UNKNOWN);
  CHECK(kb_error_session_poisoned(error) == 1);
  kb_error_release(error);
  kb_context_release(context);
  server.finish();
}

}  // namespace

int main() {
  try {
    run_contract();
    std::cout << "PASS: typed C primitives over Fastboot TCP\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
