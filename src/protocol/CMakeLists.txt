# SPDX-License-Identifier: MIT

add_library(kairosboot_protocol STATIC
    fastboot_protocol.cpp
)

target_include_directories(kairosboot_protocol
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}"
)

target_compile_features(kairosboot_protocol PUBLIC cxx_std_23)
set_target_properties(kairosboot_protocol PROPERTIES
    CXX_EXTENSIONS OFF
)

if(MSVC)
    target_compile_options(kairosboot_protocol PRIVATE /W4 /WX)
else()
    target_compile_options(kairosboot_protocol PRIVATE
        -Wall -Wextra -Wpedantic -Werror
    )
endif()
