#pragma once
#include <folio/core/types.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/screen/box.hpp>
#include <array>
#include <functional>
namespace folio::tui {
/// Browser module selection and its independent keyboard cursor.
class TopBar : public ftxui::ComponentBase {
public:
  explicit TopBar(std::function<void(core::RecordType)> select);
  ftxui::Element OnRender() override;
  bool OnEvent(ftxui::Event event) override;
  bool Focusable() const override { return true; }
private:
  void activate(int index);
  std::function<void(core::RecordType)> select_;
  int cursor_ = 0;
  int selected_ = 0;
  std::array<ftxui::Box, 3> boxes_{};
};
} // namespace folio::tui
