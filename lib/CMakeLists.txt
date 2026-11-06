set(folio_config_template "${PROJECT_SOURCE_DIR}/templates/folio_config.toml")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${folio_config_template}")
file(READ "${folio_config_template}" folio_config_contents)
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/generated/config_template.hpp"
  "#pragma once\n#include <string_view>\nnamespace folio::config { inline constexpr std::string_view factory_template = R\"folio(${folio_config_contents})folio\"; }\n")

# Code and value contracts shared by both runtime roles. No app/server includes.
add_library(folio_shared STATIC
  src/core/directory.cpp
  src/core/id.cpp
  src/core/log.cpp
  src/core/memo.cpp
  src/core/todo.cpp
  src/core/types.cpp
  src/core/version.cpp
  src/config/configuration.cpp
  src/logging/logging.cpp
  src/io/filesystem.cpp
  src/io/path.cpp
  src/ipc/ipc.cpp
  src/application/mutation/mutation_id.cpp
  src/ipc/message_transport.cpp
  src/ipc/application_codec.cpp
)
target_include_directories(folio_shared
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_BINARY_DIR}/generated
)
target_compile_features(folio_shared PUBLIC cxx_std_20)
set_target_properties(folio_shared PROPERTIES CXX_EXTENSIONS OFF)
# Keep all six levels available; runtime configuration selects filtering.
target_compile_definitions(folio_shared PUBLIC SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_TRACE)
# Shared codec headers are implementation-facing module interfaces; consumers
# compiling their templates need MessagePack's include usage requirements.
target_link_libraries(folio_shared PUBLIC spdlog::spdlog msgpack-cxx
  PRIVATE folio_sodium tomlplusplus::tomlplusplus stduuid)
target_link_libraries(folio_shared PRIVATE Threads::Threads)
if(WIN32)
  target_link_libraries(folio_shared PRIVATE shell32 ole32 advapi32)
endif()
