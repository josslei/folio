#pragma once

#include "frontend/runtime/backend_connection_handle.hpp"
#include <folio/application/error.hpp>
#include <folio/application/query.hpp>
#include <array>
#include <memory>
#include <optional>

namespace folio::tui {
/// TUI-thread Browser query state. The Backend handle owns request execution.
/// Destroyed controllers safely abandon Query completions; Backend must outlive live users.
/// Construct with shared ownership before submitting requests. All access is on the TUI thread.
class BrowserQueryController : public std::enable_shared_from_this<BrowserQueryController> {
public:
  explicit BrowserQueryController(application::BackendConnectionHandle& backend) : backend_(backend) {}
  BrowserQueryController(const BrowserQueryController&) = delete;
  BrowserQueryController& operator=(const BrowserQueryController&) = delete;
  /// Main thread: invalidate current results and defer a replacement if busy.
  void invalidate();
  void select(core::RecordType type);
  /// Main thread, after communication events: retry the latest deferred query.
  void resume_pending();
  bool loading() const { return loading_; }
  core::RecordType query_type() const { return query_type_; }
  const auto& result() const { return result_; }

private:
  class QueryCompletion;
  void submit_pending();
  application::BackendConnectionHandle& backend_;
  bool reload_pending_ = false;
  bool request_active_ = false;
  std::size_t generation_ = 0;
  bool loading_ = false;
  std::optional<application::AccessResult<application::Annotated<application::RecordMetadataList>>>
      result_;
  // Derived request/cache identity, supplied by Browser navigation.
  core::RecordType query_type_ = core::RecordType::Log;
  std::array<std::optional<application::AccessResult<
                 application::Annotated<application::RecordMetadataList>>>,
             3>
      module_results_;
};
} // namespace folio::tui
