#include "tui/logic/navigation/navigation_manager.hpp"
#include <algorithm>
#include <stdexcept>

namespace folio::tui::navigation {
Manager::Manager(INavigable& shell, Location history_start,
                 std::function<void(const Location&, const std::any*)> changed)
    : shell_(shell), history_start_(std::move(history_start)), history_{history_start_},
      changed_(std::move(changed)) {
  // TODO(Navigation): Revisit the permanent Browser/Logs/Root history anchor when
  // the future Main Page is designed. Evaluate whether Main Page should become
  // the permanent initial history location.
  transition(history_start_);
}
Result Manager::transition(Location target, std::optional<CrossMode> mode,
                           std::optional<std::size_t> restore_cursor) {
  if (target.empty()) throw std::logic_error("Empty navigation destination");

  // Resolution is read-only. Reuse live ancestry, then look up any existing suffix.
  std::size_t prefix = 0;
  std::vector<std::shared_ptr<INavigable>> retained;
  retained.reserve(target.size());
  while (prefix < target.size() && prefix < current_.size() && prefix < resolved_.size() &&
         target[prefix] == current_[prefix]) {
    auto object = resolved_[prefix].object.lock();
    if (!object) break;
    retained.push_back(std::move(object));
    ++prefix;
  }
  const auto* registered = target.back().registration();
  if (registered && !registered->visitable)
    throw std::logic_error("Navigation destination is not visitable");
  std::vector<ResolvedLevel> resolved(resolved_.begin(), resolved_.begin() + prefix);
  std::vector<bool> ancestors(navigable_ancestors_.begin(), navigable_ancestors_.begin() + prefix);
  resolved.reserve(target.size());
  ancestors.reserve(target.size());
  auto resolve_suffix = [&]() {
    auto* owner = retained.empty() ? &shell_ : retained.back().get();
    for (auto depth = resolved.size(); depth < target.size(); ++depth) {
      auto child = owner->resolve_child(target[depth], {target, depth});
      if (!child) return;
      resolved.push_back({child});
      ancestors.push_back(child->visitable({target, depth + 1}));
      owner = child.get();
      retained.push_back(std::move(child));
    }
  };
  resolve_suffix();

  // Factory access travels with the destination; no registration search or
  // construction of owning ancestors is required.
  const auto missing = resolved.size();
  auto* host = missing ? retained.back().get() : &shell_;
  std::shared_ptr<INavigable> prepared;
  if (missing < target.size()) {
    prepared = target.back().construct();
    if (!prepared) throw std::logic_error("Navigation factory returned an empty destination");
    if (!prepared->represents(target.back(), {target, target.size()}) ||
        !prepared->visitable({target, target.size()}))
      throw std::logic_error("Navigation factory returned a different destination");
  } else if (!ancestors.back())
    throw std::logic_error("Navigation destination is not visitable");

  // Prepare History before hosting. No allocation or fallible activation callback
  // remains between a successful host and the navigation-state commit.
  auto history = history_;
  auto cursor = restore_cursor.value_or(cursor_);
  if (target == current_) {
    if (mode == CrossMode::Switch) history.resize(cursor_ + 1);
  } else if (!restore_cursor) {
    history.resize(cursor_ + 1);
    if (mode == CrossMode::Push) {
      history[cursor] = current_;
      history.push_back(target);
      ++cursor;
    } else
      history[cursor] = target;
  }

  // Hosting retains the prepared object and arranges its visible composition;
  // it does not choose Current Location or execute another constructor for it.
  if (prepared) {
    host->host(prepared, {target, missing});
    resolve_suffix();
    if (resolved.size() != target.size() || retained.back() != prepared || !ancestors.back())
      throw std::logic_error("Navigation host did not retain the prepared destination path");
  }
  const bool changed = target != current_;
  resolved_.swap(resolved);
  navigable_ancestors_.swap(ancestors);
  current_.swap(target);
  history_.swap(history);
  cursor_ = cursor;
  if (changed_ && (changed || prepared)) changed_(current_, restored_position());
  return changed ? Result::Changed : Result::Unchanged;
}
Result Manager::down(const Target& target) {
  if (target.mode() || target.source() != current_)
    throw std::logic_error("Structural target used outside its declared source context");
  return transition(target.destination());
}
Result Manager::up() {
  for (auto depth = current_.size(); depth > 1;) {
    --depth;
    if (!navigable_ancestors_[depth - 1]) continue;
    auto target = current_;
    target.erase(target.begin() + static_cast<std::ptrdiff_t>(depth), target.end());
    return transition(std::move(target));
  }
  return Result::Unchanged;
}
Result Manager::cross(const Target& target) {
  if (!target.mode() || target.source() != current_)
    throw std::logic_error(
        "Cross target used outside its declared source context or is structural");
  return transition(target.destination(), target.mode());
}
Result Manager::history_back() {
  // TODO(Navigation): Once destinations expose explicit validity, skip confirmed
  // invalid historical locations and use History Start as the final fallback.
  // If restoration fails transiently, preserve the entry and navigation state;
  // an activation exception alone does not establish permanent invalidity.
  return cursor_ ? transition(history_[cursor_ - 1], {}, cursor_ - 1) : Result::Unchanged;
}
Result Manager::history_forward() {
  return cursor_ + 1 < history_.size() ? transition(history_[cursor_ + 1], {}, cursor_ + 1)
                                       : Result::Unchanged;
}
Result Manager::contextual_back() {
  // TODO(Navigation): Extend this unified Escape decision in priority order as
  // the corresponding interaction contexts are implemented:
  // if a Dialog/overlay is active, let it consume Escape without navigating;
  // else if editing, request safe Editor exit to Viewer, confirming unsaved
  // Drafts; cancellation must leave Location and History unchanged;
  // else if viewing a Record, restore its Cross Push source through History;
  // else in Browser content, perform semantic structural Up (also for virtual
  // positions), including Section Root -> Top Bar under our approved rule;
  // else at Top Bar, consume Escape as a no-op.
  // Only Browser/Top Bar contexts exist today. Their saved navigable ancestry
  // supplies Up, or no parent at Top Bar. Do not extend this into a universal
  // Up-first fallback when future contexts are added.
  return up();
}
void Manager::remember_position(std::any position) {
  remember_position(current_, std::move(position));
}
void Manager::remember_position(const Location& target, std::any position) {
  for (auto& [location, saved] : positions_)
    if (location == target) {
      saved = std::move(position);
      return;
    }
  positions_.emplace_back(target, std::move(position));
}
const std::any* Manager::restored_position() const {
  for (const auto& [location, saved] : positions_)
    if (location == current_) return &saved;
  return nullptr;
}
} // namespace folio::tui::navigation
