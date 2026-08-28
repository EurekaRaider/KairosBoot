/* Generated from abi/kairosboot-v1.json by scripts/check_abi.py. */
#ifndef KAIROSBOOT_ABI_LAYOUT_V1_H
#define KAIROSBOOT_ABI_LAYOUT_V1_H

#include <kairosboot/kairosboot.h>

#include <stddef.h>
#include <stdint.h>

#if defined(__cplusplus)
#define KB_ABI_ALIGNOF(type) alignof(type)
#define KB_ABI_STATIC_ASSERT(condition, message) static_assert(condition, message)
#else
#define KB_ABI_ALIGNOF(type) _Alignof(type)
#define KB_ABI_STATIC_ASSERT(condition, message) _Static_assert(condition, message)
#endif

KB_ABI_STATIC_ASSERT(sizeof(void *) == 8, "ABI v1 pointer size");
KB_ABI_STATIC_ASSERT(sizeof(size_t) == 8, "ABI v1 size_t size");
KB_ABI_STATIC_ASSERT(KB_API_VERSION == 1, "KB_API_VERSION value");
KB_ABI_STATIC_ASSERT(KB_COMMAND_MESSAGE_INFO == 0, "KB_COMMAND_MESSAGE_INFO value");
KB_ABI_STATIC_ASSERT(KB_COMMAND_MESSAGE_TEXT == 1, "KB_COMMAND_MESSAGE_TEXT value");
KB_ABI_STATIC_ASSERT(KB_E_AMBIGUOUS_DEVICE == 5, "KB_E_AMBIGUOUS_DEVICE value");
KB_ABI_STATIC_ASSERT(KB_E_BUSY == 6, "KB_E_BUSY value");
KB_ABI_STATIC_ASSERT(KB_E_CANCELLED == 8, "KB_E_CANCELLED value");
KB_ABI_STATIC_ASSERT(KB_E_DEVICE_FAIL == 12, "KB_E_DEVICE_FAIL value");
KB_ABI_STATIC_ASSERT(KB_E_INTERNAL == 10, "KB_E_INTERNAL value");
KB_ABI_STATIC_ASSERT(KB_E_INVALID_ARGUMENT == 1, "KB_E_INVALID_ARGUMENT value");
KB_ABI_STATIC_ASSERT(KB_E_IO == 9, "KB_E_IO value");
KB_ABI_STATIC_ASSERT(KB_E_NOT_SUPPORTED == 3, "KB_E_NOT_SUPPORTED value");
KB_ABI_STATIC_ASSERT(KB_E_NO_DEVICE == 4, "KB_E_NO_DEVICE value");
KB_ABI_STATIC_ASSERT(KB_E_OUT_OF_MEMORY == 2, "KB_E_OUT_OF_MEMORY value");
KB_ABI_STATIC_ASSERT(KB_E_PROTOCOL == 11, "KB_E_PROTOCOL value");
KB_ABI_STATIC_ASSERT(KB_E_TIMEOUT == 7, "KB_E_TIMEOUT value");
KB_ABI_STATIC_ASSERT(KB_FETCH_UNSPECIFIED == UINT64_MAX, "KB_FETCH_UNSPECIFIED value");
KB_ABI_STATIC_ASSERT(KB_FLASHING_GET_UNLOCK_ABILITY == 4, "KB_FLASHING_GET_UNLOCK_ABILITY value");
KB_ABI_STATIC_ASSERT(KB_FLASHING_LOCK == 0, "KB_FLASHING_LOCK value");
KB_ABI_STATIC_ASSERT(KB_FLASHING_LOCK_CRITICAL == 2, "KB_FLASHING_LOCK_CRITICAL value");
KB_ABI_STATIC_ASSERT(KB_FLASHING_UNLOCK == 1, "KB_FLASHING_UNLOCK value");
KB_ABI_STATIC_ASSERT(KB_FLASHING_UNLOCK_CRITICAL == 3, "KB_FLASHING_UNLOCK_CRITICAL value");
KB_ABI_STATIC_ASSERT(KB_GSI_DISABLE == 1, "KB_GSI_DISABLE value");
KB_ABI_STATIC_ASSERT(KB_GSI_STATUS == 2, "KB_GSI_STATUS value");
KB_ABI_STATIC_ASSERT(KB_GSI_WIPE == 0, "KB_GSI_WIPE value");
KB_ABI_STATIC_ASSERT(KB_OK == 0, "KB_OK value");
KB_ABI_STATIC_ASSERT(KB_OPERATION_CANCELLED == 4, "KB_OPERATION_CANCELLED value");
KB_ABI_STATIC_ASSERT(KB_OPERATION_CREATED == 0, "KB_OPERATION_CREATED value");
KB_ABI_STATIC_ASSERT(KB_OPERATION_FAILED == 3, "KB_OPERATION_FAILED value");
KB_ABI_STATIC_ASSERT(KB_OPERATION_RUNNING == 1, "KB_OPERATION_RUNNING value");
KB_ABI_STATIC_ASSERT(KB_OPERATION_SUCCEEDED == 2, "KB_OPERATION_SUCCEEDED value");
KB_ABI_STATIC_ASSERT(KB_PROGRESS_CANCEL == 1, "KB_PROGRESS_CANCEL value");
KB_ABI_STATIC_ASSERT(KB_PROGRESS_CONTINUE == 0, "KB_PROGRESS_CONTINUE value");
KB_ABI_STATIC_ASSERT(KB_REBOOT_BOOTLOADER == 1, "KB_REBOOT_BOOTLOADER value");
KB_ABI_STATIC_ASSERT(KB_REBOOT_FASTBOOT == 3, "KB_REBOOT_FASTBOOT value");
KB_ABI_STATIC_ASSERT(KB_REBOOT_RECOVERY == 2, "KB_REBOOT_RECOVERY value");
KB_ABI_STATIC_ASSERT(KB_REBOOT_SYSTEM == 0, "KB_REBOOT_SYSTEM value");
KB_ABI_STATIC_ASSERT(KB_SNAPSHOT_UPDATE_CANCEL == 0, "KB_SNAPSHOT_UPDATE_CANCEL value");
KB_ABI_STATIC_ASSERT(KB_SNAPSHOT_UPDATE_MERGE == 1, "KB_SNAPSHOT_UPDATE_MERGE value");
KB_ABI_STATIC_ASSERT(KB_TRANSFER_FULLY_TRANSFERRED == 2, "KB_TRANSFER_FULLY_TRANSFERRED value");
KB_ABI_STATIC_ASSERT(KB_TRANSFER_NOT_SENT == 0, "KB_TRANSFER_NOT_SENT value");
KB_ABI_STATIC_ASSERT(KB_TRANSFER_PARTIAL_OR_UNKNOWN == 1, "KB_TRANSFER_PARTIAL_OR_UNKNOWN value");
KB_ABI_STATIC_ASSERT(KB_WAIT_INFINITE == UINT32_C(4294967295), "KB_WAIT_INFINITE value");

