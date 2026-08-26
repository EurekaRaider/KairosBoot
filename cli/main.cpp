#include <kairosboot/kairosboot.hpp>

#include <expected>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

namespace {

std::string json_escape(std::string_view value) {
  std::string result;
  result.reserve(value.size());
  for (const char character : value) {
    switch (character) {
    case '\\':
      result += "\\\\";
      break;
    case '"':
      result += "\\\"";
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
      result += character;
      break;
    }
  }
  return result;
}

struct FlashCommand {
  std::optional<std::string_view> serial;
  bool json{false};
  std::string_view partition;
  std::string_view file;
};

struct FlashParseError {
  bool json{false};
  std::string_view message;
};

std::expected<FlashCommand, FlashParseError>
parse_flash_command(int argc, char **argv) {
  int index = 1;
  bool json = false;
  std::optional<std::string_view> serial;
  const auto error = [&json](std::string_view message) {
    return std::unexpected(FlashParseError{json, message});
  };

  while (index < argc) {
    const std::string_view argument{argv[index]};
    if (argument == "--json") {
      if (json) {
        return error("option --json may only be specified once");
      }
      json = true;
      ++index;
      continue;
    }
    if (argument == "--serial") {
      if (serial.has_value()) {
        return error("option --serial may only be specified once");
      }
      if (index + 1 >= argc || argv[index + 1][0] == '\0' ||
          std::string_view{argv[index + 1]} == "--json" ||
          std::string_view{argv[index + 1]} == "--serial") {
        return error("option --serial requires a non-empty value");
      }
      serial = std::string_view{argv[index + 1]};
      index += 2;
      continue;
    }
    break;
  }

  if (index >= argc || std::string_view{argv[index]} != "flash") {
    return error("unknown command");
  }
  for (int operand = index + 1; operand < argc; ++operand) {
    const std::string_view argument{argv[operand]};
    if (argument == "--json" || argument == "--serial") {
      return error("global options must precede the command");
    }
  }
  if (argc - index != 3) {
    return error("flash requires exactly <partition> and <file>");
  }

  const std::string_view partition{argv[index + 1]};
  const std::string_view file{argv[index + 2]};
  if (partition.empty()) {
    return error("flash partition must not be empty");
  }
  if (file.empty()) {
    return error("flash file must not be empty");
  }
  return FlashCommand{serial, json, partition, file};
}

int print_runtime_error(const kairosboot::Error &error, bool json) {
  if (json) {
    std::cout << "{\"ok\":false,\"status\":\""
              << kb_status_string(error.status()) << "\",\"message\":\""
              << json_escape(error.message()) << "\"}\n";
  } else {
    std::cerr << "kairosboot: " << error.message() << '\n';
  }
  return 4;
}

int print_parse_error(const FlashParseError &error) {
  if (error.json) {
    std::cout << "{\"ok\":false,\"status\":\"invalid_argument\","
                 "\"message\":\""
              << json_escape(error.message) << "\"}\n";
  } else {
    std::cerr << "kairosboot: " << error.message << '\n';
  }
  return 2;
}

int print_version(bool json) {
  const kairosboot::Version current = kairosboot::version();
  if (json) {
    std::cout << "{\"name\":\"KairosBoot\",\"version\":\""
              << json_escape(current.string) << "\",\"apiVersion\":"
              << current.api_version << "}\n";
  } else {
    std::cout << "KairosBoot " << current.string << '\n';
  }
  return 0;
}

int doctor_json() {
  const kairosboot::Version current = kairosboot::version();
  auto context = kairosboot::Context::create();
  if (!context) {
    const auto &error = context.error();
    std::cout << "{\"ok\":false,\"version\":\""
              << json_escape(current.string)
              << "\",\"status\":\"" << kb_status_string(error.status())
              << "\",\"message\":\"" << json_escape(error.message())
              << "\"}\n";
    return 4;
  }

  auto devices = context->devices();
  if (!devices) {
    const auto &error = devices.error();
    std::cout << "{\"ok\":false,\"version\":\""
              << json_escape(current.string)
              << "\",\"transport\":{\"available\":false,\"status\":\""
              << kb_status_string(error.status()) << "\",\"message\":\""
              << json_escape(error.message()) << "\"}}\n";
    return 4;
  }

  std::cout << "{\"ok\":true,\"version\":\""
            << json_escape(current.string)
            << "\",\"transport\":{\"available\":true},\"deviceCount\":"
            << devices->size() << "}\n";
  return 0;
}

int print_devices(bool json) {
  auto context = kairosboot::Context::create();
  if (!context) {
    const auto &error = context.error();
    if (json) {
      std::cout << "{\"ok\":false,\"status\":\""
                << kb_status_string(error.status()) << "\",\"message\":\""
                << json_escape(error.message()) << "\"}\n";
    } else {
      std::cerr << "kairosboot: " << error.message() << '\n';
    }
    return 4;
  }

  auto devices = context->devices();
  if (!devices) {
    const auto &error = devices.error();
    if (json) {
      std::cout << "{\"ok\":false,\"status\":\""
                << kb_status_string(error.status()) << "\",\"message\":\""
                << json_escape(error.message()) << "\"}\n";
    } else {
      std::cerr << "kairosboot: " << error.message() << '\n';
    }
    return 4;
  }

  if (json) {
    std::cout << "{\"ok\":true,\"devices\":[";
    for (std::size_t index = 0; index < devices->size(); ++index) {
      if (index != 0) {
        std::cout << ',';
      }
      std::cout << "{\"serial\":\"" << json_escape(devices->serial(index))
                << "\",\"usbPath\":\""
                << json_escape(devices->usb_path(index))
                << "\",\"product\":\""
                << json_escape(devices->product(index)) << "\"}";
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

int flash_file(const FlashCommand &command) {
  auto context = kairosboot::Context::create();
  if (!context) {
    return print_runtime_error(context.error(), command.json);
  }

  const std::filesystem::path file{std::string{command.file}};
  const auto result = command.serial.has_value()
                          ? context->flash_file(*command.serial,
                                                command.partition, file)
                          : context->flash_file(command.partition, file);
  if (!result) {
    return print_runtime_error(result.error(), command.json);
  }

  if (command.json) {
    std::cout << "{\"ok\":true,\"command\":\"flash\",\"partition\":\""
              << json_escape(command.partition) << "\",\"file\":\""
              << json_escape(command.file) << "\"}\n";
  } else {
    std::cout << "Flashed " << command.partition << " from " << command.file
              << '\n';
  }
  return 0;
}

void print_usage() {
  std::cerr << "Usage:\n"
               "  kairosboot --version [--json]\n"
               "  kairosboot doctor --json\n"
               "  kairosboot devices [--json]\n"
               "  kairosboot [--serial <id>] [--json] flash <partition> "
               "<file>\n";
}

} // namespace

int main(int argc, char **argv) {
  if (argc == 2 && std::string_view{argv[1]} == "--version") {
    return print_version(false);
  }
  if (argc == 3 && std::string_view{argv[1]} == "--version" &&
      std::string_view{argv[2]} == "--json") {
    return print_version(true);
  }
  if (argc == 3 && std::string_view{argv[1]} == "doctor" &&
      std::string_view{argv[2]} == "--json") {
    return doctor_json();
  }
  if (argc == 2 && std::string_view{argv[1]} == "devices") {
    return print_devices(false);
  }
  if (argc == 3 && std::string_view{argv[1]} == "devices" &&
      std::string_view{argv[2]} == "--json") {
    return print_devices(true);
  }

  if (argc > 1 &&
      (std::string_view{argv[1]} == "flash" ||
       std::string_view{argv[1]} == "--serial" ||
       std::string_view{argv[1]} == "--json")) {
    const auto command = parse_flash_command(argc, argv);
    if (!command) {
      const int result = print_parse_error(command.error());
      if (!command.error().json) {
        print_usage();
      }
      return result;
    }
    return flash_file(*command);
  }

  print_usage();
  return 2;
}
