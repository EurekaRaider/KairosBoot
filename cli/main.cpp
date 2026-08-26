#include <kairosboot/kairosboot.hpp>

#include <iostream>
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

void print_usage() {
  std::cerr << "Usage:\n"
               "  kairosboot --version [--json]\n"
               "  kairosboot doctor --json\n"
               "  kairosboot devices [--json]\n";
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

  print_usage();
  return 2;
}
