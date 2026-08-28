execute_process(
  COMMAND "${CLI}" --version
  RESULT_VARIABLE VERSION_RESULT
  OUTPUT_VARIABLE VERSION_OUTPUT
  ERROR_VARIABLE VERSION_ERROR
  OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT VERSION_RESULT EQUAL 0)
  message(FATAL_ERROR "--version failed: ${VERSION_ERROR}")
endif()
if(NOT VERSION_OUTPUT STREQUAL "KairosBoot ${EXPECTED_VERSION}")
  message(FATAL_ERROR "unexpected --version output: ${VERSION_OUTPUT}")
endif()

execute_process(
  COMMAND "${CLI}" --help
  RESULT_VARIABLE HELP_RESULT
  OUTPUT_VARIABLE HELP_OUTPUT
  ERROR_VARIABLE HELP_ERROR
  OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_STRIP_TRAILING_WHITESPACE)
if(NOT HELP_RESULT EQUAL 0 OR NOT HELP_ERROR STREQUAL "")
  message(FATAL_ERROR "--help failed: ${HELP_RESULT} ${HELP_ERROR}")
endif()
foreach(HELP_COMMAND IN ITEMS validate plan update flashall flash:raw flashing gsi
                              snapshot-update create-logical-partition
                              delete-logical-partition
                              resize-logical-partition)
  string(FIND "${HELP_OUTPUT}" " ${HELP_COMMAND}" HELP_COMMAND_POSITION)
  if(HELP_COMMAND_POSITION EQUAL -1)
    message(FATAL_ERROR
            "--help is missing ${HELP_COMMAND}: ${HELP_OUTPUT}")
  endif()
endforeach()

execute_process(
  COMMAND "${CLI}" --help --json
  RESULT_VARIABLE JSON_HELP_RESULT
  OUTPUT_VARIABLE JSON_HELP_OUTPUT
  ERROR_VARIABLE JSON_HELP_ERROR
  OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_STRIP_TRAILING_WHITESPACE)
string(JSON JSON_HELP_OK GET "${JSON_HELP_OUTPUT}" ok)
string(JSON JSON_HELP_COMMAND GET "${JSON_HELP_OUTPUT}" command)
if(NOT JSON_HELP_RESULT EQUAL 0 OR NOT JSON_HELP_ERROR STREQUAL "" OR
   NOT JSON_HELP_OK OR NOT JSON_HELP_COMMAND STREQUAL "help")
  message(FATAL_ERROR "unexpected --help --json output: ${JSON_HELP_OUTPUT}")
endif()

execute_process(
  COMMAND "${CLI}" --version --json
  RESULT_VARIABLE JSON_VERSION_RESULT
  OUTPUT_VARIABLE JSON_VERSION_OUTPUT
  ERROR_VARIABLE JSON_VERSION_ERROR
  OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT JSON_VERSION_RESULT EQUAL 0)
  message(FATAL_ERROR "--version --json failed: ${JSON_VERSION_ERROR}")
endif()
string(JSON JSON_VERSION GET "${JSON_VERSION_OUTPUT}" version)
string(JSON JSON_API_VERSION GET "${JSON_VERSION_OUTPUT}" apiVersion)
if(NOT JSON_VERSION STREQUAL EXPECTED_VERSION OR NOT JSON_API_VERSION EQUAL 1)
  message(FATAL_ERROR "unexpected version JSON: ${JSON_VERSION_OUTPUT}")
endif()

execute_process(
  COMMAND "${CLI}" doctor --json
  RESULT_VARIABLE DOCTOR_RESULT
  OUTPUT_VARIABLE DOCTOR_OUTPUT
  ERROR_VARIABLE DOCTOR_ERROR
  OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT DOCTOR_RESULT EQUAL 0)
  message(FATAL_ERROR "doctor failed: ${DOCTOR_ERROR}")
