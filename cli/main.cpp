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

#include "src/image/boot_image_builder.hpp"
#include "src/image/file_source.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <expected>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <mutex>
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
  std::optional<std::uint16_t> usb_vendor_id;
  bool verbose{};
  std::uint64_t maximum_receive_bytes{kDefaultMaximumReceiveBytes};
  bool maximum_receive_bytes_set{false};
  bool disable_verity{};
  bool disable_verification{};
  std::uint64_t sparse_limit_bytes{};
  bool sparse_limit_set{false};
  bool legacy_boot_options_set{false};
  std::optional<std::string> slot;
  bool set_active{};
  std::optional<std::string> active_slot;
};

kairosboot::ContextOptions context_options(const GlobalOptions &options);

enum class CommandKind : std::uint8_t {
  Version,
  Help,
  Doctor,
  Devices,
  Validate,
  Plan,
  Run,
  Flash,
  FlashRaw,
  Signature,
  Update,
  Flashall,
  MakeBootImage,
  WipeSuper,
  Getvar,
  Erase,
  Format,
  SetActive,
  Reboot,
  Continue,
  Oem,
  Raw,
  BootFile,
  BootStaged,
  Stage,
  Upload,
  GetStaged,
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
  std::string_view third;
  std::string_view fourth;
  std::string joined;
  kairosboot::image::LegacyBootImageOptions boot_image_options;
  kairosboot::RebootTarget reboot_target{kairosboot::RebootTarget::System};
  kairosboot::FlashingCommand flashing_command{
      kairosboot::FlashingCommand::Lock};
  kairosboot::GsiCommand gsi_command{kairosboot::GsiCommand::Wipe};
  kairosboot::SnapshotUpdateCommand snapshot_update_command{
      kairosboot::SnapshotUpdateCommand::Cancel};
  kairosboot::FetchRange fetch_range;
  std::uint64_t logical_partition_size{};
  std::string filesystem_type;
  std::uint64_t format_partition_size{};
  bool wipe{};
  bool skip_reboot{};
  bool skip_secondary{};
  bool exclude_dynamic_partitions{};
  bool disable_fastboot_info{};
  bool plan_digest{};
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
         value == "-i" || value == "--vendor-id" ||
         value == "-v" || value == "--verbose" ||
         value == "--timeout-ms" || value == "--max-receive-bytes" ||
         value == "--disable-verity" ||
         value == "--disable-verification" ||
         value == "-S" ||
         value == "--base" || value == "--cmdline" ||
         value == "--page-size" || value == "--kernel-offset" ||
         value == "--ramdisk-offset" || value == "--second-offset" ||
         value == "--tags-offset" || value == "--slot" ||
         value == "-a" || value == "--set-active" ||
         value.starts_with("--set-active=");
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

bool parse_sparse_limit(const std::string_view text,
                        std::uint64_t &value) noexcept {
  if (text.empty()) {
    return false;
  }
  std::string_view digits = text;
  if (digits.front() == '+') {
    digits.remove_prefix(1U);
  }
  std::uint64_t multiplier = 1U;
  if (!digits.empty()) {
    switch (digits.back()) {
    case 'K':
    case 'k':
      multiplier = 1024ULL;
      digits.remove_suffix(1U);
      break;
    case 'M':
    case 'm':
      multiplier = 1024ULL * 1024ULL;
      digits.remove_suffix(1U);
      break;
    case 'G':
    case 'g':
      multiplier = 1024ULL * 1024ULL * 1024ULL;
      digits.remove_suffix(1U);
      break;
    default:
      break;
    }
  }
  std::uint64_t parsed{};
  if (!parse_unsigned_decimal(digits, parsed) ||
      parsed > std::numeric_limits<std::uint64_t>::max() / multiplier) {
    return false;
  }
  value = parsed * multiplier;
  return true;
}

bool parse_u32_auto(const std::string_view text, std::uint32_t &value) {
  if (text.empty()) {
    return false;
  }
  int base = 10;
  std::string_view digits = text;
  if (digits.starts_with("0x") || digits.starts_with("0X")) {
    base = 16;
    digits.remove_prefix(2U);
  }
  if (digits.empty()) {
    return false;
  }
  const auto [end, error] = std::from_chars(
      digits.data(), digits.data() + digits.size(), value, base);
  return error == std::errc{} && end == digits.data() + digits.size();
}

template <typename Integer>
bool parse_unsigned_number(const std::string_view text, Integer &value) {
  if (text.empty()) {
    return false;
  }
  int base = 10;
  auto first = text.data();
  if (text.size() > 2U && text[0] == '0' &&
      (text[1] == 'x' || text[1] == 'X')) {
    base = 16;
    first += 2;
  }
  if (first == text.data() + text.size()) {
    return false;
  }
  const auto [end, error] =
      std::from_chars(first, text.data() + text.size(), value, base);
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
  bool vendor_id_seen = false;
  bool verbose_seen = false;
  bool cmdline_seen = false;
  bool base_seen = false;
  bool page_size_seen = false;
  bool kernel_offset_seen = false;
  bool ramdisk_offset_seen = false;
  bool second_offset_seen = false;
  bool tags_offset_seen = false;
  bool slot_seen = false;
  bool set_active_seen = false;
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
    if (argument == "-v" || argument == "--verbose") {
      if (verbose_seen) {
        return error("option --verbose may only be specified once");
      }
      result.global.verbose = true;
      verbose_seen = true;
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
    if (argument == "-i" || argument == "--vendor-id") {
      if (vendor_id_seen) {
        return error("option " + std::string{argument} +
                     " may only be specified once");
      }
      if (index + 1 >= argc) {
        return error("option " + std::string{argument} +
                     " requires a USB vendor id in [1, 0xffff]");
      }
      std::uint32_t parsed = 0;
      if (!parse_u32_auto(argv[index + 1], parsed) || parsed == 0U ||
          parsed > std::numeric_limits<std::uint16_t>::max()) {
        return error("option " + std::string{argument} +
                     " requires a USB vendor id in [1, 0xffff]");
      }
      result.global.usb_vendor_id = static_cast<std::uint16_t>(parsed);
      vendor_id_seen = true;
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
    if (argument == "--disable-verity") {
      result.global.disable_verity = true;
      ++index;
      continue;
    }
    if (argument == "--disable-verification") {
      result.global.disable_verification = true;
      ++index;
      continue;
    }
    if (argument == "-S") {
      if (result.global.sparse_limit_set) {
        return error("option -S may only be specified once");
      }
      if (index + 1 >= argc ||
          !parse_sparse_limit(argv[index + 1],
                              result.global.sparse_limit_bytes)) {
        return error("option -S requires SIZE[K|M|G]");
      }
      result.global.sparse_limit_set = true;
      index += 2;
      continue;
    }
    if (argument == "--slot") {
      if (slot_seen) {
        return error("option --slot may only be specified once");
      }
      if (index + 1 >= argc || argv[index + 1][0] == '\0' ||
          std::string_view{argv[index + 1]}.starts_with("--")) {
        return error("option --slot requires a non-empty slot");
      }
      result.global.slot = std::string{argv[index + 1]};
      slot_seen = true;
      index += 2;
      continue;
    }
    if (argument == "-a" || argument == "--set-active" ||
        argument.starts_with("--set-active=")) {
      if (set_active_seen) {
        return error("option --set-active may only be specified once");
      }
      result.global.set_active = true;
      set_active_seen = true;
      if (argument.starts_with("--set-active=")) {
        const auto value = argument.substr(
            std::string_view{"--set-active="}.size());
        if (value.empty()) {
          return error("option --set-active requires a non-empty slot after '='");
        }
        result.global.active_slot = std::string{value};
      }
      ++index;
      continue;
    }
    bool *legacy_seen = nullptr;
    std::uint32_t *legacy_number = nullptr;
    if (argument == "--cmdline") {
      legacy_seen = &cmdline_seen;
    } else if (argument == "--base") {
      legacy_seen = &base_seen;
      legacy_number = &result.boot_image_options.base;
    } else if (argument == "--page-size") {
      legacy_seen = &page_size_seen;
      legacy_number = &result.boot_image_options.page_size;
    } else if (argument == "--kernel-offset") {
      legacy_seen = &kernel_offset_seen;
      legacy_number = &result.boot_image_options.kernel_offset;
    } else if (argument == "--ramdisk-offset") {
      legacy_seen = &ramdisk_offset_seen;
      legacy_number = &result.boot_image_options.ramdisk_offset;
    } else if (argument == "--second-offset") {
      legacy_seen = &second_offset_seen;
      legacy_number = &result.boot_image_options.second_offset;
    } else if (argument == "--tags-offset") {
      legacy_seen = &tags_offset_seen;
      legacy_number = &result.boot_image_options.tags_offset;
    }
    if (legacy_seen != nullptr) {
      if (*legacy_seen) {
        return error("option " + std::string{argument} +
                     " may only be specified once");
      }
      if (index + 1 >= argc) {
        return error("option " + std::string{argument} +
                     " requires a value");
      }
      const std::string_view value{argv[index + 1]};
      if (legacy_number == nullptr) {
        result.boot_image_options.command_line = value;
      } else if (!parse_u32_auto(value, *legacy_number)) {
        return error("option " + std::string{argument} +
                     " requires a uint32 decimal or 0x hexadecimal value");
      }
      *legacy_seen = true;
      result.global.legacy_boot_options_set = true;
      index += 2;
      continue;
    }
    break;
  }

  if (index >= argc) {
    return error("unknown command");
  }
  const std::string_view command{argv[index]};
  if (command.starts_with("-") && command != "--version" &&
      command != "--help" && command != "-h") {
    return error("unknown option " + std::string{command});
  }
  const int command_index = index++;
  for (int operand = index; operand < argc; ++operand) {
    if (command != "make-boot-image" && is_global_option(argv[operand])) {
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
    if (result.global.usb_vendor_id.has_value() && command != "doctor" &&
        command != "devices") {
      return error("option --vendor-id is not valid for " +
                   std::string{command});
    }
    if (result.global.legacy_boot_options_set) {
      return error("legacy boot layout options are not valid for " +
                   std::string{command});
    }
    return std::nullopt;
  };

  if (result.global.legacy_boot_options_set && command != "boot" &&
      command != "flash:raw") {
    return error("legacy boot layout options are not valid for " +
                 std::string{command});
  }
  if ((result.global.slot.has_value() || result.global.set_active) &&
      command != "flash" && command != "flash:raw" && command != "update" &&
      command != "flashall") {
    return error("options --slot and --set-active are valid only for flash, "
                 "flash:raw, update, and flashall");
  }
  if (result.global.sparse_limit_set && command != "flash" &&
      command != "flash:raw" && command != "update" &&
      command != "flashall") {
    return error("option -S is not valid for " + std::string{command});
  }

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
  if (command == "--help" || command == "-h" || command == "help") {
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
  if (command == "validate") {
    result.kind = CommandKind::Validate;
    if (argc - command_index != 2) {
      return error("validate requires exactly <manifest>");
    }
    result.first = argv[index];
    if (result.first.empty()) {
      return error("validate manifest must not be empty");
    }
    if (const auto rejected = reject_non_json_globals()) {
      return *rejected;
    }
    return result;
  }
  if (command == "plan") {
    result.kind = CommandKind::Plan;
    if (index >= argc) {
      return error("plan requires exactly <manifest>");
    }
    result.first = argv[index++];
    if (result.first == "--digest") {
      return error("plan requires exactly <manifest>");
    }
    if (result.first.empty()) {
      return error("plan manifest must not be empty");
    }
    while (index < argc) {
      const std::string_view option{argv[index++]};
      if (option != "--digest") {
        return error("plan supports only --digest after <manifest>");
      }
      if (result.plan_digest) {
        return error("plan option --digest may only be specified once");
      }
      result.plan_digest = true;
    }
    if (result.global.json) {
      return error("option --json is not valid for plan");
    }
    if (const auto rejected = reject_non_json_globals()) {
      return *rejected;
    }
    return result;
  }
  if (command == "run") {
    result.kind = CommandKind::Run;
    if (argc - command_index != 2) {
      return error("run requires exactly <manifest>");
    }
    result.first = argv[index];
    if (result.first.empty()) {
      return error("run manifest must not be empty");
    }
    if (result.global.selector.has_value()) {
      return error("option " + std::string{result.global.selector_option} +
                   " is not valid for run");
    }
    if (result.global.maximum_receive_bytes_set) {
      return error("option --max-receive-bytes is not valid for run");
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
  if (command == "flash:raw") {
    result.kind = CommandKind::FlashRaw;
    const int operand_count = argc - command_index - 1;
    if (operand_count < 2 || operand_count > 4) {
      return error(
          "flash:raw requires <partition> <kernel> [ramdisk [second]]");
    }
    if (result.global.maximum_receive_bytes_set) {
      return error("option --max-receive-bytes is not valid for flash:raw");
    }
    result.first = argv[index++];
    result.second = argv[index++];
    if (index < argc) {
      result.third = argv[index++];
    }
    if (index < argc) {
      result.fourth = argv[index];
    }
    if (result.first.empty()) {
      return error("flash:raw partition must not be empty");
    }
    if (result.second.empty()) {
      return error("flash:raw kernel must not be empty");
    }
    if (operand_count >= 3 && result.third.empty()) {
      return error("flash:raw ramdisk must not be empty");
    }
    if (operand_count == 4 && result.fourth.empty()) {
      return error("flash:raw second stage must not be empty");
    }
    return result;
  }
  if (command == "boot") {
    result.kind = CommandKind::BootFile;
    const int operand_count = argc - command_index - 1;
    if (operand_count < 1 || operand_count > 3) {
      return error("boot requires <kernel> [ramdisk [second]]");
    }
    if (result.global.maximum_receive_bytes_set) {
      return error("option --max-receive-bytes is not valid for boot");
    }
    result.first = argv[index++];
    if (index < argc) {
      result.second = argv[index++];
    }
    if (index < argc) {
      result.third = argv[index];
    }
    if (result.first.empty()) {
      return error("boot kernel or image must not be empty");
    }
    if (operand_count >= 2 && result.second.empty()) {
      return error("boot ramdisk must not be empty");
    }
    if (operand_count == 3 && result.third.empty()) {
      return error("boot second stage must not be empty");
    }
    return result;
  }
  if (command == "update" || command == "flashall") {
    const bool is_flashall = command == "flashall";
    result.kind = is_flashall ? CommandKind::Flashall : CommandKind::Update;
    if (!is_flashall) {
      if (index >= argc || std::string_view{argv[index]}.starts_with("--")) {
        return error("update requires <package>");
      }
      result.first = argv[index++];
      if (result.first.empty()) {
        return error("update package must not be empty");
      }
    }
    while (index < argc) {
      const std::string_view option{argv[index++]};
      bool *flag = nullptr;
      if (option == "--wipe") {
        flag = &result.wipe;
      } else if (option == "--skip-reboot") {
        flag = &result.skip_reboot;
      } else if (option == "--skip-secondary") {
        flag = &result.skip_secondary;
      } else if (option == "--exclude-dynamic-partitions") {
        flag = &result.exclude_dynamic_partitions;
      } else if (option == "--disable-fastboot-info") {
        flag = &result.disable_fastboot_info;
      } else {
        return error(std::string{command} +
                     " supports only --wipe, --skip-reboot, --skip-secondary, "
                     "--exclude-dynamic-partitions and --disable-fastboot-info" +
                     (is_flashall ? "" : " after <package>"));
      }
      if (*flag) {
        return error(std::string{command} + " option " + std::string{option} +
                     " may only be specified once");
      }
      *flag = true;
    }
    if (result.global.maximum_receive_bytes_set) {
      return error("option --max-receive-bytes is not valid for " +
                   std::string{command});
    }
    return result;
  }
  if (command == "make-boot-image") {
    result.kind = CommandKind::MakeBootImage;
    if (index + 1 >= argc) {
      return error(
          "make-boot-image requires <output> <kernel> [ramdisk [second]]");
    }
    result.first = argv[index++];
    result.second = argv[index++];
    if (result.first.empty() || result.second.empty()) {
      return error("make-boot-image output and kernel must not be empty");
    }
    if (index < argc && !std::string_view{argv[index]}.starts_with("--")) {
      result.third = argv[index++];
      if (result.third.empty()) {
        return error("make-boot-image ramdisk must not be empty");
      }
    }
    if (index < argc && !std::string_view{argv[index]}.starts_with("--")) {
      result.fourth = argv[index++];
      if (result.fourth.empty()) {
        return error("make-boot-image second must not be empty");
      }
    }

    bool cmdline_seen = false;
    bool base_seen = false;
    bool page_size_seen = false;
    bool kernel_offset_seen = false;
    bool ramdisk_offset_seen = false;
    bool second_offset_seen = false;
    bool tags_offset_seen = false;
    while (index < argc) {
      const std::string_view option{argv[index++]};
      bool *seen = nullptr;
      std::uint32_t *number = nullptr;
      if (option == "--cmdline") {
        seen = &cmdline_seen;
      } else if (option == "--base") {
        seen = &base_seen;
        number = &result.boot_image_options.base;
      } else if (option == "--page-size") {
        seen = &page_size_seen;
        number = &result.boot_image_options.page_size;
      } else if (option == "--kernel-offset") {
        seen = &kernel_offset_seen;
        number = &result.boot_image_options.kernel_offset;
      } else if (option == "--ramdisk-offset") {
        seen = &ramdisk_offset_seen;
        number = &result.boot_image_options.ramdisk_offset;
      } else if (option == "--second-offset") {
        seen = &second_offset_seen;
        number = &result.boot_image_options.second_offset;
      } else if (option == "--tags-offset") {
        seen = &tags_offset_seen;
        number = &result.boot_image_options.tags_offset;
      } else {
        return error("unknown make-boot-image option " + std::string{option});
      }
      if (*seen) {
        return error("make-boot-image option " + std::string{option} +
                     " may only be specified once");
      }
      *seen = true;
      if (index >= argc) {
        return error("make-boot-image option " + std::string{option} +
                     " requires a value");
      }
      const std::string_view value{argv[index++]};
      if (number == nullptr) {
        if (value.empty()) {
          return error("make-boot-image cmdline must not be empty");
        }
        result.boot_image_options.command_line = value;
      } else if (!parse_u32_auto(value, *number)) {
        return error("make-boot-image option " + std::string{option} +
                     " requires a uint32 decimal or 0x hexadecimal value");
      }
    }
    if (const auto rejected = reject_non_json_globals()) {
      return *rejected;
    }
    return result;
  }
  if (command == "wipe-super") {
    result.kind = CommandKind::WipeSuper;
    if (argc - command_index > 2) {
      return error("wipe-super accepts at most one <super_empty.img>");
    }
    if (argc - command_index == 2) {
      result.first = argv[index];
      if (result.first.empty()) {
        return error("wipe-super image must not be empty");
      }
    }
    if (result.global.maximum_receive_bytes_set) {
      return error("option --max-receive-bytes is not valid for wipe-super");
    }
    return result;
  }
  if (command == "format" || command.starts_with("format:")) {
    result.kind = CommandKind::Format;
    if (argc - command_index != 2) {
      return error(std::string{command} + " requires exactly <partition>");
    }
    if (result.global.maximum_receive_bytes_set) {
      return error("option --max-receive-bytes is not valid for format");
    }
    result.first = argv[index];
    if (result.first.empty()) {
      return error("format partition must not be empty");
    }
    if (command.size() > std::string_view{"format"}.size()) {
      const auto suffix = command.substr(std::string_view{"format:"}.size());
      const auto separator = suffix.find(':');
      const auto type = suffix.substr(0U, separator);
      const auto size = separator == std::string_view::npos
                            ? std::string_view{}
                            : suffix.substr(separator + 1U);
      if (separator != std::string_view::npos &&
          size.find(':') != std::string_view::npos) {
        return error("format accepts at most type and size overrides");
      }
      if (!type.empty() && type != "ext4" && type != "f2fs") {
        return error("format filesystem type must be ext4 or f2fs");
      }
      result.filesystem_type = std::string{type};
      if (!size.empty() &&
          (!parse_unsigned_number(size, result.format_partition_size) ||
           result.format_partition_size == 0U ||
           result.format_partition_size >
               static_cast<std::uint64_t>(
                   std::numeric_limits<std::int64_t>::max()))) {
        return error(
            "format size must be a non-zero decimal or 0x-prefixed byte count in [1, 9223372036854775807]");
      }
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
  if (command == "signature") {
    if (result.global.maximum_receive_bytes_set) {
      return error("option --max-receive-bytes is not valid for signature");
    }
    return parse_single_operand(CommandKind::Signature, "file");
  }
  if (command == "stage") {
    return parse_single_operand(CommandKind::Stage, "file");
  }
  if (command == "upload") {
    return parse_single_operand(CommandKind::Upload, "output");
  }
  if (command == "get-staged") {
    return parse_single_operand(CommandKind::GetStaged, "output");
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
  case CommandKind::Validate:
    return "validate";
  case CommandKind::Plan:
    return "plan";
  case CommandKind::Run:
    return "run";
  case CommandKind::Flash:
    return "flash";
  case CommandKind::FlashRaw:
    return "flash:raw";
  case CommandKind::Signature:
    return "signature";
  case CommandKind::Update:
    return "update";
  case CommandKind::Flashall:
    return "flashall";
  case CommandKind::MakeBootImage:
    return "make-boot-image";
  case CommandKind::WipeSuper:
    return "wipe-super";
  case CommandKind::Getvar:
    return "getvar";
  case CommandKind::Erase:
    return "erase";
  case CommandKind::Format:
    return "format";
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
  case CommandKind::BootFile:
    return "boot";
  case CommandKind::BootStaged:
    return "boot-staged";
  case CommandKind::Stage:
    return "stage";
  case CommandKind::Upload:
    return "upload";
  case CommandKind::GetStaged:
    return "get-staged";
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

int doctor(const GlobalOptions &options) {
  const bool json = options.json;
  const kairosboot::Version current = kairosboot::version();
  auto context = kairosboot::Context::create(context_options(options));
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

int print_devices(const GlobalOptions &options) {
  const bool json = options.json;
  auto context = kairosboot::Context::create(context_options(options));
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
  result.disable_verity = options.disable_verity;
  result.disable_verification = options.disable_verification;
  result.slot = options.slot;
  result.set_active = options.set_active;
  result.active_slot = options.active_slot;
  result.sparse_limit_bytes = options.sparse_limit_bytes;
  return result;
}

kairosboot::UpdateOptions update_options(const GlobalOptions &options) {
  kairosboot::UpdateOptions result;
  if (options.timeout_ms.has_value()) {
    result.timeout = std::chrono::milliseconds{*options.timeout_ms};
  }
  result.disable_verity = options.disable_verity;
  result.disable_verification = options.disable_verification;
  result.slot = options.slot;
  result.set_active = options.set_active;
  result.active_slot = options.active_slot;
  result.sparse_limit_bytes = options.sparse_limit_bytes;
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

template <typename Writer>
std::expected<void, LocalRuntimeError>
write_output_atomically_impl(const std::string_view output_name,
                             Writer &&writer) {
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
  if (auto written = writer(stream); !written) {
    return std::unexpected(std::move(written.error()));
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

std::expected<void, LocalRuntimeError> write_source_atomically(
    const std::string_view output_name,
    const kairosboot::image::IImageSource &source) {
  return write_output_atomically_impl(
      output_name, [&source, output_name](std::ofstream &stream)
                       -> std::expected<void, LocalRuntimeError> {
        std::array<std::byte, 1024U * 1024U> buffer{};
        std::uint64_t offset = 0U;
        while (offset < source.size()) {
          const auto requested = static_cast<std::size_t>(
              std::min<std::uint64_t>(buffer.size(), source.size() - offset));
          auto read = source.read_at(offset, std::span(buffer).first(requested));
          if (!read || *read == 0U || *read > requested) {
            return std::unexpected(LocalRuntimeError{
                KB_E_IO, "failed while reading constructed boot image"});
          }
          stream.write(reinterpret_cast<const char *>(buffer.data()),
                       static_cast<std::streamsize>(*read));
          if (!stream) {
            return std::unexpected(LocalRuntimeError{
                KB_E_IO, "failed while writing temporary output for: " +
                             std::string{output_name}});
          }
          offset += *read;
        }
        return {};
      });
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
  case CommandKind::BootFile:
    break;
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
    return context.upload_file(selector, path_from_utf8(invocation.first),
                               options);
  case CommandKind::GetStaged:
    return context.get_staged_file(selector,
                                   path_from_utf8(invocation.first), options);
  case CommandKind::Fetch:
    return context.fetch_file(selector, invocation.first,
                              path_from_utf8(invocation.second),
                              invocation.fetch_range, options);
  case CommandKind::Version:
  case CommandKind::Help:
  case CommandKind::Doctor:
  case CommandKind::Devices:
  case CommandKind::Validate:
  case CommandKind::Plan:
  case CommandKind::Run:
  case CommandKind::Flash:
  case CommandKind::FlashRaw:
  case CommandKind::Signature:
  case CommandKind::Update:
  case CommandKind::Flashall:
  case CommandKind::MakeBootImage:
  case CommandKind::WipeSuper:
  case CommandKind::Format:
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
  case CommandKind::FlashRaw:
  case CommandKind::Signature:
  case CommandKind::Update:
  case CommandKind::Flashall:
  case CommandKind::MakeBootImage:
  case CommandKind::WipeSuper:
  case CommandKind::Getvar:
  case CommandKind::Erase:
  case CommandKind::Format:
  case CommandKind::SetActive:
  case CommandKind::Reboot:
  case CommandKind::Continue:
  case CommandKind::Oem:
  case CommandKind::Raw:
  case CommandKind::BootFile:
  case CommandKind::BootStaged:
  case CommandKind::Stage:
  case CommandKind::Upload:
  case CommandKind::GetStaged:
  case CommandKind::Fetch:
  case CommandKind::Validate:
  case CommandKind::Plan:
  case CommandKind::Run:
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
    std::cout << ",\"dataBytes\":" << result.received_bytes();
    if (output.has_value()) {
      std::cout << ",\"output\":\"" << json_escape(*output) << '\"';
    }
    std::cout << "}\n";
    return 0;
  }

  print_result_messages(result);
  if (output.has_value()) {
    std::cout << "Wrote " << result.received_bytes() << " bytes to " << *output
              << '\n';
  } else if (!result.terminal_payload().empty()) {
    std::cout << escape_binary_for_text(result.terminal_payload()) << '\n';
  } else {
    std::cout << command_name(invocation.kind) << " completed\n";
  }
  return 0;
}

kairosboot::ContextOptions context_options(const GlobalOptions &options) {
  return kairosboot::ContextOptions{
      .usb_vendor_id = options.usb_vendor_id.value_or(0U),
  };
}

int flash_file(const Invocation &invocation) {
  auto context = kairosboot::Context::create(context_options(invocation.global));
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

int format_partition(const Invocation &invocation) {
  auto context = kairosboot::Context::create(context_options(invocation.global));
  if (!context) {
    return print_runtime_error(context.error(), invocation.global.json);
  }
  const std::optional<std::string_view> filesystem_type =
      invocation.filesystem_type.empty()
          ? std::nullopt
          : std::optional<std::string_view>{invocation.filesystem_type};
  const auto result = context->format_partition(
      selector_view(invocation.global), invocation.first, filesystem_type,
      invocation.format_partition_size, flash_options(invocation.global));
  if (!result) {
    return print_runtime_error(result.error(), invocation.global.json);
  }
  if (invocation.global.json) {
    std::cout << "{\"ok\":true,\"command\":\"format\",\"partition\":\""
              << json_escape(invocation.first) << "\"";
    if (filesystem_type.has_value()) {
      std::cout << ",\"filesystemType\":\""
                << json_escape(*filesystem_type) << "\"";
    }
    if (invocation.format_partition_size != 0U) {
      std::cout << ",\"partitionSize\":"
                << invocation.format_partition_size;
    }
    std::cout << "}\n";
  } else {
    std::cout << "Formatted " << invocation.first << '\n';
  }
  return 0;
}

int boot_file(const Invocation &invocation) {
  auto context = kairosboot::Context::create(context_options(invocation.global));
  if (!context) {
    return print_runtime_error(context.error(), invocation.global.json);
  }
  InterruptCancellation cancellation;
  auto options = flash_options(invocation.global);
  const auto legacy = kairosboot::LegacyBootOptions{
      .command_line = invocation.boot_image_options.command_line,
      .base = invocation.boot_image_options.base,
      .page_size = invocation.boot_image_options.page_size,
      .kernel_offset = invocation.boot_image_options.kernel_offset,
      .ramdisk_offset = invocation.boot_image_options.ramdisk_offset,
      .second_offset = invocation.boot_image_options.second_offset,
      .tags_offset = invocation.boot_image_options.tags_offset,
  };
  const std::optional<std::filesystem::path> ramdisk =
      invocation.second.empty()
          ? std::nullopt
          : std::optional<std::filesystem::path>{
                path_from_utf8(invocation.second)};
  const std::optional<std::filesystem::path> second_stage =
      invocation.third.empty()
          ? std::nullopt
          : std::optional<std::filesystem::path>{path_from_utf8(invocation.third)};
  auto operation = context->boot_raw_async(
      selector_view(invocation.global), path_from_utf8(invocation.first),
      ramdisk, second_stage, legacy, options);
  if (!operation) {
    return print_runtime_error(operation.error(), invocation.global.json);
  }
  auto result = operation->wait(cancellation.token());
  if (!result) {
    return print_runtime_error(result.error(), invocation.global.json);
  }
  if (invocation.global.json) {
    std::cout << "{\"ok\":true,\"command\":\"boot\",\"image\":\""
              << json_escape(invocation.first) << "\"}\n";
  } else {
    std::cout << "Booted from " << invocation.first << '\n';
  }
  return 0;
}

std::expected<std::shared_ptr<const kairosboot::image::IImageSource>,
              LocalRuntimeError>
open_boot_component(const std::string_view path, const std::string_view name) {
  auto opened = kairosboot::image::FileImageSource::open(path_from_utf8(path));
  if (!opened) {
    return std::unexpected(LocalRuntimeError{
        KB_E_IO, "cannot open boot " + std::string{name} + " file " +
                     std::string{path} + ": " + opened.error().message});
  }
  return std::shared_ptr<const kairosboot::image::IImageSource>{
      std::move(*opened)};
}

int make_boot_image(const Invocation &invocation) {
  auto kernel = open_boot_component(invocation.second, "kernel");
  if (!kernel) {
    return print_local_runtime_error(kernel.error(), invocation.global.json);
  }
  std::shared_ptr<const kairosboot::image::IImageSource> ramdisk;
  if (!invocation.third.empty()) {
    auto opened = open_boot_component(invocation.third, "ramdisk");
    if (!opened) {
      return print_local_runtime_error(opened.error(), invocation.global.json);
    }
    ramdisk = std::move(*opened);
  }
  std::shared_ptr<const kairosboot::image::IImageSource> second;
  if (!invocation.fourth.empty()) {
    auto opened = open_boot_component(invocation.fourth, "second");
    if (!opened) {
      return print_local_runtime_error(opened.error(), invocation.global.json);
    }
    second = std::move(*opened);
  }

  auto built = kairosboot::image::build_legacy_boot_image(
      std::move(*kernel), std::move(ramdisk), std::move(second),
      invocation.boot_image_options);
  if (!built) {
    const bool invalid =
        built.error().kind ==
            kairosboot::image::BootImageBuildErrorKind::InvalidArgument ||
        built.error().kind ==
            kairosboot::image::BootImageBuildErrorKind::SizeOverflow;
    return print_local_runtime_error(
        LocalRuntimeError{invalid ? KB_E_INVALID_ARGUMENT : KB_E_IO,
                          built.error().message},
        invocation.global.json);
  }
  if (auto written = write_source_atomically(invocation.first, **built);
      !written) {
    return print_local_runtime_error(written.error(), invocation.global.json);
  }

  if (invocation.global.json) {
    std::cout << "{\"ok\":true,\"command\":\"make-boot-image\","
                 "\"output\":\""
              << json_escape(invocation.first) << "\",\"bytes\":"
              << (*built)->size()
              << ",\"headerVersion\":0,\"vendorBoot\":false}\n";
  } else {
    std::cout << "Created legacy boot image " << invocation.first << " ("
              << (*built)->size() << " bytes)\n";
  }
  return 0;
}

int flash_raw(const Invocation &invocation) {
  auto context = kairosboot::Context::create(context_options(invocation.global));
  if (!context) {
    return print_runtime_error(context.error(), invocation.global.json);
  }
  const std::optional<std::filesystem::path> ramdisk =
      invocation.third.empty()
          ? std::nullopt
          : std::optional<std::filesystem::path>{
                path_from_utf8(invocation.third)};
  const std::optional<std::filesystem::path> second_stage =
      invocation.fourth.empty()
          ? std::nullopt
          : std::optional<std::filesystem::path>{
                path_from_utf8(invocation.fourth)};
  const auto result = context->flash_raw(
      selector_view(invocation.global), invocation.first,
      path_from_utf8(invocation.second), ramdisk, second_stage,
      kairosboot::LegacyBootOptions{
          .command_line = invocation.boot_image_options.command_line,
          .base = invocation.boot_image_options.base,
          .page_size = invocation.boot_image_options.page_size,
          .kernel_offset = invocation.boot_image_options.kernel_offset,
          .ramdisk_offset = invocation.boot_image_options.ramdisk_offset,
          .second_offset = invocation.boot_image_options.second_offset,
          .tags_offset = invocation.boot_image_options.tags_offset,
      },
      flash_options(invocation.global));
  if (!result) {
    return print_runtime_error(result.error(), invocation.global.json);
  }
  if (invocation.global.json) {
    std::cout << "{\"ok\":true,\"command\":\"flash:raw\",\"partition\":\""
              << json_escape(invocation.first) << "\",\"kernel\":\""
              << json_escape(invocation.second) << "\"}\n";
  } else {
    std::cout << "Flashed raw boot image to " << invocation.first << " from "
              << invocation.second << '\n';
  }
  return 0;
}

int signature_file(const Invocation &invocation) {
  auto context = kairosboot::Context::create(context_options(invocation.global));
  if (!context) {
    return print_runtime_error(context.error(), invocation.global.json);
  }
  InterruptCancellation cancellation;
  auto operation = context->signature_file_async(
      selector_view(invocation.global), path_from_utf8(invocation.first),
      command_options(invocation.global));
  if (!operation) {
    return print_runtime_error(operation.error(), invocation.global.json);
  }
  auto result = operation->wait_result(cancellation.token());
  if (!result) {
    return print_runtime_error(result.error(), invocation.global.json);
  }
  return print_command_success(invocation, *result, std::nullopt);
}

class UpdateProgressReporter final {
public:
  UpdateProgressReporter(const bool json,
                         const std::string_view command) noexcept
      : json_(json), command_(command) {}

  kairosboot::ProgressAction operator()(const kairosboot::Progress &progress) {
    std::scoped_lock lock(mutex_);
    const std::string stage{progress.stage};
    const std::string device{progress.device_identifier};
    if (!json_ &&
        (stage != stage_ || device != device_ ||
         progress.bytes_completed != completed_ ||
         progress.bytes_total != total_)) {
      std::cerr << command_ << ": " << stage;
      if (progress.bytes_total != 0U) {
        std::cerr << ' ' << progress.bytes_completed << '/'
                  << progress.bytes_total << " bytes";
      }
      if (!device.empty()) {
        std::cerr << " [" << device << ']';
      }
      std::cerr << '\n';
    }
    stage_ = stage;
    device_ = device;
    completed_ = progress.bytes_completed;
    total_ = progress.bytes_total;
    return kairosboot::ProgressAction::Continue;
  }

private:
  bool json_{};
  std::string_view command_;
  std::mutex mutex_;
  std::string stage_;
  std::string device_;
  std::uint64_t completed_{};
  std::uint64_t total_{};
};

int update_package(const Invocation &invocation) {
  std::string package;
  if (invocation.kind == CommandKind::Update) {
    package = invocation.first;
  } else {
#if defined(_WIN32)
    constexpr wchar_t product_out_name[] = L"ANDROID_PRODUCT_OUT";
    const DWORD required =
        GetEnvironmentVariableW(product_out_name, nullptr, 0U);
    if (required == 0U) {
      return print_local_runtime_error(
          LocalRuntimeError{
              KB_E_INVALID_ARGUMENT,
              "flashall requires non-empty ANDROID_PRODUCT_OUT"},
          invocation.global.json);
    }
    std::wstring wide_package(static_cast<std::size_t>(required), L'\0');
    const DWORD copied = GetEnvironmentVariableW(
        product_out_name, wide_package.data(), required);
    if (copied == 0U || copied >= required) {
      return print_local_runtime_error(
          LocalRuntimeError{KB_E_INVALID_ARGUMENT,
                            "cannot read ANDROID_PRODUCT_OUT"},
          invocation.global.json);
    }
    wide_package.resize(static_cast<std::size_t>(copied));
    auto converted = utf8_from_wide(wide_package);
    if (!converted) {
      return print_local_runtime_error(converted.error(),
                                       invocation.global.json);
    }
    package = std::move(*converted);
#else
    const char *product_out = std::getenv("ANDROID_PRODUCT_OUT");
    if (product_out == nullptr || product_out[0] == '\0') {
      return print_local_runtime_error(
          LocalRuntimeError{
              KB_E_INVALID_ARGUMENT,
              "flashall requires non-empty ANDROID_PRODUCT_OUT"},
          invocation.global.json);
    }
    package = product_out;
#endif
  }

  auto context = kairosboot::Context::create(context_options(invocation.global));
  if (!context) {
    return print_runtime_error(context.error(), invocation.global.json);
  }

  InterruptCancellation cancellation;
  const std::string_view command = command_name(invocation.kind);
  UpdateProgressReporter progress{invocation.global.json, command};
  auto options = update_options(invocation.global);
  options.wipe = invocation.wipe;
  options.skip_reboot = invocation.skip_reboot;
  options.skip_secondary = invocation.skip_secondary;
  options.exclude_dynamic_partitions =
      invocation.exclude_dynamic_partitions;
  options.disable_fastboot_info = invocation.disable_fastboot_info;
  options.progress = [&progress](const kairosboot::Progress &value) {
    return progress(value);
  };
  auto operation = context->update_package_async(
      selector_view(invocation.global), path_from_utf8(package), options);
  if (!operation) {
    return print_runtime_error(operation.error(), invocation.global.json);
  }
  auto result = operation->wait(cancellation.token());
  if (!result) {
    return print_runtime_error(result.error(), invocation.global.json);
  }

  if (invocation.global.json) {
    std::cout << "{\"ok\":true,\"command\":\"" << command
              << "\",\"package\":\"" << json_escape(package)
              << "\",\"wipe\":"
              << (invocation.wipe ? "true" : "false")
              << ",\"skipReboot\":"
              << (invocation.skip_reboot ? "true" : "false")
              << ",\"skipSecondary\":"
              << (invocation.skip_secondary ? "true" : "false")
              << ",\"excludeDynamicPartitions\":"
              << (invocation.exclude_dynamic_partitions ? "true" : "false")
              << ",\"disableFastbootInfo\":"
              << (invocation.disable_fastboot_info ? "true" : "false")
              << "}\n";
  } else {
    std::cout << (invocation.kind == CommandKind::Flashall
                      ? "Flashed all from "
                      : "Updated from ")
              << package;
    if (invocation.wipe) {
      std::cout << " (wipe requested)";
    }
    std::cout << '\n';
  }
  return 0;
}

int wipe_super(const Invocation &invocation) {
  auto context = kairosboot::Context::create(context_options(invocation.global));
  if (!context) {
    return print_runtime_error(context.error(), invocation.global.json);
  }

  InterruptCancellation cancellation;
  UpdateProgressReporter progress{invocation.global.json, "wipe-super"};
  auto options = update_options(invocation.global);
  options.progress = [&progress](const kairosboot::Progress &value) {
    return progress(value);
  };
  std::optional<std::filesystem::path> image;
  if (!invocation.first.empty()) {
    image = path_from_utf8(invocation.first);
  }
  auto operation = context->wipe_super_async(
      selector_view(invocation.global), image, options);
  if (!operation) {
    return print_runtime_error(operation.error(), invocation.global.json);
  }
  auto result = operation->wait(cancellation.token());
  if (!result) {
    return print_runtime_error(result.error(), invocation.global.json);
  }

  if (invocation.global.json) {
    std::cout << "{\"ok\":true,\"command\":\"wipe-super\",\"image\":";
    if (invocation.first.empty()) {
      std::cout << "null";
    } else {
      std::cout << '"' << json_escape(invocation.first) << '"';
    }
    std::cout << "}\n";
  } else {
    std::cout << "Wiped super";
    if (!invocation.first.empty()) {
      std::cout << " using " << invocation.first;
    }
    std::cout << '\n';
  }
  return 0;
}

struct FleetError {
  kb_status_t status{KB_E_INTERNAL};
  std::string message;
  std::int32_t native_code{0};
};

FleetError fleet_error(const kairosboot::Error &error) {
  return FleetError{error.status(), error.message(), error.native_code()};
}

int print_fleet_error(const FleetError &error, const bool json) {
  const std::string message = error.message + " (native error " +
                              std::to_string(error.native_code) + ")";
  if (json) {
    std::cout << "{\"ok\":false,\"status\":\"" << status_name(error.status)
              << "\",\"message\":\"" << json_escape(message)
              << "\",\"nativeError\":" << error.native_code << "}\n";
  } else {
    std::cerr << "kairosboot: " << message << '\n';
  }
  return 4;
}

int validate_manifest(const Invocation &invocation) {
  const auto validated =
      kairosboot::validate_job_file(path_from_utf8(invocation.first));
  if (!validated) {
    return print_fleet_error(fleet_error(validated.error()),
                             invocation.global.json);
  }
  if (invocation.global.json) {
    std::cout << "{\"ok\":true,\"command\":\"validate\",\"manifest\":\""
              << json_escape(invocation.first) << "\"}\n";
  } else {
    std::cout << "OK " << invocation.first << '\n';
  }
  return 0;
}

int plan_manifest(const Invocation &invocation) {
  auto plan = kairosboot::plan_job_file(path_from_utf8(invocation.first));
  if (!plan) {
    return print_fleet_error(fleet_error(plan.error()), false);
  }
  if (invocation.plan_digest) {
    std::cout << plan->sha256_hex() << '\n';
    return 0;
  }
  std::cout << plan->canonical_json();
  std::cout << '\n';
  return 0;
}

class FleetProgressReporter final {
public:
  explicit FleetProgressReporter(const bool json) noexcept : json_(json) {}

  kairosboot::ProgressAction operator()(const kairosboot::Progress &progress) {
    std::scoped_lock lock(mutex_);
    if (!json_) {
      std::cerr << "run: " << progress.stage;
      if (progress.bytes_total != 0U) {
        std::cerr << ' ' << progress.bytes_completed << '/'
                  << progress.bytes_total << " bytes";
      }
      if (!progress.device_identifier.empty()) {
        std::cerr << " [" << progress.device_identifier << ']';
      }
      std::cerr << '\n';
    }
    return kairosboot::ProgressAction::Continue;
  }

private:
  bool json_{};
  std::mutex mutex_;
};

int print_fleet_run_result(const Invocation &invocation,
                           const kairosboot::JobReport &report,
                           const kairosboot::Error *error) {
  const bool succeeded = error == nullptr;
  if (invocation.global.json) {
    std::cout << "{\"ok\":" << (succeeded ? "true" : "false")
              << ",\"command\":\"run\"";
    if (error != nullptr) {
      std::cout << ",\"status\":\"" << status_name(error->status())
                << "\",\"message\":\"" << json_escape(error->message())
                << '"';
    }
    std::cout << ",\"report\":" << report.json() << "}\n";
  } else if (succeeded) {
    std::cout << "Fleet job completed\n" << report.json() << '\n';
  } else {
    std::cerr << "kairosboot: " << error->message() << '\n'
              << "Fleet report: " << report.json() << '\n';
  }
  return succeeded ? 0 : 4;
}

int run_manifest(const Invocation &invocation) {
  auto context = kairosboot::Context::create(context_options(invocation.global));
  if (!context) {
    return print_runtime_error(context.error(), invocation.global.json);
  }
  InterruptCancellation cancellation;
  FleetProgressReporter progress{invocation.global.json};
  kairosboot::JobOptions options;
  if (invocation.global.timeout_ms) {
    options.timeout =
        std::chrono::milliseconds{*invocation.global.timeout_ms};
  }
  options.progress = [&progress](const kairosboot::Progress &value) {
    return progress(value);
  };
  auto job = context->run_job_file_async(path_from_utf8(invocation.first),
                                         options);
  if (!job) {
    return print_runtime_error(job.error(), invocation.global.json);
  }
  auto waited = job->wait(cancellation.token());
  auto report = job->report();
  if (!report) {
    return print_runtime_error(report.error(), invocation.global.json);
  }
  return print_fleet_run_result(invocation, *report,
                                waited ? nullptr : &waited.error());
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

  auto context = kairosboot::Context::create(context_options(invocation.global));
  if (!context) {
    return print_runtime_error(context.error(), invocation.global.json);
  }
  auto result = execute_typed_command(*context, invocation, stage_data);
  if (!result) {
    return print_runtime_error(result.error(), invocation.global.json);
  }

  std::optional<std::string_view> output;
  if (!result->output_path().empty()) {
    output = result->output_path();
  }
  return print_command_success(invocation, *result, output);
}

int run_management_command(const Invocation &invocation) {
  auto context = kairosboot::Context::create(context_options(invocation.global));
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
               "  kairosboot -h | --help [--json]\n"
               "  kairosboot doctor [--json]\n"
               "  kairosboot devices [--json]\n"
               "  kairosboot [--json] validate <manifest>\n"
               "  kairosboot plan <manifest> [--digest]\n"
               "  kairosboot [--json] [--timeout-ms <milliseconds>] run "
               "<manifest>\n"
               "  kairosboot [global options] flash <partition> <file>\n"
               "  kairosboot [global options] flash:raw <partition> <kernel> "
               "[ramdisk [second]]\n"
               "  kairosboot [global options] signature <file>\n"
               "  kairosboot [global options] update <package> [--wipe] "
               "[--skip-reboot] [--skip-secondary] "
               "[--exclude-dynamic-partitions] [--disable-fastboot-info]\n"
               "  kairosboot [global options] flashall [--wipe] "
               "[--skip-reboot] [--skip-secondary] "
               "[--exclude-dynamic-partitions] [--disable-fastboot-info]\n"
               "  kairosboot [global options] boot <kernel-or-image> "
               "[ramdisk [second]]\n"
               "  kairosboot [--json] make-boot-image <output> <kernel> "
               "[ramdisk [second]] [layout options]\n"
               "  kairosboot [global options] wipe-super [super_empty.img]\n"
               "  kairosboot [global options] getvar <variable>\n"
               "  kairosboot [global options] erase <partition>\n"
               "  kairosboot [global options] format[:type[:size]] <partition>\n"
               "  kairosboot [global options] set-active <slot>\n"
               "  kairosboot [global options] reboot "
               "[system|bootloader|recovery|fastboot]\n"
               "  kairosboot [global options] continue\n"
               "  kairosboot [global options] oem <command...>\n"
               "  kairosboot [global options] raw <command...>\n"
               "  kairosboot [global options] boot-staged\n"
               "  kairosboot [global options] stage <file>\n"
               "  kairosboot [global options] upload <output>\n"
               "  kairosboot [global options] get-staged <output>\n"
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
               "  -i <vendor-id> | --vendor-id <vendor-id>\n"
               "  -v | --verbose\n"
               "  --slot <a|b|other|all> -a | --set-active[=<a|b|other>]\n"
               "  --json --timeout-ms <milliseconds> "
               "--max-receive-bytes <bytes>\n"
               "  --disable-verity --disable-verification\n"
               "  -S SIZE[K|M|G] (flash, flash:raw, update and flashall)\n"
               "  Legacy boot options (boot and flash:raw only):\n"
               "  --cmdline <text> --base <value> --page-size <value>\n"
               "  --kernel-offset <value> --ramdisk-offset <value>\n"
               "  --second-offset <value> --tags-offset <value>\n"
               "Exit codes: 0 success, 2 usage error, 4 runtime/cancelled\n";
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
       std::string_view{argv[1]} == "-h" ||
       std::string_view{argv[1]} == "help") &&
      std::string_view{argv[2]} == "--json") {
    return print_help(true);
  }
  if (argc == 3 && std::string_view{argv[1]} == "doctor" &&
      std::string_view{argv[2]} == "--json") {
    return doctor(GlobalOptions{.json = true});
  }
  if (argc == 3 && std::string_view{argv[1]} == "devices" &&
      std::string_view{argv[2]} == "--json") {
    return print_devices(GlobalOptions{.json = true});
  }

  const auto invocation = parse_invocation(argc, argv);
  if (!invocation) {
    const int result = print_parse_error(invocation.error());
    if (!invocation.error().json) {
      print_usage(std::cerr);
    }
    return result;
  }

  if (invocation->global.verbose && !invocation->global.json) {
    std::cerr << "kairosboot: command=" << command_name(invocation->kind);
    if (invocation->global.selector.has_value()) {
      std::cerr << " selector=" << *invocation->global.selector;
    } else {
      std::cerr << " selector=auto";
    }
    if (invocation->global.usb_vendor_id.has_value()) {
      std::cerr << " usb-vendor=0x" << std::hex
                << *invocation->global.usb_vendor_id << std::dec;
    }
    std::cerr << '\n';
  }

  switch (invocation->kind) {
  case CommandKind::Version:
    return print_version(invocation->global.json);
  case CommandKind::Help:
    return print_help(invocation->global.json);
  case CommandKind::Doctor:
    return doctor(invocation->global);
  case CommandKind::Devices:
    return print_devices(invocation->global);
  case CommandKind::Validate:
    return validate_manifest(*invocation);
  case CommandKind::Plan:
    return plan_manifest(*invocation);
  case CommandKind::Run:
    return run_manifest(*invocation);
  case CommandKind::Flash:
    return flash_file(*invocation);
  case CommandKind::FlashRaw:
    return flash_raw(*invocation);
  case CommandKind::Signature:
    return signature_file(*invocation);
  case CommandKind::Update:
  case CommandKind::Flashall:
    return update_package(*invocation);
  case CommandKind::BootFile:
    return boot_file(*invocation);
  case CommandKind::MakeBootImage:
    return make_boot_image(*invocation);
  case CommandKind::WipeSuper:
    return wipe_super(*invocation);
  case CommandKind::Format:
    return format_partition(*invocation);
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
  case CommandKind::GetStaged:
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