KB_ABI_STATIC_ASSERT(sizeof(kb_command_message_kind_t) == 4, "kb_command_message_kind_t size");
KB_ABI_STATIC_ASSERT(KB_ABI_ALIGNOF(kb_command_message_kind_t) == 4, "kb_command_message_kind_t alignment");
KB_ABI_STATIC_ASSERT(sizeof(kb_flashing_command_t) == 4, "kb_flashing_command_t size");
KB_ABI_STATIC_ASSERT(KB_ABI_ALIGNOF(kb_flashing_command_t) == 4, "kb_flashing_command_t alignment");
KB_ABI_STATIC_ASSERT(sizeof(kb_gsi_command_t) == 4, "kb_gsi_command_t size");
KB_ABI_STATIC_ASSERT(KB_ABI_ALIGNOF(kb_gsi_command_t) == 4, "kb_gsi_command_t alignment");
KB_ABI_STATIC_ASSERT(sizeof(kb_operation_state_t) == 4, "kb_operation_state_t size");
KB_ABI_STATIC_ASSERT(KB_ABI_ALIGNOF(kb_operation_state_t) == 4, "kb_operation_state_t alignment");
KB_ABI_STATIC_ASSERT(sizeof(kb_progress_action_t) == 4, "kb_progress_action_t size");
KB_ABI_STATIC_ASSERT(KB_ABI_ALIGNOF(kb_progress_action_t) == 4, "kb_progress_action_t alignment");
KB_ABI_STATIC_ASSERT(sizeof(kb_reboot_target_t) == 4, "kb_reboot_target_t size");
KB_ABI_STATIC_ASSERT(KB_ABI_ALIGNOF(kb_reboot_target_t) == 4, "kb_reboot_target_t alignment");
KB_ABI_STATIC_ASSERT(sizeof(kb_snapshot_update_command_t) == 4, "kb_snapshot_update_command_t size");
KB_ABI_STATIC_ASSERT(KB_ABI_ALIGNOF(kb_snapshot_update_command_t) == 4, "kb_snapshot_update_command_t alignment");
KB_ABI_STATIC_ASSERT(sizeof(kb_status_t) == 4, "kb_status_t size");
KB_ABI_STATIC_ASSERT(KB_ABI_ALIGNOF(kb_status_t) == 4, "kb_status_t alignment");
KB_ABI_STATIC_ASSERT(sizeof(kb_transfer_state_t) == 4, "kb_transfer_state_t size");
KB_ABI_STATIC_ASSERT(KB_ABI_ALIGNOF(kb_transfer_state_t) == 4, "kb_transfer_state_t alignment");

