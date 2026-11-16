#pragma once

#include "tui/logic/navigation/navigation.hpp"
#include <any>
#include <deque>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

namespace folio::tui::navigation {
/// TUI-event-loop coordinator, outside the navigation hierarchy. Owns Current
/// Location, History and restoration values, never the component tree. Resolves
/// UI-owned relationships, constructs/hosts destinations, then commits and notifies.
/// Only Shell must stay alive; resolved descendants are weak, transition-local pins
/// keep them alive during use, and durable paths contain semantic anchors only.
class Manager {
public:
  /// changed runs after successful commit, including initial activation. It may
  /// initiate asynchronous queries, but must not initiate another transition.
  Manager(INavigable& shell, Location history_start,
          std::function<void(const Location&, const std::any*)> changed = {});
  const Location& current_location() const { return current_; }
  /// Immutable session-bound realization of the application's History Start declaration.
  const Location& history_start() const { return history_start_; }
  const auto& history() const { return history_; }
  std::size_t history_cursor() const { return cursor_; }
  Result down(const Target& target);
  Result up();
  /// Execute the destination and Push/Switch mode declared together by the UI.
  Result cross(const Target& target);
  Result history_back();
  Result history_forward();
  /// User Escape policy entry point, called by Shell, not by navigation controls.
  /// Selects the contextual action; its underlying operation owns History effects.
  Result contextual_back();
  /// Provisional owner-defined cursor snapshot, keyed by Location; no UI references.
  /// TODO(Navigation): Replace BrowserPosition/std::any navigation snapshots with
  /// virtual-subtree Locations and per-browser remembered Locations. Keep only
  /// presentation details (such as scroll offset) local to browser components.
  void remember_position(std::any position);
  /// Save owner-defined context at an ancestor (e.g. its displayed browser).
  void remember_position(const Location& location, std::any position);
  const std::any* restored_position() const;

private:
  Result transition(Location target, std::optional<CrossMode> mode = {},
                    std::optional<std::size_t> history_cursor = {});
  struct ResolvedLevel {
    std::weak_ptr<INavigable> object;
  };
  std::vector<ResolvedLevel> resolved_;
  INavigable& shell_;
  const Location history_start_;
  Location current_;
  /// The selected slot always equals Current Location; History Start is separate.
  std::deque<Location> history_;
  std::size_t cursor_ = 0;
  std::vector<bool> navigable_ancestors_;
  std::vector<std::pair<Location, std::any>> positions_;
  std::function<void(const Location&, const std::any*)> changed_;
};
} // namespace folio::tui::navigation
