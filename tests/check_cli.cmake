# SPDX-License-Identifier: MIT

if(NOT DEFINED CLI OR NOT EXISTS "${CLI}")
  message(FATAL_ERROR "CLI executable is missing: ${CLI}")
endif()

function(run_cli NAME)
  execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
            KAIROSBOOT_INTERNAL_TEST_TIMEOUT_MS=50 "${CLI}" ${ARGN}
    RESULT_VARIABLE "${NAME}_RESULT"
    OUTPUT_VARIABLE "${NAME}_OUTPUT"
    ERROR_VARIABLE "${NAME}_ERROR"
    TIMEOUT 10)
  set("${NAME}_RESULT" "${${NAME}_RESULT}" PARENT_SCOPE)
  set("${NAME}_OUTPUT" "${${NAME}_OUTPUT}" PARENT_SCOPE)
  set("${NAME}_ERROR" "${${NAME}_ERROR}" PARENT_SCOPE)
endfunction()

function(expect_exit NAME EXPECTED)
  set(ARGUMENTS ${ARGN})
  run_cli(${NAME} ${ARGUMENTS})
  if(NOT "${${NAME}_RESULT}" STREQUAL "${EXPECTED}")
    message(FATAL_ERROR
      "${NAME}: exit ${${NAME}_RESULT}, expected ${EXPECTED}\n"
      "stdout=${${NAME}_OUTPUT}\nstderr=${${NAME}_ERROR}")
  endif()
  foreach(SUFFIX IN ITEMS RESULT OUTPUT ERROR)
    set("${NAME}_${SUFFIX}" "${${NAME}_${SUFFIX}}" PARENT_SCOPE)
  endforeach()
endfunction()

function(expect_usage NAME EXPECTED_TEXT)
  set(ARGUMENTS ${ARGN})
  expect_exit(${NAME} 2 ${ARGUMENTS})
  string(FIND "${${NAME}_ERROR}" "${EXPECTED_TEXT}" POSITION)
  if(POSITION EQUAL -1)
    message(FATAL_ERROR
      "${NAME}: missing usage error '${EXPECTED_TEXT}' in ${${NAME}_ERROR}")
  endif()
endfunction()

function(expect_accepted NAME)
  set(ARGUMENTS ${ARGN})
  run_cli(${NAME} ${ARGUMENTS})
  if("${${NAME}_RESULT}" STREQUAL "2")
    message(FATAL_ERROR
      "${NAME}: official Fastboot syntax was rejected\n"
      "stdout=${${NAME}_OUTPUT}\nstderr=${${NAME}_ERROR}")
  endif()
endfunction()

expect_exit(version 0 --version)
string(FIND "${version_OUTPUT}" "${EXPECTED_VERSION}" VERSION_POSITION)
if(VERSION_POSITION EQUAL -1)
  message(FATAL_ERROR "--version did not contain ${EXPECTED_VERSION}")
endif()

expect_exit(help 0 --help)
foreach(REQUIRED IN ITEMS
    "usage: kairosboot [OPTION...] COMMAND..."
    "devices [-l]"
    "set_active SLOT"
    "get_staged OUT_FILE"
    "lock_critical|unlock_critical"
    "get_unlock_ability"
    "-s SERIAL"
    "-w"
    "--unbuffered")
  string(FIND "${help_OUTPUT}" "${REQUIRED}" POSITION)
  if(POSITION EQUAL -1)
    message(FATAL_ERROR "help is missing official Fastboot syntax: ${REQUIRED}")
  endif()
endforeach()

foreach(FORBIDDEN IN ITEMS
    "doctor" "validate <" "plan <" "run <" "make-boot-image"
    "boot-staged" " get-staged " " set-active " "--device" "--serial"
    "--json" "--timeout-ms" "--max-receive-bytes" "--vendor-id")
  string(FIND "${help_OUTPUT}" "${FORBIDDEN}" POSITION)
  if(NOT POSITION EQUAL -1)
    message(FATAL_ERROR "help exposes non-Fastboot syntax: ${FORBIDDEN}")
  endif()
endforeach()

expect_usage(old_set_active "unknown command" set-active a)
expect_usage(old_get_staged "unknown command" get-staged out.bin)
expect_usage(old_flashing "flashing command must" flashing lock-critical)
expect_usage(doctor "unknown command" doctor)
expect_usage(raw "unknown command" raw command)
expect_usage(upload "unknown command" upload out.bin)
expect_usage(boot_staged "unknown command" boot-staged)
expect_usage(make_boot_image "unknown command" make-boot-image out boot)
expect_usage(device_option "unknown option" --device serial getvar product)
expect_usage(serial_option "unknown option" --serial serial getvar product)
expect_usage(duplicate_serial "duplicate device selector"
             -s tcp:127.0.0.1:1 -s tcp:127.0.0.1:1 getvar product)
expect_usage(json_option "unknown option" --json devices)
expect_usage(fetch_extra "fetch requires exactly" fetch system out extra)
expect_usage(reboot_target "reboot target must be bootloader" reboot recovery)

expect_exit(devices 0 devices)
expect_exit(devices_long 0 devices -l)
expect_exit(devices_long_global 0 -l devices)

expect_accepted(set_active set_active a)
expect_accepted(get_staged -s tcp:127.0.0.1:1 get_staged out.bin)
expect_accepted(flashing_lock_critical -s tcp:127.0.0.1:1 flashing lock_critical)
expect_accepted(flashing_unlock_critical -s tcp:127.0.0.1:1 flashing unlock_critical)
expect_accepted(flashing_ability -s tcp:127.0.0.1:1 flashing get_unlock_ability)
expect_accepted(reboot_bootloader -s tcp:127.0.0.1:1 reboot-bootloader)
expect_accepted(reboot_recovery -s tcp:127.0.0.1:1 reboot-recovery)
expect_accepted(reboot_fastboot -s tcp:127.0.0.1:1 reboot-fastboot)
expect_accepted(repeated_serials
                -s tcp:127.0.0.1:1 -s tcp:127.0.0.1:2 getvar product)
expect_accepted(flash_option_after_command flash boot missing.img --slot a)
expect_accepted(update_options -s tcp:127.0.0.1:1 -w --skip-reboot
                --skip-secondary --exclude-dynamic-partitions
                --disable-fastboot-info --disable-super-optimization flashall)
expect_accepted(update_options_after_command flashall -w --skip-reboot
                --skip-secondary --exclude-dynamic-partitions
                --disable-fastboot-info --disable-super-optimization
                -s tcp:127.0.0.1:1)

message(STATUS "KairosBoot CLI matches the frozen Fastboot command syntax")
