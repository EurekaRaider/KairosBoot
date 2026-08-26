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

void print_usage() {
  std::cerr << "Usage:\n"
               "  kairosboot --version [--json]\n"
               "  kairosboot doctor --json\n";
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

  print_usage();
  return 2;
}