endif()
string(JSON DOCTOR_OK GET "${DOCTOR_OUTPUT}" ok)
string(JSON DOCTOR_TRANSPORT GET "${DOCTOR_OUTPUT}" transport available)
string(JSON DOCTOR_DEVICE_COUNT GET "${DOCTOR_OUTPUT}" deviceCount)
if(NOT DOCTOR_OK OR NOT DOCTOR_TRANSPORT OR DOCTOR_DEVICE_COUNT LESS 0)
  message(FATAL_ERROR "unexpected doctor JSON: ${DOCTOR_OUTPUT}")
endif()

execute_process(
  COMMAND "${CLI}" devices --json
  RESULT_VARIABLE DEVICES_RESULT
  OUTPUT_VARIABLE DEVICES_OUTPUT
  ERROR_VARIABLE DEVICES_ERROR
  OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT DEVICES_RESULT EQUAL 0)
  message(FATAL_ERROR "devices --json failed: ${DEVICES_ERROR}")
endif()
string(JSON DEVICES_OK GET "${DEVICES_OUTPUT}" ok)
string(JSON DEVICES_TYPE TYPE "${DEVICES_OUTPUT}" devices)
if(NOT DEVICES_OK OR NOT DEVICES_TYPE STREQUAL "ARRAY")
  message(FATAL_ERROR "unexpected devices JSON: ${DEVICES_OUTPUT}")
endif()

function(expect_text_parse_error NAME EXPECTED_MESSAGE)
  execute_process(
    COMMAND "${CLI}" ${ARGN}
    RESULT_VARIABLE PARSE_RESULT
    OUTPUT_VARIABLE PARSE_OUTPUT
    ERROR_VARIABLE PARSE_ERROR
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_STRIP_TRAILING_WHITESPACE)
  if(NOT PARSE_RESULT EQUAL 2)
    message(FATAL_ERROR
            "${NAME}: expected exit 2, got ${PARSE_RESULT}: ${PARSE_ERROR}")
  endif()
  if(NOT PARSE_OUTPUT STREQUAL "")
    message(FATAL_ERROR "${NAME}: unexpected stdout: ${PARSE_OUTPUT}")
  endif()
  set(EXPECTED_PREFIX "kairosboot: ${EXPECTED_MESSAGE}\nUsage:\n")
  string(FIND "${PARSE_ERROR}" "${EXPECTED_PREFIX}" PREFIX_POSITION)
  if(NOT PREFIX_POSITION EQUAL 0)
    message(FATAL_ERROR "${NAME}: unexpected stderr: ${PARSE_ERROR}")
  endif()
endfunction()

expect_text_parse_error(
  flash_arity "flash requires exactly <partition> and <file>" flash boot)
expect_text_parse_error(
  flash_raw_arity
  "flash:raw requires <partition> <kernel> [ramdisk [second]]" flash:raw boot)
expect_text_parse_error(
  boot_arity "boot requires <kernel> [ramdisk [second]]" boot)
expect_text_parse_error(
  boot_excess_operands "boot requires <kernel> [ramdisk [second]]" boot kernel
  ramdisk second extra)
expect_text_parse_error(
  legacy_option_scope "legacy boot layout options are not valid for flash"
  --base 0x10000000 flash boot image.img)
expect_text_parse_error(
  update_arity "update requires <package>" update)
expect_text_parse_error(
  update_wipe_without_package "update requires <package>" update --wipe)
expect_text_parse_error(
  duplicate_serial "option --serial may only be specified once" --serial A
  --serial B flash boot image.img)
expect_text_parse_error(
  missing_serial "option --serial requires a non-empty value" --serial)
expect_text_parse_error(
  trailing_global_option "global options must precede the command" flash boot
  image.img --json)
expect_text_parse_error(
  force_scope
  "option --force is valid only for flash, flash:raw, format, update, and flashall"
  --force getvar product)
expect_text_parse_error(
  fs_options_scope "option --fs-options is valid only for format"
  --fs-options=casefold flash system image.img)
