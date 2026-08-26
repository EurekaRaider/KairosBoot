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
#include <string>
#include <string_view>
#include <system_error>
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
};

struct Invocation {
  GlobalOptions global;
  CommandKind kind{CommandKind::Version};
  std::string_view first;
  std::string_view second;
  std::string joined;
  kairosboot::RebootTarget reboot_target{kairosboot::RebootTarget::System};
  kairosboot::FetchRange fetch_range;
};

struct ParseError {
  bool json{false};
  std::string message;
};

struct LocalRuntimeError {
  kb_status_t status{KB_E_INTERNAL};
  std::string message;
};

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
  if (command.starts_with("--") && command != "--version") {
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
      return std::move(*rejected);
    }
    return result;
  }
  if (command == "doctor") {
    result.kind = CommandKind::Doctor;
    if (argc - command_index != 1) {
      return error("doctor does not accept operands");
    }
    if (const auto rejected = reject_non_json_globals()) {
      return std::move(*rejected);
    }
    return result;
  }
  if (command == "devices") {
    result.kind = CommandKind::Devices;
    if (argc - command_index != 1) {
      return error("devices does not accept operands");
    }
    if (const auto rejected = reject_non_json_globals()) {
      return std::move(*rejected);
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
  std::error_code exists_error;
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
    exists_error.clear();
    if (!std::filesystem::exists(temporary, exists_error) && !exists_error) {
      break;
    }
    temporary.clear();
  }
  if (temporary.empty()) {
    return std::unexpected(LocalRuntimeError{
        KB_E_IO, "cannot allocate a temporary output beside: " +
                     std::string{output_name}});
  }

  TemporaryOutput cleanup{temporary};
  std::ofstream stream{temporary, std::ios::binary | std::ios::trunc};
  if (!stream) {
    return std::unexpected(
        LocalRuntimeError{KB_E_IO, "cannot create temporary output beside: " +
                                       std::string{output_name}});
  }
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
  if (MoveFileExW(temporary.c_str(), output.c_str(),
                  MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
    rename_error = std::error_code{static_cast<int>(GetLastError()),
                                   std::system_category()};
  }
#else
  std::filesystem::rename(temporary, output, rename_error);
#endif
  if (rename_error) {
    return std::unexpected(LocalRuntimeError{
        KB_E_IO, "cannot atomically publish output " +
                     std::string{output_name} + ": " + rename_error.message()});
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
  case CommandKind::Doctor:
  case CommandKind::Devices:
  case CommandKind::Flash:
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

void print_usage() {
  std::cerr << "Usage:\n"
               "  kairosboot --version [--json]\n"
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
               "Global options:\n"
               "  --device <selector> | --serial <id>\n"
               "  --json --timeout-ms <milliseconds> "
               "--max-receive-bytes <bytes>\n";
}

bool json_requested(const int argc, char **argv) noexcept {
  for (int index = 1; index < argc; ++index) {
    if (std::string_view{argv[index]} == "--json") {
      return true;
    }
  }
  return false;
}

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
      print_usage();
    }
    return result;
  }

  switch (invocation->kind) {
  case CommandKind::Version:
    return print_version(invocation->global.json);
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
  }
  return 4;
}

} // namespace

int main(const int argc, char **argv) {
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
