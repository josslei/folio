#include "process.hpp"
// Test-only independent process. File barriers belong only to these tests;
// they are not a Folio IPC protocol or production Backend lifecycle.
#include "runtime/repository_ownership.hpp"
#include "backend.hpp"

#include <chrono>
#include <fstream>
#include <thread>
#ifndef _WIN32  // POSIX (Linux and macOS)
#include <unistd.h>
#else  // Windows
#define NOMINMAX
#include <windows.h>
#endif

namespace fs = std::filesystem;
namespace app = folio::application::internal;
namespace storage = folio::core::storage;

void report(const fs::path& target, const std::string& value) {
  auto temporary = target;
  temporary += ".writing";
  { std::ofstream output(temporary); output << value; }
  fs::rename(temporary, target);
}

#ifndef _WIN32  // POSIX (Linux and macOS)
int main(int argc, char** argv) {
#else  // Windows
int wmain(int argc, wchar_t** argv) {
#endif
  std::vector<fs::path> arguments;
  for (int i = 1; i < argc; ++i) arguments.emplace_back(argv[i]);
  if (arguments.size() == 4 && arguments[0] == "--test-launch-child") {
    auto executable = app::current_executable();
    if (std::holds_alternative<folio::application::ProcessError>(executable)) return 1;
    auto result = app::launch_process(std::get<fs::path>(executable),
                                    {arguments[1], arguments[2], arguments[3]});
    if (std::holds_alternative<folio::application::ProcessError>(result)) return 1;
    report(arguments[2] / "launcher-result", std::to_string(std::get<folio::application::LaunchedProcess>(result).id));
    return 0;
  }
  if (arguments.size() != 3) return 2;
  const auto& base = arguments[0];
  const auto& control = arguments[1];
  const auto& normal = arguments[2];
  storage::StoragePaths paths{normal, base / "vault",
      {base / "config", base / "data", base / "state", base / "cache", base / "logs", std::nullopt}};
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{15};
  report(control / "started", "started");
  while (!fs::exists(control / "go")) {
    if (std::chrono::steady_clock::now() > deadline) return 10;
    std::this_thread::sleep_for(std::chrono::milliseconds{2});
  }
  {
    auto owner = app::RepositoryOwnership::acquire(paths);
    if (auto* error = std::get_if<folio::application::ProcessError>(&owner)) {
      report(control / "result", error->code == folio::application::ProcessErrorCode::AlreadyOwned ? "busy" : "error:" + error->message);
      return error->code == folio::application::ProcessErrorCode::AlreadyOwned ? 4 : 1;
    }
    report(control / "result", "owned");
    while (!fs::exists(control / "release")) {
      if (std::chrono::steady_clock::now() > deadline) return 11;
      std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }
  }
  report(control / "released", "released");
  return 0;
}
