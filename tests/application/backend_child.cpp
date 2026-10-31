#include "application/internal/process.hpp"
// Test-only process entry point with injected paths and bounded file barriers.
#include <folio/application/backend.hpp>
#include "application/internal/backend_states.hpp"
#include <folio/application/backend_connection_handle.hpp>
#include <folio/application/ipc.hpp>
#include <fstream>
#include <thread>
namespace fs = std::filesystem;
namespace app = folio::application::internal;

void report(const fs::path& path, const std::string& text) {
  auto temp = path;
  temp += ".writing";
  {
    std::ofstream out(temp);
    out << text;
  }
  fs::rename(temp, path);
}
#ifndef _WIN32
int main(int argc, char** argv) {
#else
int wmain(int argc, wchar_t** argv) {
#endif
  if (argc != 4)
    return 2;
  const fs::path mode(argv[1]), base(argv[2]), control(argv[3]);
  folio::config::StoragePaths paths{base / "normal",
                              base / "vault",
                              {base / "config", base / "data", base / "state",
                               base / "cache", base / "runtime"}};
  if (mode == "backend") {
    const int result = app::run_backend(paths);
    report(control, std::to_string(result));
    return result;
  }
  if (mode != "frontend")
    return 2;
  auto executable = app::current_executable();
  if (!std::holds_alternative<fs::path>(executable))
    return 1;
  unsigned attempt = 0;
  auto connection = folio::application::connect_or_start_backend(
      std::get<app::ConnectionSettings>(app::connection_settings(paths)), [&] {
        return app::launch_process(
            std::get<fs::path>(executable),
            {"backend", base, control / ("backend-" + std::to_string(attempt++))});
      });
  if (auto* problem = std::get_if<app::IpcError>(&connection)) {
    report(control / "error", problem->message);
    return 1;
  }
  report(control / "connected", "ready");
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
  while (!fs::exists(control / "disconnect") &&
         std::chrono::steady_clock::now() < deadline) {
    if (!std::get<folio::application::BackendConnectionHandle>(connection).connected()) {
      report(control / "lost", "lost");
      return 1;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  connection = app::IpcError{app::IpcErrorCode::ConnectionLost, "test finished"};
  report(control / "done", "done");
  return 0;
}  // namespace folio::application::internal
