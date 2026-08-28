// SPDX-License-Identifier: MIT
#include <kairosboot/kairosboot.hpp>

#include "src/transport/tcp_fastboot.hpp"
#include "src/transport/udp_fastboot.hpp"

#include <boost/asio.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
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
using boost::asio::ip::udp;

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

struct ErrorSnapshot {
  kb_status_t status{KB_OK};
  std::string message;
  std::string device_identifier;
  int32_t native_code{0};
  kb_transfer_state_t transfer_state{KB_TRANSFER_NOT_SENT};
  std::string device_message;
  std::vector<kb_command_message_kind_t> message_kinds;
  std::vector<std::string> message_payloads;
  std::uint64_t inbound_expected_bytes{0};
  std::uint64_t inbound_transferred_bytes{0};
  kb_transfer_state_t inbound_transfer_state{KB_TRANSFER_NOT_SENT};
  int32_t session_poisoned{0};

  [[nodiscard]] bool operator==(const ErrorSnapshot&) const = default;
};

ErrorSnapshot snapshot_error(const kb_error_t* error) {
  CHECK(error != nullptr);
  ErrorSnapshot snapshot{
      .status = kb_error_status(error),
      .message = kb_error_message(error),
      .device_identifier = kb_error_device_identifier(error),
      .native_code = kb_error_native_code(error),
      .transfer_state = kb_error_transfer_state(error),
      .device_message = {},
      .message_kinds = {},
      .message_payloads = {},
      .inbound_expected_bytes = kb_error_inbound_expected_bytes(error),
      .inbound_transferred_bytes = kb_error_inbound_transferred_bytes(error),
      .inbound_transfer_state = kb_error_inbound_transfer_state(error),
      .session_poisoned = kb_error_session_poisoned(error),
  };
  std::size_t size = 0;
  const auto* device_message = kb_error_device_message(error, &size);
  if (size != 0U) {
    snapshot.device_message.assign(
        reinterpret_cast<const char*>(device_message), size);
  }
  const auto message_count = kb_error_command_message_count(error);
  snapshot.message_kinds.reserve(message_count);
  snapshot.message_payloads.reserve(message_count);
  for (std::size_t index = 0; index < message_count; ++index) {
    snapshot.message_kinds.push_back(
        kb_error_command_message_kind(error, index));
    const auto* payload = kb_error_command_message_payload(error, index, &size);
    if (size == 0U) {
      snapshot.message_payloads.emplace_back();
    } else {
      snapshot.message_payloads.emplace_back(
          reinterpret_cast<const char*>(payload), size);
    }
  }
  return snapshot;
}

class TemporaryUpdatePackage final {
public:
  TemporaryUpdatePackage(const std::string_view fastboot_info,
                         const std::span<const std::byte> image = {}) {
    static std::uint64_t sequence = 0;
    const auto nonce = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    path_ = std::filesystem::temp_directory_path() /
            ("kairosboot-public-update-" + std::to_string(nonce) + "-" +
             std::to_string(++sequence));
    CHECK(std::filesystem::create_directory(path_));
    write(path_ / "android-info.txt", {});
    write(path_ / "fastboot-info.txt",
          std::as_bytes(std::span{fastboot_info.data(), fastboot_info.size()}));
    if (!image.empty()) {
      write(path_ / "system.img", image);
    }
  }

  ~TemporaryUpdatePackage() {
    std::error_code ignored;
    static_cast<void>(std::filesystem::remove_all(path_, ignored));
  }

  TemporaryUpdatePackage(const TemporaryUpdatePackage&) = delete;
  TemporaryUpdatePackage& operator=(const TemporaryUpdatePackage&) = delete;

  [[nodiscard]] const std::filesystem::path& path() const noexcept {
    return path_;
  }

private:
  static void write(const std::filesystem::path& path,
                    const std::span<const std::byte> bytes) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    CHECK(stream.is_open());
    if (!bytes.empty()) {
      stream.write(reinterpret_cast<const char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
    }
    CHECK(stream.good());
  }

