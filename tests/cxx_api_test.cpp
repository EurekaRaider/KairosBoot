#include <kairosboot/kairosboot.hpp>

#include <iostream>
#include <optional>
#include <type_traits>

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      std::cerr << "check failed at line " << __LINE__ << ": " #condition    \
                << '\n';                                                       \
      return __LINE__;                                                         \
    }                                                                          \
  } while (false)

static_assert(__cplusplus >= 202100L);
static_assert(!std::is_copy_constructible_v<kairosboot::Context>);
static_assert(std::is_move_constructible_v<kairosboot::Context>);
static_assert(!std::is_copy_constructible_v<kairosboot::Operation>);

int main() {
  const auto version = kairosboot::version();
  CHECK(version.api_version == KB_API_VERSION);
  CHECK(!version.string.empty());

  auto context = kairosboot::Context::create();
  CHECK(context.has_value());

  auto devices = context->devices();
  CHECK(devices.has_value());
  for (std::size_t index = 0; index < devices->size(); ++index) {
    static_cast<void>(devices->serial(index));
    static_cast<void>(devices->usb_path(index));
    static_cast<void>(devices->product(index));
  }

  auto operation = context->flash_file_async(
      "system", std::filesystem::path{"kairosboot-test-does-not-exist.img"});
  CHECK(!operation.has_value());
  CHECK(operation.error().status() == KB_E_IO);
  CHECK(operation.error().transfer_state() == KB_TRANSFER_NOT_SENT);

  auto flash = context->flash_file(
      std::optional<std::string_view>{"ABC"}, "system",
      std::filesystem::path{"kairosboot-test-does-not-exist.img"});
  CHECK(!flash.has_value());
  CHECK(flash.error().status() == KB_E_IO);
  CHECK(flash.error().device_identifier() == "ABC");
  return 0;
}
