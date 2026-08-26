#include <kairosboot/kairosboot.h>

int main(void) {
  kb_version_t version = {0};
  kb_version_init(&version);
  return kb_get_version(&version) == KB_OK ? 0 : 1;
}
