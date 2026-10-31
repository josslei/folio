add_library(folio_backend STATIC
  src/application/internal/configuration.cpp
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
  src/application/internal/backend_state_machine.cpp
  src/application/internal/backend_states.cpp
  src/application/internal/process.cpp
  src/application/internal/repository_ownership.cpp
  src/storage/catalog_coordinator.cpp
  src/storage/codec.cpp
  src/storage/filesystem.cpp
  src/storage/index_codec.cpp
  src/storage/metadata_catalog.cpp
  src/storage/normal_repository.cpp
)
target_include_directories(folio_backend
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_features(folio_backend PUBLIC cxx_std_20)
set_target_properties(folio_backend PROPERTIES CXX_EXTENSIONS OFF)
target_link_libraries(folio_backend PRIVATE folio_sodium tomlplusplus::tomlplusplus msgpack-cxx stduuid)
if(UNIX)
  find_package(Threads REQUIRED)
  target_link_libraries(folio_backend PRIVATE Threads::Threads)
endif()

if(WIN32)
  target_link_libraries(folio_backend PRIVATE shell32 ole32 advapi32)
endif()
