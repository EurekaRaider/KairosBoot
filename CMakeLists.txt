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
include(FetchContent)
include(cmake/KairosBootLibusb.cmake)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(WIN32)
  add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

option(KAIROSBOOT_RELEASE_SYMBOLS
       "Generate external debug symbols without disabling Release optimization"
       OFF)
if(KAIROSBOOT_RELEASE_SYMBOLS)
  if(MSVC)
    add_compile_options("$<$<CONFIG:Release>:/Zi>")
    add_link_options("$<$<CONFIG:Release>:/DEBUG:FULL>"
                     "$<$<CONFIG:Release>:/OPT:REF>"
                     "$<$<CONFIG:Release>:/OPT:ICF>")
  else()
    add_compile_options("$<$<CONFIG:Release>:-g>")
  endif()
endif()

kairosboot_configure_libusb()
find_package(Threads REQUIRED)

# Boost.Asio is an internal, header-only transport dependency. The default is
# the newest stable Boost release validated by this source revision. Version,
# URL, and digest come from the committed dependency lock so every configure of
# the same source revision resolves the same verified archive.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/compat/aosp.lock.json"
     KAIROSBOOT_DEPENDENCY_LOCK_JSON)
string(JSON KAIROSBOOT_LOCKED_BOOST_VERSION GET
       "${KAIROSBOOT_DEPENDENCY_LOCK_JSON}" boost requiredVersion)
string(JSON KAIROSBOOT_LOCKED_BOOST_URL GET
       "${KAIROSBOOT_DEPENDENCY_LOCK_JSON}" boost cmakeArchive)
string(JSON KAIROSBOOT_LOCKED_BOOST_SHA256 GET
       "${KAIROSBOOT_DEPENDENCY_LOCK_JSON}" boost cmakeArchiveSha256)
set(KAIROSBOOT_BOOST_VERSION "${KAIROSBOOT_LOCKED_BOOST_VERSION}")
set(KAIROSBOOT_BOOST_URL "${KAIROSBOOT_LOCKED_BOOST_URL}")
set(KAIROSBOOT_BOOST_SHA256 "${KAIROSBOOT_LOCKED_BOOST_SHA256}")
string(LENGTH "${KAIROSBOOT_BOOST_SHA256}" KAIROSBOOT_BOOST_SHA256_LENGTH)
if(NOT KAIROSBOOT_BOOST_SHA256_LENGTH EQUAL 64
   OR NOT KAIROSBOOT_BOOST_SHA256 MATCHES "^[0-9a-fA-F]+$")
  message(FATAL_ERROR "KAIROSBOOT_BOOST_SHA256 must be a SHA-256 digest")
endif()

set(BOOST_INCLUDE_LIBRARIES asio)
if(POLICY CMP0135)
  cmake_policy(SET CMP0135 NEW)
endif()
if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.28)
  FetchContent_Declare(
    Boost
    URL "${KAIROSBOOT_BOOST_URL}"
    URL_HASH "SHA256=${KAIROSBOOT_BOOST_SHA256}"
    EXCLUDE_FROM_ALL)
  FetchContent_MakeAvailable(Boost)
else()
  FetchContent_Declare(
    Boost
    URL "${KAIROSBOOT_BOOST_URL}"
    URL_HASH "SHA256=${KAIROSBOOT_BOOST_SHA256}")
  FetchContent_GetProperties(Boost)
  if(NOT boost_POPULATED)
    FetchContent_Populate(Boost)
    add_subdirectory("${boost_SOURCE_DIR}" "${boost_BINARY_DIR}"
                     EXCLUDE_FROM_ALL)
  endif()
endif()
if(NOT TARGET Boost::asio_core)
  message(FATAL_ERROR
          "Boost ${KAIROSBOOT_BOOST_VERSION} did not provide Boost::asio_core")
endif()
if(NOT EXISTS "${boost_SOURCE_DIR}/LICENSE_1_0.txt")
  message(FATAL_ERROR "Boost release is missing LICENSE_1_0.txt")
endif()

if(MSVC)
  set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_SKIP TRUE)
  set(CMAKE_INSTALL_UCRT_LIBRARIES FALSE)
  include(InstallRequiredSystemLibraries)
  if(NOT CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS)
    message(FATAL_ERROR "MSVC app-local runtime libraries were not found")
  endif()
  foreach(_msvc_runtime IN LISTS CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS)
    if(NOT EXISTS "${_msvc_runtime}")
      message(FATAL_ERROR "MSVC app-local runtime is missing: ${_msvc_runtime}")
    endif()
  endforeach()
  install(PROGRAMS ${CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS}
          DESTINATION "${CMAKE_INSTALL_BINDIR}")
