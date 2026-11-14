#include "tui/tui.hpp"
#include "frontend/runtime/backend_connection_handle.hpp"
#include "tui/logic/browser/browser_query_controller.hpp"
#include "tui/pages/browser/browser_page.hpp"
#include "tui/shell/tui_shell.hpp"
#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <iostream>

int folio::tui::run() {
  using namespace ftxui;
  auto connection = application::connect_or_start_backend();
  if (auto* error = std::get_if<application::internal::IpcError>(&connection)) {
    std::cerr << error->message << '\n';
    return 1;
  }
  auto& session = std::get<std::unique_ptr<application::ConnectionSession>>(connection);
  auto screen = ScreenInteractive::Fullscreen();
  const auto frontend_work = Event::Special("folio.frontend.work");
  application::BackendConnectionHandle backend(std::move(session),
                                               [&] { screen.PostEvent(frontend_work); });
  BrowserQueryController queries(backend);
  auto page = std::make_shared<BrowserPage>(queries, [&] { screen.PostEvent(Event::Custom); });
  bool available = true;
  auto component = std::make_shared<TuiShell>(
      page,
      [page, &backend, &available] {
        if (!backend.connected()) return text("Disconnected");
        if (!available) return text("Backend unavailable");
        return page->feedback();
      },
      [&] { screen.Exit(); });
  auto events = CatchEvent(component, [&](Event event) {
    if (event != frontend_work) return false;
    const auto work = backend.take_pending_work();
    if (work.completion) {
      try {
        backend.process_completion();
      } catch (...) {
        available = false;
      }
    }
    if (work.session_changed) available = backend.connected();
    bool reload = false;
    if (work.push_notifications)
      for (const auto& notification : backend.take_push_notifications()) {
        std::visit(
            [&](const auto& notice) {
              using Notice = std::decay_t<decltype(notice)>;
              if constexpr (std::is_same_v<Notice, application::RecordChangeNotice>)
                reload |= notice.metadata;
              else if constexpr (std::is_same_v<Notice, application::CatalogChangeNotice>)
                reload = true;
              else if constexpr (std::is_same_v<Notice, application::AvailabilityChangeNotice>) {
                available = notice.state == application::SharedAvailability::Ready;
                reload |= available;
              }
            },
            notification);
      }
    if (reload) queries.invalidate();
    queries.resume_pending();
    return true;
  });
  screen.Loop(events);
  try {
    backend.finish_requests();
  } catch (...) {
    return 1;
  }
  return 0;
}