expect_text_parse_error(
  fs_options_unknown
  "option --fs-options supports only unique casefold, projid, and compress values"
  --fs-options=casefold,unknown format:ext4 userdata)
expect_text_parse_error(
  fs_options_duplicate
  "option --fs-options supports only unique casefold, projid, and compress values"
  --fs-options=casefold,casefold format:ext4 userdata)

execute_process(
  COMMAND "${CLI}" --serial "" flash boot image.img
  RESULT_VARIABLE EMPTY_SERIAL_RESULT
  OUTPUT_VARIABLE EMPTY_SERIAL_OUTPUT
  ERROR_VARIABLE EMPTY_SERIAL_ERROR
  OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_STRIP_TRAILING_WHITESPACE)
if(NOT EMPTY_SERIAL_RESULT EQUAL 2 OR NOT EMPTY_SERIAL_OUTPUT STREQUAL "")
  message(FATAL_ERROR
          "empty serial did not produce a text parse error: ${EMPTY_SERIAL_ERROR}")
endif()
set(EMPTY_SERIAL_PREFIX
    "kairosboot: option --serial requires a non-empty value\nUsage:\n")
string(FIND "${EMPTY_SERIAL_ERROR}" "${EMPTY_SERIAL_PREFIX}"
            EMPTY_SERIAL_PREFIX_POSITION)
if(NOT EMPTY_SERIAL_PREFIX_POSITION EQUAL 0)
  message(FATAL_ERROR "unexpected empty serial error: ${EMPTY_SERIAL_ERROR}")
endif()

execute_process(
  COMMAND "${CLI}" --json flash boot
  RESULT_VARIABLE JSON_PARSE_RESULT
  OUTPUT_VARIABLE JSON_PARSE_OUTPUT
  ERROR_VARIABLE JSON_PARSE_ERROR
  OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_STRIP_TRAILING_WHITESPACE)
set(EXPECTED_JSON_PARSE
    "{\"ok\":false,\"status\":\"invalid_argument\",\"message\":\"flash requires exactly <partition> and <file>\"}")
if(NOT JSON_PARSE_RESULT EQUAL 2 OR NOT JSON_PARSE_ERROR STREQUAL "" OR
   NOT JSON_PARSE_OUTPUT STREQUAL EXPECTED_JSON_PARSE)
  message(FATAL_ERROR
          "unexpected JSON parse contract: ${JSON_PARSE_OUTPUT} ${JSON_PARSE_ERROR}")
endif()

execute_process(
  COMMAND "${CLI}" --json --json flash boot image.img
  RESULT_VARIABLE DUPLICATE_JSON_RESULT
  OUTPUT_VARIABLE DUPLICATE_JSON_OUTPUT
  ERROR_VARIABLE DUPLICATE_JSON_ERROR
  OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_STRIP_TRAILING_WHITESPACE)
set(EXPECTED_DUPLICATE_JSON
    "{\"ok\":false,\"status\":\"invalid_argument\",\"message\":\"option --json may only be specified once\"}")
if(NOT DUPLICATE_JSON_RESULT EQUAL 2 OR NOT DUPLICATE_JSON_ERROR STREQUAL "" OR
   NOT DUPLICATE_JSON_OUTPUT STREQUAL EXPECTED_DUPLICATE_JSON)
  message(FATAL_ERROR
          "unexpected duplicate JSON contract: ${DUPLICATE_JSON_OUTPUT} ${DUPLICATE_JSON_ERROR}")
endif()

