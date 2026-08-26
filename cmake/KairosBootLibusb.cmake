include(CheckCXXSourceCompiles)

set(KAIROSBOOT_LIBUSB_ROOT "" CACHE PATH
    "Prefix containing the locked dynamic libusb 1.0.30 dependency")
set(KAIROSBOOT_LIBUSB_LICENSE_FILE "" CACHE FILEPATH
    "Optional path to the libusb LGPL license text")
set(KAIROSBOOT_LIBUSB_MANIFEST_FILE "" CACHE FILEPATH
    "Optional path to the prepared libusb dependency manifest")
option(KAIROSBOOT_INSTALL_LIBUSB_RUNTIME
       "Install the dynamically linked libusb runtime with KairosBoot" ON)

function(kairosboot_configure_libusb)
  if(TARGET kairosboot_libusb)
    return()
  endif()

  if(NOT KAIROSBOOT_LIBUSB_ROOT)
    message(
      FATAL_ERROR
        "KAIROSBOOT_LIBUSB_ROOT must point to a prefix produced by "
        "scripts/prepare_libusb.py; KairosBoot does not fall back to an "
        "unlocked system libusb")
  endif()

  set(_runtime_install_name "")
  set(_license_file "${KAIROSBOOT_LIBUSB_LICENSE_FILE}")
  set(_manifest_file "${KAIROSBOOT_LIBUSB_MANIFEST_FILE}")

  cmake_path(ABSOLUTE_PATH KAIROSBOOT_LIBUSB_ROOT NORMALIZE
             OUTPUT_VARIABLE _root)
  find_path(
    _include_dir
    NAMES libusb.h
    PATHS "${_root}/include/libusb-1.0" "${_root}/include"
    NO_DEFAULT_PATH
    NO_CACHE
    REQUIRED)
  if(WIN32)
    find_library(
      _link_library
      NAMES libusb-1.0
      PATHS "${_root}/lib"
      NO_DEFAULT_PATH
      NO_CACHE
      REQUIRED)
    find_file(
      _runtime_library
      NAMES libusb-1.0.dll
      PATHS "${_root}/bin"
      NO_DEFAULT_PATH
      NO_CACHE
      REQUIRED)
    set(_runtime_install_name "libusb-1.0.dll")
  elseif(APPLE)
    find_library(
      _link_library
      NAMES usb-1.0
      PATHS "${_root}/lib"
      NO_DEFAULT_PATH
      NO_CACHE
      REQUIRED)
    find_file(
      _runtime_library
      NAMES libusb-1.0.0.dylib
      PATHS "${_root}/lib"
      NO_DEFAULT_PATH
      NO_CACHE
      REQUIRED)
    set(_runtime_install_name "libusb-1.0.0.dylib")
  else()
    find_library(
      _link_library
      NAMES usb-1.0 libusb-1.0
      PATHS "${_root}/lib" "${_root}/lib64"
      NO_DEFAULT_PATH
      NO_CACHE
      REQUIRED)
    find_file(
      _runtime_library
      NAMES libusb-1.0.so.0
      PATHS "${_root}/lib" "${_root}/lib64"
      NO_DEFAULT_PATH
      NO_CACHE
      REQUIRED)
    set(_runtime_install_name "libusb-1.0.so.0")
  endif()
  if(NOT _license_file AND EXISTS "${_root}/share/libusb/COPYING")
    set(_license_file "${_root}/share/libusb/COPYING")
  endif()
  if(NOT _manifest_file AND EXISTS "${_root}/share/libusb/kairosboot-libusb.json")
    set(_manifest_file "${_root}/share/libusb/kairosboot-libusb.json")
  endif()

  if(NOT WIN32 AND _link_library MATCHES "\\.(a|lib)$")
    message(FATAL_ERROR
            "KairosBoot requires dynamic libusb 1.0.30; found static archive: ${_link_library}")
  endif()

  set(_saved_required_includes "${CMAKE_REQUIRED_INCLUDES}")
  set(CMAKE_REQUIRED_INCLUDES "${_include_dir}")
  unset(KAIROSBOOT_LIBUSB_HEADER_IS_1_0_30 CACHE)
  check_cxx_source_compiles(
    "#include <libusb.h>
     #if !defined(LIBUSB_API_VERSION) || LIBUSB_API_VERSION != 0x0100010C
     #error KairosBoot requires the libusb 1.0.30 header
     #endif
     int main() { return 0; }"
    KAIROSBOOT_LIBUSB_HEADER_IS_1_0_30)
  set(CMAKE_REQUIRED_INCLUDES "${_saved_required_includes}")
  if(NOT KAIROSBOOT_LIBUSB_HEADER_IS_1_0_30)
    message(FATAL_ERROR "KairosBoot requires the exact libusb 1.0.30 header")
  endif()

  add_library(kairosboot_libusb SHARED IMPORTED GLOBAL)
  if(WIN32)
    set_target_properties(
      kairosboot_libusb
      PROPERTIES IMPORTED_IMPLIB "${_link_library}"
                 IMPORTED_LOCATION "${_runtime_library}"
                 INTERFACE_INCLUDE_DIRECTORIES "${_include_dir}")
  else()
    set_target_properties(
      kairosboot_libusb
      PROPERTIES IMPORTED_LOCATION "${_link_library}"
                 INTERFACE_INCLUDE_DIRECTORIES "${_include_dir}")
  endif()

  if(KAIROSBOOT_INSTALL_LIBUSB_RUNTIME)
    if(NOT _license_file OR NOT _manifest_file)
      message(
        FATAL_ERROR
          "The prepared libusb prefix must contain COPYING and "
          "kairosboot-libusb.json when runtime redistribution is enabled")
    endif()
    file(REAL_PATH "${_runtime_library}" _runtime_real)
    if(WIN32)
      install(FILES "${_runtime_real}" DESTINATION "${CMAKE_INSTALL_BINDIR}"
              RENAME "${_runtime_install_name}")
      if(EXISTS "${_root}/symbols/libusb-1.0.pdb")
        install(FILES "${_root}/symbols/libusb-1.0.pdb"
                DESTINATION "${CMAKE_INSTALL_DATADIR}/kairosboot/libusb/symbols")
      endif()
    else()
      install(FILES "${_runtime_real}" DESTINATION "${CMAKE_INSTALL_LIBDIR}"
              RENAME "${_runtime_install_name}")
    endif()
    install(FILES "${_license_file}" "${_manifest_file}"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/kairosboot/libusb")
  endif()

  set(KAIROSBOOT_LIBUSB_RUNTIME_FILE "${_runtime_library}" PARENT_SCOPE)
endfunction()