KB_ABI_STATIC_ASSERT(sizeof(kb_command_options_t) == 40, "kb_command_options_t size");
KB_ABI_STATIC_ASSERT(KB_ABI_ALIGNOF(kb_command_options_t) == 8, "kb_command_options_t alignment");
KB_ABI_STATIC_ASSERT(offsetof(kb_command_options_t, struct_size) == 0, "kb_command_options_t.struct_size offset");
KB_ABI_STATIC_ASSERT(sizeof(((kb_command_options_t *)0)->struct_size) == 4, "kb_command_options_t.struct_size size");
KB_ABI_STATIC_ASSERT(offsetof(kb_command_options_t, api_version) == 4, "kb_command_options_t.api_version offset");
KB_ABI_STATIC_ASSERT(sizeof(((kb_command_options_t *)0)->api_version) == 4, "kb_command_options_t.api_version size");
KB_ABI_STATIC_ASSERT(offsetof(kb_command_options_t, timeout_ms) == 8, "kb_command_options_t.timeout_ms offset");
KB_ABI_STATIC_ASSERT(sizeof(((kb_command_options_t *)0)->timeout_ms) == 4, "kb_command_options_t.timeout_ms size");
KB_ABI_STATIC_ASSERT(offsetof(kb_command_options_t, progress_callback) == 16, "kb_command_options_t.progress_callback offset");
KB_ABI_STATIC_ASSERT(sizeof(((kb_command_options_t *)0)->progress_callback) == 8, "kb_command_options_t.progress_callback size");
KB_ABI_STATIC_ASSERT(offsetof(kb_command_options_t, progress_user_data) == 24, "kb_command_options_t.progress_user_data offset");
KB_ABI_STATIC_ASSERT(sizeof(((kb_command_options_t *)0)->progress_user_data) == 8, "kb_command_options_t.progress_user_data size");
KB_ABI_STATIC_ASSERT(offsetof(kb_command_options_t, maximum_receive_bytes) == 32, "kb_command_options_t.maximum_receive_bytes offset");
KB_ABI_STATIC_ASSERT(sizeof(((kb_command_options_t *)0)->maximum_receive_bytes) == 8, "kb_command_options_t.maximum_receive_bytes size");
KB_ABI_STATIC_ASSERT(KB_COMMAND_OPTIONS_V1_SIZE == 40, "KB_COMMAND_OPTIONS_V1_SIZE value");

