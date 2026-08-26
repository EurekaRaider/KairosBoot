#include <kairosboot/kairosboot.hpp>

static_assert(__cplusplus >= 202100L);

int main() { return kairosboot::version().api_version == KB_API_VERSION ? 0 : 1; }
