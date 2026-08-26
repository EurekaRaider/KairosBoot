cmake_minimum_required(VERSION 3.22)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/version.json" KAIROSBOOT_VERSION_JSON)
string(JSON KAIROSBOOT_VERSION_STRING GET "${KAIROSBOOT_VERSION_JSON}" version)
string(REGEX MATCH "^([0-9]+)\\.([0-9]+)\\.([0-9]+)" KAIROSBOOT_VERSION_MATCH
             "${KAIROSBOOT_VERSION_STRING}")
if(NOT KAIROSBOOT_VERSION_MATCH)
  message(FATAL_ERROR "version.json must contain a semantic version")
endif()

project(
  KairosBoot
  VERSION "${CMAKE_MATCH_1}.${CMAKE_MATCH_2}.${CMAKE_MATCH_3}"
  DESCRIPTION "Cross-platform host SDK and CLI for the Fastboot protocol"
  LANGUAGES C CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(CTest)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

function(kairosboot_enable_strict_warnings target)
  if(MSVC)
    target_compile_options(
      ${target} PRIVATE /W4 /WX "$<$<COMPILE_LANGUAGE:CXX>:/Zc:__cplusplus>")
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
endfunction()

add_library(
  kairosboot_internal STATIC
  src/fleet/controller_scheduler.cpp
  src/protocol/fastboot_protocol.cpp
  src/transport/adaptive_tuner.cpp
  src/transport/buffer_budget.cpp
  src/transport/transfer_ring.cpp)
set_target_properties(
  kairosboot_internal
  PROPERTIES POSITION_INDEPENDENT_CODE ON
             CXX_VISIBILITY_PRESET hidden
             VISIBILITY_INLINES_HIDDEN YES)
target_compile_features(kairosboot_internal PUBLIC cxx_std_23)
target_include_directories(kairosboot_internal PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
kairosboot_enable_strict_warnings(kairosboot_internal)

add_library(kairosboot SHARED src/kairosboot.cpp)
add_library(KairosBoot::C ALIAS kairosboot)
set_target_properties(
  kairosboot
  PROPERTIES EXPORT_NAME C
             OUTPUT_NAME kairosboot
             VERSION "${PROJECT_VERSION}"
             SOVERSION "${PROJECT_VERSION_MAJOR}"
             CXX_VISIBILITY_PRESET hidden
             VISIBILITY_INLINES_HIDDEN YES)
target_compile_features(kairosboot PRIVATE cxx_std_23)
target_compile_definitions(
  kairosboot
  PRIVATE KAIROSBOOT_BUILDING_LIBRARY
          KAIROSBOOT_VERSION_STRING="${KAIROSBOOT_VERSION_STRING}"
          KAIROSBOOT_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
          KAIROSBOOT_VERSION_MINOR=${PROJECT_VERSION_MINOR}
          KAIROSBOOT_VERSION_PATCH=${PROJECT_VERSION_PATCH})
target_include_directories(
  kairosboot
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(kairosboot PRIVATE kairosboot_internal)
kairosboot_enable_strict_warnings(kairosboot)

add_library(kairosboot_cxx INTERFACE)
add_library(KairosBoot::Cxx ALIAS kairosboot_cxx)
set_target_properties(kairosboot_cxx PROPERTIES EXPORT_NAME Cxx)
target_compile_features(kairosboot_cxx INTERFACE cxx_std_23)
target_include_directories(
  kairosboot_cxx
  INTERFACE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(kairosboot_cxx INTERFACE KairosBoot::C)

add_executable(kairosboot_cli cli/main.cpp)
set_target_properties(kairosboot_cli PROPERTIES OUTPUT_NAME kairosboot)
if(APPLE)
  set_target_properties(
    kairosboot_cli
    PROPERTIES INSTALL_RPATH "@loader_path/../${CMAKE_INSTALL_LIBDIR}")
elseif(UNIX)
  set_target_properties(
    kairosboot_cli PROPERTIES INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}")
endif()
target_compile_features(kairosboot_cli PRIVATE cxx_std_23)
target_link_libraries(kairosboot_cli PRIVATE KairosBoot::Cxx)
kairosboot_enable_strict_warnings(kairosboot_cli)

option(KAIROSBOOT_BUILD_EXAMPLES "Build KairosBoot examples" ON)
if(KAIROSBOOT_BUILD_EXAMPLES)
  add_executable(kairosboot_example_c examples/list_devices.c)
  set_target_properties(
    kairosboot_example_c
    PROPERTIES C_STANDARD 11
               C_STANDARD_REQUIRED ON
               C_EXTENSIONS OFF)
  target_link_libraries(kairosboot_example_c PRIVATE KairosBoot::C)
  kairosboot_enable_strict_warnings(kairosboot_example_c)

  add_executable(kairosboot_example_cxx examples/version.cpp)
  target_compile_features(kairosboot_example_cxx PRIVATE cxx_std_23)
  target_link_libraries(kairosboot_example_cxx PRIVATE KairosBoot::Cxx)
  kairosboot_enable_strict_warnings(kairosboot_example_cxx)
endif()

if(BUILD_TESTING)
  add_executable(kairosboot_c_api_test tests/c_api_test.c)
  set_target_properties(
    kairosboot_c_api_test
    PROPERTIES C_STANDARD 11
               C_STANDARD_REQUIRED ON
               C_EXTENSIONS OFF)
  target_link_libraries(kairosboot_c_api_test PRIVATE KairosBoot::C)
  kairosboot_enable_strict_warnings(kairosboot_c_api_test)
  add_test(NAME c_api COMMAND kairosboot_c_api_test)

  add_executable(kairosboot_cxx_api_test tests/cxx_api_test.cpp)
  target_compile_features(kairosboot_cxx_api_test PRIVATE cxx_std_23)
  target_link_libraries(kairosboot_cxx_api_test PRIVATE KairosBoot::Cxx)
  kairosboot_enable_strict_warnings(kairosboot_cxx_api_test)
  add_test(NAME cxx_api COMMAND kairosboot_cxx_api_test)

  add_executable(kairosboot_protocol_tests tests/protocol/protocol_tests.cpp
                                            tests/protocol/scripted_transport.cpp)
  target_compile_features(kairosboot_protocol_tests PRIVATE cxx_std_23)
  target_include_directories(kairosboot_protocol_tests
                             PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
                                     "${CMAKE_CURRENT_SOURCE_DIR}/src/protocol")
  target_link_libraries(kairosboot_protocol_tests PRIVATE kairosboot_internal)
  kairosboot_enable_strict_warnings(kairosboot_protocol_tests)
  add_test(NAME protocol COMMAND kairosboot_protocol_tests)

  add_executable(kairosboot_performance_tests tests/performance/test_main.cpp)
  target_compile_features(kairosboot_performance_tests PRIVATE cxx_std_23)
  target_include_directories(kairosboot_performance_tests
                             PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  target_link_libraries(kairosboot_performance_tests PRIVATE kairosboot_internal)
  kairosboot_enable_strict_warnings(kairosboot_performance_tests)
  add_test(NAME performance COMMAND kairosboot_performance_tests)

  find_package(Python3 COMPONENTS Interpreter REQUIRED)
  add_test(
    NAME abi_symbols
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_abi.py" --library
            "$<TARGET_FILE:kairosboot>")

  add_test(
    NAME cli_contract
    COMMAND
      "${CMAKE_COMMAND}" "-DCLI=$<TARGET_FILE:kairosboot_cli>"
      "-DEXPECTED_VERSION=${KAIROSBOOT_VERSION_STRING}" -P
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_cli.cmake")
endif()

install(
  TARGETS kairosboot kairosboot_cxx kairosboot_cli
  EXPORT KairosBootTargets
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
  LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
install(DIRECTORY include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

configure_package_config_file(
  cmake/KairosBootConfig.cmake.in
  "${CMAKE_CURRENT_BINARY_DIR}/KairosBootConfig.cmake"
  INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/KairosBoot")
write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/KairosBootConfigVersion.cmake"
  VERSION "${PROJECT_VERSION}"
  COMPATIBILITY SameMajorVersion)
install(
  FILES "${CMAKE_CURRENT_BINARY_DIR}/KairosBootConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/KairosBootConfigVersion.cmake"
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/KairosBoot")
install(
  EXPORT KairosBootTargets
  FILE KairosBootTargets.cmake
  NAMESPACE KairosBoot::
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/KairosBoot")
