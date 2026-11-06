# Frontend-only facilities, reusable by terminal/CLI and future GUI presentation.
add_library(folio_frontend STATIC
  src/backend_launcher.cpp
  src/backend_connection_handle.cpp
  src/process.cpp
  src/tui/tui.cpp
)
target_include_directories(folio_frontend
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_compile_features(folio_frontend PUBLIC cxx_std_20)
set_target_properties(folio_frontend PROPERTIES CXX_EXTENSIONS OFF)
target_link_libraries(folio_frontend PUBLIC folio_shared
  PRIVATE ftxui::component Threads::Threads)

# Shared process dispatcher: the deliberate consumer of both role entry points.
add_executable(folio src/main.cpp)
target_include_directories(folio PRIVATE
  "${PROJECT_SOURCE_DIR}/app/include" "${PROJECT_SOURCE_DIR}/server/include")
target_compile_features(folio PRIVATE cxx_std_20)
set_target_properties(folio PROPERTIES CXX_EXTENSIONS OFF)
target_link_libraries(folio PRIVATE folio_frontend folio_backend CLI11::CLI11)
if(MINGW)
  target_link_options(folio PRIVATE -municode)
endif()
# Preserve the existing executable location despite the CMake subdirectory.
set_target_properties(folio PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}")
