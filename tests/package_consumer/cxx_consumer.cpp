#include <kairosboot/kairosboot.hpp>

static_assert(__cplusplus >= 202100L);

int main() {
  if (kairosboot::version().api_version != KB_API_VERSION) {
    return 1;
  }
  auto context = kairosboot::Context::create();
  if (!context) {
    return 2;
  }
  return context->devices() ? 0 : 3;
}
