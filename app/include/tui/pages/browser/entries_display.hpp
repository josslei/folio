#pragma once
#include "tui/pages/browser/record_query.hpp"
#include <ftxui/component/component_base.hpp>
namespace folio::tui {
/// Presentation of query results only; owns no Backend or request state.
class EntriesDisplay : public ftxui::ComponentBase {
public:
  explicit EntriesDisplay(const RecordQuery& queries);
  ftxui::Element OnRender() override;
private:
  const RecordQuery& queries_;
  ftxui::Component logs_;
  ftxui::Component memos_;
  ftxui::Component todos_;
};
} // namespace folio::tui
