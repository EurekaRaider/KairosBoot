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