foreach(FLASH_PREFIX IN ITEMS "--serial;CLI-NO-SUCH-DEVICE;--json"
                              "--json;--serial;CLI-NO-SUCH-DEVICE")
  execute_process(
    COMMAND "${CLI}" ${FLASH_PREFIX} flash boot
            __kairosboot_cli_missing_image_4d2fba59__.img
    RESULT_VARIABLE FLASH_RESULT
    OUTPUT_VARIABLE FLASH_OUTPUT
    ERROR_VARIABLE FLASH_ERROR
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_STRIP_TRAILING_WHITESPACE)
  if(NOT FLASH_RESULT EQUAL 4 OR NOT FLASH_ERROR STREQUAL "")
    message(FATAL_ERROR
            "valid flash syntax did not reach runtime: ${FLASH_RESULT} ${FLASH_ERROR}")
  endif()
  string(JSON FLASH_OK GET "${FLASH_OUTPUT}" ok)
  string(JSON FLASH_STATUS GET "${FLASH_OUTPUT}" status)
  string(JSON FLASH_MESSAGE GET "${FLASH_OUTPUT}" message)
  if(FLASH_OK OR FLASH_STATUS STREQUAL "invalid_argument" OR
     FLASH_MESSAGE STREQUAL "")
    message(FATAL_ERROR "unexpected flash runtime JSON: ${FLASH_OUTPUT}")
  endif()
endforeach()

execute_process(
  COMMAND "${CLI}" --serial CLI-NO-SUCH-DEVICE flash boot
          __kairosboot_cli_missing_image_4d2fba59__.img
  RESULT_VARIABLE TEXT_FLASH_RESULT
  OUTPUT_VARIABLE TEXT_FLASH_OUTPUT
  ERROR_VARIABLE TEXT_FLASH_ERROR
  OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_STRIP_TRAILING_WHITESPACE)
string(FIND "${TEXT_FLASH_ERROR}" "kairosboot: " TEXT_FLASH_PREFIX_POSITION)
string(FIND "${TEXT_FLASH_ERROR}" "Usage:" TEXT_FLASH_USAGE_POSITION)
if(NOT TEXT_FLASH_RESULT EQUAL 4 OR NOT TEXT_FLASH_OUTPUT STREQUAL "" OR
   NOT TEXT_FLASH_PREFIX_POSITION EQUAL 0 OR
   NOT TEXT_FLASH_USAGE_POSITION EQUAL -1)
  message(FATAL_ERROR
          "valid text flash syntax did not reach runtime: ${TEXT_FLASH_RESULT} ${TEXT_FLASH_ERROR}")
endif()

function(expect_json_parse_error NAME EXPECTED_MESSAGE)
  execute_process(
    COMMAND "${CLI}" --json ${ARGN}
    RESULT_VARIABLE PARSE_RESULT
    OUTPUT_VARIABLE PARSE_OUTPUT
    ERROR_VARIABLE PARSE_ERROR
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_STRIP_TRAILING_WHITESPACE)
  if(NOT PARSE_RESULT EQUAL 2 OR NOT PARSE_ERROR STREQUAL "")
    message(FATAL_ERROR
            "${NAME}: expected JSON parse exit 2: ${PARSE_RESULT} ${PARSE_ERROR}")
  endif()
  string(JSON PARSE_OK GET "${PARSE_OUTPUT}" ok)
  string(JSON PARSE_STATUS GET "${PARSE_OUTPUT}" status)
  string(JSON PARSE_MESSAGE GET "${PARSE_OUTPUT}" message)
  if(PARSE_OK OR NOT PARSE_STATUS STREQUAL "invalid_argument" OR
     NOT PARSE_MESSAGE STREQUAL EXPECTED_MESSAGE)
    message(FATAL_ERROR "${NAME}: unexpected JSON parse error: ${PARSE_OUTPUT}")
  endif()
endfunction()

expect_json_parse_error(
  duplicate_device "option --device may only be specified once" --device A
  --device B getvar product)
expect_json_parse_error(
  mixed_selectors "options --device and --serial are mutually exclusive"
  --device A --serial B getvar product)
expect_json_parse_error(
  missing_device "option --device requires a non-empty value" --device)
expect_json_parse_error(
  missing_timeout
  "option --timeout-ms requires an integer in [0, 4294967294]" --timeout-ms)
expect_json_parse_error(
  negative_timeout
  "option --timeout-ms requires an integer in [0, 4294967294]" --timeout-ms -1
  getvar product)
