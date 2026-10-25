cmake_minimum_required(VERSION 3.20)

project(Folio VERSION 0.1.0 LANGUAGES C CXX)

include(CTest)

add_library(folio_core STATIC
  src/core/codec.cpp
  src/core/directory.cpp
  src/core/id.cpp
  src/core/log.cpp
  src/core/memo.cpp
  src/core/todo.cpp
  src/core/types.cpp
  src/core/version.cpp
)
add_library(Folio::Core ALIAS folio_core)
target_include_directories(folio_core
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(folio_core PUBLIC cxx_std_20)
set_target_properties(folio_core PROPERTIES CXX_EXTENSIONS OFF)

add_subdirectory(thirdparty/libsodium-cmake)
target_link_libraries(folio_core PRIVATE folio_sodium)

add_subdirectory(thirdparty/tomlplusplus EXCLUDE_FROM_ALL)
target_link_libraries(folio_core PUBLIC tomlplusplus::tomlplusplus)

set(UUID_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(UUID_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(UUID_USING_CXX20_SPAN ON CACHE BOOL "" FORCE)
add_subdirectory(thirdparty/uuidv4 EXCLUDE_FROM_ALL)
target_link_libraries(folio_core PRIVATE stduuid)

add_library(folio_storage STATIC
  src/storage/filesystem.cpp
)
add_library(Folio::Storage ALIAS folio_storage)
target_include_directories(folio_storage
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(folio_storage PUBLIC cxx_std_20)
set_target_properties(folio_storage PROPERTIES CXX_EXTENSIONS OFF)
target_link_libraries(folio_storage PUBLIC Folio::Core)

set(FTXUI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(FTXUI_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(FTXUI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(FTXUI_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory(thirdparty/ftxui EXCLUDE_FROM_ALL)

add_executable(folio src/tui/main.cpp)
target_compile_features(folio PRIVATE cxx_std_20)
set_target_properties(folio PROPERTIES CXX_EXTENSIONS OFF)
target_link_libraries(folio PRIVATE Folio::Core ftxui::component)

if(BUILD_TESTING)
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  add_subdirectory(thirdparty/googletest EXCLUDE_FROM_ALL)
  add_subdirectory(tests)
endif()
