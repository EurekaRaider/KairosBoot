#define KAIROSBOOT_DISABLE_SIZED_INITIALIZER_MACROS
#include <kairosboot/kairosboot.h>

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define CHECK_LEGACY_INIT_BOUND(initializer, type, legacy_size)                \
  do {                                                                         \
    _Alignas(type) unsigned char storage[(legacy_size) + 16U];                 \
    uint32_t initialized_size = 0U;                                            \
    uint32_t initialized_version = 0U;                                         \
    size_t canary_index = 0U;                                                  \
    void(KB_CALL *initializer_pointer)(type *) = (initializer);                \
    memset(storage, 0xa5, sizeof(storage));                                    \
    initializer_pointer((type *)(void *)storage);                              \
    memcpy(&initialized_size, storage, sizeof(initialized_size));              \
    memcpy(&initialized_version, storage + sizeof(uint32_t),                   \
           sizeof(initialized_version));                                       \
    if (initialized_size != (legacy_size) ||                                  \
        initialized_version != KB_API_VERSION) {                              \
      return __LINE__;                                                         \
    }                                                                          \
    for (canary_index = (legacy_size); canary_index < sizeof(storage);         \
         ++canary_index) {                                                     \
      if (storage[canary_index] != 0xa5U) {                                   \
        return __LINE__;                                                       \
      }                                                                        \
    }                                                                          \
  } while (0)

int kb_test_legacy_initializer_bounds(void) {
  CHECK_LEGACY_INIT_BOUND(kb_context_options_init, kb_context_options_t,
                          KB_CONTEXT_OPTIONS_V1_SIZE);
  CHECK_LEGACY_INIT_BOUND(kb_flash_options_init, kb_flash_options_t,
                          KB_FLASH_OPTIONS_V1_SIZE);
  CHECK_LEGACY_INIT_BOUND(kb_legacy_boot_options_init,
                          kb_legacy_boot_options_t,
                          KB_LEGACY_BOOT_OPTIONS_V1_SIZE);
  CHECK_LEGACY_INIT_BOUND(kb_update_options_init, kb_update_options_t,
                          KB_UPDATE_OPTIONS_V1_SIZE);
  CHECK_LEGACY_INIT_BOUND(kb_command_options_init, kb_command_options_t,
                          KB_COMMAND_OPTIONS_V1_SIZE);
  CHECK_LEGACY_INIT_BOUND(kb_job_options_init, kb_job_options_t,
                          KB_JOB_OPTIONS_V1_SIZE);
  CHECK_LEGACY_INIT_BOUND(kb_version_init, kb_version_t, KB_VERSION_V1_SIZE);
  return 0;
}