expect_json_parse_error(
  overflow_timeout
  "option --timeout-ms requires an integer in [0, 4294967294]" --timeout-ms
  4294967295 getvar product)
expect_json_parse_error(
  duplicate_timeout "option --timeout-ms may only be specified once"
  --timeout-ms 1 --timeout-ms 2 getvar product)
expect_json_parse_error(
  missing_receive_limit
  "option --max-receive-bytes requires a positive integer"
  --max-receive-bytes)
expect_json_parse_error(
  zero_receive_limit
  "option --max-receive-bytes requires a positive integer"
  --max-receive-bytes 0 upload output.bin)
expect_json_parse_error(
  overflow_receive_limit
  "option --max-receive-bytes requires a positive integer"
  --max-receive-bytes 18446744073709551616 upload output.bin)
expect_json_parse_error(
  duplicate_receive_limit
  "option --max-receive-bytes may only be specified once"
  --max-receive-bytes 1 --max-receive-bytes 2 upload output.bin)
expect_json_parse_error(
  receive_limit_flash "option --max-receive-bytes is not valid for flash"
  --max-receive-bytes 1 flash boot image.img)
expect_json_parse_error(
  update_arity "update requires <package>" update)
expect_json_parse_error(
  update_wipe_without_package "update requires <package>" update --wipe)
expect_json_parse_error(
  update_unknown_option
  "update supports only --wipe, --skip-reboot, --skip-secondary, --exclude-dynamic-partitions, --disable-fastboot-info and --disable-super-optimization after <package>"
  update
  package.zip --unknown)
expect_json_parse_error(
  update_duplicate_wipe "update option --wipe may only be specified once"
  update package.zip --wipe --wipe)
expect_json_parse_error(
  flashall_operand
  "flashall supports only --wipe, --skip-reboot, --skip-secondary, --exclude-dynamic-partitions, --disable-fastboot-info and --disable-super-optimization"
  flashall images)
expect_json_parse_error(
  flashall_unknown_option
  "flashall supports only --wipe, --skip-reboot, --skip-secondary, --exclude-dynamic-partitions, --disable-fastboot-info and --disable-super-optimization"
  flashall --unknown)
expect_json_parse_error(
  flashall_duplicate_wipe "flashall option --wipe may only be specified once"
  flashall --wipe --wipe)
expect_json_parse_error(
  update_duplicate_policy
  "update option --skip-reboot may only be specified once"
  update package.zip --skip-reboot --skip-reboot)
expect_json_parse_error(
  receive_limit_update "option --max-receive-bytes is not valid for update"
  --max-receive-bytes 1 update package.zip)
expect_json_parse_error(
  receive_limit_flashall
  "option --max-receive-bytes is not valid for flashall"
  --max-receive-bytes 1 flashall)
expect_json_parse_error(
  slot_missing "option --slot requires a non-empty slot" --slot)
expect_json_parse_error(
  slot_duplicate "option --slot may only be specified once"
  --slot a --slot b flash system image.img)
expect_json_parse_error(
  set_active_duplicate "option --set-active may only be specified once"
  --set-active --set-active=b flash system image.img)
expect_json_parse_error(
  set_active_empty
  "option --set-active requires a non-empty slot after '='"
  --set-active= flash system image.img)
expect_json_parse_error(
  slot_wrong_command
  "options --slot and --set-active are valid only for flash, flash:raw, update, and flashall"
  --slot a getvar product)
expect_json_parse_error(
  typed_trailing_global "global options must precede the command" getvar
  product --timeout-ms 1)
expect_json_parse_error(
  invalid_reboot_target
  "reboot target must be system, bootloader, recovery, or fastboot" reboot
  invalid)
expect_json_parse_error(
  reboot_arity
  "reboot accepts at most one target: system, bootloader, recovery, or fastboot"
  reboot system extra)
expect_json_parse_error(
  continue_arity "continue does not accept operands" continue extra)
