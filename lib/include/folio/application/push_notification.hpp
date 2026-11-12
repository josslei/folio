#pragma once

#include <folio/application/maintenance.hpp>
#include <folio/application/error.hpp>
#include <cstdint>

namespace folio::application {
enum class SharedAvailability { Ready, Inconsistent, Unavailable, Stopping, Fatal };
/// Invalidates previously observed state; never an instruction to replace a Draft.
/// Flags identify possibly changed portions, not proof that their values differ.
struct RecordChangeNotice {
  core::Id id;
  bool metadata = true;
  bool content = true;
};
/// Scope includes potentially loaded content after Refresh; no bodies are replicated.
struct CatalogChangeNotice {
  RefreshScope scope;
  bool content = false; // Refresh/uncertain effects also invalidate loaded content in scope.
};
struct EditLockChangeNotice {
  core::Id id;
};
struct AvailabilityChangeNotice {
  SharedAvailability state;
  std::optional<ApplicationError> error; // Sanitized shared-service failure only.
};
/// One change notice per ordered Push delivery and receipt ACK. No private context.
using PushNotification = std::variant<RecordChangeNotice, CatalogChangeNotice, EditLockChangeNotice,
                                      AvailabilityChangeNotice>;
} // namespace folio::application
