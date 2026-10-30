#pragma once
#include <filesystem>

namespace folio::application::internal {
// Narrow same-executable Backend dispatch interface. No Storage types escape.
inline const std::filesystem::path backend_process_argument{"--backend-process"};
int run_backend();
}
