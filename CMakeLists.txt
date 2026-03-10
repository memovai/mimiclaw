# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

option(MIMI_ENABLE_MICROPYTHON "Enable MicroPython integration" OFF)

# MicroPython helper (local clone to avoid component-manager object lib issues)
if(MIMI_ENABLE_MICROPYTHON)
  set(MICROPY_HELPER_DIR "${CMAKE_CURRENT_LIST_DIR}/third_party/micropython-helper")
  if(NOT EXISTS "${MICROPY_HELPER_DIR}")
    find_package(Git QUIET)
    if(GIT_FOUND)
      execute_process(
        COMMAND "${GIT_EXECUTABLE}" clone --depth 1 --recurse-submodules
                https://github.com/mr9you/micropython-helper.git
                "${MICROPY_HELPER_DIR}"
        RESULT_VARIABLE _mimi_git_clone_rc
      )
    endif()
  endif()
endif()
if(NOT MICROPY_BOARD)
  set(MICROPY_BOARD "ESP32_GENERIC_S3")
endif()
if(MIMI_ENABLE_MICROPYTHON AND EXISTS "${MICROPY_HELPER_DIR}/mpy.cmake")
  # Work around IDF 5.5 + micropython-helper 1.22.1: linking micropy_extmod_btree
  # (OBJECT_LIBRARY) breaks ldgen when TARGET_FILE is expanded.
  set(_mimi_extmod_cmake "${MICROPY_HELPER_DIR}/micropython/extmod/extmod.cmake")
  if(EXISTS "${_mimi_extmod_cmake}")
    file(READ "${_mimi_extmod_cmake}" _mimi_extmod_src)
    string(REPLACE
      "add_library(micropy_extmod_btree OBJECT"
      "add_library(micropy_extmod_btree STATIC"
      _mimi_extmod_src
      "${_mimi_extmod_src}"
    )
    file(WRITE "${_mimi_extmod_cmake}" "${_mimi_extmod_src}")
  endif()
  set(_mimi_mpy_common "${MICROPY_HELPER_DIR}/micropython/ports/esp32/esp32_common.cmake")
  if(EXISTS "${_mimi_mpy_common}")
    file(READ "${_mimi_mpy_common}" _mimi_mpy_common_src)
    string(REGEX REPLACE
      "[^\n]*target_link_libraries\\([^\n]*micropy_extmod_btree[^\n]*\\)\n"
      ""
      _mimi_mpy_common_src
      "${_mimi_mpy_common_src}"
    )
    file(WRITE "${_mimi_mpy_common}" "${_mimi_mpy_common_src}")
  endif()
  include(${MICROPY_HELPER_DIR}/mpy.cmake)
  # If btree is an OBJECT library, wrap it as STATIC and replace references.
  if(TARGET micropy_extmod_btree)
    get_target_property(_mimi_btree_type micropy_extmod_btree TYPE)
    if(_mimi_btree_type STREQUAL "OBJECT_LIBRARY")
      add_library(micropy_extmod_btree_static STATIC $<TARGET_OBJECTS:micropy_extmod_btree>)
      get_target_property(_mimi_btree_iface micropy_extmod_btree INTERFACE_LINK_LIBRARIES)
      if(_mimi_btree_iface)
        target_link_libraries(micropy_extmod_btree_static INTERFACE ${_mimi_btree_iface})
      endif()
    endif()
  endif()
  # Remove problematic link entry even if helper CMake layout changes.
  get_property(_mimi_all_targets GLOBAL PROPERTY TARGETS)
  foreach(_mimi_tgt IN LISTS _mimi_all_targets)
    foreach(_mimi_prop IN ITEMS LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
      get_target_property(_mimi_links ${_mimi_tgt} ${_mimi_prop})
      if(_mimi_links)
        set(_mimi_links_new "${_mimi_links}")
        list(REMOVE_ITEM _mimi_links_new micropy_extmod_btree "$<LINK_ONLY:micropy_extmod_btree>")
        if(TARGET micropy_extmod_btree_static)
          list(REMOVE_ITEM _mimi_links_new micropy_extmod_btree_static "$<LINK_ONLY:micropy_extmod_btree_static>")
          list(APPEND _mimi_links_new micropy_extmod_btree_static)
        endif()
        if(NOT _mimi_links_new STREQUAL _mimi_links)
          set_property(TARGET ${_mimi_tgt} PROPERTY ${_mimi_prop} "${_mimi_links_new}")
        endif()
      endif()
    endforeach()
  endforeach()
endif()
if(EXISTS "${CMAKE_BINARY_DIR}/sdkconfig.combined")
  file(READ "${CMAKE_BINARY_DIR}/sdkconfig.combined" _sc)
  string(REPLACE "CONFIG_MBEDTLS_SSL_ALPN=n" "CONFIG_MBEDTLS_SSL_ALPN=y" _sc "${_sc}")
  string(REPLACE "CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=n" "CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y" _sc "${_sc}")
  file(WRITE "${CMAKE_BINARY_DIR}/sdkconfig.combined" "${_sc}")
  file(APPEND "${CMAKE_BINARY_DIR}/sdkconfig.combined"
    "\nCONFIG_MBEDTLS_SSL_ALPN=y\nCONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y\n")
endif()
set(_mimi_sdkconfig_defaults
  "sdkconfig.defaults"
  "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults.esp32s3"
)
if(MIMI_ENABLE_MICROPYTHON)
  list(APPEND _mimi_sdkconfig_defaults
    "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.micropython.override"
  )
endif()
if(EXISTS "${CMAKE_BINARY_DIR}/sdkconfig.combined")
  list(INSERT _mimi_sdkconfig_defaults 1 "${CMAKE_BINARY_DIR}/sdkconfig.combined")
endif()
set(SDKCONFIG_DEFAULTS "${_mimi_sdkconfig_defaults}" CACHE STRING "Kconfig defaults" FORCE)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(mimiclaw)

# Pre-flash a valid SPIFFS image so first boot does not need runtime formatting.
spiffs_create_partition_image(spiffs spiffs_data FLASH_IN_PROJECT)
