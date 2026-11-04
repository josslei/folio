add_executable(folio_core_tests
  core/core_smoke_test.cpp
  core/core_types_test.cpp
  core/log_test.cpp
  core/memo_test.cpp
  core/todo_test.cpp
)
target_compile_features(folio_core_tests PRIVATE cxx_std_20)
set_target_properties(folio_core_tests PROPERTIES CXX_EXTENSIONS OFF)
target_link_libraries(folio_core_tests PRIVATE folio_backend GTest::gtest_main)

add_executable(folio_storage_tests
  storage/codec_test.cpp
  storage/storage_test.cpp
  storage/normal_repository_test.cpp
  storage/normal_repository_callbacks_test.cpp
  storage/metadata_catalog_test.cpp
  storage/catalog_rebuild_test.cpp
  storage/index_codec_test.cpp
  storage/binary_io_test.cpp
  storage/catalog_coordinator_test.cpp
  storage/serialization_samples_test.cpp
  storage/tomlplusplus_test.cpp
)
target_include_directories(folio_storage_tests PRIVATE "${PROJECT_SOURCE_DIR}/lib/src")
target_compile_features(folio_storage_tests PRIVATE cxx_std_20)
target_compile_definitions(folio_storage_tests PRIVATE
  "FOLIO_SERIALIZATION_SAMPLES_DIR=\"${PROJECT_BINARY_DIR}/serialization-samples/$<CONFIG>\""
)
set_target_properties(folio_storage_tests PROPERTIES CXX_EXTENSIONS OFF)
target_link_libraries(folio_storage_tests PRIVATE
  folio_backend
  GTest::gtest_main
  tomlplusplus::tomlplusplus
  msgpack-cxx
)

include(GoogleTest)
# Allow for first-launch delays after linking (for example, macOS executable
# checks). This limits test-name discovery, not individual test execution.
gtest_discover_tests(folio_core_tests DISCOVERY_TIMEOUT 30)
gtest_discover_tests(folio_storage_tests DISCOVERY_TIMEOUT 30)

option(FOLIO_BUILD_RESOURCE_LIMIT_TESTS "Build opt-in resource limit tests" OFF)
if(FOLIO_BUILD_RESOURCE_LIMIT_TESTS)
  add_executable(folio_resource_limit_tests
    resource_limits/index_resource_limits_test.cpp
  )
  target_include_directories(folio_resource_limit_tests PRIVATE "${PROJECT_SOURCE_DIR}/lib/src")
  target_compile_features(folio_resource_limit_tests PRIVATE cxx_std_20)
  set_target_properties(folio_resource_limit_tests PROPERTIES CXX_EXTENSIONS OFF)
  target_link_libraries(folio_resource_limit_tests PRIVATE
    folio_backend
    GTest::gtest_main
    msgpack-cxx
  )
  gtest_discover_tests(folio_resource_limit_tests
    DISCOVERY_TIMEOUT 30
    PROPERTIES LABELS resource_limits
  )
endif()

# Private process fixtures: no production test switches or application IPC.
add_executable(folio_process_child application/process_child.cpp)
target_include_directories(folio_process_child PRIVATE "${PROJECT_SOURCE_DIR}/lib/src")
target_compile_features(folio_process_child PRIVATE cxx_std_20)
set_target_properties(folio_process_child PROPERTIES CXX_EXTENSIONS OFF)
target_link_libraries(folio_process_child PRIVATE folio_backend)
if(MINGW)
  target_link_options(folio_process_child PRIVATE -municode)
endif()

add_executable(folio_backend_child application/backend_child.cpp)
target_include_directories(folio_backend_child PRIVATE "${PROJECT_SOURCE_DIR}/lib/src")
target_compile_features(folio_backend_child PRIVATE cxx_std_20)
target_link_libraries(folio_backend_child PRIVATE folio_backend)
if(MINGW)
  target_link_options(folio_backend_child PRIVATE -municode)
endif()
add_executable(folio_application_tests application/mutation_contract_test.cpp application/access_codec_test.cpp application/metadata_access_test.cpp application/metadata_access_integration_test.cpp application/logging_test.cpp application/configuration_test.cpp application/backend_states_test.cpp application/backend_lifecycle_test.cpp application/process_test.cpp application/backend_test.cpp)
target_include_directories(folio_application_tests PRIVATE "${PROJECT_SOURCE_DIR}/lib/src")
target_compile_features(folio_application_tests PRIVATE cxx_std_20)
set_target_properties(folio_application_tests PROPERTIES CXX_EXTENSIONS OFF)
target_link_libraries(folio_application_tests PRIVATE folio_backend GTest::gtest_main)
target_compile_definitions(folio_application_tests PRIVATE
  "FOLIO_PROCESS_CHILD=\"$<TARGET_FILE:folio_process_child>\"")
target_compile_definitions(folio_application_tests PRIVATE
  "FOLIO_BACKEND_CHILD=\"$<TARGET_FILE:folio_backend_child>\"")
add_dependencies(folio_application_tests folio_process_child folio_backend_child folio)
gtest_discover_tests(folio_application_tests DISCOVERY_TIMEOUT 30 PROPERTIES TIMEOUT 30)

# Compile the same entry point with an injected log root. This keeps CLI tests
# away from real user logs on Windows as well as POSIX, without a production
# environment override or a test switch in folio.
add_executable(folio_cli_test ../app/src/main.cpp ../app/src/tui/tui.cpp)
target_include_directories(folio_cli_test PRIVATE "${PROJECT_SOURCE_DIR}/app/src")
target_link_libraries(folio_cli_test PRIVATE folio_backend ftxui::component CLI11::CLI11)
target_compile_definitions(folio_cli_test PRIVATE
  "FOLIO_TEST_LOG_ROOT=\"${CMAKE_CURRENT_BINARY_DIR}/cli-logs\"")
if(MINGW)
  target_link_options(folio_cli_test PRIVATE -municode)
endif()

# Exercise production CLI11 dispatch without opening an interactive TUI.
add_test(NAME Cli.Help COMMAND folio_cli_test --help)
set_tests_properties(Cli.Help PROPERTIES PASS_REGULAR_EXPRESSION "Print this help message and exit")
add_test(NAME Cli.RejectsUnknownCommand COMMAND folio_cli_test unknown-command)
set_tests_properties(Cli.RejectsUnknownCommand PROPERTIES WILL_FAIL TRUE)
# Invalid synthetic configuration makes private-mode parser tests exit before Storage.
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/invalid-config.toml" "normal_root = 42\n")
add_test(NAME Cli.BackendMode COMMAND folio_cli_test --backend-process)
add_test(NAME Cli.BackendIgnoresOtherArguments COMMAND folio_cli_test --help unknown --backend-process --invalid)
set_tests_properties(Cli.BackendMode Cli.BackendIgnoresOtherArguments PROPERTIES WILL_FAIL TRUE)

set_tests_properties(Cli.BackendMode Cli.BackendIgnoresOtherArguments PROPERTIES
  ENVIRONMENT "FOLIO_CONFIG_FILE=${CMAKE_CURRENT_BINARY_DIR}/invalid-config.toml")
