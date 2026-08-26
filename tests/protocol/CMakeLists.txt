# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.24)

project(KairosBootProtocolTests LANGUAGES CXX)

enable_testing()

add_subdirectory(
    "${CMAKE_CURRENT_LIST_DIR}/../../src/protocol"
    "${CMAKE_CURRENT_BINARY_DIR}/protocol"
)

add_executable(kairosboot_protocol_tests
    protocol_tests.cpp
    scripted_transport.cpp
)

target_link_libraries(kairosboot_protocol_tests PRIVATE kairosboot_protocol)
target_include_directories(kairosboot_protocol_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
)
target_compile_features(kairosboot_protocol_tests PRIVATE cxx_std_23)
set_target_properties(kairosboot_protocol_tests PROPERTIES CXX_EXTENSIONS OFF)

if(MSVC)
    target_compile_options(kairosboot_protocol_tests PRIVATE /W4 /WX)
else()
    target_compile_options(kairosboot_protocol_tests PRIVATE
        -Wall -Wextra -Wpedantic -Werror
    )
endif()

add_test(NAME protocol COMMAND kairosboot_protocol_tests)
