# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.24)

project(KairosBootTcpTests LANGUAGES CXX)

enable_testing()

add_executable(kairosboot_tcp_tests
    tcp_fastboot_tests.cpp
    scripted_socket.cpp
    "${CMAKE_CURRENT_LIST_DIR}/../../src/transport/tcp_fastboot.cpp"
)

target_include_directories(kairosboot_tcp_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_LIST_DIR}/../../src/transport"
    "${CMAKE_CURRENT_LIST_DIR}/../../src/protocol"
)
target_compile_features(kairosboot_tcp_tests PRIVATE cxx_std_23)
set_target_properties(kairosboot_tcp_tests PROPERTIES CXX_EXTENSIONS OFF)

if(MSVC)
    target_compile_options(kairosboot_tcp_tests PRIVATE /W4 /WX)
    target_link_libraries(kairosboot_tcp_tests PRIVATE Ws2_32)
else()
    target_compile_options(kairosboot_tcp_tests PRIVATE
        -Wall -Wextra -Wpedantic -Werror
    )
endif()

add_test(NAME tcp-fastboot COMMAND kairosboot_tcp_tests)
set_tests_properties(tcp-fastboot PROPERTIES TIMEOUT 10)

if(NOT WIN32)
    add_executable(kairosboot_tcp_sigpipe_regression
        sigpipe_regression.cpp
        "${CMAKE_CURRENT_LIST_DIR}/../../src/transport/tcp_fastboot.cpp"
    )
    target_include_directories(kairosboot_tcp_sigpipe_regression PRIVATE
        "${CMAKE_CURRENT_LIST_DIR}/../../src/transport"
        "${CMAKE_CURRENT_LIST_DIR}/../../src/protocol"
    )
    target_compile_features(kairosboot_tcp_sigpipe_regression PRIVATE cxx_std_23)
    set_target_properties(kairosboot_tcp_sigpipe_regression PROPERTIES
        CXX_EXTENSIONS OFF
    )
    target_compile_options(kairosboot_tcp_sigpipe_regression PRIVATE
        -Wall -Wextra -Wpedantic -Werror
    )

    add_test(NAME tcp-sigpipe-regression COMMAND kairosboot_tcp_sigpipe_regression)
    set_tests_properties(tcp-sigpipe-regression PROPERTIES TIMEOUT 10)
endif()