endif()

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
  src/api/device_selection.cpp
  src/api/error_mapping.cpp
  src/api/operation_state.cpp
  src/fastboot/primitive_service.cpp
  src/fleet/controller_scheduler.cpp
  src/image/file_source.cpp
  src/image/sha256.cpp
  src/image/sparse_flash_plan.cpp
  src/image/sparse_image.cpp
  src/protocol/fastboot_protocol.cpp
  src/transport/adaptive_tuner.cpp
  src/transport/buffer_budget.cpp
  src/transport/image_transfer_source.cpp
  src/transport/libusb_runtime.cpp
  src/transport/tcp_fastboot.cpp
  src/transport/transfer_ring.cpp
  src/transport/udp_fastboot.cpp
  src/transport/usb_fastboot.cpp)
set_target_properties(
  kairosboot_internal
  PROPERTIES POSITION_INDEPENDENT_CODE ON
             CXX_VISIBILITY_PRESET hidden
             VISIBILITY_INLINES_HIDDEN YES)
target_compile_features(kairosboot_internal PUBLIC cxx_std_23)
target_include_directories(
  kairosboot_internal
  PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}"
         "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(kairosboot_internal
                      PRIVATE kairosboot_libusb Boost::asio_core Threads::Threads
                              ${CMAKE_DL_LIBS})
if(WIN32)
  # Boost.Asio still uses the operating system's Winsock implementation. Keep
  # the platform link dependency explicit for every supported Windows toolchain.
  target_compile_definitions(kairosboot_internal
                             PRIVATE _WIN32_WINNT=0x0A00 WINVER=0x0A00)
  target_link_libraries(kairosboot_internal PRIVATE ws2_32)
endif()
kairosboot_enable_strict_warnings(kairosboot_internal)

add_library(kairosboot SHARED src/kairosboot.cpp)
if(WIN32)
  target_sources(kairosboot PRIVATE abi/kairosboot.def)
endif()
add_library(KairosBoot::C ALIAS kairosboot)
set_target_properties(
  kairosboot
  PROPERTIES EXPORT_NAME C
             OUTPUT_NAME kairosboot
             VERSION "${PROJECT_VERSION}"
             SOVERSION "${PROJECT_VERSION_MAJOR}"
             CXX_VISIBILITY_PRESET hidden
             VISIBILITY_INLINES_HIDDEN YES)