KB_ABI_STATIC_ASSERT(sizeof(kb_context_options_t) == 24, "kb_context_options_t size");
KB_ABI_STATIC_ASSERT(KB_ABI_ALIGNOF(kb_context_options_t) == 8, "kb_context_options_t alignment");
KB_ABI_STATIC_ASSERT(offsetof(kb_context_options_t, struct_size) == 0, "kb_context_options_t.struct_size offset");
KB_ABI_STATIC_ASSERT(sizeof(((kb_context_options_t *)0)->struct_size) == 4, "kb_context_options_t.struct_size size");
KB_ABI_STATIC_ASSERT(offsetof(kb_context_options_t, api_version) == 4, "kb_context_options_t.api_version offset");
KB_ABI_STATIC_ASSERT(sizeof(((kb_context_options_t *)0)->api_version) == 4, "kb_context_options_t.api_version size");
KB_ABI_STATIC_ASSERT(offsetof(kb_context_options_t, log_callback) == 8, "kb_context_options_t.log_callback offset");
KB_ABI_STATIC_ASSERT(sizeof(((kb_context_options_t *)0)->log_callback) == 8, "kb_context_options_t.log_callback size");
KB_ABI_STATIC_ASSERT(offsetof(kb_context_options_t, log_user_data) == 16, "kb_context_options_t.log_user_data offset");
KB_ABI_STATIC_ASSERT(sizeof(((kb_context_options_t *)0)->log_user_data) == 8, "kb_context_options_t.log_user_data size");
KB_ABI_STATIC_ASSERT(KB_CONTEXT_OPTIONS_V1_SIZE == 24, "KB_CONTEXT_OPTIONS_V1_SIZE value");

KB_ABI_STATIC_ASSERT(sizeof(kb_flash_options_t) == 64, "kb_flash_options_t size");
KB_ABI_STATIC_ASSERT(KB_ABI_ALIGNOF(kb_flash_options_t) == 8, "kb_flash_options_t alignment");
KB_ABI_STATIC_ASSERT(offsetof(kb_flash_options_t, struct_size) == 0, "kb_flash_options_t.struct_size offset");
KB_ABI_STATIC_ASSERT(sizeof(((kb_flash_options_t *)0)->struct_size) == 4, "kb_flash_options_t.struct_size size");
KB_ABI_STATIC_ASSERT(offsetof(kb_flash_options_t, api_version) == 4, "kb_flash_options_t.api_version offset");
KB_ABI_STATIC_ASSERT(sizeof(((kb_flash_options_t *)0)->api_version) == 4, "kb_flash_options_t.api_version size");
KB_ABI_STATIC_ASSERT(offsetof(kb_flash_options_t, timeout_ms) == 8, "kb_flash_options_t.timeout_ms offset");
KB_ABI_STATIC_ASSERT(sizeof(((kb_flash_options_t *)0)->timeout_ms) == 4, "kb_flash_options_t.timeout_ms size");
KB_ABI_STATIC_ASSERT(offsetof(kb_flash_options_t, progress_callback) == 16, "kb_flash_options_t.progress_callback offset");
KB_ABI_STATIC_ASSERT(sizeof(((kb_flash_options_t *)0)->progress_callback) == 8, "kb_flash_options_t.progress_callback size");
KB_ABI_STATIC_ASSERT(offsetof(kb_flash_options_t, progress_user_data) == 24, "kb_flash_options_t.progress_user_data offset");
KB_ABI_STATIC_ASSERT(sizeof(((kb_flash_options_t *)0)->progress_user_data) == 8, "kb_flash_options_t.progress_user_data size");
KB_ABI_STATIC_ASSERT(offsetof(kb_flash_options_t, disable_verity) == 32, "kb_flash_options_t.disable_verity offset");
KB_ABI_STATIC_ASSERT(sizeof(((kb_flash_options_t *)0)->disable_verity) == 4, "kb_flash_options_t.disable_verity size");
KB_ABI_STATIC_ASSERT(offsetof(kb_flash_options_t, disable_verification) == 36, "kb_flash_options_t.disable_verification offset");
KB_ABI_STATIC_ASSERT(sizeof(((kb_flash_options_t *)0)->disable_verification) == 4, "kb_flash_options_t.disable_verification size");
KB_ABI_STATIC_ASSERT(offsetof(kb_flash_options_t, slot) == 40, "kb_flash_options_t.slot offset");
KB_ABI_STATIC_ASSERT(sizeof(((kb_flash_options_t *)0)->slot) == 8, "kb_flash_options_t.slot size");
KB_ABI_STATIC_ASSERT(offsetof(kb_flash_options_t, set_active) == 48, "kb_flash_options_t.set_active offset");
KB_ABI_STATIC_ASSERT(sizeof(((kb_flash_options_t *)0)->set_active) == 4, "kb_flash_options_t.set_active size");
KB_ABI_STATIC_ASSERT(offsetof(kb_flash_options_t, active_slot) == 56, "kb_flash_options_t.active_slot offset");
KB_ABI_STATIC_ASSERT(sizeof(((kb_flash_options_t *)0)->active_slot) == 8, "kb_flash_options_t.active_slot size");
KB_ABI_STATIC_ASSERT(KB_FLASH_OPTIONS_SLOT_POLICY_SIZE == 64, "KB_FLASH_OPTIONS_SLOT_POLICY_SIZE value");

