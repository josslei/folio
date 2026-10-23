#include "folio/core/version.hpp"

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>

int main() {
  using namespace ftxui;

  auto screen = ScreenInteractive::Fullscreen();
  auto renderer = Renderer([&] {
    return vbox({text(std::string(folio::core::application_name())) | bold | center,
                 text("Welcome to Folio") | center,
                 text("Press q to quit") | dim | center}) |
           center;
  });
  auto component = CatchEvent(renderer, [&](const Event& event) {
    if (event == Event::Character('q') || event == Event::Character('Q')) {
      screen.Exit();
      return true;
    }
    return false;
  });

  screen.Loop(component);
  return 0;
}
