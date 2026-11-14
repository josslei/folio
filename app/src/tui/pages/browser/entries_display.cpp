#include "tui/pages/browser/entries_display.hpp"

namespace folio::tui {
namespace {
using namespace ftxui;

// The common frame only handles result availability and scrolling. Each list
// owns its column layout so module-specific presentation can evolve separately.
class RecordList : public ComponentBase {
public:
  explicit RecordList(const RecordQuery& queries) : queries_(queries) {}
  Element OnRender() override {
    Elements rows;
    if (const auto& result = queries_.result()) {
      if (const auto* records =
              std::get_if<application::Annotated<std::vector<core::RecordMetadata>>>(&*result)) {
        for (const auto& record : records->value) rows.push_back(row(record));
        if (rows.empty()) rows.push_back(text("No Records") | dim);
      }
    }
    if (rows.empty()) rows.push_back(text(""));
    return vbox({header() | xflex | dim,
                 vbox(std::move(rows)) | vscroll_indicator | frame | flex}) | flex;
  }

private:
  virtual Element header() const = 0;
  virtual Element row(const core::RecordMetadata& record) const = 0;
  const RecordQuery& queries_;
};

class LogList final : public RecordList {
public:
  using RecordList::RecordList;
private:
  Element header() const override {
    return hbox({text("DATE") | size(WIDTH, EQUAL, 14),
                 text("TIME") | size(WIDTH, EQUAL, 11), text("TITLE") | flex});
  }
  Element row(const core::RecordMetadata& record) const override {
    return hbox({text("") | size(WIDTH, EQUAL, 25),
                 text(std::get<core::LogMetadata>(record).title)});
  }
};

class MemoList final : public RecordList {
public:
  using RecordList::RecordList;
private:
  Element header() const override {
    return hbox({text("NAME / TITLE") | xflex_grow_factor(1),
                 text("TAGS") | xflex_grow_factor(2),
                 text("MODIFIED") | size(WIDTH, EQUAL, 12)});
  }
  Element row(const core::RecordMetadata& record) const override {
    return text(std::get<core::MemoMetadata>(record).title);
  }
};

class TodoList final : public RecordList {
public:
  using RecordList::RecordList;
private:
  Element header() const override {
    return hbox({text("     TITLE") | flex, text("PRIORITY") | size(WIDTH, EQUAL, 13),
                 text("DUE DATE") | size(WIDTH, EQUAL, 13), text("SUBTASKS")});
  }
  Element row(const core::RecordMetadata& record) const override {
    const auto& todo = std::get<core::TodoMetadata>(record);
    return hbox({text(todo.status == core::TodoStatus::Completed ? "[x]  " : "[ ]  "),
                 text(todo.title)});
  }
};
} // namespace

EntriesDisplay::EntriesDisplay(const RecordQuery& queries)
    : queries_(queries), logs_(std::make_shared<LogList>(queries)),
      memos_(std::make_shared<MemoList>(queries)), todos_(std::make_shared<TodoList>(queries)) {
  Add(logs_);
  Add(memos_);
  Add(todos_);
}

ftxui::Element EntriesDisplay::OnRender() {
  switch (queries_.selected()) {
  case core::RecordType::Log: return logs_->Render();
  case core::RecordType::Memo: return memos_->Render();
  case core::RecordType::Todo: return todos_->Render();
  }
  return ftxui::text("");
}
} // namespace folio::tui