if(MSVC)
  set_target_properties(
    kairosboot
    PROPERTIES PDB_NAME kairosboot
               PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/symbols/$<CONFIG>")
endif()
if(APPLE)
  set_target_properties(kairosboot PROPERTIES INSTALL_RPATH "@loader_path")
elseif(UNIX)
  set_target_properties(kairosboot PROPERTIES INSTALL_RPATH "$ORIGIN")
endif()
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
target_link_libraries(kairosboot PRIVATE kairosboot_internal kairosboot_libusb)
kairosboot_enable_strict_warnings(kairosboot)
if(WIN32)
  # Keep the build-tree DLL and CLI directly runnable. The imported libusb
  # target identifies its runtime DLL, but CMake does not copy imported
  # runtimes beside consumers automatically.
  add_custom_command(
    TARGET kairosboot
    POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "$<TARGET_FILE:kairosboot_libusb>" "$<TARGET_FILE_DIR:kairosboot>"
    VERBATIM)
endif()

add_library(kairosboot_cxx INTERFACE)
add_library(KairosBoot::Cxx ALIAS kairosboot_cxx)
set_target_properties(kairosboot_cxx PROPERTIES EXPORT_NAME Cxx)
target_compile_features(kairosboot_cxx INTERFACE cxx_std_23)
target_compile_options(
  kairosboot_cxx
  INTERFACE "$<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/Zc:__cplusplus>")
target_include_directories(
  kairosboot_cxx
  INTERFACE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(kairosboot_cxx INTERFACE KairosBoot::C)

add_executable(kairosboot_cli cli/main.cpp)
set_target_properties(kairosboot_cli PROPERTIES OUTPUT_NAME kairosboot)
if(MSVC)
  set_target_properties(
    kairosboot_cli
    PROPERTIES PDB_NAME kairosboot-cli
               PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/symbols/$<CONFIG>")
endif()
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
  add_executable(kairosboot_error_mapping_tests
                 tests/api/error_mapping_tests.cpp)
  target_compile_features(kairosboot_error_mapping_tests PRIVATE cxx_std_23)
  target_include_directories(kairosboot_error_mapping_tests
                             PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  target_link_libraries(kairosboot_error_mapping_tests
                        PRIVATE kairosboot_internal kairosboot_libusb)
  kairosboot_enable_strict_warnings(kairosboot_error_mapping_tests)
  add_test(NAME error_mapping COMMAND kairosboot_error_mapping_tests)
  set_tests_properties(error_mapping PROPERTIES TIMEOUT 10)

  add_executable(kairosboot_primitive_service_tests
                 tests/fastboot/primitive_service_tests.cpp
                 tests/protocol/scripted_transport.cpp)
  target_compile_features(kairosboot_primitive_service_tests PRIVATE cxx_std_23)
  target_include_directories(kairosboot_primitive_service_tests
                             PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
                                     "${CMAKE_CURRENT_SOURCE_DIR}/src/protocol")
  target_link_libraries(kairosboot_primitive_service_tests
                        PRIVATE kairosboot_internal)
  kairosboot_enable_strict_warnings(kairosboot_primitive_service_tests)
  add_test(NAME primitive_service COMMAND kairosboot_primitive_service_tests)
  set_tests_properties(primitive_service PROPERTIES TIMEOUT 15)

  add_executable(kairosboot_variable_parser_tests
                 tests/fastboot/variable_parser_tests.cpp)
  target_compile_features(kairosboot_variable_parser_tests PRIVATE cxx_std_23)
  target_include_directories(kairosboot_variable_parser_tests
                             PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  kairosboot_enable_strict_warnings(kairosboot_variable_parser_tests)
  add_test(NAME variable_parser COMMAND kairosboot_variable_parser_tests)
  set_tests_properties(variable_parser PROPERTIES TIMEOUT 10)

  add_executable(kairosboot_device_selection_tests
                 tests/api/device_selection_tests.cpp)
  target_compile_features(kairosboot_device_selection_tests PRIVATE cxx_std_23)
  target_include_directories(kairosboot_device_selection_tests
                             PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  target_link_libraries(kairosboot_device_selection_tests
                        PRIVATE kairosboot_internal kairosboot_libusb)
  kairosboot_enable_strict_warnings(kairosboot_device_selection_tests)
  add_test(NAME device_selection COMMAND kairosboot_device_selection_tests)
  set_tests_properties(device_selection PROPERTIES TIMEOUT 10)

  add_executable(kairosboot_operation_state_tests
                 tests/api/operation_state_tests.cpp)
  target_compile_features(kairosboot_operation_state_tests PRIVATE cxx_std_23)
  target_include_directories(kairosboot_operation_state_tests
                             PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  target_link_libraries(kairosboot_operation_state_tests
                        PRIVATE kairosboot_internal)
  kairosboot_enable_strict_warnings(kairosboot_operation_state_tests)
  add_test(NAME operation_state COMMAND kairosboot_operation_state_tests)
  set_tests_properties(operation_state PROPERTIES TIMEOUT 15)

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

  add_executable(kairosboot_sparse_tests tests/sparse/sparse_image_tests.cpp)
  target_compile_features(kairosboot_sparse_tests PRIVATE cxx_std_23)
  target_include_directories(kairosboot_sparse_tests
                             PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/image")
  target_link_libraries(kairosboot_sparse_tests PRIVATE kairosboot_internal)
  kairosboot_enable_strict_warnings(kairosboot_sparse_tests)
  add_test(NAME sparse_image COMMAND kairosboot_sparse_tests)
  set_tests_properties(sparse_image PROPERTIES TIMEOUT 10)

  add_executable(kairosboot_file_source_tests
                 tests/image/file_source_tests.cpp)
  target_compile_features(kairosboot_file_source_tests PRIVATE cxx_std_23)
  target_include_directories(kairosboot_file_source_tests
                             PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  target_link_libraries(kairosboot_file_source_tests PRIVATE kairosboot_internal)
  kairosboot_enable_strict_warnings(kairosboot_file_source_tests)
  add_test(NAME file_source COMMAND kairosboot_file_source_tests)
  set_tests_properties(file_source PROPERTIES TIMEOUT 10)

  add_executable(kairosboot_sha256_tests tests/image/sha256_tests.cpp)
  target_compile_features(kairosboot_sha256_tests PRIVATE cxx_std_23)
  target_include_directories(kairosboot_sha256_tests
                             PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  target_link_libraries(kairosboot_sha256_tests PRIVATE kairosboot_internal)
  kairosboot_enable_strict_warnings(kairosboot_sha256_tests)
  add_test(NAME sha256 COMMAND kairosboot_sha256_tests)
  set_tests_properties(sha256 PROPERTIES TIMEOUT 15)

  add_executable(kairosboot_tcp_tests tests/tcp/tcp_fastboot_tests.cpp
                                      tests/tcp/scripted_socket.cpp)
  target_compile_features(kairosboot_tcp_tests PRIVATE cxx_std_23)
  target_include_directories(kairosboot_tcp_tests
                             PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests/tcp"
                                     "${CMAKE_CURRENT_SOURCE_DIR}/src/transport")
  target_link_libraries(kairosboot_tcp_tests PRIVATE kairosboot_internal)
  kairosboot_enable_strict_warnings(kairosboot_tcp_tests)
  add_test(NAME tcp_fastboot COMMAND kairosboot_tcp_tests)
  set_tests_properties(tcp_fastboot PROPERTIES TIMEOUT 10)

  add_executable(kairosboot_udp_tests tests/udp/udp_fastboot_tests.cpp
                                      tests/udp/scripted_datagram.cpp)
  target_compile_features(kairosboot_udp_tests PRIVATE cxx_std_23)
  target_include_directories(
    kairosboot_udp_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests/udp"
                                 "${CMAKE_CURRENT_SOURCE_DIR}/src/transport")
  target_link_libraries(kairosboot_udp_tests PRIVATE kairosboot_internal)
  kairosboot_enable_strict_warnings(kairosboot_udp_tests)
  add_test(NAME udp_fastboot COMMAND kairosboot_udp_tests)
  set_tests_properties(udp_fastboot PROPERTIES TIMEOUT 15)

  add_executable(kairosboot_udp_loopback tests/udp/udp_loopback_smoke.cpp)
  target_compile_features(kairosboot_udp_loopback PRIVATE cxx_std_23)
  target_include_directories(
    kairosboot_udp_loopback
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/transport")
  target_link_libraries(kairosboot_udp_loopback PRIVATE kairosboot_internal)
  kairosboot_enable_strict_warnings(kairosboot_udp_loopback)
  add_test(NAME udp_loopback COMMAND kairosboot_udp_loopback)
  set_tests_properties(udp_loopback PROPERTIES TIMEOUT 15)

  if(NOT WIN32)
    add_executable(kairosboot_tcp_sigpipe_regression
                   tests/tcp/sigpipe_regression.cpp)
    target_compile_features(kairosboot_tcp_sigpipe_regression PRIVATE cxx_std_23)
    target_include_directories(kairosboot_tcp_sigpipe_regression
                               PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/transport")
    target_link_libraries(kairosboot_tcp_sigpipe_regression
                          PRIVATE kairosboot_internal)
    kairosboot_enable_strict_warnings(kairosboot_tcp_sigpipe_regression)
    add_test(NAME tcp_sigpipe_regression
             COMMAND kairosboot_tcp_sigpipe_regression)
    set_tests_properties(tcp_sigpipe_regression PROPERTIES TIMEOUT 10)
  endif()

  add_executable(kairosboot_libusb_runtime_tests
                 tests/libusb_runtime/test_main.cpp)
  target_compile_features(kairosboot_libusb_runtime_tests PRIVATE cxx_std_23)
  target_include_directories(kairosboot_libusb_runtime_tests
                             PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  target_link_libraries(kairosboot_libusb_runtime_tests
                        PRIVATE kairosboot_internal kairosboot_libusb)
  kairosboot_enable_strict_warnings(kairosboot_libusb_runtime_tests)
  if(WIN32)
    # Building this test target in isolation must also stage libusb. Depending
    # on kairosboot reuses its single app-local copy and avoids parallel copies
    # racing into the shared Visual Studio runtime directory.
    add_dependencies(kairosboot_libusb_runtime_tests kairosboot)
  endif()
  add_test(NAME libusb_runtime COMMAND kairosboot_libusb_runtime_tests)
  set_tests_properties(libusb_runtime PROPERTIES TIMEOUT 15)

  find_package(Python3 COMPONENTS Interpreter REQUIRED)
  add_test(
    NAME hil_evidence_gate
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/tooling/test_hil_evidence.py")
  add_test(
    NAME release_tooling
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/tooling/test_release_tools.py")
  add_test(
    NAME libusb_dependency_tooling
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/tooling/test_prepare_libusb.py")
  if(WIN32)
    add_test(
      NAME abi_symbols
      COMMAND "${Python3_EXECUTABLE}"
              "${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_abi.py")
  else()
    add_test(
      NAME abi_symbols
      COMMAND "${Python3_EXECUTABLE}"
              "${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_abi.py" --library
              "$<TARGET_FILE:kairosboot>")
  endif()

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
install(
  FILES LICENSE THIRD_PARTY_NOTICES.md UNSIGNED.txt signing-status.json
  DESTINATION "${CMAKE_INSTALL_DATADIR}/kairosboot")
install(FILES "${boost_SOURCE_DIR}/LICENSE_1_0.txt"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/kairosboot/boost")

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
