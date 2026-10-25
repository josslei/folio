add_executable(folio_core_tests
  core_smoke_test.cpp
  core_types_test.cpp
  tomlplusplus_test.cpp
  codec_test.cpp
  serialization_samples_test.cpp
  log_test.cpp
  memo_test.cpp
  todo_test.cpp
)
target_compile_features(folio_core_tests PRIVATE cxx_std_20)
target_compile_definitions(folio_core_tests PRIVATE
  "FOLIO_SERIALIZATION_SAMPLES_DIR=\"${PROJECT_BINARY_DIR}/serialization-samples/$<CONFIG>\""
)
set_target_properties(folio_core_tests PROPERTIES CXX_EXTENSIONS OFF)
target_link_libraries(folio_core_tests PRIVATE
  Folio::Core
  GTest::gtest_main
  tomlplusplus::tomlplusplus
)

include(GoogleTest)
gtest_discover_tests(folio_core_tests)

add_executable(folio_storage_tests storage_test.cpp)
target_compile_features(folio_storage_tests PRIVATE cxx_std_20)
set_target_properties(folio_storage_tests PROPERTIES CXX_EXTENSIONS OFF)
target_link_libraries(folio_storage_tests PRIVATE Folio::Storage GTest::gtest_main)
gtest_discover_tests(folio_storage_tests)
