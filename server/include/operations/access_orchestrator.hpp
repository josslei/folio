#pragma once

#include <folio/ipc/application_protocol.hpp>
#include <functional>

namespace folio::core::storage {
class MetadataCatalog;
class NormalRepository;
class CatalogCoordinator;
} // namespace folio::core::storage

namespace folio::application::internal {

// Access preparation runs on a reserved Access worker; prepared actions
// use retained views or reserved authoritative reads on execution workers. Borrows Storage
// dependencies; owns no Catalog, Repository or connection state. Uses known locators without
// discovery.
class AccessOrchestrator {
public:
  AccessOrchestrator(core::storage::MetadataCatalog& catalog,
                     core::storage::NormalRepository& repository,
                     core::storage::CatalogCoordinator& coordinator);
  /// Convenience composition for serialized callers; dependencies must outlive work.
  /// Returns metadata or a targeted authoritative read, never implicit repair.
  OperationResponse handle(const OperationRequest& request) const;
  /// In-memory preparation captures status/view or current locator, with no file I/O.
  /// Caller must reserve a Load against conflicting mutations through action completion.
  std::function<OperationResponse()> prepare(OperationRequest request) const;

private:
  core::storage::MetadataCatalog& catalog_;
  core::storage::NormalRepository& repository_;
  core::storage::CatalogCoordinator& coordinator_;
};

} // namespace folio::application::internal