KB_ABI_STATIC_ASSERT(sizeof(kb_job_options_t) == 32, "kb_job_options_t size");
KB_ABI_STATIC_ASSERT(KB_ABI_ALIGNOF(kb_job_options_t) == 8, "kb_job_options_t alignment");
KB_ABI_STATIC_ASSERT(offsetof(kb_job_options_t, struct_size) == 0, "kb_job_options_t.struct_size offset");
KB_ABI_STATIC_ASSERT(sizeof(((kb_job_options_t *)0)->struct_size) == 4, "kb_job_options_t.struct_size size");
KB_ABI_STATIC_ASSERT(offsetof(kb_job_options_t, api_version) == 4, "kb_job_options_t.api_version offset");
KB_ABI_STATIC_ASSERT(sizeof(((kb_job_options_t *)0)->api_version) == 4, "kb_job_options_t.api_version size");
KB_ABI_STATIC_ASSERT(offsetof(kb_job_options_t, timeout_ms) == 8, "kb_job_options_t.timeout_ms offset");
KB_ABI_STATIC_ASSERT(sizeof(((kb_job_options_t *)0)->timeout_ms) == 4, "kb_job_options_t.timeout_ms size");
KB_ABI_STATIC_ASSERT(offsetof(kb_job_options_t, progress_callback) == 16, "kb_job_options_t.progress_callback offset");
KB_ABI_STATIC_ASSERT(sizeof(((kb_job_options_t *)0)->progress_callback) == 8, "kb_job_options_t.progress_callback size");
KB_ABI_STATIC_ASSERT(offsetof(kb_job_options_t, progress_user_data) == 24, "kb_job_options_t.progress_user_data offset");
KB_ABI_STATIC_ASSERT(sizeof(((kb_job_options_t *)0)->progress_user_data) == 8, "kb_job_options_t.progress_user_data size");
KB_ABI_STATIC_ASSERT(KB_JOB_OPTIONS_V1_SIZE == 32, "KB_JOB_OPTIONS_V1_SIZE value");

