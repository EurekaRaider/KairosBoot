cmake_minimum_required(VERSION 3.24)

project(kairosboot_performance_slice LANGUAGES CXX)

enable_testing()

add_executable(kairosboot_performance_tests
    ../../src/fleet/controller_scheduler.cpp
    ../../src/transport/adaptive_tuner.cpp
    ../../src/transport/buffer_budget.cpp
    ../../src/transport/transfer_ring.cpp
    test_main.cpp
)

target_include_directories(kairosboot_performance_tests PRIVATE ../..)
target_compile_features(kairosboot_performance_tests PRIVATE cxx_std_23)
set_target_properties(kairosboot_performance_tests PROPERTIES
    CXX_EXTENSIONS OFF
)

if(MSVC)
    target_compile_options(kairosboot_performance_tests PRIVATE /W4 /WX /permissive-)
else()
    target_compile_options(kairosboot_performance_tests PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Werror
    )
endif()

add_test(NAME kairosboot.performance COMMAND kairosboot_performance_tests)
