// SPDX-License-Identifier: MIT
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <kairosboot/kairosboot.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <random>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr std::uint64_t kDefaultMaximumReceiveBytes = 64ULL * 1024ULL * 1024ULL;

std::string_view status_name(const kb_status_t status) noexcept {
  switch (status) {
  case KB_OK:
    return "ok";
  case KB_E_INVALID_ARGUMENT:
    return "invalid_argument";
  case KB_E_OUT_OF_MEMORY:
    return "out_of_memory";
  case KB_E_NOT_SUPPORTED:
    return "not_supported";
  case KB_E_NO_DEVICE:
    return "no_device";
  case KB_E_AMBIGUOUS_DEVICE:
    return "ambiguous_device";
  case KB_E_BUSY:
    return "busy";
  case KB_E_TIMEOUT:
    return "timeout";
  case KB_E_CANCELLED:
    return "cancelled";
  case KB_E_IO:
    return "io";
  case KB_E_INTERNAL:
    return "internal";
  case KB_E_PROTOCOL:
    return "protocol";
  case KB_E_DEVICE_FAIL:
    return "device_fail";
  default:
    return "unknown";
  }
}

std::string_view transfer_state_name(const kb_transfer_state_t state) noexcept {
  switch (state) {
  case KB_TRANSFER_NOT_SENT:
    return "not_sent";
  case KB_TRANSFER_PARTIAL_OR_UNKNOWN:
    return "partial_or_unknown";
  case KB_TRANSFER_FULLY_TRANSFERRED:
    return "fully_transferred";
  default:
    return "unknown";
  }
}

std::string json_escape(const std::string_view value) {
  constexpr char hex[] = "0123456789abcdef";
  std::string result;
  result.reserve(value.size());
  for (const unsigned char character : value) {
    switch (character) {
    case '\\':
      result += "\\\\";
      break;
    case '"':
      result += "\\\"";
      break;
    case '\b':
      result += "\\b";
      break;
    case '\f':
      result += "\\f";
      break;
    case '\n':
      result += "\\n";
      break;
    case '\r':
      result += "\\r";
      break;
    case '\t':
      result += "\\t";
      break;
    default:
      if (character < 0x20U) {
        result += "\\u00";
        result += hex[(character >> 4U) & 0x0fU];
        result += hex[character & 0x0fU];
      } else {
        result += static_cast<char>(character);
      }
      break;
    }
  }
  return result;
}

std::string base64_encode(const std::span<const std::byte> bytes) {
  constexpr std::string_view alphabet =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string result;
  result.reserve(((bytes.size() + 2U) / 3U) * 4U);

  std::size_t index = 0;
  while (index + 3U <= bytes.size()) {
    const auto first = std::to_integer<unsigned int>(bytes[index]);
    const auto second = std::to_integer<unsigned int>(bytes[index + 1U]);
    const auto third = std::to_integer<unsigned int>(bytes[index + 2U]);
    result += alphabet[(first >> 2U) & 0x3fU];
    result += alphabet[((first & 0x03U) << 4U) | (second >> 4U)];
    result += alphabet[((second & 0x0fU) << 2U) | (third >> 6U)];
    result += alphabet[third & 0x3fU];
    index += 3U;
  }

  const std::size_t remaining = bytes.size() - index;
  if (remaining != 0U) {
    const auto first = std::to_integer<unsigned int>(bytes[index]);
    result += alphabet[(first >> 2U) & 0x3fU];
    if (remaining == 1U) {
      result += alphabet[(first & 0x03U) << 4U];
      result += "==";
    } else {
      const auto second = std::to_integer<unsigned int>(bytes[index + 1U]);
      result += alphabet[((first & 0x03U) << 4U) | (second >> 4U)];
      result += alphabet[(second & 0x0fU) << 2U];
      result += '=';
    }
  }
  return result;
}

std::string escape_binary_for_text(const std::span<const std::byte> bytes) {
  constexpr char hex[] = "0123456789abcdef";
  constexpr std::size_t maximum_visible_bytes = 256U;
  const std::size_t visible = std::min(bytes.size(), maximum_visible_bytes);
  std::string result;
  result.reserve(visible * 4U);
  for (std::size_t index = 0; index < visible; ++index) {
    const auto value = std::to_integer<unsigned int>(bytes[index]);
    if (value >= 0x20U && value <= 0x7eU && value != '\\') {
      result += static_cast<char>(value);
    } else if (value == '\\') {
      result += "\\\\";
    } else {
      result += "\\x";
      result += hex[(value >> 4U) & 0x0fU];
      result += hex[value & 0x0fU];
    }
  }
  if (visible != bytes.size()) {
    result += "... (";
    result += std::to_string(bytes.size());
    result += " bytes)";
  }
  return result;
}

std::filesystem::path path_from_utf8(const std::string_view value) {
#if defined(_WIN32)
  std::u8string converted;
  converted.reserve(value.size());
  for (const unsigned char character : value) {
    converted.push_back(static_cast<char8_t>(character));
  }
  return std::filesystem::path{converted};
#else
  return std::filesystem::path{std::string{value}};
#endif
}

struct GlobalOptions {
  std::optional<std::string> selector;
  std::string_view selector_option;
  bool json{false};
  std::optional<std::uint32_t> timeout_ms;
  std::uint64_t maximum_receive_bytes{kDefaultMaximumReceiveBytes};
  bool maximum_receive_bytes_set{false};
};

enum class CommandKind : std::uint8_t {
  Version,
  Help,
  Doctor,
  Devices,
  Flash,
  Getvar,
  Erase,
  SetActive,
  Reboot,
  Continue,
  Oem,
  Raw,
  BootStaged,
  Stage,
  Upload,
  Fetch,
  Flashing,
  Gsi,
  SnapshotUpdate,
  CreateLogicalPartition,
  DeleteLogicalPartition,
  ResizeLogicalPartition,
};

struct Invocation {
  GlobalOptions global;
  CommandKind kind{CommandKind::Version};
  std::string_view first;
  std::string_view second;
  std::string joined;
  kairosboot::RebootTarget reboot_target{kairosboot::RebootTarget::System};
  kairosboot::FlashingCommand flashing_command{
      kairosboot::FlashingCommand::Lock};
  kairosboot::GsiCommand gsi_command{kairosboot::GsiCommand::Wipe};
  kairosboot::SnapshotUpdateCommand snapshot_update_command{
      kairosboot::SnapshotUpdateCommand::Cancel};
  kairosboot::FetchRange fetch_range;
  std::uint64_t logical_partition_size{};
};

struct ParseError {
  bool json{false};
  std::string message;
};