KB_ABI_STATIC_ASSERT(sizeof(kb_legacy_boot_options_t) == 40, "kb_legacy_boot_options_t size");
KB_ABI_STATIC_ASSERT(KB_ABI_ALIGNOF(kb_legacy_boot_options_t) == 8, "kb_legacy_boot_options_t alignment");
KB_ABI_STATIC_ASSERT(offsetof(kb_legacy_boot_options_t, struct_size) == 0, "kb_legacy_boot_options_t.struct_size offset");
KB_ABI_STATIC_ASSERT(sizeof(((kb_legacy_boot_options_t *)0)->struct_size) == 4, "kb_legacy_boot_options_t.struct_size size");
KB_ABI_STATIC_ASSERT(offsetof(kb_legacy_boot_options_t, api_version) == 4, "kb_legacy_boot_options_t.api_version offset");
KB_ABI_STATIC_ASSERT(sizeof(((kb_legacy_boot_options_t *)0)->api_version) == 4, "kb_legacy_boot_options_t.api_version size");
KB_ABI_STATIC_ASSERT(offsetof(kb_legacy_boot_options_t, command_line) == 8, "kb_legacy_boot_options_t.command_line offset");
KB_ABI_STATIC_ASSERT(sizeof(((kb_legacy_boot_options_t *)0)->command_line) == 8, "kb_legacy_boot_options_t.command_line size");
KB_ABI_STATIC_ASSERT(offsetof(kb_legacy_boot_options_t, base) == 16, "kb_legacy_boot_options_t.base offset");
KB_ABI_STATIC_ASSERT(sizeof(((kb_legacy_boot_options_t *)0)->base) == 4, "kb_legacy_boot_options_t.base size");
KB_ABI_STATIC_ASSERT(offsetof(kb_legacy_boot_options_t, page_size) == 20, "kb_legacy_boot_options_t.page_size offset");
KB_ABI_STATIC_ASSERT(sizeof(((kb_legacy_boot_options_t *)0)->page_size) == 4, "kb_legacy_boot_options_t.page_size size");
KB_ABI_STATIC_ASSERT(offsetof(kb_legacy_boot_options_t, kernel_offset) == 24, "kb_legacy_boot_options_t.kernel_offset offset");
KB_ABI_STATIC_ASSERT(sizeof(((kb_legacy_boot_options_t *)0)->kernel_offset) == 4, "kb_legacy_boot_options_t.kernel_offset size");
KB_ABI_STATIC_ASSERT(offsetof(kb_legacy_boot_options_t, ramdisk_offset) == 28, "kb_legacy_boot_options_t.ramdisk_offset offset");
KB_ABI_STATIC_ASSERT(sizeof(((kb_legacy_boot_options_t *)0)->ramdisk_offset) == 4, "kb_legacy_boot_options_t.ramdisk_offset size");
KB_ABI_STATIC_ASSERT(offsetof(kb_legacy_boot_options_t, second_offset) == 32, "kb_legacy_boot_options_t.second_offset offset");
KB_ABI_STATIC_ASSERT(sizeof(((kb_legacy_boot_options_t *)0)->second_offset) == 4, "kb_legacy_boot_options_t.second_offset size");
KB_ABI_STATIC_ASSERT(offsetof(kb_legacy_boot_options_t, tags_offset) == 36, "kb_legacy_boot_options_t.tags_offset offset");
KB_ABI_STATIC_ASSERT(sizeof(((kb_legacy_boot_options_t *)0)->tags_offset) == 4, "kb_legacy_boot_options_t.tags_offset size");
KB_ABI_STATIC_ASSERT(KB_LEGACY_BOOT_OPTIONS_V1_SIZE == 40, "KB_LEGACY_BOOT_OPTIONS_V1_SIZE value");

KB_ABI_STATIC_ASSERT(sizeof(kb_progress_t) == 40, "kb_progress_t size");
KB_ABI_STATIC_ASSERT(KB_ABI_ALIGNOF(kb_progress_t) == 8, "kb_progress_t alignment");
KB_ABI_STATIC_ASSERT(offsetof(kb_progress_t, struct_size) == 0, "kb_progress_t.struct_size offset");
KB_ABI_STATIC_ASSERT(sizeof(((kb_progress_t *)0)->struct_size) == 4, "kb_progress_t.struct_size size");
KB_ABI_STATIC_ASSERT(offsetof(kb_progress_t, api_version) == 4, "kb_progress_t.api_version offset");
KB_ABI_STATIC_ASSERT(sizeof(((kb_progress_t *)0)->api_version) == 4, "kb_progress_t.api_version size");
KB_ABI_STATIC_ASSERT(offsetof(kb_progress_t, bytes_completed) == 8, "kb_progress_t.bytes_completed offset");
KB_ABI_STATIC_ASSERT(sizeof(((kb_progress_t *)0)->bytes_completed) == 8, "kb_progress_t.bytes_completed size");
KB_ABI_STATIC_ASSERT(offsetof(kb_progress_t, bytes_total) == 16, "kb_progress_t.bytes_total offset");
KB_ABI_STATIC_ASSERT(sizeof(((kb_progress_t *)0)->bytes_total) == 8, "kb_progress_t.bytes_total size");
KB_ABI_STATIC_ASSERT(offsetof(kb_progress_t, stage) == 24, "kb_progress_t.stage offset");
KB_ABI_STATIC_ASSERT(sizeof(((kb_progress_t *)0)->stage) == 8, "kb_progress_t.stage size");
KB_ABI_STATIC_ASSERT(offsetof(kb_progress_t, device_identifier) == 32, "kb_progress_t.device_identifier offset");
KB_ABI_STATIC_ASSERT(sizeof(((kb_progress_t *)0)->device_identifier) == 8, "kb_progress_t.device_identifier size");

