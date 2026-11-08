#pragma once

#include "ipc/connection_session.hpp"
#include "storage/catalog_coordinator.hpp"
#include <folio/ipc/application_protocol.hpp>
#include <map>

namespace folio::application::internal {

// Executor-owned mutation orchestration and edit-lock state. Access operations
// are routed separately by the executor callback.
// Borrows Storage components; Repository owns persistence sequencing and invokes
// Coordinator callbacks for Catalog synchronization. This handler does not own
// worker threads, scheduling or Backend lifecycle. Only Normal storage exists
// today; storage-domain selection belongs behind this Application boundary.
// Expired session owners are removed before each mutation/editing request; no leases,
// timed expiry, cross-session permissions or persistence are implied.
class MutationOrchestrator {
public:
  MutationOrchestrator(core::storage::MetadataCatalog& catalog,
                       core::storage::NormalRepository& repository,
                       core::storage::CatalogCoordinator& coordinator);
  OperationResponse handle(const OperationRequest& request, const Session& session);

private:
  std::optional<ApplicationError> check_lock(const core::Id& id, const Session& session,
                                             bool required);
  OperationResponse acquire_edit(const core::Id& id, const Session& session);
  OperationResponse release_edit(const core::Id& id, const Session& session);
  NormalMutationResponse mutate(const MutationRequest& request, const Session& session);
  core::storage::MetadataCatalog& catalog_;
  core::storage::NormalRepository& repository_;
  core::storage::CatalogCoordinator& coordinator_;
  std::map<std::string, std::weak_ptr<ConnectionSession>> locks_;
};
} // namespace folio::application::internal