  std::filesystem::path path_;
};

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

  static void serve_flash_preflight(tcp::socket& socket) {
    CHECK(as_string(read_frame(socket)) == "getvar:is-userspace");
    write_frame(socket, "OKAYno");
    CHECK(as_string(read_frame(socket)) == "getvar:has-slot:system");
    write_frame(socket, "OKAYno");
    CHECK(as_string(read_frame(socket)) == "getvar:is-logical:system");
    write_frame(socket, "OKAYno");
  }

  void serve_flash_success() {
    auto socket = accept();
    serve_flash_preflight(socket);
    CHECK(as_string(read_frame(socket)) == "getvar:max-download-size");
    write_frame(socket, "OKAY0x00100000");
    CHECK(as_string(read_frame(socket)) == "download:00000010");
    write_frame(socket, "DATA00000010");
    const auto payload = read_frame(socket);
    CHECK(payload.size() == 16U);
    for (std::size_t index = 0; index < payload.size(); ++index) {
      CHECK(payload[index] == std::byte{static_cast<unsigned char>(index)});
    }
    write_frame(socket, "OKAYdownloaded");
    CHECK(as_string(read_frame(socket)) == "flash:system");
    write_frame(socket, "OKAYflashed");
  }

  void serve_flash_failure() {
    auto socket = accept();
    serve_flash_preflight(socket);
    CHECK(as_string(read_frame(socket)) == "getvar:max-download-size");
    write_frame(socket, "OKAY0x00100000");
    CHECK(as_string(read_frame(socket)) == "download:00000010");
    write_frame(socket, "DATA00000010");
    const auto payload = read_frame(socket);
    CHECK(payload.size() == 16U);
    for (std::size_t index = 0; index < payload.size(); ++index) {
      CHECK(payload[index] == std::byte{static_cast<unsigned char>(index)});
    }
    write_frame(socket, "OKAYdownloaded");
    CHECK(as_string(read_frame(socket)) == "flash:system");
    write_frame(socket, "INFOpolicy");
    write_frame(socket, "TEXTlocked partition");
    write_frame(socket, "FAILpartition locked");
  }

  void serve_cancelled_flash() {
    auto socket = accept();
    serve_flash_preflight(socket);
    CHECK(as_string(read_frame(socket)) == "getvar:max-download-size");
    write_frame(socket, "OKAY0x00100000");
    std::array<std::byte, 1> trailing{};
    boost::system::error_code closed;
    static_cast<void>(socket.read_some(boost::asio::buffer(trailing), closed));
    CHECK(closed);
  }

  void serve_signature(const bool accepted) {
    auto socket = accept();
    CHECK(as_string(read_frame(socket)) == "download:00000100");
    write_frame(socket, "DATA00000100");
    const auto payload = read_frame(socket);
    CHECK(payload.size() == 256U);
    for (std::size_t index = 0; index < payload.size(); ++index) {
      CHECK(payload[index] == std::byte{static_cast<unsigned char>(index)});
    }
    write_frame(socket, "OKAYdownloaded");
    CHECK(as_string(read_frame(socket)) == "signature");
    write_frame(socket, "INFOsignature policy");
    write_frame(socket, accepted ? "OKAYaccepted" : "FAILsignature rejected");
  }

  void serve_cancelled_signature() {
    auto socket = accept();
    CHECK(as_string(read_frame(socket)) == "download:00000100");
    write_frame(socket, "DATA00000100");
    std::array<std::byte, 1> trailing{};
    boost::system::error_code closed;
    static_cast<void>(socket.read_some(boost::asio::buffer(trailing), closed));
    CHECK(closed);
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
      serve_signature(true);
      serve_signature(true);
      serve_signature(false);
      serve_cancelled_signature();
      serve_flash_success();
      serve_flash_success();
      serve_flash_failure();
      serve_flash_failure();
      serve_cancelled_flash();
      {
        auto socket = accept();
        CHECK(as_string(read_frame(socket)) == "getvar:max-download-size");
        write_frame(socket, "OKAY0x00100000");
        CHECK(as_string(read_frame(socket)) == "download:00000010");
        write_frame(socket, "DATA00000010");
        const auto payload = read_frame(socket);
        CHECK(payload.size() == 16U);
        for (std::size_t index = 0; index < payload.size(); ++index) {
          CHECK(payload[index] ==
                std::byte{static_cast<unsigned char>(index)});
        }
        write_frame(socket, "OKAYdownloaded");
        CHECK(as_string(read_frame(socket)) == "flash:system");
        write_frame(socket, "OKAYflashed");
      }
      {
        auto socket = accept();
        CHECK(as_string(read_frame(socket)) == "getvar:is-userspace");
        write_frame(socket, "FAILvariable not found");
      }
      {
        auto socket = accept();
        CHECK(as_string(read_frame(socket)) == "getvar:max-download-size");
        write_frame(socket, "OKAY0x00100000");
        CHECK(as_string(read_frame(socket)) == "download:00000010");
        write_frame(socket, "DATA00000010");
        CHECK(read_frame(socket).size() == 16U);
        write_frame(socket, "OKAYdownloaded");
        CHECK(as_string(read_frame(socket)) == "flash:system");
        write_frame(socket, "INFOpolicy");
        write_frame(socket, "FAILpartition locked");
      }
      {
        auto socket = accept();
        CHECK(as_string(read_frame(socket)) == "erase:userdata");
        write_frame(socket, "OKAYwiped");
      }
      {
        auto socket = accept();
        CHECK(as_string(read_frame(socket)) == "erase:cache");
        std::this_thread::sleep_for(std::chrono::milliseconds{600});
        write_frame(socket, "OKAYerased cache");
        CHECK(as_string(read_frame(socket)) == "erase:metadata");
        std::this_thread::sleep_for(std::chrono::milliseconds{1000});
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

class FlashRawServer final {
public:
  FlashRawServer()
      : acceptor_(context_, tcp::endpoint(tcp::v4(), 0)),
        port_(acceptor_.local_endpoint().port()),
        worker_([this] { run(); }) {}

  ~FlashRawServer() {
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
  void run() noexcept {
    try {
      tcp::socket socket(context_);
      acceptor_.accept(socket);
      std::array<char, 4> handshake{};
      boost::asio::read(socket, boost::asio::buffer(handshake));
      CHECK((handshake == std::array<char, 4>{'F', 'B', '0', '1'}));
      boost::asio::write(socket, boost::asio::buffer(handshake));

      CHECK(as_string(read_frame(socket)) == "getvar:is-userspace");
      write_frame(socket, "OKAYno");
      CHECK(as_string(read_frame(socket)) == "getvar:has-slot:boot");
      write_frame(socket, "OKAYno");
      CHECK(as_string(read_frame(socket)) == "getvar:is-logical:boot");
      write_frame(socket, "OKAYno");
      CHECK(as_string(read_frame(socket)) == "getvar:max-download-size");
      write_frame(socket, "OKAY0x00100000");
      CHECK(as_string(read_frame(socket)) == "download:00002000");
      write_frame(socket, "DATA00002000");
      const auto image = read_frame(socket);
      CHECK(image.size() == 8192U);
      CHECK(std::memcmp(image.data(), "ANDROID!", 8U) == 0);
      const auto le32 = [&image](const std::size_t offset) {
        std::uint32_t result = 0;
        for (std::size_t byte = 0; byte < 4U; ++byte) {
          result |= std::to_integer<std::uint32_t>(image[offset + byte])
                    << (byte * 8U);
        }
        return result;
      };
      CHECK(le32(8U) == 2048U);
      CHECK(le32(12U) == 0x10008000U);
      CHECK(le32(16U) == 3U);
      CHECK(le32(20U) == 0x11000000U);
      CHECK(le32(24U) == 0U);
      CHECK(le32(32U) == 0x10000100U);
      CHECK(le32(36U) == 2048U);
      CHECK(le32(40U) == 2U);
      CHECK(le32(44U) ==
            ((15U << 25U) | (1U << 18U) | (25U << 4U) | 2U));
      CHECK(le32(1644U) == 1660U);
      CHECK(le32(1648U) == 3U);
      CHECK(le32(1652U) == 0x11200000U);
      CHECK(le32(1656U) == 0U);
      for (std::size_t index = 0; index < 2048U; ++index) {
        CHECK(image[2048U + index] ==
              std::byte{static_cast<unsigned char>(index & 0xffU)});
      }
      CHECK(image[4096U] == std::byte{'r'});
      CHECK(image[4097U] == std::byte{0});
      CHECK(image[4098U] == std::byte{'d'});
      CHECK(image[6144U] == std::byte{'d'});
      CHECK(image[6145U] == std::byte{'t'});
      CHECK(image[6146U] == std::byte{'b'});
      write_frame(socket, "OKAYdownloaded");
      CHECK(as_string(read_frame(socket)) == "flash:boot");
      write_frame(socket, "OKAYflashed");
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

  static void serve_flash_preflight(tcp::socket& socket) {
    CHECK(as_string(read_frame(socket)) == "getvar:is-userspace");
    write_frame(socket, "OKAYno");
    CHECK(as_string(read_frame(socket)) == "getvar:has-slot:system");
    write_frame(socket, "OKAYno");
    CHECK(as_string(read_frame(socket)) == "getvar:is-logical:system");
    write_frame(socket, "OKAYno");
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

  void serve_flash_success() {
    auto socket = accept();
    serve_flash_preflight(socket);
    CHECK(as_string(read_frame(socket)) == "getvar:max-download-size");
    write_frame(socket, "OKAY0x00100000");
    CHECK(as_string(read_frame(socket)) == "download:00000010");
    write_frame(socket, "DATA00000010");
    const auto payload = read_frame(socket);
    CHECK(payload.size() == 16U);
    for (std::size_t index = 0; index < payload.size(); ++index) {
      CHECK(payload[index] == std::byte{static_cast<unsigned char>(index)});
    }
    write_frame(socket, "OKAYdownloaded");
    CHECK(as_string(read_frame(socket)) == "flash:system");
    write_frame(socket, "OKAYflashed");
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
      serve_flash_success();
      serve_flash_success();
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

std::vector<std::byte> udp_packet(
    const kairosboot::transport::UdpPacketId id,
    const std::uint16_t sequence,
    const std::span<const std::byte> payload = {}) {
  const auto header = kairosboot::transport::encode_udp_header({
      .id = id,
      .sequence = sequence,
  });
  std::vector<std::byte> result(header.begin(), header.end());
  result.insert(result.end(), payload.begin(), payload.end());
  return result;
}

std::vector<std::byte> udp_bytes(const std::string_view text) {
  return std::vector<std::byte>(
      reinterpret_cast<const std::byte*>(text.data()),
      reinterpret_cast<const std::byte*>(text.data() + text.size()));
}

std::array<std::byte, 2> udp_be16(const std::uint16_t value) {
  return {
      static_cast<std::byte>((value >> 8U) & 0xffU),
      static_cast<std::byte>(value & 0xffU),
  };
}

std::array<std::byte, 4> udp_initialization(
    const std::uint16_t packet_bytes) {
  return {
      std::byte{0},
      std::byte{1},
      static_cast<std::byte>((packet_bytes >> 8U) & 0xffU),
      static_cast<std::byte>(packet_bytes & 0xffU),
  };
}

class ScriptedUdpFlashServer final {
public:
  explicit ScriptedUdpFlashServer(std::vector<bool> failure_sequence)
      : socket_(context_, udp::endpoint(udp::v4(), 0)),
        port_(socket_.local_endpoint().port()),
        failure_sequence_(std::move(failure_sequence)),
        worker_([this] { run(); }) {}

  ~ScriptedUdpFlashServer() {
    if (worker_.joinable()) {
      boost::system::error_code ignored;
      socket_.cancel(ignored);
      socket_.close(ignored);
      worker_.join();
    }
  }

  [[nodiscard]] std::uint16_t port() const noexcept { return port_; }

  void finish() {
    worker_.join();
    if (failure_) {
      std::rethrow_exception(failure_);
    }
  }

private:
  [[nodiscard]] udp::endpoint receive(
      const std::span<const std::byte> expected,
      const std::optional<udp::endpoint>& expected_peer = std::nullopt) {
    std::array<std::byte, 8192> buffer{};
    udp::endpoint peer;
    const auto received = socket_.receive_from(boost::asio::buffer(buffer), peer);
    CHECK(received == expected.size());
    CHECK(std::ranges::equal(
        std::span<const std::byte>{buffer}.first(received), expected));
    if (expected_peer.has_value()) {
      CHECK(peer == *expected_peer);
    }
    return peer;
  }

  void send(const std::span<const std::byte> packet,
            const udp::endpoint& peer) {
    CHECK(socket_.send_to(boost::asio::buffer(packet), peer) == packet.size());
  }

  void serve_flash(const bool fail) {
    using kairosboot::transport::UdpPacketId;
    const auto query = udp_packet(UdpPacketId::Query, 0);
    const auto peer = receive(query);
    const auto starting_sequence = udp_be16(100);
    send(udp_packet(UdpPacketId::Query, 0, starting_sequence), peer);

    const auto host_initialization = udp_initialization(8192);
    static_cast<void>(receive(
        udp_packet(UdpPacketId::Initialization, 100, host_initialization),
        peer));
    const auto target_initialization = udp_initialization(512);
    send(udp_packet(UdpPacketId::Initialization, 100,
                    target_initialization),
         peer);

    std::uint16_t sequence = 101;
    const auto send_request = [&](const std::span<const std::byte> request) {
      static_cast<void>(receive(
          udp_packet(UdpPacketId::Fastboot, sequence, request), peer));
      send(udp_packet(UdpPacketId::Fastboot, sequence), peer);
      ++sequence;
    };
    const auto respond = [&](const std::span<const std::byte> response) {
      static_cast<void>(
          receive(udp_packet(UdpPacketId::Fastboot, sequence), peer));
      send(udp_packet(UdpPacketId::Fastboot, sequence, response), peer);
      ++sequence;
    };
    const auto exchange = [&](const std::span<const std::byte> request,
                              const std::span<const std::byte> response) {
      send_request(request);
      respond(response);
    };

    exchange(udp_bytes("getvar:is-userspace"), udp_bytes("OKAYno"));
    exchange(udp_bytes("getvar:has-slot:system"), udp_bytes("OKAYno"));
    exchange(udp_bytes("getvar:is-logical:system"), udp_bytes("OKAYno"));
    exchange(udp_bytes("getvar:max-download-size"),
             udp_bytes("OKAY0x00100000"));
    exchange(udp_bytes("download:00000010"), udp_bytes("DATA00000010"));
    std::array<std::byte, 16> image{};
    for (std::size_t index = 0; index < image.size(); ++index) {
      image[index] = std::byte{static_cast<unsigned char>(index)};
    }
    exchange(image, udp_bytes("OKAYdownloaded"));
    send_request(udp_bytes("flash:system"));
    if (fail) {
      respond(udp_bytes("INFOpolicy"));
      respond(udp_bytes("TEXTlocked partition"));
      respond(udp_bytes("FAILpartition locked"));
    } else {
      respond(udp_bytes("OKAYflashed"));
    }
  }

  void run() noexcept {
    try {
      for (const auto fail : failure_sequence_) {
        serve_flash(fail);
      }
    } catch (...) {
      failure_ = std::current_exception();
    }
  }

  boost::asio::io_context context_;
  udp::socket socket_;
  std::uint16_t port_;
  std::vector<bool> failure_sequence_;
  std::thread worker_;
  std::exception_ptr failure_;
};

std::uint16_t unavailable_tcp_port() {
  boost::asio::io_context context;
  tcp::acceptor acceptor(context, tcp::endpoint(tcp::v4(), 0));
  const auto port = acceptor.local_endpoint().port();
  acceptor.close();
  return port;
}

std::uint16_t unavailable_udp_port() {
  boost::asio::io_context context;
  udp::socket socket(context, udp::endpoint(udp::v4(), 0));
  const auto port = socket.local_endpoint().port();
  socket.close();
  return port;
}

kb_progress_action_t KB_CALL record_progress(
    const kb_progress_t* progress, void* user_data) {
  auto& watermarks = *static_cast<std::vector<std::uint64_t>*>(user_data);
  watermarks.push_back(progress->bytes_completed);
  return KB_PROGRESS_CONTINUE;
}

kb_progress_action_t KB_CALL cancel_flash_at_download(
    const kb_progress_t* progress, void*) {
  return progress != nullptr && progress->stage != nullptr &&
                 std::string_view{progress->stage} == "download"
             ? KB_PROGRESS_CANCEL
             : KB_PROGRESS_CONTINUE;
}

struct CancelOnTaskFailureProbe final {
  std::size_t execute_callbacks{};
};

kb_progress_action_t KB_CALL cancel_on_second_execute(
    const kb_progress_t* progress, void* user_data) {
  auto& probe = *static_cast<CancelOnTaskFailureProbe*>(user_data);
  if (progress != nullptr && progress->stage != nullptr &&
      std::string_view{progress->stage} == "execute" &&
      ++probe.execute_callbacks == 2U) {
    return KB_PROGRESS_CANCEL;
  }
  return KB_PROGRESS_CONTINUE;
}

struct ReleaseContextProbe final {
  kb_context_t* context{};
  std::atomic<bool> released{false};
};

kb_progress_action_t KB_CALL release_context_during_preflight(
    const kb_progress_t* progress, void* user_data) {
  auto& probe = *static_cast<ReleaseContextProbe*>(user_data);
  if (progress != nullptr && progress->stage != nullptr &&
      std::string_view{progress->stage} == "preflight" &&
      !probe.released.exchange(true, std::memory_order_acq_rel)) {
    kb_context_release(probe.context);
  }
  return KB_PROGRESS_CONTINUE;
}

struct DelayOpenProbe final {
  bool delayed{};
};

kb_progress_action_t KB_CALL delay_transport_open(
    const kb_progress_t* progress, void* user_data) {
  auto& probe = *static_cast<DelayOpenProbe*>(user_data);
  if (progress != nullptr && progress->stage != nullptr &&
      std::string_view{progress->stage} == "open" && !probe.delayed) {
    probe.delayed = true;
    std::this_thread::sleep_for(std::chrono::milliseconds{600});
  }
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

  std::array<std::byte, 16> update_image{};
  for (std::size_t index = 0; index < update_image.size(); ++index) {
    update_image[index] = std::byte{static_cast<unsigned char>(index)};
  }
  TemporaryUpdatePackage update_package(
      "version 1\nflash system system.img\n", update_image);
  const auto image_path = (update_package.path() / "system.img").string();

  std::array<std::byte, 256> signature_bytes{};
  for (std::size_t index = 0; index < signature_bytes.size(); ++index) {
    signature_bytes[index] =
        std::byte{static_cast<unsigned char>(index)};
  }
  TemporaryUpdatePackage signature_package("version 1\n", signature_bytes);
  const auto signature_path =
      (signature_package.path() / "system.img").string();

  kb_operation_t* signature_operation = nullptr;
  CHECK(kb_signature_file_async(
            context, selector.c_str(), image_path.c_str(), nullptr,
            &signature_operation, &error) == KB_E_INVALID_ARGUMENT);
  CHECK(signature_operation == nullptr);
  CHECK(error != nullptr);
  CHECK(kb_error_transfer_state(error) == KB_TRANSFER_NOT_SENT);
  kb_error_release(error);
  error = nullptr;

  kb_command_options_init(&options);
  watermarks.clear();
  options.progress_callback = record_progress;
  options.progress_user_data = &watermarks;
  result = nullptr;
  CHECK(kb_signature_file(context, selector.c_str(), signature_path.c_str(),
                          &options, &result, &error) == KB_OK);
  CHECK(result != nullptr);
  CHECK(error == nullptr);
  terminal = kb_command_result_terminal_payload(result, &size);
  CHECK(std::string(reinterpret_cast<const char*>(terminal), size) ==
        "accepted");
  CHECK(kb_command_result_message_count(result) == 1U);
  CHECK(watermarks == std::vector<std::uint64_t>({0U, 256U}));
  kb_command_result_release(result);
  result = nullptr;

  signature_operation = nullptr;
  CHECK(kb_signature_file_async(
            context, selector.c_str(), signature_path.c_str(), &options,
            &signature_operation, &error) == KB_OK);
  CHECK(signature_operation != nullptr);
  CHECK(kb_operation_wait(signature_operation, KB_WAIT_INFINITE) == KB_OK);
  CHECK(kb_operation_command_result(signature_operation, &result, &error) ==
        KB_OK);
  kb_operation_release(signature_operation);
  kb_command_result_release(result);
  result = nullptr;

  CHECK(kb_signature_file(context, selector.c_str(), signature_path.c_str(),
                          &options, &result, &error) == KB_E_DEVICE_FAIL);
  CHECK(result == nullptr);
  CHECK(error != nullptr);
  CHECK(kb_error_transfer_state(error) == KB_TRANSFER_FULLY_TRANSFERRED);
  CHECK(kb_error_session_poisoned(error) == 0);
  CHECK(kb_error_command_message_count(error) == 1U);
  kb_error_release(error);
  error = nullptr;

  kb_command_options_init(&options);
  options.progress_callback = cancel_flash_at_download;
  signature_operation = nullptr;
  CHECK(kb_signature_file_async(
            context, selector.c_str(), signature_path.c_str(), &options,
            &signature_operation, &error) == KB_OK);
  CHECK(kb_operation_wait(signature_operation, KB_WAIT_INFINITE) ==
        KB_E_CANCELLED);
  CHECK(kb_error_transfer_state(kb_operation_error(signature_operation)) ==
        KB_TRANSFER_NOT_SENT);
  kb_operation_release(signature_operation);

  kb_flash_options_t flash_options;
  kb_flash_options_init(&flash_options);
  std::vector<std::uint64_t> flash_watermarks;
  flash_options.progress_callback = record_progress;
  flash_options.progress_user_data = &flash_watermarks;
  kb_operation_t* flash_operation = nullptr;
  CHECK(kb_flash_file_async(
            context, selector.c_str(), "system", image_path.c_str(),
            &flash_options, &flash_operation, &error) == KB_OK);
  CHECK(flash_operation != nullptr);
  CHECK(error == nullptr);
  CHECK(kb_operation_wait(flash_operation, KB_WAIT_INFINITE) == KB_OK);
  CHECK(kb_operation_state(flash_operation) == KB_OPERATION_SUCCEEDED);
  CHECK(kb_operation_error(flash_operation) == nullptr);
  kb_operation_release(flash_operation);
  CHECK(flash_watermarks ==
        std::vector<std::uint64_t>({0U, 0U, 16U, 16U}));

  flash_watermarks.clear();
  CHECK(kb_flash_file(context, selector.c_str(), "system", image_path.c_str(),
                      &flash_options, &error) == KB_OK);
  CHECK(error == nullptr);
  CHECK(flash_watermarks ==
        std::vector<std::uint64_t>({0U, 0U, 16U, 16U}));

  kb_flash_options_init(&flash_options);
  flash_operation = nullptr;
  CHECK(kb_flash_file_async(
            context, selector.c_str(), "system", image_path.c_str(),
            &flash_options, &flash_operation, &error) == KB_OK);
  CHECK(flash_operation != nullptr);
  CHECK(error == nullptr);
  CHECK(kb_operation_wait(flash_operation, KB_WAIT_INFINITE) ==
        KB_E_DEVICE_FAIL);
  CHECK(kb_operation_state(flash_operation) == KB_OPERATION_FAILED);
  const auto asynchronous_flash_error =
      snapshot_error(kb_operation_error(flash_operation));
  kb_operation_release(flash_operation);
  CHECK(asynchronous_flash_error.device_message == "partition locked");
  CHECK(asynchronous_flash_error.message_kinds ==
        std::vector<kb_command_message_kind_t>(
            {KB_COMMAND_MESSAGE_INFO, KB_COMMAND_MESSAGE_TEXT}));
  CHECK(asynchronous_flash_error.message_payloads ==
        std::vector<std::string>({"policy", "locked partition"}));
  CHECK(asynchronous_flash_error.session_poisoned == 0);

  CHECK(kb_flash_file(context, selector.c_str(), "system", image_path.c_str(),
                      &flash_options, &error) == KB_E_DEVICE_FAIL);
  const auto blocking_flash_error = snapshot_error(error);
  kb_error_release(error);
  error = nullptr;
  CHECK(blocking_flash_error == asynchronous_flash_error);

  kb_flash_options_init(&flash_options);
  flash_options.progress_callback = cancel_flash_at_download;
  flash_operation = nullptr;
  CHECK(kb_flash_file_async(
            context, selector.c_str(), "system", image_path.c_str(),
            &flash_options, &flash_operation, &error) == KB_OK);
  CHECK(flash_operation != nullptr);
  CHECK(kb_operation_wait(flash_operation, KB_WAIT_INFINITE) ==
        KB_E_CANCELLED);
  const auto* cancelled_flash = kb_operation_error(flash_operation);
  CHECK(cancelled_flash != nullptr);
  CHECK(kb_error_status(cancelled_flash) == KB_E_CANCELLED);
  CHECK(kb_error_transfer_state(cancelled_flash) == KB_TRANSFER_NOT_SENT);
  CHECK(std::strcmp(kb_error_device_identifier(cancelled_flash),
                    selector.c_str()) == 0);
  kb_operation_release(flash_operation);

  CHECK(kb_flash_file_async(context, "tcp:", "system", image_path.c_str(),
                            nullptr, &flash_operation,
                            &error) == KB_E_INVALID_ARGUMENT);
  CHECK(flash_operation == nullptr);
  CHECK(error != nullptr);
  CHECK(kb_error_transfer_state(error) == KB_TRANSFER_NOT_SENT);
  CHECK(std::strcmp(kb_error_device_identifier(error), "tcp:") == 0);
  kb_error_release(error);
  error = nullptr;

  std::vector<std::uint64_t> update_watermarks;
  kb_update_options_t update_options;
  kb_update_options_init(&update_options);
  update_options.skip_reboot = 1;
  update_options.progress_callback = record_progress;
  update_options.progress_user_data = &update_watermarks;
  const auto package_path = update_package.path().string();
  CHECK(kb_update_package(context, selector.c_str(), package_path.c_str(),
                          &update_options, &error) == KB_OK);
  CHECK(error == nullptr);
  CHECK(std::ranges::find(update_watermarks, 16U) !=
        update_watermarks.end());

  TemporaryUpdatePackage fastbootd_package(
      "version 1\nreboot fastboot\n");
  const auto fastbootd_path = fastbootd_package.path().string();
  CHECK(kb_update_package(context, selector.c_str(), fastbootd_path.c_str(),
                          nullptr, &error) == KB_E_NOT_SUPPORTED);
  CHECK(error != nullptr);
  CHECK(kb_error_status(error) == KB_E_NOT_SUPPORTED);
  CHECK(kb_error_transfer_state(error) == KB_TRANSFER_NOT_SENT);
  CHECK(strstr(kb_error_message(error), "fastbootd") != nullptr);
  kb_error_release(error);
  error = nullptr;

  CancelOnTaskFailureProbe cancellation_probe;
  kb_update_options_init(&update_options);
  update_options.progress_callback = cancel_on_second_execute;
  update_options.progress_user_data = &cancellation_probe;
  CHECK(kb_update_package(context, selector.c_str(), package_path.c_str(),
                          &update_options, &error) == KB_E_DEVICE_FAIL);
  CHECK(cancellation_probe.execute_callbacks == 2U);
  CHECK(error != nullptr);
  CHECK(kb_error_status(error) == KB_E_DEVICE_FAIL);
  const auto* update_device_message = kb_error_device_message(error, &size);
  CHECK(std::string(reinterpret_cast<const char*>(update_device_message), size) ==
        "partition locked");
  CHECK(kb_error_command_message_count(error) == 1U);
  kb_error_release(error);
  error = nullptr;

  TemporaryUpdatePackage wipe_package(
      "version 1\nif-wipe erase userdata\n");
  const auto wipe_path = wipe_package.path().string();
  kb_update_options_init(&update_options);
  update_options.wipe = 1;
  update_options.skip_reboot = 1;
  CHECK(kb_update_package(context, selector.c_str(), wipe_path.c_str(),
                          &update_options, &error) == KB_OK);
  CHECK(error == nullptr);

  TemporaryUpdatePackage deadline_package(
      "version 1\nerase cache\nerase metadata\n");
  const auto deadline_path = deadline_package.path().string();
  kb_update_options_init(&update_options);
  update_options.timeout_ms = 1000U;
  const auto deadline_started = std::chrono::steady_clock::now();
  CHECK(kb_update_package(context, selector.c_str(), deadline_path.c_str(),
                          &update_options, &error) == KB_E_TIMEOUT);
  const auto deadline_elapsed = std::chrono::steady_clock::now() -
                                deadline_started;
  CHECK(deadline_elapsed < std::chrono::milliseconds{1350});
  CHECK(error != nullptr);
  CHECK(kb_error_status(error) == KB_E_TIMEOUT);
  CHECK(kb_error_transfer_state(error) ==
        KB_TRANSFER_PARTIAL_OR_UNKNOWN);
  kb_error_release(error);
  error = nullptr;

  kb_context_release(context);
  server.finish();
}

void run_flash_raw_contract() {
  FlashRawServer server;
  const auto selector = "tcp:127.0.0.1:" + std::to_string(server.port());

  std::vector<std::byte> kernel(2048U);
  for (std::size_t index = 0; index < kernel.size(); ++index) {
    kernel[index] = std::byte{static_cast<unsigned char>(index & 0xffU)};
  }
  const std::array<std::byte, 3> ramdisk{std::byte{'r'}, std::byte{0},
                                        std::byte{'d'}};
  const std::array<std::byte, 3> dtb{std::byte{'d'}, std::byte{'t'},
                                    std::byte{'b'}};
  TemporaryUpdatePackage kernel_files("version 1\n", kernel);
  TemporaryUpdatePackage ramdisk_files("version 1\n", ramdisk);
  TemporaryUpdatePackage dtb_files("version 1\n", dtb);
  const auto kernel_path = (kernel_files.path() / "system.img").string();
  const auto ramdisk_path = (ramdisk_files.path() / "system.img").string();
  const auto dtb_path = (dtb_files.path() / "system.img").string();

  kb_context_t* context = nullptr;
  kb_error_t* error = nullptr;
  CHECK(kb_context_create(nullptr, &context, &error) == KB_OK);
  kb_legacy_boot_options_t boot_options;
  kb_legacy_boot_options_init(&boot_options);
  boot_options.header_version = 2U;
  boot_options.os_version = "15.1";
  boot_options.os_patch_level = "2025-02-05";
  boot_options.dtb_path = dtb_path.c_str();
  boot_options.dtb_offset = 0x01200000ULL;
  CHECK(kb_flash_raw_with_boot_options(
            context, selector.c_str(), "boot", kernel_path.c_str(),
            ramdisk_path.c_str(), nullptr, &boot_options, nullptr, &error) ==
        KB_OK);
  CHECK(error == nullptr);
  kb_context_release(context);
  server.finish();
}

void context_release_is_safe_after_async_update_start() {
  TemporaryUpdatePackage package("version 1\n");
  kb_context_t* context = nullptr;
  kb_error_t* error = nullptr;
  CHECK(kb_context_create(nullptr, &context, &error) == KB_OK);
  CHECK(context != nullptr);
  CHECK(error == nullptr);

  ReleaseContextProbe probe{.context = context, .released = false};
  kb_update_options_t options;
  kb_update_options_init(&options);
  options.progress_callback = release_context_during_preflight;
  options.progress_user_data = &probe;
  kb_operation_t* operation = nullptr;
  const auto package_path = package.path().string();
  CHECK(kb_update_package_async(
            context, "usb:255-255", package_path.c_str(), &options,
            &operation, &error) == KB_OK);
  CHECK(operation != nullptr);
  CHECK(error == nullptr);
  const auto status = kb_operation_wait(operation, KB_WAIT_INFINITE);
  CHECK(status == KB_E_NO_DEVICE || status == KB_E_IO);
  CHECK(probe.released.load(std::memory_order_acquire));
  kb_operation_release(operation);
}

void whole_update_timeout_includes_progress_callbacks() {
  TemporaryUpdatePackage package("version 1\n");
  kb_context_t* context = nullptr;
  kb_error_t* error = nullptr;
  CHECK(kb_context_create(nullptr, &context, &error) == KB_OK);

  DelayOpenProbe probe;
  kb_update_options_t options;
  kb_update_options_init(&options);
  options.timeout_ms = 500U;
  options.progress_callback = delay_transport_open;
  options.progress_user_data = &probe;
  const auto package_path = package.path().string();
  CHECK(kb_update_package(context, "tcp:127.0.0.1:1",
                          package_path.c_str(), &options,
                          &error) == KB_E_TIMEOUT);
  CHECK(probe.delayed);
  CHECK(error != nullptr);
  CHECK(kb_error_status(error) == KB_E_TIMEOUT);
  CHECK(kb_error_transfer_state(error) == KB_TRANSFER_NOT_SENT);
  CHECK(strstr(kb_error_message(error), "transport open") != nullptr);
  kb_error_release(error);
  kb_context_release(context);
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

  std::array<std::byte, 16> flash_data{};
  for (std::size_t index = 0; index < flash_data.size(); ++index) {
    flash_data[index] = std::byte{static_cast<unsigned char>(index)};
  }
  TemporaryUpdatePackage flash_package("version 1\n", flash_data);
  const auto flash_path = flash_package.path() / "system.img";
  std::vector<std::uint64_t> flash_watermarks;
  kairosboot::FlashOptions flash_options;
  flash_options.progress = [&](const kairosboot::Progress& progress) {
    CHECK(progress.device_identifier == selector_text);
    CHECK(progress.stage == "download" || progress.stage == "complete");
    flash_watermarks.push_back(progress.bytes_completed);
    return kairosboot::ProgressAction::Continue;
  };
  auto flash_operation = context->flash_file_async(
      std::optional<std::string_view>{selector_text}, "system", flash_path,
      flash_options);
  CHECK(flash_operation.has_value());
  CHECK(flash_operation->wait().has_value());
  CHECK(flash_operation->state() == KB_OPERATION_SUCCEEDED);
  CHECK(flash_watermarks ==
        std::vector<std::uint64_t>({0U, 0U, 16U, 16U}));

  flash_watermarks.clear();
  auto flashed = context->flash_file(
      std::optional<std::string_view>{selector_text}, "system", flash_path,
      flash_options);
  CHECK(flashed.has_value());
  CHECK(flash_watermarks ==
        std::vector<std::uint64_t>({0U, 0U, 16U, 16U}));

  auto cancelled_operation = context->getvar_async(selector, "cancel");
  CHECK(cancelled_operation.has_value());
  server.wait_for_cancel_command();
  std::stop_source cancellation;
  CHECK(cancellation.request_stop());
  auto cancelled = cancelled_operation->wait_result(
      cancellation.get_token(), std::chrono::milliseconds::zero());
  CHECK(!cancelled.has_value());
  CHECK(cancelled.error().status() == KB_E_CANCELLED);
  CHECK(cancelled_operation->state() == KB_OPERATION_CANCELLED);

  server.finish();
}

void run_udp_flash_contract() {
  ScriptedUdpFlashServer server(
      {false, false, true, true, false, false});
  const auto selector_text =
      "udp:127.0.0.1:" + std::to_string(server.port());
  std::array<std::byte, 16> image{};
  for (std::size_t index = 0; index < image.size(); ++index) {
    image[index] = std::byte{static_cast<unsigned char>(index)};
  }
  TemporaryUpdatePackage package("version 1\n", image);
  const auto image_path = (package.path() / "system.img").string();

  kb_context_t* c_context = nullptr;
  kb_error_t* error = nullptr;
  CHECK(kb_context_create(nullptr, &c_context, &error) == KB_OK);
  kb_flash_options_t c_options;
  kb_flash_options_init(&c_options);
  c_options.timeout_ms = 2'000U;
  kb_operation_t* operation = nullptr;
  CHECK(kb_flash_file_async(
            c_context, selector_text.c_str(), "system", image_path.c_str(),
            &c_options, &operation, &error) == KB_OK);
  CHECK(operation != nullptr);
  CHECK(kb_operation_wait(operation, KB_WAIT_INFINITE) == KB_OK);
  CHECK(kb_operation_state(operation) == KB_OPERATION_SUCCEEDED);
  kb_operation_release(operation);
  CHECK(error == nullptr);

  CHECK(kb_flash_file(c_context, selector_text.c_str(), "system",
                      image_path.c_str(), &c_options, &error) == KB_OK);
  CHECK(error == nullptr);

  operation = nullptr;
  CHECK(kb_flash_file_async(
            c_context, selector_text.c_str(), "system", image_path.c_str(),
            &c_options, &operation, &error) == KB_OK);
  CHECK(operation != nullptr);
  CHECK(error == nullptr);
  CHECK(kb_operation_wait(operation, KB_WAIT_INFINITE) == KB_E_DEVICE_FAIL);
  CHECK(kb_operation_state(operation) == KB_OPERATION_FAILED);
  const auto asynchronous_flash_error =
      snapshot_error(kb_operation_error(operation));
  kb_operation_release(operation);
  CHECK(asynchronous_flash_error.device_message == "partition locked");
  CHECK(asynchronous_flash_error.message_kinds ==
        std::vector<kb_command_message_kind_t>(
            {KB_COMMAND_MESSAGE_INFO, KB_COMMAND_MESSAGE_TEXT}));
  CHECK(asynchronous_flash_error.message_payloads ==
        std::vector<std::string>({"policy", "locked partition"}));
  CHECK(asynchronous_flash_error.session_poisoned == 0);

  CHECK(kb_flash_file(c_context, selector_text.c_str(), "system",
                      image_path.c_str(), &c_options, &error) ==
        KB_E_DEVICE_FAIL);
  const auto blocking_flash_error = snapshot_error(error);
  kb_error_release(error);
  error = nullptr;
  CHECK(blocking_flash_error == asynchronous_flash_error);

  kb_flash_options_init(&c_options);
  c_options.timeout_ms = 0U;
  operation = nullptr;
  CHECK(kb_flash_file_async(
            c_context, selector_text.c_str(), "system", image_path.c_str(),
            &c_options, &operation, &error) == KB_OK);
  CHECK(operation != nullptr);
  CHECK(error == nullptr);
  CHECK(kb_operation_wait(operation, KB_WAIT_INFINITE) == KB_E_TIMEOUT);
  const auto* timeout_error = kb_operation_error(operation);
  CHECK(timeout_error != nullptr);
  CHECK(kb_error_status(timeout_error) == KB_E_TIMEOUT);
  CHECK(kb_error_transfer_state(timeout_error) == KB_TRANSFER_NOT_SENT);
  CHECK(std::strcmp(kb_error_device_identifier(timeout_error),
                    selector_text.c_str()) == 0);
  kb_operation_release(operation);

  operation = nullptr;
  CHECK(kb_flash_file_async(
            c_context, "udp:127.0.0.1:70000", "system", image_path.c_str(),
            &c_options, &operation, &error) == KB_E_INVALID_ARGUMENT);
  CHECK(operation == nullptr);
  CHECK(error != nullptr);
  CHECK(kb_error_transfer_state(error) == KB_TRANSFER_NOT_SENT);
  kb_error_release(error);
  kb_context_release(c_context);

  auto cxx_context = kairosboot::Context::create();
  CHECK(cxx_context.has_value());
  kairosboot::FlashOptions cxx_options;
  cxx_options.timeout = std::chrono::seconds{2};
  auto cxx_operation = cxx_context->flash_file_async(
      std::optional<std::string_view>{selector_text}, "system",
      package.path() / "system.img", cxx_options);
  CHECK(cxx_operation.has_value());
  CHECK(cxx_operation->wait().has_value());
  auto cxx_result = cxx_context->flash_file(
      std::optional<std::string_view>{selector_text}, "system",
      package.path() / "system.img", cxx_options);
  CHECK(cxx_result.has_value());

  cxx_options.timeout = std::chrono::milliseconds{100};
  const auto unavailable_tcp =
      "tcp:127.0.0.1:" + std::to_string(unavailable_tcp_port());
  auto tcp_failure = cxx_context->flash_file(
      std::optional<std::string_view>{unavailable_tcp}, "system",
      package.path() / "system.img", cxx_options);
  CHECK(!tcp_failure.has_value());
  CHECK(tcp_failure.error().status() == KB_E_IO ||
        tcp_failure.error().status() == KB_E_TIMEOUT);
  CHECK(tcp_failure.error().transfer_state() == KB_TRANSFER_NOT_SENT);
  CHECK(tcp_failure.error().device_identifier() == unavailable_tcp);

  const auto unavailable_udp =
      "udp:127.0.0.1:" + std::to_string(unavailable_udp_port());
  auto udp_failure = cxx_context->flash_file(
      std::optional<std::string_view>{unavailable_udp}, "system",
      package.path() / "system.img", cxx_options);
  CHECK(!udp_failure.has_value());
  CHECK(udp_failure.error().status() == KB_E_TIMEOUT ||
        udp_failure.error().status() == KB_E_IO);
  CHECK(udp_failure.error().transfer_state() == KB_TRANSFER_NOT_SENT);
  CHECK(udp_failure.error().device_identifier() == unavailable_udp);

  server.finish();
}

}  // namespace

int main() {
  try {
    run_contract();
    run_flash_raw_contract();
    context_release_is_safe_after_async_update_start();
    whole_update_timeout_includes_progress_callbacks();
    run_cxx_contract();
    run_udp_flash_contract();
    std::cout << "PASS: typed C and C++ primitives over Fastboot TCP/UDP\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