KB_ABI_STATIC_ASSERT(sizeof(kb_update_options_t) == 80, "kb_update_options_t size");
KB_ABI_STATIC_ASSERT(KB_ABI_ALIGNOF(kb_update_options_t) == 8, "kb_update_options_t alignment");
KB_ABI_STATIC_ASSERT(offsetof(kb_update_options_t, struct_size) == 0, "kb_update_options_t.struct_size offset");
KB_ABI_STATIC_ASSERT(sizeof(((kb_update_options_t *)0)->struct_size) == 4, "kb_update_options_t.struct_size size");
KB_ABI_STATIC_ASSERT(offsetof(kb_update_options_t, api_version) == 4, "kb_update_options_t.api_version offset");
KB_ABI_STATIC_ASSERT(sizeof(((kb_update_options_t *)0)->api_version) == 4, "kb_update_options_t.api_version size");
KB_ABI_STATIC_ASSERT(offsetof(kb_update_options_t, timeout_ms) == 8, "kb_update_options_t.timeout_ms offset");
KB_ABI_STATIC_ASSERT(sizeof(((kb_update_options_t *)0)->timeout_ms) == 4, "kb_update_options_t.timeout_ms size");
KB_ABI_STATIC_ASSERT(offsetof(kb_update_options_t, wipe) == 12, "kb_update_options_t.wipe offset");
KB_ABI_STATIC_ASSERT(sizeof(((kb_update_options_t *)0)->wipe) == 4, "kb_update_options_t.wipe size");
KB_ABI_STATIC_ASSERT(offsetof(kb_update_options_t, progress_callback) == 16, "kb_update_options_t.progress_callback offset");
KB_ABI_STATIC_ASSERT(sizeof(((kb_update_options_t *)0)->progress_callback) == 8, "kb_update_options_t.progress_callback size");
KB_ABI_STATIC_ASSERT(offsetof(kb_update_options_t, progress_user_data) == 24, "kb_update_options_t.progress_user_data offset");
KB_ABI_STATIC_ASSERT(sizeof(((kb_update_options_t *)0)->progress_user_data) == 8, "kb_update_options_t.progress_user_data size");
KB_ABI_STATIC_ASSERT(offsetof(kb_update_options_t, skip_reboot) == 32, "kb_update_options_t.skip_reboot offset");
KB_ABI_STATIC_ASSERT(sizeof(((kb_update_options_t *)0)->skip_reboot) == 4, "kb_update_options_t.skip_reboot size");
KB_ABI_STATIC_ASSERT(offsetof(kb_update_options_t, skip_secondary) == 36, "kb_update_options_t.skip_secondary offset");
KB_ABI_STATIC_ASSERT(sizeof(((kb_update_options_t *)0)->skip_secondary) == 4, "kb_update_options_t.skip_secondary size");
KB_ABI_STATIC_ASSERT(offsetof(kb_update_options_t, exclude_dynamic_partitions) == 40, "kb_update_options_t.exclude_dynamic_partitions offset");
KB_ABI_STATIC_ASSERT(sizeof(((kb_update_options_t *)0)->exclude_dynamic_partitions) == 4, "kb_update_options_t.exclude_dynamic_partitions size");
KB_ABI_STATIC_ASSERT(offsetof(kb_update_options_t, disable_fastboot_info) == 44, "kb_update_options_t.disable_fastboot_info offset");
KB_ABI_STATIC_ASSERT(sizeof(((kb_update_options_t *)0)->disable_fastboot_info) == 4, "kb_update_options_t.disable_fastboot_info size");
KB_ABI_STATIC_ASSERT(offsetof(kb_update_options_t, disable_verity) == 48, "kb_update_options_t.disable_verity offset");
KB_ABI_STATIC_ASSERT(sizeof(((kb_update_options_t *)0)->disable_verity) == 4, "kb_update_options_t.disable_verity size");
KB_ABI_STATIC_ASSERT(offsetof(kb_update_options_t, disable_verification) == 52, "kb_update_options_t.disable_verification offset");
KB_ABI_STATIC_ASSERT(sizeof(((kb_update_options_t *)0)->disable_verification) == 4, "kb_update_options_t.disable_verification size");
KB_ABI_STATIC_ASSERT(offsetof(kb_update_options_t, slot) == 56, "kb_update_options_t.slot offset");
KB_ABI_STATIC_ASSERT(sizeof(((kb_update_options_t *)0)->slot) == 8, "kb_update_options_t.slot size");
KB_ABI_STATIC_ASSERT(offsetof(kb_update_options_t, set_active) == 64, "kb_update_options_t.set_active offset");
KB_ABI_STATIC_ASSERT(sizeof(((kb_update_options_t *)0)->set_active) == 4, "kb_update_options_t.set_active size");
KB_ABI_STATIC_ASSERT(offsetof(kb_update_options_t, active_slot) == 72, "kb_update_options_t.active_slot offset");
KB_ABI_STATIC_ASSERT(sizeof(((kb_update_options_t *)0)->active_slot) == 8, "kb_update_options_t.active_slot size");
KB_ABI_STATIC_ASSERT(KB_UPDATE_OPTIONS_SLOT_POLICY_SIZE == 80, "KB_UPDATE_OPTIONS_SLOT_POLICY_SIZE value");

