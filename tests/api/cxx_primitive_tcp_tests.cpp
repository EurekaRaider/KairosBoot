// SPDX-License-Identifier: MIT
#include <kairosboot/kairosboot.hpp>

#include <boost/asio.hpp>

#include <array>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <initializer_list>
#include <iostream>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using boost::asio::ip::tcp;

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      throw std::runtime_error(std::string("check failed at line ") +          \
                               std::to_string(__LINE__) + ": " #condition);    \
    }                                                                          \
  } while (false)

std::array<std::byte, 8> encode_frame_length(const std::uint64_t length) {
  std::array<std::byte, 8> result{};
  for (std::size_t index = 0; index < result.size(); ++index) {
    result[index] = static_cast<std::byte>(
        (length >> ((result.size() - index - 1U) * 8U)) & 0xffU);
  }
  return result;
}

std::uint64_t decode_frame_length(const std::span<const std::byte, 8> bytes) {
  std::uint64_t result = 0;
  for (const auto byte : bytes) {
    result = (result << 8U) | std::to_integer<std::uint8_t>(byte);
  }
  return result;
}

std::vector<std::byte> read_frame(tcp::socket &socket) {
  std::array<std::byte, 8> header{};
  boost::asio::read(socket, boost::asio::buffer(header));
  const std::uint64_t length = decode_frame_length(header);
  CHECK(length <= 4U * 1024U * 1024U);
  std::vector<std::byte> payload(static_cast<std::size_t>(length));
  boost::asio::read(socket, boost::asio::buffer(payload));
  return payload;
}

void write_frame(tcp::socket &socket,
                 const std::span<const std::byte> payload) {
  const auto header = encode_frame_length(payload.size());
  boost::asio::write(socket, boost::asio::buffer(header));
  boost::asio::write(socket, boost::asio::buffer(payload));
}

void write_frame(tcp::socket &socket, const std::string_view payload) {
  write_frame(socket, std::as_bytes(std::span{payload.data(), payload.size()}));
}

std::vector<std::byte>
binary_payload(const std::string_view prefix,
               const std::initializer_list<unsigned char> suffix) {
  std::vector<std::byte> result;
  result.reserve(prefix.size() + suffix.size());
  for (const unsigned char value : prefix) {
    result.push_back(static_cast<std::byte>(value));
  }
  for (const unsigned char value : suffix) {
    result.push_back(static_cast<std::byte>(value));
  }
  return result;
}

std::string as_string(const std::span<const std::byte> bytes) {
  return {reinterpret_cast<const char *>(bytes.data()), bytes.size()};
}

bool bytes_equal(const std::span<const std::byte> actual,
                 const std::initializer_list<unsigned char> expected) {
  if (actual.size() != expected.size()) {
    return false;
  }
  std::size_t index = 0;
  for (const unsigned char value : expected) {
    if (actual[index] != static_cast<std::byte>(value)) {
      return false;
    }
    ++index;
  }
  return true;
}

class ScriptedTcpDevice final {
public:
  ScriptedTcpDevice()
      : acceptor_(context_, tcp::endpoint(tcp::v4(), 0)),
        port_(acceptor_.local_endpoint().port()), worker_([this] { run(); }) {}

  ~ScriptedTcpDevice() {
    stop();
    if (worker_.joinable()) {
      worker_.join();
    }
  }

  ScriptedTcpDevice(const ScriptedTcpDevice &) = delete;
  ScriptedTcpDevice &operator=(const ScriptedTcpDevice &) = delete;

  [[nodiscard]] std::uint16_t port() const noexcept { return port_; }

  void wait_for_cancel_command() {
    std::unique_lock lock(mutex_);
    condition_.wait(lock, [this] { return cancel_ready_ || failure_; });
    rethrow_failure_locked();
  }

