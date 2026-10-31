cmake_minimum_required(VERSION 3.20)

project(Folio VERSION 0.1.0 LANGUAGES C CXX)

include(CTest)

add_subdirectory(thirdparty/libsodium-cmake)

add_subdirectory(thirdparty/tomlplusplus EXCLUDE_FROM_ALL)

set(MSGPACK_USE_BOOST OFF CACHE BOOL "" FORCE)
set(MSGPACK_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(MSGPACK_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(MSGPACK_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory(thirdparty/msgpack-cxx EXCLUDE_FROM_ALL)

set(UUID_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(UUID_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(UUID_USING_CXX20_SPAN ON CACHE BOOL "" FORCE)
# The pinned stduuid declares CMake 3.7 compatibility. Apply modern policies
# locally without editing the submodule or suppressing deprecation warnings.
function(folio_add_stduuid)
  if(CMAKE_VERSION VERSION_GREATER_EQUAL 4.0 AND
     (NOT DEFINED CMAKE_POLICY_VERSION_MINIMUM OR
      CMAKE_POLICY_VERSION_MINIMUM VERSION_LESS 3.10))
    set(CMAKE_POLICY_VERSION_MINIMUM 3.10)
  endif()
  add_subdirectory(thirdparty/uuidv4 EXCLUDE_FROM_ALL)
endfunction()
folio_add_stduuid()

set(FTXUI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(FTXUI_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(FTXUI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(FTXUI_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory(thirdparty/ftxui EXCLUDE_FROM_ALL)

set(CLI11_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(CLI11_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CLI11_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(CLI11_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory(thirdparty/cli11 EXCLUDE_FROM_ALL)

set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_BENCH OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(SPDLOG_INSTALL OFF CACHE BOOL "" FORCE)
if(WIN32)
  set(SPDLOG_WCHAR_FILENAMES ON CACHE BOOL "" FORCE)
endif()
add_subdirectory(thirdparty/spdlog EXCLUDE_FROM_ALL)

# Phase 4.1a uses native desktop process and ownership APIs only.
if(NOT CMAKE_SYSTEM_NAME MATCHES "^(Linux|Darwin|Windows)$")
  message(FATAL_ERROR "Folio Application process support requires Linux, macOS, or Windows")
endif()

add_subdirectory(lib)
add_subdirectory(app)

if(BUILD_TESTING)
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  add_subdirectory(thirdparty/googletest EXCLUDE_FROM_ALL)
  add_subdirectory(tests)
endif()
