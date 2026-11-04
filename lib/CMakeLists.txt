set(folio_config_template "${PROJECT_SOURCE_DIR}/templates/folio_config.toml")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${folio_config_template}")
file(READ "${folio_config_template}" folio_config_contents)
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/generated/config_template.hpp"
  "#pragma once\n#include <string_view>\nnamespace folio::config { inline constexpr std::string_view factory_template = R\"folio(${folio_config_contents})folio\"; }\n")

add_library(folio_backend STATIC
  src/logging/logging.cpp
  src/config/configuration.cpp
  src/io/filesystem.cpp
  src/io/path.cpp
  src/application/ipc.cpp
  src/core/directory.cpp
  src/core/id.cpp
  src/core/log.cpp
  src/core/memo.cpp
  src/core/todo.cpp
  src/core/types.cpp
  src/core/version.cpp
  src/application/backend.cpp
  src/application/backend_connection_handle.cpp
  src/application/mutation/mutation_id.cpp
  src/application/internal/backend_state_machine.cpp
  src/application/internal/backend_states.cpp
  src/application/internal/metadata_access.cpp
  src/application/internal/message_transport.cpp
  src/application/internal/application_codec.cpp
  src/application/internal/process.cpp
  src/application/internal/repository_ownership.cpp
  src/storage/catalog_coordinator.cpp
  src/storage/codec.cpp
  src/storage/common.cpp
  src/storage/index_codec.cpp
  src/storage/metadata_catalog.cpp
  src/storage/normal_repository.cpp
)
target_include_directories(folio_backend
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_BINARY_DIR}/generated
)
target_compile_features(folio_backend PUBLIC cxx_std_20)
# Keep all six levels available; runtime configuration selects filtering.
target_compile_definitions(folio_backend PUBLIC SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_TRACE)
target_link_libraries(folio_backend PUBLIC spdlog::spdlog)
set_target_properties(folio_backend PROPERTIES CXX_EXTENSIONS OFF)
target_link_libraries(folio_backend PRIVATE folio_sodium tomlplusplus::tomlplusplus msgpack-cxx stduuid)
if(UNIX)
  find_package(Threads REQUIRED)
  target_link_libraries(folio_backend PRIVATE Threads::Threads)
endif()

if(WIN32)
  target_link_libraries(folio_backend PRIVATE shell32 ole32 advapi32)
endif()