struct LocalRuntimeError {
  kb_status_t status{KB_E_INTERNAL};
  std::string message;
};

#if defined(_WIN32)
std::expected<std::string, LocalRuntimeError>
utf8_from_wide(const std::wstring_view value) {
  if (value.empty()) {
    return std::string{};
  }
  if (value.size() >
      static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    return std::unexpected(
        LocalRuntimeError{KB_E_INVALID_ARGUMENT,
                          "Windows command-line argument is too long"});
  }

  const int input_size = static_cast<int>(value.size());
  const int output_size = WideCharToMultiByte(
      CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), input_size, nullptr, 0,
      nullptr, nullptr);
  if (output_size == 0) {
    const unsigned long error = GetLastError();
    return std::unexpected(LocalRuntimeError{
        KB_E_INVALID_ARGUMENT,
        "cannot convert Windows command-line argument to UTF-8: " +
            std::to_string(error)});
  }

  std::string result(static_cast<std::size_t>(output_size), '\0');
  if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                          input_size, result.data(), output_size, nullptr,
                          nullptr) != output_size) {
    const unsigned long error = GetLastError();
    return std::unexpected(LocalRuntimeError{
        KB_E_INVALID_ARGUMENT,
        "cannot convert Windows command-line argument to UTF-8: " +
            std::to_string(error)});
  }
  return result;
}
#endif

bool is_global_option(const std::string_view value) noexcept {
  return value == "--json" || value == "--device" || value == "--serial" ||
         value == "--timeout-ms" || value == "--max-receive-bytes";
}

template <typename Integer>
bool parse_unsigned_decimal(const std::string_view text, Integer &value) {
  if (text.empty()) {
    return false;
  }
  const auto [end, error] =
      std::from_chars(text.data(), text.data() + text.size(), value, 10);
  return error == std::errc{} && end == text.data() + text.size();
}

std::string join_operands(char **argv, const int first, const int argc) {
  std::string result;
  for (int index = first; index < argc; ++index) {
    if (!result.empty()) {
      result += ' ';
    }
    result += argv[index];
  }
  return result;
}