expect_json_parse_error(
  boot_staged_arity "boot-staged does not accept operands" boot-staged extra)
expect_json_parse_error(
  getvar_arity "getvar requires exactly <variable>" getvar)
expect_json_parse_error(
  erase_arity "erase requires exactly <partition>" erase)
expect_json_parse_error(
  set_active_arity "set-active requires exactly <slot>" set-active)
expect_json_parse_error(
  oem_arity "oem requires a command string" oem)
expect_json_parse_error(
  raw_arity "raw requires a command string" raw)
expect_json_parse_error(
  stage_arity "stage requires exactly <file>" stage)
expect_json_parse_error(
  upload_arity "upload requires exactly <output>" upload)
expect_json_parse_error(
  flashing_arity
  "flashing requires exactly <lock|unlock|lock-critical|unlock-critical|get-unlock-ability>"
  flashing)
expect_json_parse_error(
  flashing_command
  "flashing command must be lock, unlock, lock-critical, unlock-critical, or get-unlock-ability"
  flashing sideways)
expect_json_parse_error(
  gsi_arity "gsi requires exactly <wipe|disable|status>" gsi)
expect_json_parse_error(
  gsi_command "gsi command must be wipe, disable, or status" gsi enable)
expect_json_parse_error(
  snapshot_update_arity
  "snapshot-update requires exactly <cancel|merge>" snapshot-update)
expect_json_parse_error(
  snapshot_update_command
  "snapshot-update command must be cancel or merge" snapshot-update pause)
expect_json_parse_error(
  create_logical_arity
  "create-logical-partition requires exactly <partition> and <size-bytes>"
  create-logical-partition system_ext)
expect_json_parse_error(
  create_logical_negative_size
  "create-logical-partition size-bytes requires an integer in [0, 18446744073709551615]"
  create-logical-partition system_ext -1)
expect_json_parse_error(
  create_logical_overflow_size
  "create-logical-partition size-bytes requires an integer in [0, 18446744073709551615]"
  create-logical-partition system_ext 18446744073709551616)
expect_json_parse_error(
  delete_logical_arity
  "delete-logical-partition requires exactly <partition>"
  delete-logical-partition)
expect_json_parse_error(
  resize_logical_arity
  "resize-logical-partition requires exactly <partition> and <size-bytes>"
  resize-logical-partition system_ext)
expect_json_parse_error(
  resize_logical_bad_size
  "resize-logical-partition size-bytes requires an integer in [0, 18446744073709551615]"
  resize-logical-partition system_ext 12x)
expect_json_parse_error(
  fetch_arity "fetch requires <partition> and <output>" fetch vendor)
expect_json_parse_error(
  fetch_size_without_offset "fetch --size requires --offset" fetch vendor
  output.bin --size 1)
expect_json_parse_error(
  fetch_duplicate_offset
  "fetch option --offset may only be specified once" fetch vendor output.bin
  --offset 1 --offset 2)
expect_json_parse_error(
  fetch_bad_offset
  "fetch option --offset requires an integer in [0, 9223372036854775807]"
  fetch vendor output.bin --offset nope)
expect_json_parse_error(
  fetch_overflow_size
  "fetch option --size requires an integer in [0, 9223372036854775807]"
  fetch vendor output.bin --offset 0 --size 9223372036854775808)
expect_json_parse_error(
  fetch_unknown_option
  "fetch supports only --offset <bytes> and --size <bytes> after <output>"
  fetch vendor output.bin --length 1)

