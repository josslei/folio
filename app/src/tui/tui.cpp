#include "tui.hpp"
#include <folio/application/backend_connection_handle.hpp>
#include "folio/core/version.hpp"
#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>

int folio::tui::run() {
  namespace app = folio::application::internal;
  using namespace ftxui;

  auto connection = folio::application::connect_or_start_backend();
  if (auto* error = std::get_if<app::IpcError>(&connection)) {
    std::cerr << error->message << '\n';
    return 1;
  }
  auto& backend = std::get<folio::application::BackendConnectionHandle>(connection);

  auto screen = ScreenInteractive::Fullscreen();
  auto renderer = Renderer([&] {
    return vbox({text(std::string(folio::core::application_name())) | bold | center,
                 text("Welcome to Folio") | center,
                 text("Press q to quit") | dim | center}) |
           center;
  });
  std::atomic<bool> connection_lost{false};
  auto component = CatchEvent(renderer, [&](const Event& event) {
    if (connection_lost.load()) { screen.Exit(); return true; }
    if (event == Event::Character('q') || event == Event::Character('Q')) {
      screen.Exit();
      return true;
    }
    return false;
  });

  std::jthread monitor([&](std::stop_token stop) {
    while (!stop.stop_requested()) {
      if (!backend.connected()) {
        connection_lost = true;
        screen.PostEvent(Event::Custom);
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  });
  screen.Loop(component);
  monitor.request_stop();
  monitor.join();
  if (connection_lost) std::cerr << "Backend connection lost; restart Folio to reconnect\n";
  return connection_lost ? 1 : 0;
}