  void finish() {
    if (worker_.joinable()) {
      worker_.join();
    }
    std::scoped_lock lock(mutex_);
    rethrow_failure_locked();
    CHECK(cancel_drained_);
  }

private:
  std::shared_ptr<tcp::socket> accept() {
    auto socket = std::make_shared<tcp::socket>(context_);
    acceptor_.accept(*socket);
    {
      std::scoped_lock lock(mutex_);
      active_socket_ = socket;
    }

    std::array<char, 4> handshake{};
    boost::asio::read(*socket, boost::asio::buffer(handshake));
    CHECK((handshake == std::array<char, 4>{'F', 'B', '0', '1'}));
    boost::asio::write(*socket, boost::asio::buffer(handshake));
    return socket;
  }

  void clear_active() {
    std::scoped_lock lock(mutex_);
    active_socket_.reset();
  }

  void serve_binary_getvar() {
    auto socket = accept();
    CHECK(as_string(read_frame(*socket)) == "getvar:binary");
    write_frame(*socket, binary_payload("INFO", {'i', 0, 0xff}));
    write_frame(*socket, binary_payload("TEXT", {'t', 0, 0xfe}));
    write_frame(*socket, binary_payload("OKAY", {'v', 0, 0xfd}));
    clear_active();
  }

  void serve_binary_fail() {
    auto socket = accept();
    CHECK(as_string(read_frame(*socket)) == "erase:userdata");
    write_frame(*socket, binary_payload("INFO", {'w', 0, 0xfc}));
    write_frame(*socket, binary_payload("TEXT", {'h', 0, 0xfb}));
    write_frame(*socket, binary_payload("FAIL", {'e', 0, 0xfa}));
    clear_active();
  }

  void serve_partial_upload() {
    auto socket = accept();
    CHECK(as_string(read_frame(*socket)) == "upload");
    write_frame(*socket, "DATA00000005");
    const std::array partial{std::byte{'p'}, std::byte{0}};
    write_frame(*socket, partial);
    boost::system::error_code ignored;
    socket->shutdown(tcp::socket::shutdown_both, ignored);
    socket->close(ignored);
    clear_active();
  }

  void serve_cancelled_getvar() {
    auto socket = accept();
    CHECK(as_string(read_frame(*socket)) == "getvar:cancel");
    {
      std::scoped_lock lock(mutex_);
      cancel_ready_ = true;
    }
    condition_.notify_all();

    std::array<std::byte, 1> discarded{};
    boost::system::error_code closed;
    static_cast<void>(
        socket->read_some(boost::asio::buffer(discarded), closed));
    CHECK(closed);
    {
      std::scoped_lock lock(mutex_);
      cancel_drained_ = true;
    }
    condition_.notify_all();
    clear_active();
  }

  void run() noexcept {
    try {
      serve_binary_getvar();
      serve_binary_fail();
      serve_partial_upload();
      serve_cancelled_getvar();
    } catch (...) {
      {
        std::scoped_lock lock(mutex_);
        if (!stopping_) {
          failure_ = std::current_exception();
        }
      }
      condition_.notify_all();
    }
  }

  void stop() noexcept {
    std::shared_ptr<tcp::socket> active;
    {
      std::scoped_lock lock(mutex_);
      stopping_ = true;
      active = active_socket_;
    }
    condition_.notify_all();
    boost::system::error_code ignored;
    acceptor_.close(ignored);
    if (active) {
      active->close(ignored);
    }
  }

  void rethrow_failure_locked() const {
    if (failure_) {
      std::rethrow_exception(failure_);
    }
  }

  boost::asio::io_context context_;
  tcp::acceptor acceptor_;
  std::uint16_t port_;
  std::thread worker_;
  mutable std::mutex mutex_;
  std::condition_variable condition_;
  std::shared_ptr<tcp::socket> active_socket_;
  bool cancel_ready_{false};
  bool cancel_drained_{false};
  bool stopping_{false};
  std::exception_ptr failure_;
};

void binary_result_and_selector_passthrough(kairosboot::Context &context,
                                            const std::string &selector_text) {
  const kairosboot::DeviceSelector selector{std::string_view{selector_text}};
  auto result = context.getvar(selector, "binary");
  CHECK(result.has_value());
  CHECK(result->device_identifier() == selector_text);
  CHECK(bytes_equal(result->terminal_payload(), {'v', 0, 0xfd}));
  CHECK(result->message_count() == 2U);

  const auto info = result->message(0);
  CHECK(info.has_value());
  CHECK(info->kind == kairosboot::CommandMessageKind::Info);
  CHECK(bytes_equal(info->payload, {'i', 0, 0xff}));

  const auto text = result->message(1);
  CHECK(text.has_value());
  CHECK(text->kind == kairosboot::CommandMessageKind::Text);
  CHECK(bytes_equal(text->payload, {'t', 0, 0xfe}));
  CHECK(!result->message(2).has_value());
}

