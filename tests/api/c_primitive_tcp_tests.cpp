// SPDX-License-Identifier: MIT
#include <kairosboot/kairosboot.hpp>

#include "src/transport/tcp_fastboot.hpp"

#include <boost/asio.hpp>

#include <array>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <initializer_list>
#include <iostream>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
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
      constexpr std::array<const char*, 13> management_commands{
          "flashing lock",
          "flashing unlock",
          "flashing lock_critical",
          "flashing unlock_critical",
          "flashing get_unlock_ability",
          "gsi:wipe",
          "gsi:disable",
          "gsi:status",
          "snapshot-update:cancel",
          "snapshot-update:merge",
          "create-logical-partition:system_ext:0",
          "delete-logical-partition:system_ext",
          "resize-logical-partition:system_ext:18446744073709551615",
      };
      for (std::size_t index = 0; index < management_commands.size(); ++index) {
        auto socket = accept();
        CHECK(as_string(read_frame(socket)) == management_commands[index]);
        if (index == 0) {
          write_frame(socket, std::string("INFOone\0two", 11));
          write_frame(socket, std::string("TEXThuman\0text", 14));
          write_frame(socket, std::string("OKAYdone\0x", 10));
        } else {
          write_frame(socket, "OKAYdone");
        }
      }
      constexpr std::array<const char*, 4> rejected_commands{
          "flashing unlock",
          "gsi:status",
          "snapshot-update:merge",
          "delete-logical-partition:system_ext",
      };
      for (const auto* command : rejected_commands) {
        auto socket = accept();
        CHECK(as_string(read_frame(socket)) == command);
        write_frame(socket, "INFOpolicy");
        write_frame(socket, std::string("FAILdenied\0x", 12));
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

class CxxScriptedServer final {
public:
  CxxScriptedServer()
      : acceptor_(context_, tcp::endpoint(tcp::v4(), 0)),
        port_(acceptor_.local_endpoint().port()),
        worker_([this] { run(); }) {}

  ~CxxScriptedServer() {
    if (worker_.joinable()) {
      {
        std::scoped_lock lock(mutex_);
        release_delayed_ = true;
      }
      condition_.notify_all();
      boost::system::error_code ignored;
      acceptor_.close(ignored);
      worker_.join();
    }
  }

  [[nodiscard]] std::uint16_t port() const noexcept { return port_; }

  void wait_for_delayed_command() {
    std::unique_lock lock(mutex_);
    condition_.wait(lock, [this] { return delayed_ready_ || failure_; });
    rethrow_failure();
  }

  void release_delayed_command() {
    {
      std::scoped_lock lock(mutex_);
      release_delayed_ = true;
    }
    condition_.notify_all();
  }

  void wait_for_cancel_command() {
    std::unique_lock lock(mutex_);
    condition_.wait(lock, [this] { return cancel_ready_ || failure_; });
    rethrow_failure();
  }

  void finish() {
    worker_.join();
    rethrow_failure();
  }

private:
  void rethrow_failure() const {
    if (failure_ != nullptr) {
      std::rethrow_exception(failure_);
    }
  }

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

  static std::string binary_frame(
      const std::string_view prefix,
      const std::initializer_list<unsigned char> bytes) {
    std::string result{prefix};
    for (const auto byte : bytes) {
      result.push_back(static_cast<char>(byte));
    }
    return result;
  }

  void run() noexcept {
    try {
      {
        auto socket = accept();
        CHECK(as_string(read_frame(socket)) == "getvar:binary");
        write_frame(socket, binary_frame("INFO", {'i', 0, 0xff}));
        write_frame(socket, binary_frame("TEXT", {'t', 0, 0xff}));
        write_frame(socket, binary_frame("OKAY", {'v', 0, 0xff}));
      }
      {
        auto socket = accept();
        CHECK(as_string(read_frame(socket)) == "erase:userdata");
        write_frame(socket, binary_frame("INFO", {'w', 0, 0xff}));
        write_frame(socket, binary_frame("FAIL", {'e', 0, 0xff}));
      }
      {
        auto socket = accept();
        CHECK(as_string(read_frame(socket)) == "getvar:delayed");
        {
          std::unique_lock lock(mutex_);
          delayed_ready_ = true;
          condition_.notify_all();
          condition_.wait(lock, [this] { return release_delayed_; });
        }
        write_frame(socket, "OKAYready");
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
        write_frame(socket, "OKAYstaged");
      }
      {
        auto socket = accept();
        CHECK(as_string(read_frame(socket)) == "upload");
        write_frame(socket, "DATA00000003");
        write_frame(socket, binary_frame("", {'d', 0, 0xff}));
        write_frame(socket, "OKAYuploaded");
      }
      {
        auto socket = accept();
        CHECK(as_string(read_frame(socket)) ==
              "fetch:vendor:0x00000002:0x00000003");
        write_frame(socket, "DATA00000003");
        write_frame(socket, binary_frame("", {'f', 0, 0xff}));
        write_frame(socket, "OKAYfetched");
      }
      {
        auto socket = accept();
        CHECK(as_string(read_frame(socket)) == "upload");
        write_frame(socket, "DATA00000020");
      }
      {
        auto socket = accept();
        CHECK(as_string(read_frame(socket)) == "getvar:cancel");
        {
          std::scoped_lock lock(mutex_);
          cancel_ready_ = true;
        }
        condition_.notify_all();
        std::array<std::byte, 1> byte{};
        boost::system::error_code closed;
        static_cast<void>(socket.read_some(boost::asio::buffer(byte), closed));
        CHECK(closed);
      }
    } catch (...) {
      {
        std::scoped_lock lock(mutex_);
        failure_ = std::current_exception();
      }
      condition_.notify_all();
    }
  }

  boost::asio::io_context context_;
  tcp::acceptor acceptor_;
  std::uint16_t port_;
  std::thread worker_;
  mutable std::mutex mutex_;
  std::condition_variable condition_;
  bool delayed_ready_{false};
  bool release_delayed_{false};
  bool cancel_ready_{false};
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
  error = nullptr;

  kb_operation_t* management_operation = nullptr;
  CHECK(kb_flashing_async(context, selector.c_str(), KB_FLASHING_LOCK,
                          nullptr, &management_operation, &error) == KB_OK);
  CHECK(management_operation != nullptr);
  CHECK(error == nullptr);
  CHECK(kb_operation_wait(management_operation, KB_WAIT_INFINITE) == KB_OK);
  CHECK(kb_operation_command_result(management_operation, &result, &error) ==
        KB_OK);
  kb_operation_release(management_operation);
  CHECK(result != nullptr);
  terminal = kb_command_result_terminal_payload(result, &size);
  CHECK(size == 6U);
  CHECK(std::memcmp(terminal, "done\0x", size) == 0);
  CHECK(kb_command_result_message_count(result) == 2U);
  first = kb_command_result_message_payload(result, 0, &size);
  CHECK(size == 7U);
  CHECK(std::memcmp(first, "one\0two", size) == 0);
  CHECK(kb_command_result_message_kind(result, 0) == KB_COMMAND_MESSAGE_INFO);
  CHECK(kb_command_result_message_kind(result, 1) == KB_COMMAND_MESSAGE_TEXT);
  kb_command_result_release(result);
  result = nullptr;

  const auto check_management_success = [&](const kb_status_t status) {
    CHECK(status == KB_OK);
    CHECK(result != nullptr);
    CHECK(error == nullptr);
    CHECK(std::strcmp(kb_command_result_device_identifier(result),
                      selector.c_str()) == 0);
    kb_command_result_release(result);
    result = nullptr;
  };
  check_management_success(kb_flashing(
      context, selector.c_str(), KB_FLASHING_UNLOCK, nullptr, &result, &error));
  check_management_success(kb_flashing(
      context, selector.c_str(), KB_FLASHING_LOCK_CRITICAL, nullptr, &result,
      &error));
  check_management_success(kb_flashing(
      context, selector.c_str(), KB_FLASHING_UNLOCK_CRITICAL, nullptr, &result,
      &error));
  check_management_success(kb_flashing(
      context, selector.c_str(), KB_FLASHING_GET_UNLOCK_ABILITY, nullptr,
      &result, &error));
  check_management_success(kb_gsi(
      context, selector.c_str(), KB_GSI_WIPE, nullptr, &result, &error));
  check_management_success(kb_gsi(
      context, selector.c_str(), KB_GSI_DISABLE, nullptr, &result, &error));
  check_management_success(kb_gsi(
      context, selector.c_str(), KB_GSI_STATUS, nullptr, &result, &error));
  check_management_success(kb_snapshot_update(
      context, selector.c_str(), KB_SNAPSHOT_UPDATE_CANCEL, nullptr, &result,
      &error));
  check_management_success(kb_snapshot_update(
      context, selector.c_str(), KB_SNAPSHOT_UPDATE_MERGE, nullptr, &result,
      &error));
  check_management_success(kb_create_logical_partition(
      context, selector.c_str(), "system_ext", 0, nullptr, &result, &error));
  check_management_success(kb_delete_logical_partition(
      context, selector.c_str(), "system_ext", nullptr, &result, &error));
  check_management_success(kb_resize_logical_partition(
      context, selector.c_str(), "system_ext", UINT64_MAX, nullptr, &result,
      &error));

  const auto check_management_failure = [&](const kb_status_t status) {
    CHECK(status == KB_E_DEVICE_FAIL);
    CHECK(result == nullptr);
    CHECK(error != nullptr);
    CHECK(kb_error_status(error) == KB_E_DEVICE_FAIL);
    const auto* message = kb_error_device_message(error, &size);
    CHECK(size == 8U);
    CHECK(std::memcmp(message, "denied\0x", size) == 0);
    CHECK(kb_error_command_message_count(error) == 1U);
    CHECK(kb_error_command_message_kind(error, 0) == KB_COMMAND_MESSAGE_INFO);
    CHECK(kb_error_session_poisoned(error) == 0);
    kb_error_release(error);
    error = nullptr;
  };
  check_management_failure(kb_flashing(
      context, selector.c_str(), KB_FLASHING_UNLOCK, nullptr, &result, &error));
  check_management_failure(kb_gsi(
      context, selector.c_str(), KB_GSI_STATUS, nullptr, &result, &error));
  check_management_failure(kb_snapshot_update(
      context, selector.c_str(), KB_SNAPSHOT_UPDATE_MERGE, nullptr, &result,
      &error));
  check_management_failure(kb_delete_logical_partition(
      context, selector.c_str(), "system_ext", nullptr, &result, &error));

  kb_context_release(context);
  server.finish();
}

void run_cxx_contract() {
  CxxScriptedServer server;
  const auto selector_text =
      "tcp:127.0.0.1:" + std::to_string(server.port());
  const kairosboot::DeviceSelector selector{std::string_view{selector_text}};
  auto context = kairosboot::Context::create();
  CHECK(context.has_value());

  auto binary = context->getvar(selector, "binary");
  CHECK(binary.has_value());
  CHECK(binary->device_identifier() == selector_text);
  CHECK(binary->terminal_payload().size() == 3U);
  CHECK(binary->terminal_payload()[0] == std::byte{'v'});
  CHECK(binary->terminal_payload()[1] == std::byte{0});
  CHECK(binary->terminal_payload()[2] == std::byte{0xff});
  CHECK(binary->message_count() == 2U);
  const auto info = binary->message(0);
  CHECK(info.has_value());
  CHECK(info->kind == kairosboot::CommandMessageKind::Info);
  CHECK(info->payload.size() == 3U);
  CHECK(info->payload[1] == std::byte{0});
  CHECK(info->payload[2] == std::byte{0xff});
  const auto text = binary->message(1);
  CHECK(text.has_value());
  CHECK(text->kind == kairosboot::CommandMessageKind::Text);
  CHECK(!binary->message(2).has_value());

  auto failed = context->erase(selector, "userdata");
  CHECK(!failed.has_value());
  CHECK(failed.error().status() == KB_E_DEVICE_FAIL);
  CHECK(failed.error().device_message().size() == 3U);
  CHECK(failed.error().device_message()[0] == std::byte{'e'});
  CHECK(failed.error().device_message()[1] == std::byte{0});
  CHECK(failed.error().device_message()[2] == std::byte{0xff});
  CHECK(failed.error().command_messages().size() == 1U);
  CHECK(failed.error().command_messages()[0].payload.size() == 3U);
  CHECK(failed.error().command_messages()[0].payload[2] == std::byte{0xff});

  std::optional<kairosboot::CommandResult> retained;
  {
    auto operation = context->getvar_async(selector, "delayed");
    CHECK(operation.has_value());
    server.wait_for_delayed_command();
    auto premature = operation->command_result();
    CHECK(!premature.has_value());
    CHECK(premature.error().status() == KB_E_BUSY);
    server.release_delayed_command();
    auto completed = operation->wait_result();
    CHECK(completed.has_value());
    retained.emplace(std::move(*completed));
  }
  CHECK(retained.has_value());
  CHECK(as_string(retained->terminal_payload()) == "ready");
  CHECK(retained->device_identifier() == selector_text);

  std::vector<std::uint64_t> stage_watermarks;
  kairosboot::CommandOptions stage_options;
  stage_options.progress = [&](const kairosboot::Progress& progress) {
    CHECK(progress.device_identifier == selector_text);
    CHECK(progress.stage == "stage");
    stage_watermarks.push_back(progress.bytes_completed);
    return kairosboot::ProgressAction::Continue;
  };
  std::vector<std::byte> stage_data(16U);
  for (std::size_t index = 0; index < stage_data.size(); ++index) {
    stage_data[index] = std::byte{static_cast<unsigned char>(index)};
  }
  auto stage_operation = context->stage_async(selector, stage_data,
                                               stage_options);
  CHECK(stage_operation.has_value());
  stage_data.clear();
  stage_data.shrink_to_fit();
  auto staged = stage_operation->wait_result();
  CHECK(staged.has_value());
  CHECK(as_string(staged->terminal_payload()) == "staged");
  CHECK(stage_watermarks == std::vector<std::uint64_t>({0, 16}));

  kairosboot::CommandOptions receive_options;
  receive_options.maximum_receive_bytes = 3;
  auto uploaded = context->upload(selector, receive_options);
  CHECK(uploaded.has_value());
  CHECK(uploaded->data().size() == 3U);
  CHECK(uploaded->data()[0] == std::byte{'d'});
  CHECK(uploaded->data()[1] == std::byte{0});
  CHECK(uploaded->data()[2] == std::byte{0xff});

  auto fetched = context->fetch(
      selector, "vendor", kairosboot::FetchRange{.offset = 2, .size = 3},
      receive_options);
  CHECK(fetched.has_value());
  CHECK(fetched->data().size() == 3U);
  CHECK(fetched->data()[0] == std::byte{'f'});
  CHECK(fetched->data()[1] == std::byte{0});
  CHECK(fetched->data()[2] == std::byte{0xff});

  receive_options.maximum_receive_bytes = 16;
  auto oversized = context->upload(selector, receive_options);
  CHECK(!oversized.has_value());
  CHECK(oversized.error().status() == KB_E_PROTOCOL);
  CHECK(oversized.error().inbound_expected_bytes() == 32U);
  CHECK(oversized.error().inbound_transferred_bytes() == 0U);
  CHECK(oversized.error().inbound_transfer_state() ==
        KB_TRANSFER_PARTIAL_OR_UNKNOWN);
  CHECK(oversized.error().session_poisoned());

  auto cancelled_operation = context->getvar_async(selector, "cancel");
  CHECK(cancelled_operation.has_value());
  server.wait_for_cancel_command();
  std::stop_source cancellation;
  CHECK(cancellation.request_stop());
  auto cancelled =
      cancelled_operation->wait_result(cancellation.get_token());
  CHECK(!cancelled.has_value());
  CHECK(cancelled.error().status() == KB_E_CANCELLED);
  CHECK(cancelled_operation->state() == KB_OPERATION_CANCELLED);

  server.finish();
}

}  // namespace

int main() {
  try {
    run_contract();
    run_cxx_contract();
    std::cout << "PASS: typed C and C++ primitives over Fastboot TCP\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
