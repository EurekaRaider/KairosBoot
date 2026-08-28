# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.24)

project(KairosBootParserRobustnessTests LANGUAGES CXX)

include(CTest)

# This project is intentionally standalone. It has no install rules and is not
# added by the product's root CMake, so sanitizer instrumentation cannot enter
# native SDK or CLI Release assets.

set(KAIROSBOOT_PARSER_SANITIZER "address-undefined" CACHE STRING
    "Parser robustness sanitizer: address-undefined or none")
set_property(CACHE KAIROSBOOT_PARSER_SANITIZER PROPERTY STRINGS
             address-undefined none)

add_executable(kairosboot_parser_robustness_tests
    parser_robustness_tests.cpp
    "${CMAKE_CURRENT_LIST_DIR}/../../src/image/sparse_image.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../../src/protocol/fastboot_protocol.cpp"
)
target_include_directories(kairosboot_parser_robustness_tests PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../.."
    "${CMAKE_CURRENT_LIST_DIR}/../../src/image"
    "${CMAKE_CURRENT_LIST_DIR}/../../src/protocol"
)
target_compile_features(kairosboot_parser_robustness_tests PRIVATE cxx_std_23)
set_target_properties(kairosboot_parser_robustness_tests PROPERTIES
    CXX_EXTENSIONS OFF
)
if(MSVC)
    target_compile_options(kairosboot_parser_robustness_tests PRIVATE /W4 /WX)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(kairosboot_parser_robustness_tests PRIVATE
        -Wall -Wextra -Wpedantic -Werror
    )
else()
    message(FATAL_ERROR "Parser robustness tests require MSVC, Clang, or GCC")
endif()

if(KAIROSBOOT_PARSER_SANITIZER STREQUAL "address-undefined")
    if(MSVC)
        message(FATAL_ERROR
                "Combined AddressSanitizer/UBSan requires Clang or GCC")
    endif()
    target_compile_options(kairosboot_parser_robustness_tests PRIVATE
        -fno-omit-frame-pointer
        -fno-sanitize-recover=undefined
        -fsanitize=address,undefined
    )
    target_link_options(kairosboot_parser_robustness_tests PRIVATE
        -fno-sanitize-recover=undefined
        -fsanitize=address,undefined
    )
elseif(NOT KAIROSBOOT_PARSER_SANITIZER STREQUAL "none")
    message(FATAL_ERROR "Unknown KAIROSBOOT_PARSER_SANITIZER value")
endif()

add_test(
    NAME parser-regression-corpus
    COMMAND kairosboot_parser_robustness_tests
            --source-dir "${CMAKE_CURRENT_LIST_DIR}/../.." --replay-only
)
add_test(
    NAME parser-bounded-randomized-smoke
    COMMAND kairosboot_parser_robustness_tests
            --source-dir "${CMAKE_CURRENT_LIST_DIR}/../.."
            --random-cases 2048
)
set_tests_properties(
    parser-regression-corpus parser-bounded-randomized-smoke
    PROPERTIES
        ENVIRONMENT
            "ASAN_OPTIONS=allocator_may_return_null=1;UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1"
        LABELS "parser;robustness;sanitizer"
        TIMEOUT 30
)