std::expected<Invocation, ParseError> parse_invocation(const int argc,
                                                       char **argv) {
  Invocation result;
  int index = 1;
  bool device_seen = false;
  bool serial_seen = false;
  bool timeout_seen = false;
  const auto error = [&result](std::string message) {
    return std::unexpected(ParseError{result.global.json, std::move(message)});
  };

  while (index < argc) {
    const std::string_view argument{argv[index]};
    if (argument == "--json") {
      if (result.global.json) {
        return error("option --json may only be specified once");
      }
      result.global.json = true;
      ++index;
      continue;
    }
    if (argument == "--device" || argument == "--serial") {
      const bool is_device = argument == "--device";
      if ((is_device && device_seen) || (!is_device && serial_seen)) {
        return error("option " + std::string{argument} +
                     " may only be specified once");
      }
      if ((is_device && serial_seen) || (!is_device && device_seen)) {
        return error("options --device and --serial are mutually exclusive");
      }
      if (index + 1 >= argc || argv[index + 1][0] == '\0' ||
          std::string_view{argv[index + 1]}.starts_with("--")) {
        return error("option " + std::string{argument} +
                     " requires a non-empty value");
      }
      result.global.selector = std::string{argv[index + 1]};
      result.global.selector_option = argument;
      device_seen = is_device;
      serial_seen = !is_device;
      index += 2;
      continue;
    }
    if (argument == "--timeout-ms") {
      if (timeout_seen) {
        return error("option --timeout-ms may only be specified once");
      }
      if (index + 1 >= argc) {
        return error(
            "option --timeout-ms requires an integer in [0, 4294967294]");
      }
      std::uint64_t parsed = 0;
      const std::string_view value{argv[index + 1]};
      if (!parse_unsigned_decimal(value, parsed) ||
          parsed >= std::numeric_limits<std::uint32_t>::max()) {
        return error(
            "option --timeout-ms requires an integer in [0, 4294967294]");
      }
      result.global.timeout_ms = static_cast<std::uint32_t>(parsed);
      timeout_seen = true;
      index += 2;
      continue;
    }
    if (argument == "--max-receive-bytes") {
      if (result.global.maximum_receive_bytes_set) {
        return error("option --max-receive-bytes may only be specified once");
      }
      if (index + 1 >= argc) {
        return error("option --max-receive-bytes requires a positive integer");
      }
      std::uint64_t parsed = 0;
      const std::string_view value{argv[index + 1]};
      if (!parse_unsigned_decimal(value, parsed) || parsed == 0U) {
        return error("option --max-receive-bytes requires a positive integer");
      }
      result.global.maximum_receive_bytes = parsed;
      result.global.maximum_receive_bytes_set = true;
      index += 2;
      continue;
    }
    break;
  }

  if (index >= argc) {
    return error("unknown command");
  }
  const std::string_view command{argv[index]};
  if (command.starts_with("--") && command != "--version" &&
      command != "--help") {
    return error("unknown option " + std::string{command});
  }
  const int command_index = index++;
  for (int operand = index; operand < argc; ++operand) {
    if (is_global_option(argv[operand])) {
      return error("global options must precede the command");
    }
  }

  const auto reject_non_json_globals =
      [&]() -> std::optional<std::expected<Invocation, ParseError>> {
    if (result.global.selector.has_value()) {
      return error("option " + std::string{result.global.selector_option} +
                   " is not valid for " + std::string{command});
    }
    if (result.global.timeout_ms.has_value()) {
      return error("option --timeout-ms is not valid for " +
                   std::string{command});
    }
    if (result.global.maximum_receive_bytes_set) {
      return error("option --max-receive-bytes is not valid for " +
                   std::string{command});
    }
    return std::nullopt;
  };

  if (command == "--version") {
    result.kind = CommandKind::Version;
    if (argc - command_index != 1) {
      return error("--version does not accept operands");
    }
    if (const auto rejected = reject_non_json_globals()) {
      return *rejected;
    }
    return result;
  }
  if (command == "--help" || command == "help") {
    result.kind = CommandKind::Help;
    if (argc - command_index != 1) {
      return error(std::string{command} + " does not accept operands");
    }
    if (const auto rejected = reject_non_json_globals()) {
      return *rejected;
    }
    return result;
  }
  if (command == "doctor") {
    result.kind = CommandKind::Doctor;
    if (argc - command_index != 1) {
      return error("doctor does not accept operands");
    }
    if (const auto rejected = reject_non_json_globals()) {
      return *rejected;
    }
    return result;
  }
  if (command == "devices") {
    result.kind = CommandKind::Devices;
    if (argc - command_index != 1) {
      return error("devices does not accept operands");
    }
    if (const auto rejected = reject_non_json_globals()) {
      return *rejected;
    }
    return result;
  }
  if (command == "flash") {
    result.kind = CommandKind::Flash;
    if (argc - command_index != 3) {
      return error("flash requires exactly <partition> and <file>");
    }
    if (result.global.maximum_receive_bytes_set) {
      return error("option --max-receive-bytes is not valid for flash");
    }
    result.first = argv[index];
    result.second = argv[index + 1];
    if (result.first.empty()) {
      return error("flash partition must not be empty");
    }
    if (result.second.empty()) {
      return error("flash file must not be empty");
    }
    return result;
  }

  const auto parse_single_operand = [&](const CommandKind kind,
                                        const std::string_view description)
      -> std::expected<Invocation, ParseError> {
    result.kind = kind;
    if (argc - command_index != 2) {
      return error(std::string{command} + " requires exactly <" +
                   std::string{description} + ">");
    }
    result.first = argv[index];
    if (result.first.empty()) {
      return error(std::string{command} + " " + std::string{description} +
                   " must not be empty");
    }
    return result;
  };

  if (command == "getvar") {
    return parse_single_operand(CommandKind::Getvar, "variable");
  }
  if (command == "erase") {
    return parse_single_operand(CommandKind::Erase, "partition");
  }
  if (command == "set-active") {
    return parse_single_operand(CommandKind::SetActive, "slot");
  }
  if (command == "stage") {
    return parse_single_operand(CommandKind::Stage, "file");
  }
  if (command == "upload") {
    return parse_single_operand(CommandKind::Upload, "output");
  }
  if (command == "flashing") {
    result.kind = CommandKind::Flashing;
    if (argc - command_index != 2) {
      return error(
          "flashing requires exactly "
          "<lock|unlock|lock-critical|unlock-critical|get-unlock-ability>");
    }
    const std::string_view action{argv[index]};
    if (action == "lock") {
      result.flashing_command = kairosboot::FlashingCommand::Lock;
    } else if (action == "unlock") {
      result.flashing_command = kairosboot::FlashingCommand::Unlock;
    } else if (action == "lock-critical") {
      result.flashing_command = kairosboot::FlashingCommand::LockCritical;
    } else if (action == "unlock-critical") {
      result.flashing_command = kairosboot::FlashingCommand::UnlockCritical;
    } else if (action == "get-unlock-ability") {
      result.flashing_command = kairosboot::FlashingCommand::GetUnlockAbility;
    } else {
      return error(
          "flashing command must be lock, unlock, lock-critical, "
          "unlock-critical, or get-unlock-ability");
    }
    return result;
  }
  if (command == "gsi") {
    result.kind = CommandKind::Gsi;
    if (argc - command_index != 2) {
      return error("gsi requires exactly <wipe|disable|status>");
    }
    const std::string_view action{argv[index]};
    if (action == "wipe") {
      result.gsi_command = kairosboot::GsiCommand::Wipe;
    } else if (action == "disable") {
      result.gsi_command = kairosboot::GsiCommand::Disable;
    } else if (action == "status") {
      result.gsi_command = kairosboot::GsiCommand::Status;
    } else {
      return error("gsi command must be wipe, disable, or status");
    }
    return result;
  }
  if (command == "snapshot-update") {
    result.kind = CommandKind::SnapshotUpdate;
    if (argc - command_index != 2) {
      return error("snapshot-update requires exactly <cancel|merge>");
    }
    const std::string_view action{argv[index]};
    if (action == "cancel") {
      result.snapshot_update_command =
          kairosboot::SnapshotUpdateCommand::Cancel;
    } else if (action == "merge") {
      result.snapshot_update_command =
          kairosboot::SnapshotUpdateCommand::Merge;
    } else {
      return error("snapshot-update command must be cancel or merge");
    }
    return result;
  }
  if (command == "create-logical-partition" ||
      command == "resize-logical-partition") {
    const bool create = command == "create-logical-partition";
    result.kind = create ? CommandKind::CreateLogicalPartition
                         : CommandKind::ResizeLogicalPartition;
    if (argc - command_index != 3) {
      return error(std::string{command} +
                   " requires exactly <partition> and <size-bytes>");
    }
    result.first = argv[index];
    if (result.first.empty()) {
      return error(std::string{command} + " partition must not be empty");
    }
    const std::string_view size{argv[index + 1]};
    if (!parse_unsigned_decimal(size, result.logical_partition_size)) {
      return error(std::string{command} +
                   " size-bytes requires an integer in "
                   "[0, 18446744073709551615]");
    }
    return result;
  }
  if (command == "delete-logical-partition") {
    return parse_single_operand(CommandKind::DeleteLogicalPartition,
                                "partition");
  }
  if (command == "reboot") {
    result.kind = CommandKind::Reboot;
    if (argc - command_index > 2) {
      return error(
          "reboot accepts at most one target: system, bootloader, recovery, "
          "or fastboot");
    }
    if (argc - command_index == 2) {
      const std::string_view target{argv[index]};
      if (target == "system") {
        result.reboot_target = kairosboot::RebootTarget::System;
      } else if (target == "bootloader") {
        result.reboot_target = kairosboot::RebootTarget::Bootloader;
      } else if (target == "recovery") {
        result.reboot_target = kairosboot::RebootTarget::Recovery;
      } else if (target == "fastboot") {
        result.reboot_target = kairosboot::RebootTarget::Fastboot;
      } else {
        return error(
            "reboot target must be system, bootloader, recovery, or fastboot");
      }
    }
    return result;
  }
  if (command == "continue") {
    result.kind = CommandKind::Continue;
    if (argc - command_index != 1) {
      return error("continue does not accept operands");
    }
    return result;
  }
  if (command == "boot-staged") {
    result.kind = CommandKind::BootStaged;
    if (argc - command_index != 1) {
      return error("boot-staged does not accept operands");
    }
    return result;
  }
  if (command == "oem" || command == "raw") {
    result.kind = command == "oem" ? CommandKind::Oem : CommandKind::Raw;
    if (argc - command_index < 2) {
      return error(std::string{command} + " requires a command string");
    }
    for (int operand = index; operand < argc; ++operand) {
      if (argv[operand][0] == '\0') {
        return error(std::string{command} +
                     " command operands must not be empty");
      }
    }
    result.joined = join_operands(argv, index, argc);
    return result;
  }
  if (command == "fetch") {
    result.kind = CommandKind::Fetch;
    if (argc - index < 2) {
      return error("fetch requires <partition> and <output>");
    }
    result.first = argv[index++];
    result.second = argv[index++];
    if (result.first.empty()) {
      return error("fetch partition must not be empty");
    }
    if (result.second.empty()) {
      return error("fetch output must not be empty");
    }
    bool offset_seen = false;
    bool size_seen = false;
    constexpr std::uint64_t maximum_range =
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    while (index < argc) {
      const std::string_view option{argv[index++]};
      if (option != "--offset" && option != "--size") {
        return error(
            "fetch supports only --offset <bytes> and --size <bytes> after "
            "<output>");
      }
      bool &seen = option == "--offset" ? offset_seen : size_seen;
      if (seen) {
        return error("fetch option " + std::string{option} +
                     " may only be specified once");
      }
      if (index >= argc) {
        return error("fetch option " + std::string{option} +
                     " requires an integer in [0, 9223372036854775807]");
      }
      std::uint64_t parsed = 0;
      const std::string_view value{argv[index++]};
      if (!parse_unsigned_decimal(value, parsed) || parsed > maximum_range) {
        return error("fetch option " + std::string{option} +
                     " requires an integer in [0, 9223372036854775807]");
      }
      seen = true;
      if (option == "--offset") {
        result.fetch_range.offset = parsed;
      } else {
        result.fetch_range.size = parsed;
      }
    }
    if (size_seen && !offset_seen) {
      return error("fetch --size requires --offset");
    }
    return result;
  }

  return error("unknown command");
}

