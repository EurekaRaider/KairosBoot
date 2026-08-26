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
  duplicate_serial "option --serial may only be specified once" --serial A
  --serial B flash boot image.img)
expect_text_parse_error(
  missing_serial "option --serial requires a non-empty value" --serial)
expect_text_parse_error(
  trailing_global_option "global options must precede the command" flash boot
  image.img --json)

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
