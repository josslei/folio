#include "application/backend.hpp"
#include "tui/frontend.hpp"

#include <CLI/CLI.hpp>

namespace app = folio::application::internal;

#ifndef _WIN32  // POSIX (Linux and macOS)
int main(int argc, char** argv) {
#else  // Windows
int wmain(int argc, wchar_t** argv) {
#endif
  // Detect the private mode without interpreting help or unrelated arguments.
  CLI::App mode_parser;
  mode_parser.set_help_flag();
  mode_parser.allow_extras();
  bool backend_process = false;
  mode_parser.add_flag(app::backend_process_argument.string(), backend_process)
      ->group("")->disable_flag_override();
  CLI11_PARSE(mode_parser, argc, argv);
  if (backend_process) return app::run_backend();

  CLI::App cli{"Folio"};
  CLI11_PARSE(cli, argc, argv);
  return folio::tui::run();
}
