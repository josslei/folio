#include "backend.hpp"
#include <folio/logging/logging.hpp>
#include "tui/tui.hpp"

#include <CLI/CLI.hpp>
#include <cstdio>

#ifndef _WIN32 // POSIX (Linux and macOS)
int main(int argc, char** argv) {
#else // Windows
int wmain(int argc, wchar_t** argv) {
#endif
  // Detect the private mode without interpreting help or unrelated arguments.
  CLI::App mode_parser;
  mode_parser.set_help_flag();
  mode_parser.allow_extras();
  bool backend_process = false;
  mode_parser.add_flag("--backend-process", backend_process)->group("")->disable_flag_override();
  CLI11_PARSE(mode_parser, argc, argv);
  const auto role = backend_process ? folio::logging::ProcessRole::Backend
                                    : folio::logging::ProcessRole::Frontend;
#ifdef FOLIO_TEST_LOG_ROOT
  // The CLI integration-test executable injects its own directory. This branch
  // is not compiled into folio; no production environment/CLI override is added.
  const auto log_file = folio::logging::initialize(role, FOLIO_TEST_LOG_ROOT);
#else
  const auto log_file = folio::logging::initialize(role);
#endif
  if (!log_file) {
    // Startup only, before the TUI owns the terminal. Continue without file logs.
    std::fputs(
        "Warning: Folio could not initialize diagnostic logging; continuing without file logs.\n",
        stderr);
  }
  const int result = [&] {
    if (backend_process) return folio::application::run_backend();
    CLI::App cli{"Folio"};
    CLI11_PARSE(cli, argc, argv);
    return folio::tui::run();
  }();
  folio::logging::shutdown();
  return result;
}
