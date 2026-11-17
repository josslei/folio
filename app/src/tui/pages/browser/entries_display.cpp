#include "tui/pages/browser/entries_display.hpp"
#include "tui/pages/browser/top_bar.hpp"
#include "tui/layouts/virtual_rows.hpp"
#include "tui/logic/navigation/navigable_component.hpp"

#include <ftxui/component/event.hpp>
#include <algorithm>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <type_traits>

namespace folio::tui {
namespace {
using namespace ftxui;

std::string date_text(std::chrono::year_month_day date) {
  if (!date.ok()) return "—";
  std::ostringstream out;
  out << std::setfill('0') << std::setw(4) << int(date.year()) << '-' << std::setw(2)
      << unsigned(date.month()) << '-' << std::setw(2) << unsigned(date.day());
  return out.str();
}

// Use the OS local timezone, including the offset/DST applicable at this instant.
std::string local_timestamp_text(core::Timestamp timestamp, const char* format) {
  const auto seconds = std::chrono::system_clock::to_time_t(timestamp);
  std::tm local{};
#ifdef _WIN32
  if (localtime_s(&local, &seconds) != 0) return "—";
#else
  if (localtime_r(&seconds, &local) == nullptr) return "—";
#endif
  std::ostringstream out;
  out << std::put_time(&local, format);
  return out.str();
}

std::string time_text(core::Timestamp timestamp) {
  return local_timestamp_text(timestamp, "%H:%M");
}

std::string timestamp_text(const core::TimestampContext& value) {
  return local_timestamp_text(value.timestamp, "%Y-%m-%d %H:%M");
}

enum class ColumnAlignment { Left, Center, Right };

// Headers and values share alignment. Spacing is always two cells, and exactly
// one column absorbs remaining width.
Element column(std::string value, int width, ColumnAlignment alignment = ColumnAlignment::Left) {
  auto content = text(std::move(value)) | xflex_shrink;
  switch (alignment) {
  case ColumnAlignment::Left:
    return hbox({content, filler()}) | size(WIDTH, EQUAL, width);
  case ColumnAlignment::Center:
    return hbox({filler(), content, filler()}) | size(WIDTH, EQUAL, width);
  case ColumnAlignment::Right:
    return hbox({filler(), content}) | size(WIDTH, EQUAL, width);
  }
  return content;
}

Element log_columns(std::string date, std::string time, std::string title) {
  return hbox({column(std::move(date), 10), text("  "), column(std::move(time), 5), text("  "),
               text(std::move(title)) | xflex});
}

Element memo_columns(std::string title, std::string tags, std::string modified) {
  return hbox({text(std::move(title)) | xflex, text("  "), column(std::move(tags), 24), text("  "),
               column(std::move(modified), 16)});
}

Element todo_columns(std::string checkbox, std::string title, std::string priority, std::string due,
                     std::string subtasks) {
  return hbox({column(std::move(checkbox), 3, ColumnAlignment::Center), text("  "),
               text(std::move(title)) | xflex, text("  "), column(std::move(priority), 8),
               text("  "), column(std::move(due), 16), text("  "),
               column(std::move(subtasks), 8, ColumnAlignment::Right)});
}

// The common browser frame handles result availability, cursor and scrolling.
// Each concrete browser owns its current column layout and record presentation.
} // namespace

class LogsBrowser;
class MemosBrowser;
class TodosBrowser;
template <> const navigation::Description& navigation::description<LogsBrowser>();
template <> const navigation::Description& navigation::description<MemosBrowser>();
template <> const navigation::Description& navigation::description<TodosBrowser>();

class RecordBrowser : public navigation::NavigableComponent {
public:
  RecordBrowser(const BrowserQueryController& queries, const ColorScheme& scheme,
                const navigation::Description& description)
      : navigation::NavigableComponent(description), queries_(queries), scheme_(scheme) {}
  BrowserPosition position() {
    if (!queries_.loading())
      if (const auto* values = records()) reconcile_cursor(*values);
    return {cursor_, cursor_id_};
  }
  void restore_position(const BrowserPosition& value) {
    cursor_ = value.index;
    cursor_id_ = value.entry;
  }
  bool Focusable() const override {
    return !queries_.loading() && records() && !records()->empty();
  }

