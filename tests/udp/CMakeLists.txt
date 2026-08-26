# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.24)

project(KairosBootUdpTests LANGUAGES CXX)

enable_testing()
find_package(Threads REQUIRED)

set(KAIROSBOOT_UDP_SANITIZER "none" CACHE STRING
    "UDP test sanitizer: none, address-undefined, or thread")
set_property(CACHE KAIROSBOOT_UDP_SANITIZER PROPERTY STRINGS
    none address-undefined thread)

function(kairosboot_udp_test_settings target)
    target_include_directories(${target} PRIVATE
        "${CMAKE_CURRENT_LIST_DIR}/../../src/transport"
        "${CMAKE_CURRENT_LIST_DIR}/../../src/protocol"
        "${CMAKE_CURRENT_LIST_DIR}"
    )
    target_compile_features(${target} PRIVATE cxx_std_23)
    set_target_properties(${target} PROPERTIES CXX_EXTENSIONS OFF)
    target_link_libraries(${target} PRIVATE Threads::Threads)

    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX /permissive-)
        target_link_libraries(${target} PRIVATE Ws2_32)
        if(NOT KAIROSBOOT_UDP_SANITIZER STREQUAL "none")
            message(FATAL_ERROR "UDP sanitizer modes require Clang or GCC")
        endif()
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Werror
        )
        if(KAIROSBOOT_UDP_SANITIZER STREQUAL "address-undefined")
            target_compile_options(${target} PRIVATE
                -fno-omit-frame-pointer -fsanitize=address,undefined
            )
            target_link_options(${target} PRIVATE
                -fno-omit-frame-pointer -fsanitize=address,undefined
            )
        elseif(KAIROSBOOT_UDP_SANITIZER STREQUAL "thread")
            target_compile_options(${target} PRIVATE
                -fno-omit-frame-pointer -fsanitize=thread
            )
            target_link_options(${target} PRIVATE
                -fno-omit-frame-pointer -fsanitize=thread
            )
        elseif(NOT KAIROSBOOT_UDP_SANITIZER STREQUAL "none")
            message(FATAL_ERROR "Unknown KAIROSBOOT_UDP_SANITIZER value")
        endif()
    endif()
endfunction()

add_executable(kairosboot_udp_tests
    udp_fastboot_tests.cpp
    scripted_datagram.cpp
    "${CMAKE_CURRENT_LIST_DIR}/../../src/transport/udp_fastboot.cpp"
)
kairosboot_udp_test_settings(kairosboot_udp_tests)
add_test(NAME udp-fastboot COMMAND kairosboot_udp_tests)
set_tests_properties(udp-fastboot PROPERTIES TIMEOUT 15)

add_executable(kairosboot_udp_loopback
    udp_loopback_smoke.cpp
    "${CMAKE_CURRENT_LIST_DIR}/../../src/transport/udp_fastboot.cpp"
)
kairosboot_udp_test_settings(kairosboot_udp_loopback)
add_test(NAME udp-loopback COMMAND kairosboot_udp_loopback)
set_tests_properties(udp-loopback PROPERTIES TIMEOUT 15)
