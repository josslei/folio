#pragma once

#include <folio/ipc/application_protocol.hpp>

namespace folio::core::storage {
class MetadataCatalog;
class NormalRepository;
class CatalogCoordinator;
} // namespace folio::core::storage

namespace folio::application::internal {

// Executor-only read orchestration. Borrows Storage dependencies; owns no
// Catalog, Repository or connection state. Uses known locators without discovery.
class AccessOrchestrator {
public:
  AccessOrchestrator(core::storage::MetadataCatalog& catalog,
                     core::storage::NormalRepository& repository,
                     core::storage::CatalogCoordinator& coordinator);
  /// Execute only on the shared executor; dependencies must outlive this object.
  /// Returns metadata or a targeted authoritative read, never implicit repair.
  OperationResponse handle(const OperationRequest& request) const;

private:
  core::storage::MetadataCatalog& catalog_;
  core::storage::NormalRepository& repository_;
  core::storage::CatalogCoordinator& coordinator_;
};

} // namespace folio::application::internal
