#include "tui/layouts/virtual_rows.hpp"

#include <ftxui/util/autoreset.hpp>
#include <algorithm>
#include <utility>

namespace folio::tui {
VirtualRows::VirtualRows(int count, int cursor, std::function<ftxui::Element(int)> row)
    : count_(count), cursor_(cursor), row_(std::move(row)) {}

void VirtualRows::ComputeRequirement() {
  requirement_ = {};
  requirement_.min_y = count_;
  // Report the logical extent and cursor to FTXUI without constructing hidden rows.
  requirement_.focused.enabled = count_ > 0;
  requirement_.focused.node = this;
  requirement_.focused.box = {0, 0, cursor_, cursor_};
}

void VirtualRows::Render(ftxui::Screen& screen) {
  const auto clip = ftxui::Box::Intersection(box_, screen.stencil);
  if (clip.x_min > clip.x_max || clip.y_min > clip.y_max) return;
  const ftxui::AutoReset<ftxui::Box> stencil(&screen.stencil, clip);
  const int first = std::max(0, clip.y_min - box_.y_min);
  const int end = std::min(count_, clip.y_max - box_.y_min + 1);
  for (int index = first; index < end; ++index) {
    auto element = row_(index);
    element->ComputeRequirement();
    auto row_box = box_;
    row_box.y_min = row_box.y_max = box_.y_min + index;
    element->SetBox(row_box);
    element->Render(screen);
  }
}
} // namespace folio::tui