std::string_view command_name(const CommandKind kind) noexcept {
  switch (kind) {
  case CommandKind::Version:
    return "version";
  case CommandKind::Help:
    return "help";
  case CommandKind::Doctor:
    return "doctor";
  case CommandKind::Devices:
    return "devices";
  case CommandKind::Flash:
    return "flash";
  case CommandKind::Getvar:
    return "getvar";
  case CommandKind::Erase:
    return "erase";
  case CommandKind::SetActive:
    return "set-active";
  case CommandKind::Reboot:
    return "reboot";
  case CommandKind::Continue:
    return "continue";
  case CommandKind::Oem:
    return "oem";
  case CommandKind::Raw:
    return "raw";
  case CommandKind::BootStaged:
    return "boot-staged";
  case CommandKind::Stage:
    return "stage";
  case CommandKind::Upload:
    return "upload";
  case CommandKind::Fetch:
    return "fetch";
  case CommandKind::Flashing:
    return "flashing";
  case CommandKind::Gsi:
    return "gsi";
  case CommandKind::SnapshotUpdate:
    return "snapshot-update";
  case CommandKind::CreateLogicalPartition:
    return "create-logical-partition";
  case CommandKind::DeleteLogicalPartition:
    return "delete-logical-partition";
  case CommandKind::ResizeLogicalPartition:
    return "resize-logical-partition";
  }
  return "unknown";
}

int print_parse_error(const ParseError &error) {
  if (error.json) {
    std::cout << "{\"ok\":false,\"status\":\"invalid_argument\","
                 "\"message\":\""
              << json_escape(error.message) << "\"}\n";
  } else {
    std::cerr << "kairosboot: " << error.message << '\n';
  }
  return 2;
}

void print_error_messages(const kairosboot::Error &error) {
  if (!error.device_message().empty()) {
    std::cerr << "device: " << escape_binary_for_text(error.device_message())
              << '\n';
  }
  for (const auto &message : error.command_messages()) {
    std::cerr << (message.kind == kairosboot::CommandMessageKind::Info ? "INFO"
                                                                       : "TEXT")
              << ": " << escape_binary_for_text(message.payload) << '\n';
  }
}

void print_json_messages(
    const std::vector<kairosboot::CommandMessage> &messages) {
  std::cout << '[';
  for (std::size_t index = 0; index < messages.size(); ++index) {
    if (index != 0U) {
      std::cout << ',';
    }
    const auto &message = messages[index];
    std::cout << "{\"kind\":\""
              << (message.kind == kairosboot::CommandMessageKind::Info ? "INFO"
                                                                       : "TEXT")
              << "\",\"base64\":\"" << base64_encode(message.payload)
              << "\",\"bytes\":" << message.payload.size() << '}';
  }
  std::cout << ']';
}

int print_runtime_error(const kairosboot::Error &error, const bool json) {
  if (json) {
    std::cout << "{\"ok\":false,\"status\":\"" << status_name(error.status())
              << "\",\"message\":\"" << json_escape(error.message())
              << "\",\"device\":\"" << json_escape(error.device_identifier())
              << "\",\"deviceMessage\":{\"base64\":\""
              << base64_encode(error.device_message())
              << "\",\"bytes\":" << error.device_message().size()
              << "},\"messages\":";
    print_json_messages(error.command_messages());
    std::cout << ",\"transferState\":\""
              << transfer_state_name(error.transfer_state())
              << "\",\"inboundExpectedBytes\":";
    if (error.inbound_expected_bytes().has_value()) {
      std::cout << *error.inbound_expected_bytes();
    } else {
      std::cout << "null";
    }
    std::cout << ",\"inboundTransferredBytes\":"
              << error.inbound_transferred_bytes()
              << ",\"inboundTransferState\":\""
              << transfer_state_name(error.inbound_transfer_state())
              << "\",\"sessionPoisoned\":"
              << (error.session_poisoned() ? "true" : "false") << "}\n";
  } else {
    std::cerr << "kairosboot: " << error.message() << '\n';
    print_error_messages(error);
  }
  return 4;
}

int print_local_runtime_error(const LocalRuntimeError &error, const bool json) {
  if (json) {
    std::cout << "{\"ok\":false,\"status\":\"" << status_name(error.status)
              << "\",\"message\":\"" << json_escape(error.message) << "\"}\n";
  } else {
    std::cerr << "kairosboot: " << error.message << '\n';
  }
  return 4;
}

int print_version(const bool json) {
  const kairosboot::Version current = kairosboot::version();
  if (json) {
    std::cout << "{\"name\":\"KairosBoot\",\"version\":\""
              << json_escape(current.string)
              << "\",\"apiVersion\":" << current.api_version << "}\n";
  } else {
    std::cout << "KairosBoot " << current.string << '\n';
  }
  return 0;
}