  bool OnEvent(Event event) override {
    if (!Focused()) return false;
    const auto* values = records();
    if (!values || values->empty()) return false;
    reconcile_cursor(*values);
    if (event == Event::ArrowUp) {
      // Let the parent focus container move upward to the Top Bar.
      if (cursor_ == 0) return false;
      --cursor_;
    } else if (event == Event::ArrowDown) {
      if (cursor_ + 1 < values->size()) ++cursor_;
    } else {
      return false;
    }
    cursor_id_ = id((*values)[cursor_]);
    return true;
  }

  Element OnRender() override {
    Element body = text("");
    if (const auto* values = queries_.loading() ? nullptr : records()) {
      reconcile_cursor(*values);
      if (values->empty()) {
        body = text("No Records") | color(scheme_.text_muted());
      } else {
        const bool focused = Focused();
        body = std::make_shared<VirtualRows>(
            static_cast<int>(values->size()), static_cast<int>(cursor_),
            [this, values, focused](int index) {
              const auto position = static_cast<std::size_t>(index);
              auto rendered =
                  row((*values)[position], position ? &(*values)[position - 1] : nullptr);
              if (position == cursor_ && focused)
                rendered = rendered | bgcolor(scheme_.primary_container()) |
                           color(scheme_.on_primary_container());
              return rendered;
            });
      }
    }
    return vbox({// Match the body's one-cell scroll-indicator gutter even when it is hidden.
                 hbox({header() | xflex | color(scheme_.on_surface_variant()), text(" ")}),
                 body | vscroll_indicator | frame | flex}) |
           flex;
  }

private:
  const application::RecordMetadataList* records() const {
    if (!queries_.result()) return nullptr;
    const auto* value =
        std::get_if<application::Annotated<application::RecordMetadataList>>(&*queries_.result());
    return value ? &value->value : nullptr;
  }

  static core::Id id(const core::RecordMetadata& record) {
    return std::visit([](const auto& metadata) { return metadata.id; }, record);
  }

  // Reconcile only when the result changes; ordinary cursor movement is O(1).
  void reconcile_cursor(const application::RecordMetadataList& values) {
    if (values.empty()) {
      cursor_ = 0;
      return;
    }
    if (cursor_id_ && cursor_ < values.size() && id(values[cursor_]) == *cursor_id_) return;
    if (cursor_id_) {
      const auto found = std::find_if(values.begin(), values.end(), [&](const auto& record) {
        return id(record) == *cursor_id_;
      });
      if (found != values.end()) cursor_ = static_cast<std::size_t>(found - values.begin());
    }
    cursor_ = std::min(cursor_, values.size() - 1);
    cursor_id_ = id(values[cursor_]);
  }

  std::size_t cursor_ = 0;
  std::optional<core::Id> cursor_id_;
  virtual Element header() const = 0;
  virtual Element row(const core::RecordMetadata& record,
                      const core::RecordMetadata* previous) const = 0;
  const BrowserQueryController& queries_;
  const ColorScheme& scheme_;
};

class LogsBrowser final : public RecordBrowser {
public:
  using NavigationChildren = navigation::Children<>;
  static navigation::Factory factory(BrowserQueryController& queries, const ColorScheme& scheme) {
    return [&queries, &scheme] { return std::make_shared<LogsBrowser>(queries, scheme); };
  }

  LogsBrowser(const BrowserQueryController& queries, const ColorScheme& scheme)
      : RecordBrowser(queries, scheme, folio::tui::navigation::description<LogsBrowser>()) {}

private:
  Element header() const override { return log_columns("DATE", "TIME", "TITLE"); }
  Element row(const core::RecordMetadata& record,
              const core::RecordMetadata* previous) const override {
    const auto& log = std::get<core::LogMetadata>(record);
    const bool repeated =
        previous && std::get<core::LogMetadata>(*previous).log_date.date == log.log_date.date;
    return log_columns(repeated ? "" : date_text(log.log_date.date),
                       time_text(log.created.timestamp), log.title);
  }
};

class MemosBrowser final : public RecordBrowser {
public:
  using NavigationChildren = navigation::Children<>;
  static navigation::Factory factory(BrowserQueryController& queries, const ColorScheme& scheme) {
    return [&queries, &scheme] { return std::make_shared<MemosBrowser>(queries, scheme); };
  }

