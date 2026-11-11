# Backend-only implementation. Only its narrow execution entry point is exported.
add_library(folio_backend STATIC
  src/backend.cpp
  src/operations/catalog_maintenance.cpp
  src/operations/access_orchestrator.cpp
  src/operations/mutation_orchestrator.cpp
  src/execution/execution_pool.cpp
  src/execution/request_coordinator.cpp
  src/runtime/backend_state_machine.cpp
  src/runtime/backend_states.cpp
  src/runtime/repository_ownership.cpp
  src/ipc/ipc_server.cpp
  src/storage/catalog_coordinator.cpp
  src/storage/codec.cpp
  src/storage/common.cpp
  src/storage/index_codec.cpp
  src/storage/metadata_catalog.cpp
  src/storage/normal_repository.cpp
)
target_include_directories(folio_backend
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_compile_features(folio_backend PUBLIC cxx_std_20)
set_target_properties(folio_backend PROPERTIES CXX_EXTENSIONS OFF)
target_link_libraries(folio_backend PUBLIC folio_shared
  PRIVATE immer tomlplusplus::tomlplusplus msgpack-cxx Threads::Threads)
