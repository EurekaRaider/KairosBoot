# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.24)

project(KairosBootNormalizedDifferentialTests LANGUAGES NONE)

include(CTest)
find_package(Python3 3.9 REQUIRED COMPONENTS Interpreter)

if(NOT CMAKE_BUILD_TYPE STREQUAL "Release")
  message(FATAL_ERROR
          "Normalized differential gates must be configured in Release mode")
endif()

set(KAIROSBOOT_REPOSITORY_ROOT "${CMAKE_CURRENT_LIST_DIR}/../..")
set(KAIROSBOOT_DIFFERENTIAL_TEST
    "${CMAKE_CURRENT_LIST_DIR}/test_normalized_differential.py")
set(KAIROSBOOT_OFFICIAL_DIFFERENTIAL_TEST
    "${CMAKE_CURRENT_LIST_DIR}/test_official_fastboot_differential.py")
set(KAIROSBOOT_OFFICIAL_DIFFERENTIAL_RUNNER
    "${KAIROSBOOT_REPOSITORY_ROOT}/scripts/run_official_fastboot_differential.py")

set(KAIROSBOOT_AOSP_FASTBOOT_BINARY "" CACHE FILEPATH
    "Official locked Platform-Tools fastboot binary for opt-in differential capture")
set(KAIROSBOOT_DIFFERENTIAL_KAIROSBOOT_CLI "" CACHE FILEPATH
    "Release KairosBoot CLI for opt-in official differential capture")
option(KAIROSBOOT_REQUIRE_OFFICIAL_DIFFERENTIAL
       "Fail instead of skip when official differential binaries are absent" OFF)

add_test(
  NAME normalized_differential_contract
  COMMAND
    "${Python3_EXECUTABLE}" "${KAIROSBOOT_DIFFERENTIAL_TEST}"
    --repository-root "${KAIROSBOOT_REPOSITORY_ROOT}")
set_tests_properties(
  normalized_differential_contract
  PROPERTIES
    ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
    LABELS "compatibility;differential"
    TIMEOUT 30)

add_test(
  NAME official_differential_runner_contract
  COMMAND
    "${Python3_EXECUTABLE}" "${KAIROSBOOT_OFFICIAL_DIFFERENTIAL_TEST}"
    --repository-root "${KAIROSBOOT_REPOSITORY_ROOT}")
set_tests_properties(
  official_differential_runner_contract
  PROPERTIES
    ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
    LABELS "compatibility;differential;fixture"
    TIMEOUT 30)

set(KAIROSBOOT_OFFICIAL_DIFFERENTIAL_COMMAND
    "${Python3_EXECUTABLE}" "${KAIROSBOOT_OFFICIAL_DIFFERENTIAL_RUNNER}"
    --repository-root "${KAIROSBOOT_REPOSITORY_ROOT}"
    --output-dir "${CMAKE_CURRENT_BINARY_DIR}/official-differential-evidence")
if(KAIROSBOOT_AOSP_FASTBOOT_BINARY)
  list(APPEND KAIROSBOOT_OFFICIAL_DIFFERENTIAL_COMMAND
       --fastboot "${KAIROSBOOT_AOSP_FASTBOOT_BINARY}")
endif()
if(KAIROSBOOT_DIFFERENTIAL_KAIROSBOOT_CLI)
  list(APPEND KAIROSBOOT_OFFICIAL_DIFFERENTIAL_COMMAND
       --kairosboot "${KAIROSBOOT_DIFFERENTIAL_KAIROSBOOT_CLI}")
endif()
if(KAIROSBOOT_REQUIRE_OFFICIAL_DIFFERENTIAL)
  list(APPEND KAIROSBOOT_OFFICIAL_DIFFERENTIAL_COMMAND --require)
endif()

add_test(NAME official_aosp_differential_capture
         COMMAND ${KAIROSBOOT_OFFICIAL_DIFFERENTIAL_COMMAND})
set_tests_properties(
  official_aosp_differential_capture
  PROPERTIES
    ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
    LABELS "compatibility;differential;official;opt-in"
    SKIP_RETURN_CODE 77
    TIMEOUT 120)
