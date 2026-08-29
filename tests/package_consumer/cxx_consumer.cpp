#include <kairosboot/kairosboot.hpp>

#include <expected>
#include <type_traits>

static_assert(__cplusplus >= 202100L);
static_assert(!std::is_copy_constructible_v<kairosboot::CommandResult>);
static_assert(std::is_nothrow_move_constructible_v<kairosboot::CommandResult>);
static_assert(!std::is_copy_constructible_v<kairosboot::Device>);
static_assert(std::is_nothrow_move_constructible_v<kairosboot::Device>);

int main() {
  if (kairosboot::version().api_version != KB_API_VERSION) {
    return 1;
  }
  auto context = kairosboot::Context::create();
  if (!context) {
    return 2;
  }
  if (!context->devices()) {
    return 3;
  }
  const auto invalid_device = context->open_device("unknown:device");
  if (invalid_device ||
      invalid_device.error().status() != KB_E_INVALID_ARGUMENT) {
    return 4;
  }
  return 0;
}
