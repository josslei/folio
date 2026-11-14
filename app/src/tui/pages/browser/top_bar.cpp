#include "tui/pages/browser/top_bar.hpp"
#include <ftxui/component/event.hpp>
#include <ftxui/component/mouse.hpp>
#include <ftxui/dom/elements.hpp>
namespace folio::tui {
TopBar::TopBar(std::function<void(core::RecordType)> select) : select_(std::move(select)) {}
void TopBar::activate(int index) {
  if (selected_ == index) return;
  selected_ = index;
  select_(index == 0 ? core::RecordType::Log : index == 1 ? core::RecordType::Memo : core::RecordType::Todo);
}
ftxui::Element TopBar::OnRender() {
  using namespace ftxui;
  const std::array<const char*, 3> labels{"LOGS", "MEMOS", "TODOS"};
  Elements items;
  for (int i = 0; i < 3; ++i) {
    const bool focused = i == cursor_ && Focused();
    auto item = text(std::string(focused ? "[     " : "      ") + labels[i] +
                     (focused ? "     ]" : "      "));
    if (i == selected_) item = item | bgcolor(Color::White) | color(Color::Black);
    items.push_back(item | reflect(boxes_[i]));
  }
  Elements line;
  for (int i = 0; i < 3; ++i) {
    const auto width = std::string_view(labels[i]).size() + 12;
    std::string segment;
    for (std::size_t j = 0; j < width; ++j) segment += "▔";
    auto part = text(segment);
    if (i == selected_) part = part | color(Color::White);
    line.push_back(part);
  }
  line.push_back(separatorCharacter("▔") | flex);
  return vbox({hbox(std::move(items)), hbox(std::move(line))});
}
bool TopBar::OnEvent(ftxui::Event event) {
  using namespace ftxui;
  if (event.is_mouse()) {
    if (event.mouse().button != Mouse::Left || event.mouse().motion != Mouse::Pressed) return false;
    for (int i = 0; i < 3; ++i) if (boxes_[i].Contain(event.mouse().x, event.mouse().y)) {
      activate(i);
      return true;
    }
    return false;
  }
  if (!Focused()) return false;
  if (event == Event::ArrowRight || event == Event::Tab) cursor_ = (cursor_ + 1) % 3;
  else if (event == Event::ArrowLeft || event == Event::TabReverse) cursor_ = (cursor_ + 2) % 3;
  else if (event == Event::Return) activate(cursor_);
  else return false;
  return true;
}
} // namespace folio::tui