void device_fail_diagnostics(kairosboot::Context &context,
                             const std::string &selector_text) {
  const kairosboot::DeviceSelector selector{std::string_view{selector_text}};
  auto result = context.erase(selector, "userdata");
  CHECK(!result.has_value());
  const auto &error = result.error();
  CHECK(error.status() == KB_E_DEVICE_FAIL);
  CHECK(error.device_identifier() == selector_text);
  CHECK(error.native_code() == 0);
  CHECK(error.transfer_state() == KB_TRANSFER_FULLY_TRANSFERRED);
  CHECK(bytes_equal(error.device_message(), {'e', 0, 0xfa}));
  CHECK(error.command_messages().size() == 2U);
  CHECK(error.command_messages()[0].kind ==
        kairosboot::CommandMessageKind::Info);
  CHECK(bytes_equal(error.command_messages()[0].payload, {'w', 0, 0xfc}));
  CHECK(error.command_messages()[1].kind ==
        kairosboot::CommandMessageKind::Text);
  CHECK(bytes_equal(error.command_messages()[1].payload, {'h', 0, 0xfb}));
  CHECK(!error.inbound_expected_bytes().has_value());
  CHECK(error.inbound_transferred_bytes() == 0U);
  CHECK(error.inbound_transfer_state() == KB_TRANSFER_NOT_SENT);
  CHECK(!error.session_poisoned());
}

void partial_upload_diagnostics(kairosboot::Context &context,
                                const std::string &selector_text) {
  const kairosboot::DeviceSelector selector{std::string_view{selector_text}};
  kairosboot::CommandOptions options;
  options.maximum_receive_bytes = 5U;
  auto result = context.upload(selector, options);
  CHECK(!result.has_value());
  const auto &error = result.error();
  CHECK(error.status() == KB_E_NO_DEVICE);
  CHECK(error.device_identifier() == selector_text);
  CHECK(error.transfer_state() == KB_TRANSFER_FULLY_TRANSFERRED);
  CHECK(error.inbound_expected_bytes() == 5U);
  CHECK(error.inbound_transferred_bytes() == 2U);
  CHECK(error.inbound_transfer_state() == KB_TRANSFER_PARTIAL_OR_UNKNOWN);
  CHECK(error.session_poisoned());
}

void stop_token_cancels_and_drains(kairosboot::Context &context,
                                   ScriptedTcpDevice &device,
                                   const std::string &selector_text) {
  const kairosboot::DeviceSelector selector{std::string_view{selector_text}};
  auto operation = context.getvar_async(selector, "cancel");
  CHECK(operation.has_value());
  device.wait_for_cancel_command();

  std::stop_source cancellation;
  CHECK(cancellation.request_stop());
  auto result = operation->wait_result(cancellation.get_token(),
                                       std::chrono::milliseconds::zero());
  CHECK(!result.has_value());
  CHECK(result.error().status() == KB_E_CANCELLED);
  CHECK(result.error().device_identifier() == selector_text);
  CHECK(result.error().session_poisoned());
  CHECK(operation->state() == KB_OPERATION_CANCELLED);
}

void run_contract() {
  ScriptedTcpDevice device;
  const std::string selector_text =
      "tcp:127.0.0.1:" + std::to_string(device.port());
  auto context = kairosboot::Context::create();
  CHECK(context.has_value());

  binary_result_and_selector_passthrough(*context, selector_text);
  device_fail_diagnostics(*context, selector_text);
  partial_upload_diagnostics(*context, selector_text);
  stop_token_cancels_and_drains(*context, device, selector_text);
  device.finish();
}

} // namespace

int main() {
  try {
    run_contract();
    std::cout << "PASS: C++23 primitives match the scripted TCP trace\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