int doctor(const bool json) {
  const kairosboot::Version current = kairosboot::version();
  auto context = kairosboot::Context::create();
  if (!context) {
    if (!json) {
      return print_runtime_error(context.error(), false);
    }
    const auto &error = context.error();
    std::cout << "{\"ok\":false,\"version\":\"" << json_escape(current.string)
              << "\",\"status\":\"" << status_name(error.status())
              << "\",\"message\":\"" << json_escape(error.message()) << "\"}\n";
    return 4;
  }

  auto devices = context->devices();
  if (!devices) {
    if (!json) {
      return print_runtime_error(devices.error(), false);
    }
    const auto &error = devices.error();
    std::cout << "{\"ok\":false,\"version\":\"" << json_escape(current.string)
              << "\",\"transport\":{\"available\":false,\"status\":\""
              << status_name(error.status()) << "\",\"message\":\""
              << json_escape(error.message()) << "\"}}\n";
    return 4;
  }

  if (json) {
    std::cout << "{\"ok\":true,\"version\":\"" << json_escape(current.string)
              << "\",\"transport\":{\"available\":true},\"deviceCount\":"
              << devices->size() << "}\n";
  } else {
    std::cout << "KairosBoot " << current.string << '\n'
              << "Transport: available\nDevices: " << devices->size() << '\n';
  }
  return 0;
}

int print_devices(const bool json) {
  auto context = kairosboot::Context::create();
  if (!context) {
    return print_runtime_error(context.error(), json);
  }
  auto devices = context->devices();
  if (!devices) {
    return print_runtime_error(devices.error(), json);
  }

  if (json) {
    std::cout << "{\"ok\":true,\"devices\":[";
    for (std::size_t index = 0; index < devices->size(); ++index) {
      if (index != 0U) {
        std::cout << ',';
      }
      std::cout << "{\"serial\":\"" << json_escape(devices->serial(index))
                << "\",\"usbPath\":\"" << json_escape(devices->usb_path(index))
                << "\",\"product\":\"" << json_escape(devices->product(index))
                << "\"}";
    }
    std::cout << "]}\n";
    return 0;
  }

  for (std::size_t index = 0; index < devices->size(); ++index) {
    std::cout << devices->serial(index) << '\t' << devices->usb_path(index)
              << '\t' << devices->product(index) << '\n';
  }
  return 0;
}

kairosboot::CommandOptions command_options(const GlobalOptions &options) {
  kairosboot::CommandOptions result;
  if (options.timeout_ms.has_value()) {
    result.timeout = std::chrono::milliseconds{*options.timeout_ms};
  }
  result.maximum_receive_bytes = options.maximum_receive_bytes;
  return result;
}

kairosboot::FlashOptions flash_options(const GlobalOptions &options) {
  kairosboot::FlashOptions result;
  if (options.timeout_ms.has_value()) {
    result.timeout = std::chrono::milliseconds{*options.timeout_ms};
  }
  return result;
}

kairosboot::DeviceSelector selector_view(const GlobalOptions &options) {
  if (!options.selector.has_value()) {
    return std::nullopt;
  }
  return std::string_view{*options.selector};
}

#if defined(_WIN32)
volatile LONG interrupt_requested_flag = 0;

BOOL WINAPI interrupt_handler(const DWORD control_type) noexcept {
  if (control_type != CTRL_C_EVENT && control_type != CTRL_BREAK_EVENT) {
    return FALSE;
  }
  InterlockedExchange(&interrupt_requested_flag, 1);
  return TRUE;
}

void reset_interrupt_request() noexcept {
  InterlockedExchange(&interrupt_requested_flag, 0);
}

bool interrupt_requested() noexcept {
  return InterlockedCompareExchange(&interrupt_requested_flag, 0, 0) != 0;
}
#else
std::atomic_flag interrupt_requested_flag = ATOMIC_FLAG_INIT;

void interrupt_handler(int /*signal*/) noexcept {
  static_cast<void>(
      interrupt_requested_flag.test_and_set(std::memory_order_relaxed));
}

void reset_interrupt_request() noexcept {
  interrupt_requested_flag.clear(std::memory_order_relaxed);
}

bool interrupt_requested() noexcept {
  return interrupt_requested_flag.test(std::memory_order_relaxed);
}
#endif

class InterruptCancellation final {
public:
  InterruptCancellation() {
    reset_interrupt_request();
#if defined(_WIN32)
    installed_ = SetConsoleCtrlHandler(interrupt_handler, TRUE) != 0;
#else
    previous_handler_ = std::signal(SIGINT, interrupt_handler);
    installed_ = previous_handler_ != SIG_ERR;
#endif
    try {
      watcher_ = std::jthread([this](const std::stop_token watcher_stop) {
        while (!watcher_stop.stop_requested()) {
          if (interrupt_requested()) {
            static_cast<void>(cancellation_.request_stop());
            return;
          }
          std::this_thread::sleep_for(std::chrono::milliseconds{2});
        }
      });
    } catch (...) {
      restore_handler();
      throw;
    }
  }

  ~InterruptCancellation() {
    watcher_.request_stop();
    if (watcher_.joinable()) {
      watcher_.join();
    }
    restore_handler();
  }

  InterruptCancellation(const InterruptCancellation &) = delete;
  InterruptCancellation &operator=(const InterruptCancellation &) = delete;

  [[nodiscard]] std::stop_token token() const noexcept {
    return cancellation_.get_token();
  }

private:
  void restore_handler() noexcept {
    if (!installed_) {
      return;
    }
#if defined(_WIN32)
    static_cast<void>(SetConsoleCtrlHandler(interrupt_handler, FALSE));
#else
    static_cast<void>(std::signal(SIGINT, previous_handler_));
#endif
    installed_ = false;
  }

  std::stop_source cancellation_;
  std::jthread watcher_;
#if !defined(_WIN32)
  using SignalHandler = void (*)(int);
  SignalHandler previous_handler_{SIG_DFL};
#endif
  bool installed_{false};
};

std::expected<std::vector<std::byte>, LocalRuntimeError>
read_stage_file(const std::string_view file_name) {
  const std::filesystem::path path = path_from_utf8(file_name);
  std::ifstream input{path, std::ios::binary};
  if (!input) {
    return std::unexpected(LocalRuntimeError{
        KB_E_IO, "cannot open stage file: " + std::string{file_name}});
  }

  std::error_code size_error;
  const std::uintmax_t reported_size =
      std::filesystem::file_size(path, size_error);
  if (!size_error &&
      reported_size > std::numeric_limits<std::uint32_t>::max()) {
    return std::unexpected(LocalRuntimeError{
        KB_E_INVALID_ARGUMENT,
        "stage file exceeds UINT32_MAX bytes: " + std::string{file_name}});
  }

  std::vector<std::byte> data;
  if (!size_error) {
    data.reserve(static_cast<std::size_t>(reported_size));
  }
  std::array<char, 64U * 1024U> buffer{};
  while (input) {
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const std::streamsize count = input.gcount();
    if (count > 0) {
      const auto unsigned_count = static_cast<std::uint64_t>(count);
      if (data.size() >
          std::numeric_limits<std::uint32_t>::max() - unsigned_count) {
        return std::unexpected(LocalRuntimeError{
            KB_E_INVALID_ARGUMENT,
            "stage file exceeds UINT32_MAX bytes: " + std::string{file_name}});
      }
      const auto *first = reinterpret_cast<const std::byte *>(buffer.data());
      data.insert(data.end(), first, first + count);
    }
  }
  if (!input.eof()) {
    return std::unexpected(LocalRuntimeError{
        KB_E_IO, "failed while reading stage file: " + std::string{file_name}});
  }
  if (data.empty()) {
    return std::unexpected(LocalRuntimeError{KB_E_INVALID_ARGUMENT,
                                             "stage file must not be empty: " +
                                                 std::string{file_name}});
  }
  return data;
}

