# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.24)

project(KairosBootSparseTests LANGUAGES CXX)

enable_testing()

add_executable(kairosboot_sparse_tests
    sparse_image_tests.cpp
    "${CMAKE_CURRENT_LIST_DIR}/../../src/image/sparse_image.cpp"
)

target_include_directories(kairosboot_sparse_tests PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../src/image"
)
target_compile_features(kairosboot_sparse_tests PRIVATE cxx_std_23)
set_target_properties(kairosboot_sparse_tests PROPERTIES CXX_EXTENSIONS OFF)

if(MSVC)
    target_compile_options(kairosboot_sparse_tests PRIVATE /W4 /WX)
else()
    target_compile_options(kairosboot_sparse_tests PRIVATE
        -Wall -Wextra -Wpedantic -Werror
    )
endif()

add_test(NAME sparse-image COMMAND kairosboot_sparse_tests)
set_tests_properties(sparse-image PROPERTIES TIMEOUT 10)
