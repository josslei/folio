# Build the pinned libsodium submodule without modifying its source tree.
set(sodium_source_dir "${PROJECT_SOURCE_DIR}/thirdparty/libsodium/src/libsodium")

file(GLOB_RECURSE sodium_sources CONFIGURE_DEPENDS "${sodium_source_dir}/*.c")
# Use portable implementations by default. Most optimized variants need
# per-file compiler flags.
list(FILTER sodium_sources EXCLUDE REGEX
  "(/|_|-)(aesni|armcrypto|avx2|avx512f|neon|sse|sse2|sse41|ssse3|wasm32|xmm6|xmm6int|sandy2x|armsha3)(/|\\.|_|-)")

# Argon2 selects NEON directly from compiler macros on ARM64, independently of
# configure feature checks. Keep its implementation in sync with that selector.
# This source has the same architecture guard, so it is inert on other targets
# and needs no extra compiler flags on ARM64.
list(APPEND sodium_sources
  "${sodium_source_dir}/crypto_pwhash/argon2/argon2-fill-block-neon.c"
)

add_library(folio_sodium STATIC ${sodium_sources})
set_target_properties(folio_sodium PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON)

if(NOT WIN32)
  find_package(Threads REQUIRED)
  target_link_libraries(folio_sodium PRIVATE Threads::Threads)
  if(CMAKE_USE_PTHREADS_INIT)
    target_compile_definitions(folio_sodium PRIVATE HAVE_PTHREAD=1)
  else()
    message(FATAL_ERROR "This libsodium build requires pthreads on Unix-like systems")
  endif()
endif()

# These values are from the pinned libsodium 1.0.22 configure.ac.
set(VERSION 1.0.22)
set(SODIUM_LIBRARY_VERSION_MAJOR 26)
set(SODIUM_LIBRARY_VERSION_MINOR 4)
configure_file(
  "${sodium_source_dir}/include/sodium/version.h.in"
  "${CMAKE_CURRENT_BINARY_DIR}/include/sodium/version.h"
  @ONLY
)
configure_file(
  "${sodium_source_dir}/include/sodium/export.h"
  "${CMAKE_CURRENT_BINARY_DIR}/include/sodium/export.h"
  COPYONLY
)

target_include_directories(folio_sodium
  PUBLIC
    "${sodium_source_dir}/include"
    "${CMAKE_CURRENT_BINARY_DIR}/include"
  PRIVATE
    "${sodium_source_dir}/include/sodium"
    "${CMAKE_CURRENT_BINARY_DIR}/include/sodium"
)
target_compile_definitions(folio_sodium
  PUBLIC SODIUM_STATIC
  PRIVATE CONFIGURED=1 $<$<C_COMPILER_ID:MSVC>:_CRT_SECURE_NO_WARNINGS>
)