function(expect_json_runtime NAME)
  execute_process(
    COMMAND "${CLI}" --device tcp:127.0.0.1:1 --json --timeout-ms 100
            ${ARGN}
    RESULT_VARIABLE RUNTIME_RESULT
    OUTPUT_VARIABLE RUNTIME_OUTPUT
    ERROR_VARIABLE RUNTIME_ERROR
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_STRIP_TRAILING_WHITESPACE
    TIMEOUT 10)
  if(NOT RUNTIME_RESULT EQUAL 4 OR NOT RUNTIME_ERROR STREQUAL "")
    message(FATAL_ERROR
            "${NAME}: valid syntax did not reach runtime: ${RUNTIME_RESULT} ${RUNTIME_ERROR}")
  endif()
  string(JSON RUNTIME_OK GET "${RUNTIME_OUTPUT}" ok)
  string(JSON RUNTIME_STATUS GET "${RUNTIME_OUTPUT}" status)
  string(JSON RUNTIME_MESSAGE GET "${RUNTIME_OUTPUT}" message)
  if(RUNTIME_OK OR RUNTIME_STATUS STREQUAL "invalid_argument" OR
     RUNTIME_MESSAGE STREQUAL "")
    message(FATAL_ERROR "${NAME}: unexpected runtime JSON: ${RUNTIME_OUTPUT}")
  endif()
endfunction()

set(CLI_STAGE_FILE "${CMAKE_CURRENT_BINARY_DIR}/kairosboot-cli-stage.bin")
set(CLI_EMPTY_STAGE_FILE
    "${CMAKE_CURRENT_BINARY_DIR}/kairosboot-cli-empty-stage.bin")
set(CLI_UPLOAD_FILE "${CMAKE_CURRENT_BINARY_DIR}/kairosboot-cli-upload.bin")
set(CLI_FETCH_FILE "${CMAKE_CURRENT_BINARY_DIR}/kairosboot-cli-fetch.bin")
set(CLI_UPDATE_PACKAGE
    "${CMAKE_CURRENT_BINARY_DIR}/kairosboot-cli-update-package")
file(MAKE_DIRECTORY "${CLI_UPDATE_PACKAGE}")
file(WRITE "${CLI_UPDATE_PACKAGE}/android-info.txt" "")
# Keep the fixture byte-identical on every host. CMake may translate the final
# newline to CRLF on Windows, while the frozen AOSP lexer intentionally treats
# only a literal space as a token separator.
file(WRITE "${CLI_UPDATE_PACKAGE}/fastboot-info.txt" "version 1")
file(WRITE "${CLI_STAGE_FILE}" "stage-data")
file(WRITE "${CLI_EMPTY_STAGE_FILE}" "")
file(REMOVE "${CLI_UPLOAD_FILE}" "${CLI_FETCH_FILE}")

expect_json_runtime(getvar_runtime getvar product)
expect_json_runtime(erase_runtime erase userdata)
expect_json_runtime(set_active_runtime set-active a)
expect_json_runtime(reboot_default_runtime reboot)
foreach(REBOOT_TARGET IN ITEMS system bootloader recovery fastboot)
  expect_json_runtime(reboot_${REBOOT_TARGET}_runtime reboot ${REBOOT_TARGET})
endforeach()
expect_json_runtime(continue_runtime continue)
expect_json_runtime(oem_runtime oem flashing unlock)
expect_json_runtime(raw_runtime raw getvar:product)
expect_json_runtime(boot_staged_runtime boot-staged)
expect_json_runtime(stage_runtime stage "${CLI_STAGE_FILE}")
expect_json_runtime(upload_runtime --max-receive-bytes 16 upload
                    "${CLI_UPLOAD_FILE}")
expect_json_runtime(fetch_runtime --max-receive-bytes 16 fetch vendor
                    "${CLI_FETCH_FILE}" --offset 2 --size 3)
expect_json_runtime(update_runtime update "${CLI_UPDATE_PACKAGE}" --wipe)
foreach(FLASHING_ACTION IN ITEMS lock unlock lock-critical unlock-critical
                                 get-unlock-ability)
  expect_json_runtime(flashing_${FLASHING_ACTION}_runtime flashing
                      ${FLASHING_ACTION})
endforeach()
foreach(GSI_ACTION IN ITEMS wipe disable status)
  expect_json_runtime(gsi_${GSI_ACTION}_runtime gsi ${GSI_ACTION})