class TemporaryOutput final {
public:
  explicit TemporaryOutput(std::filesystem::path path)
      : path_(std::move(path)) {}
  ~TemporaryOutput() {
    if (!committed_) {
      std::error_code ignored;
      std::filesystem::remove(path_, ignored);
    }
  }

  TemporaryOutput(const TemporaryOutput &) = delete;
  TemporaryOutput &operator=(const TemporaryOutput &) = delete;

  [[nodiscard]] const std::filesystem::path &path() const noexcept {
    return path_;
  }
  void commit() noexcept { committed_ = true; }

private:
  std::filesystem::path path_;
  bool committed_{false};
};

std::expected<void, LocalRuntimeError>
write_output_atomically(const std::string_view output_name,
                        const std::span<const std::byte> data) {
  const std::filesystem::path output = path_from_utf8(output_name);
  std::filesystem::path directory = output.parent_path();
  if (directory.empty()) {
    directory = std::filesystem::path{"."};
  }

  static std::atomic<std::uint64_t> sequence{0};
  std::filesystem::path temporary;
  std::ofstream stream;
  for (std::uint32_t attempt = 0; attempt < 32U; ++attempt) {
    const std::uint64_t clock = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const std::uint64_t random =
        (static_cast<std::uint64_t>(std::random_device{}()) << 32U) ^
        static_cast<std::uint64_t>(std::random_device{}());
    const std::uint64_t token =
        clock ^ random ^ sequence.fetch_add(1U, std::memory_order_relaxed) ^
        attempt;
    std::array<char, 17> encoded{};
    const auto [end, error] =
        std::to_chars(encoded.data(), encoded.data() + 16, token, 16);
    if (error != std::errc{}) {
      return std::unexpected(LocalRuntimeError{
          KB_E_INTERNAL, "failed to create a temporary output name"});
    }
    const std::string name =
        ".kairosboot-" + std::string{encoded.data(), end} + ".tmp";
    temporary = directory / path_from_utf8(name);
    stream.open(temporary,
                std::ios::binary | std::ios::out | std::ios::noreplace);
    if (stream) {
      break;
    }

    // `noreplace` makes creation atomic: an existing file or symlink is never
    // opened or truncated. Retry only an actual name collision; other failures
    // (missing directory, permissions, and unsupported paths) are actionable.
    stream.clear();
    std::error_code exists_error;
    if (!std::filesystem::exists(temporary, exists_error) || exists_error) {
      return std::unexpected(LocalRuntimeError{
          KB_E_IO, "cannot create temporary output beside: " +
                       std::string{output_name}});
    }
    temporary.clear();
  }
  if (temporary.empty()) {
    return std::unexpected(LocalRuntimeError{
        KB_E_IO, "cannot allocate a temporary output beside: " +
                     std::string{output_name}});
  }

  TemporaryOutput cleanup{std::move(temporary)};
  constexpr std::size_t maximum_chunk = 1024U * 1024U;
  std::size_t offset = 0;
  while (offset < data.size()) {
    const std::size_t chunk = std::min(maximum_chunk, data.size() - offset);
    stream.write(reinterpret_cast<const char *>(data.data() + offset),
                 static_cast<std::streamsize>(chunk));
    if (!stream) {
      return std::unexpected(LocalRuntimeError{
          KB_E_IO, "failed while writing temporary output for: " +
                       std::string{output_name}});
    }
    offset += chunk;
  }
  stream.close();
  if (!stream) {
    return std::unexpected(LocalRuntimeError{
        KB_E_IO, "failed while closing temporary output for: " +
                     std::string{output_name}});
  }

  std::error_code rename_error;
#if defined(_WIN32)
  if (MoveFileExW(cleanup.path().c_str(), output.c_str(),
                  MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
    rename_error = std::error_code{static_cast<int>(GetLastError()),
                                   std::system_category()};
  }
#else
  std::filesystem::rename(cleanup.path(), output, rename_error);
#endif
  if (rename_error) {
    return std::unexpected(LocalRuntimeError{
        KB_E_IO, "cannot atomically publish output " +
                     std::string{output_name} + " (native error " +
                     std::to_string(rename_error.value()) + ")"});
  }
  cleanup.commit();
  return {};
}

std::expected<kairosboot::CommandResult, kairosboot::Error>
execute_typed_command(kairosboot::Context &context,
                      const Invocation &invocation,
                      const std::span<const std::byte> stage_data) {
  const auto selector = selector_view(invocation.global);
  const auto options = command_options(invocation.global);
  switch (invocation.kind) {
  case CommandKind::Getvar:
    return context.getvar(selector, invocation.first, options);
  case CommandKind::Erase:
    return context.erase(selector, invocation.first, options);
  case CommandKind::SetActive:
    return context.set_active(selector, invocation.first, options);
  case CommandKind::Reboot:
    return context.reboot(selector, invocation.reboot_target, options);
  case CommandKind::Continue:
    return context.continue_boot(selector, options);
  case CommandKind::Oem:
    return context.oem(selector, invocation.joined, options);
  case CommandKind::Raw:
    return context.raw_command(selector, invocation.joined, options);
  case CommandKind::BootStaged:
    return context.boot(selector, options);
  case CommandKind::Stage: {
    auto operation = context.stage_async(selector, stage_data, options);
    if (!operation) {
      return std::unexpected(std::move(operation.error()));
    }
    return operation->wait_result();
  }
  case CommandKind::Upload:
    return context.upload(selector, options);
  case CommandKind::Fetch:
    return context.fetch(selector, invocation.first, invocation.fetch_range,
                         options);
  case CommandKind::Version:
  case CommandKind::Help:
  case CommandKind::Doctor:
  case CommandKind::Devices:
  case CommandKind::Flash:
  case CommandKind::Flashing:
  case CommandKind::Gsi:
  case CommandKind::SnapshotUpdate:
  case CommandKind::CreateLogicalPartition:
  case CommandKind::DeleteLogicalPartition:
  case CommandKind::ResizeLogicalPartition:
    break;
  }
  std::terminate();
}

std::expected<kairosboot::Operation, kairosboot::Error>
start_management_command(kairosboot::Context &context,
                         const Invocation &invocation) {
  const auto selector = selector_view(invocation.global);
  const auto options = command_options(invocation.global);
  switch (invocation.kind) {
  case CommandKind::Flashing:
    return context.flashing_async(selector, invocation.flashing_command,
                                  options);
  case CommandKind::Gsi:
    return context.gsi_async(selector, invocation.gsi_command, options);
  case CommandKind::SnapshotUpdate:
    return context.snapshot_update_async(
        selector, invocation.snapshot_update_command, options);
  case CommandKind::CreateLogicalPartition:
    return context.create_logical_partition_async(
        selector, invocation.first, invocation.logical_partition_size,
        options);
  case CommandKind::DeleteLogicalPartition:
    return context.delete_logical_partition_async(selector, invocation.first,
                                                  options);
  case CommandKind::ResizeLogicalPartition:
    return context.resize_logical_partition_async(
        selector, invocation.first, invocation.logical_partition_size,
        options);
  case CommandKind::Version:
  case CommandKind::Help:
  case CommandKind::Doctor:
  case CommandKind::Devices:
  case CommandKind::Flash:
  case CommandKind::Getvar:
  case CommandKind::Erase:
  case CommandKind::SetActive:
  case CommandKind::Reboot:
  case CommandKind::Continue:
  case CommandKind::Oem:
  case CommandKind::Raw:
  case CommandKind::BootStaged:
  case CommandKind::Stage:
  case CommandKind::Upload:
  case CommandKind::Fetch:
    break;
  }
  std::terminate();
}

void print_result_messages(const kairosboot::CommandResult &result) {
  for (std::size_t index = 0; index < result.message_count(); ++index) {
    const auto message = result.message(index);
    if (!message.has_value()) {
      continue;
    }
    std::cout << (message->kind == kairosboot::CommandMessageKind::Info
                      ? "INFO"
                      : "TEXT")
              << ": " << escape_binary_for_text(message->payload) << '\n';
  }
}

void print_json_result_messages(const kairosboot::CommandResult &result) {
  std::cout << '[';
  bool first = true;
  for (std::size_t index = 0; index < result.message_count(); ++index) {
    const auto message = result.message(index);
    if (!message.has_value()) {
      continue;
    }
    if (!first) {
      std::cout << ',';
    }
    first = false;
    std::cout << "{\"kind\":\""
              << (message->kind == kairosboot::CommandMessageKind::Info
                      ? "INFO"
                      : "TEXT")
              << "\",\"base64\":\"" << base64_encode(message->payload)
              << "\",\"bytes\":" << message->payload.size() << '}';
  }
  std::cout << ']';
}

int print_command_success(const Invocation &invocation,
                          const kairosboot::CommandResult &result,
                          const std::optional<std::string_view> output) {
  if (invocation.global.json) {
    std::cout << "{\"ok\":true,\"command\":\"" << command_name(invocation.kind)
              << "\",\"device\":\"" << json_escape(result.device_identifier())
              << "\",\"terminal\":{\"base64\":\""
              << base64_encode(result.terminal_payload())
              << "\",\"bytes\":" << result.terminal_payload().size()
              << "},\"messages\":";
    print_json_result_messages(result);
    std::cout << ",\"dataBytes\":" << result.data().size();
    if (output.has_value()) {
      std::cout << ",\"output\":\"" << json_escape(*output) << '\"';
    }
    std::cout << "}\n";
    return 0;
  }

  print_result_messages(result);
  if (output.has_value()) {
    std::cout << "Wrote " << result.data().size() << " bytes to " << *output
              << '\n';
  } else if (!result.terminal_payload().empty()) {
    std::cout << escape_binary_for_text(result.terminal_payload()) << '\n';
  } else {
    std::cout << command_name(invocation.kind) << " completed\n";
  }
  return 0;
}

int flash_file(const Invocation &invocation) {
  auto context = kairosboot::Context::create();
  if (!context) {
    return print_runtime_error(context.error(), invocation.global.json);
  }
  const auto result = context->flash_file(
      selector_view(invocation.global), invocation.first,
      path_from_utf8(invocation.second), flash_options(invocation.global));
  if (!result) {
    return print_runtime_error(result.error(), invocation.global.json);
  }
  if (invocation.global.json) {
    std::cout << "{\"ok\":true,\"command\":\"flash\",\"partition\":\""
              << json_escape(invocation.first) << "\",\"file\":\""
              << json_escape(invocation.second) << "\"}\n";
  } else {
    std::cout << "Flashed " << invocation.first << " from " << invocation.second
              << '\n';
  }
  return 0;
}

int run_typed_command(const Invocation &invocation) {
  std::vector<std::byte> stage_data;
  if (invocation.kind == CommandKind::Stage) {
    auto loaded = read_stage_file(invocation.first);
    if (!loaded) {
      return print_local_runtime_error(loaded.error(), invocation.global.json);
    }
    stage_data = std::move(*loaded);
  }

  auto context = kairosboot::Context::create();
  if (!context) {
    return print_runtime_error(context.error(), invocation.global.json);
  }
  auto result = execute_typed_command(*context, invocation, stage_data);
  if (!result) {
    return print_runtime_error(result.error(), invocation.global.json);
  }

  std::optional<std::string_view> output;
  if (invocation.kind == CommandKind::Upload) {
    output = invocation.first;
  } else if (invocation.kind == CommandKind::Fetch) {
    output = invocation.second;
  }
  if (output.has_value()) {
    auto written = write_output_atomically(*output, result->data());
    if (!written) {
      return print_local_runtime_error(written.error(), invocation.global.json);
    }
  }
  return print_command_success(invocation, *result, output);
}

int run_management_command(const Invocation &invocation) {
  auto context = kairosboot::Context::create();
  if (!context) {
    return print_runtime_error(context.error(), invocation.global.json);
  }

  InterruptCancellation cancellation;
  auto operation = start_management_command(*context, invocation);
  if (!operation) {
    return print_runtime_error(operation.error(), invocation.global.json);
  }
  auto result = operation->wait_result(cancellation.token());
  if (!result) {
    return print_runtime_error(result.error(), invocation.global.json);
  }
  return print_command_success(invocation, *result, std::nullopt);
}

constexpr std::string_view usage_text() noexcept {
  return "Usage:\n"
               "  kairosboot --version [--json]\n"
               "  kairosboot --help [--json]\n"
               "  kairosboot doctor [--json]\n"
               "  kairosboot devices [--json]\n"
               "  kairosboot [global options] flash <partition> <file>\n"
               "  kairosboot [global options] getvar <variable>\n"
               "  kairosboot [global options] erase <partition>\n"
               "  kairosboot [global options] set-active <slot>\n"
               "  kairosboot [global options] reboot "
               "[system|bootloader|recovery|fastboot]\n"
               "  kairosboot [global options] continue\n"
               "  kairosboot [global options] oem <command...>\n"
               "  kairosboot [global options] raw <command...>\n"
               "  kairosboot [global options] boot-staged\n"
               "  kairosboot [global options] stage <file>\n"
               "  kairosboot [global options] upload <output>\n"
               "  kairosboot [global options] fetch <partition> <output> "
               "[--offset <bytes>] [--size <bytes>]\n"
               "  kairosboot [global options] flashing "
               "<lock|unlock|lock-critical|unlock-critical|get-unlock-ability>\n"
               "  kairosboot [global options] gsi <wipe|disable|status>\n"
               "  kairosboot [global options] snapshot-update <cancel|merge>\n"
               "  kairosboot [global options] create-logical-partition "
               "<partition> <size-bytes>\n"
               "  kairosboot [global options] delete-logical-partition "
               "<partition>\n"
               "  kairosboot [global options] resize-logical-partition "
               "<partition> <size-bytes>\n"
               "Global options:\n"
               "  --device <selector> | --serial <id>\n"
               "  --json --timeout-ms <milliseconds> "
               "--max-receive-bytes <bytes>\n";
}

void print_usage(std::ostream &output) { output << usage_text(); }

int print_help(const bool json) {
  if (json) {
    std::cout << "{\"ok\":true,\"command\":\"help\",\"usage\":\""
              << json_escape(usage_text()) << "\"}\n";
  } else {
    print_usage(std::cout);
  }
  return 0;
}

bool json_requested(const int argc, char **argv) noexcept {
  for (int index = 1; index < argc; ++index) {
    if (std::string_view{argv[index]} == "--json") {
      return true;
    }
  }
  return false;
}

#if defined(_WIN32)
bool json_requested(const int argc, wchar_t **argv) noexcept {
  for (int index = 1; index < argc; ++index) {
    if (std::wstring_view{argv[index]} == L"--json") {
      return true;
    }
  }
  return false;
}
#endif

int run_cli(const int argc, char **argv) {
  // Preserve the original trailing-JSON spellings while requiring all global
  // options before the command for the general parser.
  if (argc == 2 && std::string_view{argv[1]} == "--version") {
    return print_version(false);
  }
  if (argc == 3 && std::string_view{argv[1]} == "--version" &&
      std::string_view{argv[2]} == "--json") {
    return print_version(true);
  }
  if (argc == 3 &&
      (std::string_view{argv[1]} == "--help" ||
       std::string_view{argv[1]} == "help") &&
      std::string_view{argv[2]} == "--json") {
    return print_help(true);
  }
  if (argc == 3 && std::string_view{argv[1]} == "doctor" &&
      std::string_view{argv[2]} == "--json") {
    return doctor(true);
  }
  if (argc == 3 && std::string_view{argv[1]} == "devices" &&
      std::string_view{argv[2]} == "--json") {
    return print_devices(true);
  }

  const auto invocation = parse_invocation(argc, argv);
  if (!invocation) {
    const int result = print_parse_error(invocation.error());
    if (!invocation.error().json) {
      print_usage(std::cerr);
    }
    return result;
  }

  switch (invocation->kind) {
  case CommandKind::Version:
    return print_version(invocation->global.json);
  case CommandKind::Help:
    return print_help(invocation->global.json);
  case CommandKind::Doctor:
    return doctor(invocation->global.json);
  case CommandKind::Devices:
    return print_devices(invocation->global.json);
  case CommandKind::Flash:
    return flash_file(*invocation);
  case CommandKind::Getvar:
  case CommandKind::Erase:
  case CommandKind::SetActive:
  case CommandKind::Reboot:
  case CommandKind::Continue:
  case CommandKind::Oem:
  case CommandKind::Raw:
  case CommandKind::BootStaged:
  case CommandKind::Stage:
  case CommandKind::Upload:
  case CommandKind::Fetch:
    return run_typed_command(*invocation);
  case CommandKind::Flashing:
  case CommandKind::Gsi:
  case CommandKind::SnapshotUpdate:
  case CommandKind::CreateLogicalPartition:
  case CommandKind::DeleteLogicalPartition:
  case CommandKind::ResizeLogicalPartition:
    return run_management_command(*invocation);
  }
  return 4;
}

} // namespace