  MemosBrowser(const BrowserQueryController& queries, const ColorScheme& scheme)
      : RecordBrowser(queries, scheme, folio::tui::navigation::description<MemosBrowser>()) {}

private:
  Element header() const override { return memo_columns("NAME / TITLE", "TAGS", "MODIFIED"); }
  Element row(const core::RecordMetadata& record, const core::RecordMetadata*) const override {
    const auto& memo = std::get<core::MemoMetadata>(record);
    std::string tags;
    for (const auto& tag : memo.tags) {
      if (!tags.empty()) tags += ", ";
      tags += tag.value;
    }
    return memo_columns(memo.title, std::move(tags), timestamp_text(memo.modified));
  }
};

class TodosBrowser final : public RecordBrowser {
public:
  using NavigationChildren = navigation::Children<>;
  static navigation::Factory factory(BrowserQueryController& queries, const ColorScheme& scheme) {
    return [&queries, &scheme] { return std::make_shared<TodosBrowser>(queries, scheme); };
  }

  TodosBrowser(const BrowserQueryController& queries, const ColorScheme& scheme)
      : RecordBrowser(queries, scheme, folio::tui::navigation::description<TodosBrowser>()) {}

private:
  Element header() const override {
    return todo_columns("", "TITLE", "PRIORITY", "DUE DATE", "SUBTASKS");
  }
  Element row(const core::RecordMetadata& record, const core::RecordMetadata*) const override {
    const auto& todo = std::get<core::TodoMetadata>(record);
    std::string priority;
    switch (todo.priority) {
    case core::TodoPriority::None:
      priority = "None";
      break;
    case core::TodoPriority::Low:
      priority = "Low";
      break;
    case core::TodoPriority::Medium:
      priority = "Medium";
      break;
    case core::TodoPriority::High:
      priority = "High";
      break;
    }
    std::string due = "—";
    if (todo.due_date) {
      due = std::visit(
          [](const auto& value) {
            if constexpr (std::is_same_v<std::decay_t<decltype(value)>, core::DateContext>)
              return date_text(value.date);
            else
              return timestamp_text(value);
          },
          *todo.due_date);
    }
    // Query metadata has no subtask counts; do not load Record bodies or imply zero.
    auto result = todo_columns(todo.status == core::TodoStatus::Completed ? "[x]" : "[ ]",
                               todo.title, std::move(priority), std::move(due), "—");
    return todo.status == core::TodoStatus::Completed ? result | dim : result;
  }
};

template <> const navigation::Description& navigation::description<LogsBrowser>() {
  static const auto description = navigation::NavigableComponent::describe<LogsBrowser>();
  return description;
}
template <> const navigation::Description& navigation::description<MemosBrowser>() {
  static const auto description = navigation::NavigableComponent::describe<MemosBrowser>();
  return description;
}
template <> const navigation::Description& navigation::description<TodosBrowser>() {
  static const auto description = navigation::NavigableComponent::describe<TodosBrowser>();
  return description;
}

navigation::Anchor browser_anchor(BrowserKind browser) {
  switch (browser) {
  case BrowserKind::Logs:
    return navigation::description<LogsBrowser>().anchor;
  case BrowserKind::Memos:
    return navigation::description<MemosBrowser>().anchor;
  case BrowserKind::Todos:
    return navigation::description<TodosBrowser>().anchor;
  }
  throw std::invalid_argument("Unknown Browser kind");
}
std::optional<BrowserKind> browser_kind(const navigation::Anchor& anchor) {
  if (anchor.is<LogsBrowser>()) return BrowserKind::Logs;
  if (anchor.is<MemosBrowser>()) return BrowserKind::Memos;
  if (anchor.is<TodosBrowser>()) return BrowserKind::Todos;
  return {};
}

EntriesDisplay::EntriesDisplay(BrowserQueryController& queries, std::function<std::optional<BrowserKind>()> active,
                               const ColorScheme& scheme,
                               std::shared_ptr<navigation::INavigable> prepared)
    : active_(std::move(active)), scheme_(scheme), queries_(queries) {
  if (prepared && prepared->static_description())
    adopt_browser(prepared->static_description()->anchor, prepared);
}
ftxui::Component& EntriesDisplay::browser_slot(BrowserKind kind) {
  switch (kind) {
  case BrowserKind::Logs:
    return logs_;
  case BrowserKind::Memos:
    return memos_;
  case BrowserKind::Todos:
    return todos_;
  }
  throw std::invalid_argument("Unknown Browser kind");
}
bool EntriesDisplay::adopt_browser(const navigation::Anchor& anchor,
                                   const std::shared_ptr<navigation::INavigable>& browser) {
  const auto kind = browser_kind(anchor);
  auto component = std::dynamic_pointer_cast<RecordBrowser>(browser);
  if (!kind || !component || !browser->static_description() ||
      browser->static_description()->anchor != anchor)
    return false;
  auto& slot = browser_slot(*kind);
  if (slot) return slot.get() == component.get();
  Add(component);
  slot = std::move(component);
  return true;
}

bool EntriesDisplay::Focusable() const {
  return active_() && !queries_.loading() && queries_.result() &&
         std::holds_alternative<application::Annotated<application::RecordMetadataList>>(
             *queries_.result()) &&
         !std::get<application::Annotated<application::RecordMetadataList>>(*queries_.result())
              .value.empty();
}

ftxui::Component EntriesDisplay::ActiveChild() {
  const auto active = active_();
  if (!active) return {};
  auto& slot = browser_slot(*active);
  if (!slot) {
    // A selected child may be displayed while the navigation cursor is still at
    // Top Bar. Use the same registered factory, not a second constructor list.
    adopt_browser(browser_anchor(*active), browser_factory(*active)());
  }
  return slot;
}

navigation::Factory EntriesDisplay::browser_factory(BrowserKind kind) const {
  return browser_factory(kind, queries_, scheme_);
}

navigation::Factory EntriesDisplay::browser_factory(BrowserKind kind,
                                                    BrowserQueryController& queries,
                                                    const ColorScheme& scheme) {
  switch (kind) {
  case BrowserKind::Logs:
    return LogsBrowser::factory(queries, scheme);
  case BrowserKind::Memos:
    return MemosBrowser::factory(queries, scheme);
  case BrowserKind::Todos:
    return TodosBrowser::factory(queries, scheme);
  }
  throw std::logic_error("Unknown Browser kind");
}

bool EntriesDisplay::OnEvent(ftxui::Event event) {
  auto child = ActiveChild();
  return child && child->OnEvent(event);
}

std::shared_ptr<navigation::INavigable> EntriesDisplay::browser(BrowserKind kind) const {
  switch (kind) {
  case BrowserKind::Logs:
    return std::dynamic_pointer_cast<navigation::INavigable>(logs_);
  case BrowserKind::Memos:
    return std::dynamic_pointer_cast<navigation::INavigable>(memos_);
  case BrowserKind::Todos:
    return std::dynamic_pointer_cast<navigation::INavigable>(todos_);
  }
  return nullptr;
}
BrowserPosition EntriesDisplay::position() {
  auto child = ActiveChild();
  return child ? static_cast<RecordBrowser&>(*child).position() : BrowserPosition{};
}
void EntriesDisplay::restore_position(const BrowserPosition& value) {
  if (auto child = ActiveChild()) static_cast<RecordBrowser&>(*child).restore_position(value);
}
ftxui::Element EntriesDisplay::OnRender() {
  auto child = ActiveChild();
  return child ? child->Render() : ftxui::text("");
}
} // namespace folio::tui