KB_ABI_STATIC_ASSERT(sizeof(kb_version_t) == 32, "kb_version_t size");
KB_ABI_STATIC_ASSERT(KB_ABI_ALIGNOF(kb_version_t) == 8, "kb_version_t alignment");
KB_ABI_STATIC_ASSERT(offsetof(kb_version_t, struct_size) == 0, "kb_version_t.struct_size offset");
KB_ABI_STATIC_ASSERT(sizeof(((kb_version_t *)0)->struct_size) == 4, "kb_version_t.struct_size size");
KB_ABI_STATIC_ASSERT(offsetof(kb_version_t, api_version) == 4, "kb_version_t.api_version offset");
KB_ABI_STATIC_ASSERT(sizeof(((kb_version_t *)0)->api_version) == 4, "kb_version_t.api_version size");
KB_ABI_STATIC_ASSERT(offsetof(kb_version_t, major) == 8, "kb_version_t.major offset");
KB_ABI_STATIC_ASSERT(sizeof(((kb_version_t *)0)->major) == 4, "kb_version_t.major size");
KB_ABI_STATIC_ASSERT(offsetof(kb_version_t, minor) == 12, "kb_version_t.minor offset");
KB_ABI_STATIC_ASSERT(sizeof(((kb_version_t *)0)->minor) == 4, "kb_version_t.minor size");
KB_ABI_STATIC_ASSERT(offsetof(kb_version_t, patch) == 16, "kb_version_t.patch offset");
KB_ABI_STATIC_ASSERT(sizeof(((kb_version_t *)0)->patch) == 4, "kb_version_t.patch size");
KB_ABI_STATIC_ASSERT(offsetof(kb_version_t, string) == 24, "kb_version_t.string offset");
KB_ABI_STATIC_ASSERT(sizeof(((kb_version_t *)0)->string) == 8, "kb_version_t.string size");
KB_ABI_STATIC_ASSERT(KB_VERSION_V1_SIZE == 32, "KB_VERSION_V1_SIZE value");

#undef KB_ABI_STATIC_ASSERT
#undef KB_ABI_ALIGNOF

#endif
