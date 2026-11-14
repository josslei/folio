#pragma once

#include <ftxui/dom/node.hpp>
#include <functional>

namespace folio::tui {
/// One-terminal-line rows, materialized only inside the rendering clip.
/// This DOM layout primitive uses Node rather than ComponentBase: the owning
/// Component handles input/state; FTXUI frame and vscroll_indicator handle scrolling.
class VirtualRows final : public ftxui::Node {
public:
  /// Main-thread rendering only. Callback and its borrowed data must outlive the
  /// element. count is nonnegative; cursor is a valid row index when count > 0.
  VirtualRows(int count, int cursor, std::function<ftxui::Element(int)> row);
  void ComputeRequirement() override;
  void Render(ftxui::Screen& screen) override;

private:
  int count_;
  int cursor_;
  std::function<ftxui::Element(int)> row_;
};
} // namespace folio::tui
