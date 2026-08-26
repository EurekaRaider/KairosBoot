cmake_minimum_required(VERSION 3.24)

project(kairosboot_libusb_runtime_slice LANGUAGES CXX)

set(LIBUSB_INCLUDE_DIR "" CACHE PATH "Directory containing libusb.h")
set(LIBUSB_LIBRARY "" CACHE FILEPATH "Path to the libusb 1.0 library")
set(LIBUSB_VERSION_FOR_CROSS_COMPILE "" CACHE STRING
    "Required when cross-compiling with explicit libusb paths")

if((LIBUSB_INCLUDE_DIR AND NOT LIBUSB_LIBRARY) OR
   (LIBUSB_LIBRARY AND NOT LIBUSB_INCLUDE_DIR))
    message(FATAL_ERROR
        "Set both LIBUSB_INCLUDE_DIR and LIBUSB_LIBRARY, or leave both empty for pkg-config")
endif()

if(LIBUSB_INCLUDE_DIR AND LIBUSB_LIBRARY)
    if(NOT EXISTS "${LIBUSB_INCLUDE_DIR}/libusb.h")
        message(FATAL_ERROR "LIBUSB_INCLUDE_DIR does not contain libusb.h: ${LIBUSB_INCLUDE_DIR}")
    endif()
    if(NOT EXISTS "${LIBUSB_LIBRARY}")
        message(FATAL_ERROR "LIBUSB_LIBRARY does not exist: ${LIBUSB_LIBRARY}")
    endif()

    add_library(kairosboot_libusb_dependency UNKNOWN IMPORTED)
    set_target_properties(kairosboot_libusb_dependency PROPERTIES
        IMPORTED_LOCATION "${LIBUSB_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${LIBUSB_INCLUDE_DIR}"
    )

    if(CMAKE_CROSSCOMPILING)
        if(NOT LIBUSB_VERSION_FOR_CROSS_COMPILE STREQUAL "1.0.30")
            message(FATAL_ERROR
                "Cross-compiling with explicit paths requires "
                "-DLIBUSB_VERSION_FOR_CROSS_COMPILE=1.0.30; runtime validation remains enabled")
        endif()
    else()
        set(_libusb_version_probe "${CMAKE_CURRENT_BINARY_DIR}/libusb_version_probe.cpp")
        file(WRITE "${_libusb_version_probe}" [=[
#include <libusb.h>
int main() {
    const libusb_version* version = libusb_get_version();
    return version != nullptr && version->major == 1 && version->minor == 0 &&
                   version->micro == 30 &&
                   (version->rc == nullptr || version->rc[0] == '\0')
               ? 0
               : 1;
}
]=])
        try_run(LIBUSB_VERSION_RUN_RESULT LIBUSB_VERSION_COMPILE_RESULT
            "${CMAKE_CURRENT_BINARY_DIR}/libusb_version_probe_build"
            "${_libusb_version_probe}"
            CMAKE_FLAGS
                "-DINCLUDE_DIRECTORIES:STRING=${LIBUSB_INCLUDE_DIR}"
                "-DCMAKE_CXX_STANDARD:STRING=23"
                "-DCMAKE_CXX_STANDARD_REQUIRED:BOOL=ON"
            LINK_LIBRARIES "${LIBUSB_LIBRARY}"
            COMPILE_OUTPUT_VARIABLE LIBUSB_VERSION_COMPILE_OUTPUT
            RUN_OUTPUT_VARIABLE LIBUSB_VERSION_RUN_OUTPUT
        )
        if(NOT LIBUSB_VERSION_COMPILE_RESULT)
            message(FATAL_ERROR
                "Failed to compile/link the libusb version probe:\n${LIBUSB_VERSION_COMPILE_OUTPUT}")
        endif()
        if(NOT LIBUSB_VERSION_RUN_RESULT EQUAL 0)
            message(FATAL_ERROR
                "KairosBoot requires libusb exactly 1.0.30; the explicit library reported another version")
        endif()
    endif()
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LIBUSB REQUIRED IMPORTED_TARGET "libusb-1.0=1.0.30")
    add_library(kairosboot_libusb_dependency ALIAS PkgConfig::LIBUSB)
endif()

find_package(Threads REQUIRED)
enable_testing()

add_executable(kairosboot_libusb_runtime_tests
    ../../src/transport/buffer_budget.cpp
    ../../src/transport/libusb_runtime.cpp
    ../../src/transport/transfer_ring.cpp
    ../../src/transport/usb_fastboot.cpp
    test_main.cpp
)

target_include_directories(kairosboot_libusb_runtime_tests PRIVATE ../..)
target_compile_features(kairosboot_libusb_runtime_tests PRIVATE cxx_std_23)
target_link_libraries(kairosboot_libusb_runtime_tests PRIVATE
    kairosboot_libusb_dependency
    ${CMAKE_DL_LIBS}
    Threads::Threads
)
set_target_properties(kairosboot_libusb_runtime_tests PROPERTIES
    CXX_EXTENSIONS OFF
)

if(MSVC)
    target_compile_options(kairosboot_libusb_runtime_tests PRIVATE /W4 /WX /permissive-)
else()
    target_compile_options(kairosboot_libusb_runtime_tests PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Werror
    )
endif()

add_test(NAME kairosboot.libusb-runtime COMMAND kairosboot_libusb_runtime_tests)
