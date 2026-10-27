add_executable(folio_core_tests
  core/core_smoke_test.cpp
  core/core_types_test.cpp
  core/log_test.cpp
  core/memo_test.cpp
  core/todo_test.cpp
)
target_compile_features(folio_core_tests PRIVATE cxx_std_20)
set_target_properties(folio_core_tests PROPERTIES CXX_EXTENSIONS OFF)
target_link_libraries(folio_core_tests PRIVATE Folio::Core GTest::gtest_main)

add_executable(folio_storage_tests
  storage/codec_test.cpp
  storage/storage_test.cpp
  storage/normal_repository_test.cpp
  storage/metadata_catalog_test.cpp
  storage/index_codec_test.cpp
  storage/serialization_samples_test.cpp
  storage/tomlplusplus_test.cpp
)
target_include_directories(folio_storage_tests PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_compile_features(folio_storage_tests PRIVATE cxx_std_20)
target_compile_definitions(folio_storage_tests PRIVATE
  "FOLIO_SERIALIZATION_SAMPLES_DIR=\"${PROJECT_BINARY_DIR}/serialization-samples/$<CONFIG>\""
)
set_target_properties(folio_storage_tests PROPERTIES CXX_EXTENSIONS OFF)
target_link_libraries(folio_storage_tests PRIVATE
  Folio::Core
  GTest::gtest_main
  tomlplusplus::tomlplusplus
  msgpack-cxx
)

include(GoogleTest)
# Allow for first-launch delays after linking (for example, macOS executable
# checks). This limits test-name discovery, not individual test execution.
gtest_discover_tests(folio_core_tests DISCOVERY_TIMEOUT 30)
gtest_discover_tests(folio_storage_tests DISCOVERY_TIMEOUT 30)
