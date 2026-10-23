add_executable(folio_core_tests core_smoke_test.cpp)
target_compile_features(folio_core_tests PRIVATE cxx_std_20)
set_target_properties(folio_core_tests PROPERTIES CXX_EXTENSIONS OFF)
target_link_libraries(folio_core_tests PRIVATE Folio::Core GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(folio_core_tests)