int run_cli_with_exception_boundary(const int argc, char **argv) {
  try {
    return run_cli(argc, argv);
  } catch (const std::bad_alloc &) {
    return print_local_runtime_error(
        LocalRuntimeError{KB_E_OUT_OF_MEMORY, "out of memory"},
        json_requested(argc, argv));
  } catch (const std::exception &exception) {
    return print_local_runtime_error(
        LocalRuntimeError{KB_E_INTERNAL,
                          std::string{"unexpected CLI failure: "} +
                              exception.what()},
        json_requested(argc, argv));
  } catch (...) {
    return print_local_runtime_error(
        LocalRuntimeError{KB_E_INTERNAL, "unexpected CLI failure"},
        json_requested(argc, argv));
  }
}

#if defined(_WIN32)
int wmain(const int argc, wchar_t **argv) {
  const bool json = json_requested(argc, argv);
  try {
    std::vector<std::string> arguments;
    arguments.reserve(static_cast<std::size_t>(argc));
    for (int index = 0; index < argc; ++index) {
      auto converted = utf8_from_wide(argv[index]);
      if (!converted) {
        return print_local_runtime_error(converted.error(), json);
      }
      arguments.push_back(std::move(*converted));
    }

    std::vector<char *> pointers;
    pointers.reserve(arguments.size());
    for (std::string &argument : arguments) {
      pointers.push_back(argument.data());
    }
    return run_cli_with_exception_boundary(argc, pointers.data());
  } catch (const std::bad_alloc &) {
    return print_local_runtime_error(
        LocalRuntimeError{KB_E_OUT_OF_MEMORY, "out of memory"}, json);
  } catch (const std::exception &exception) {
    return print_local_runtime_error(
        LocalRuntimeError{KB_E_INTERNAL,
                          std::string{"unexpected CLI failure: "} +
                              exception.what()},
        json);
  } catch (...) {
    return print_local_runtime_error(
        LocalRuntimeError{KB_E_INTERNAL, "unexpected CLI failure"}, json);
  }
}
#else
int main(const int argc, char **argv) {
  return run_cli_with_exception_boundary(argc, argv);
}
#endif