endforeach()
foreach(SNAPSHOT_ACTION IN ITEMS cancel merge)
  expect_json_runtime(snapshot_${SNAPSHOT_ACTION}_runtime snapshot-update
                      ${SNAPSHOT_ACTION})
endforeach()
expect_json_runtime(create_logical_runtime create-logical-partition system_ext
                    0)
expect_json_runtime(delete_logical_runtime delete-logical-partition system_ext)
expect_json_runtime(resize_logical_runtime resize-logical-partition system_ext
                    18446744073709551615)

execute_process(
  COMMAND "${CLI}" --device tcp:127.0.0.1:1 --json stage
          "${CLI_EMPTY_STAGE_FILE}"
  RESULT_VARIABLE EMPTY_STAGE_RESULT
  OUTPUT_VARIABLE EMPTY_STAGE_OUTPUT
  ERROR_VARIABLE EMPTY_STAGE_ERROR
  OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_STRIP_TRAILING_WHITESPACE)
string(JSON EMPTY_STAGE_OK GET "${EMPTY_STAGE_OUTPUT}" ok)
string(JSON EMPTY_STAGE_STATUS GET "${EMPTY_STAGE_OUTPUT}" status)
if(NOT EMPTY_STAGE_RESULT EQUAL 4 OR EMPTY_STAGE_OK OR
   NOT EMPTY_STAGE_STATUS STREQUAL "invalid_argument" OR
   NOT EMPTY_STAGE_ERROR STREQUAL "")
  message(FATAL_ERROR
          "empty stage file did not fail before transport: ${EMPTY_STAGE_OUTPUT} ${EMPTY_STAGE_ERROR}")
endif()

execute_process(
  COMMAND "${CLI}" --device udp:127.0.0.1:1 --json --timeout-ms 20 getvar
          product
  RESULT_VARIABLE UDP_RUNTIME_RESULT
  OUTPUT_VARIABLE UDP_RUNTIME_OUTPUT
  ERROR_VARIABLE UDP_RUNTIME_ERROR
  OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_STRIP_TRAILING_WHITESPACE
  TIMEOUT 10)
string(JSON UDP_RUNTIME_OK GET "${UDP_RUNTIME_OUTPUT}" ok)
if(NOT UDP_RUNTIME_RESULT EQUAL 4 OR UDP_RUNTIME_OK OR
   NOT UDP_RUNTIME_ERROR STREQUAL "")
  message(FATAL_ERROR
          "valid UDP selector did not reach runtime: ${UDP_RUNTIME_RESULT} ${UDP_RUNTIME_OUTPUT} ${UDP_RUNTIME_ERROR}")
endif()

find_program(CLI_PYTHON_EXECUTABLE NAMES python3 python)
if(NOT CLI_PYTHON_EXECUTABLE)
  message(FATAL_ERROR "Python is required for scripted CLI TCP tests")
endif()
execute_process(
  COMMAND "${CLI_PYTHON_EXECUTABLE}"
          "${CMAKE_CURRENT_LIST_DIR}/cli/scripted_cli_test.py" --cli "${CLI}"
  RESULT_VARIABLE SCRIPTED_RESULT
  OUTPUT_VARIABLE SCRIPTED_OUTPUT
  ERROR_VARIABLE SCRIPTED_ERROR
  OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_STRIP_TRAILING_WHITESPACE
  TIMEOUT 90)
if(NOT SCRIPTED_RESULT EQUAL 0)
  message(FATAL_ERROR
          "scripted CLI TCP test failed: ${SCRIPTED_RESULT}\nstdout: ${SCRIPTED_OUTPUT}\nstderr: ${SCRIPTED_ERROR}")
endif()

file(REMOVE "${CLI_STAGE_FILE}" "${CLI_EMPTY_STAGE_FILE}"
            "${CLI_UPLOAD_FILE}" "${CLI_FETCH_FILE}")
file(REMOVE_RECURSE "${CLI_UPDATE_PACKAGE}")
