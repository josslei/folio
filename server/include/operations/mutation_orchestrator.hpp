#pragma once

#include "ipc/connection_session.hpp"
#include "storage/catalog_coordinator.hpp"
#include <folio/ipc/application_protocol.hpp>
#include <map>
#include <mutex>
#include <functional>
#include <folio/application/push_notification.hpp>

namespace folio::application::internal {

// One mutation worker owns authoritative orchestration; the request coordination
// thread handles editing control through the synchronized ownership registry.
// Borrows Storage components; Repository owns persistence sequencing and invokes
// Coordinator callbacks for Catalog synchronization. This handler does not own
// worker threads, scheduling or Backend lifecycle. Only Normal storage exists
// today; storage-domain selection belongs behind this Application boundary.
// Final session-reference release removes owners promptly; requests also prune expired owners.
// No leases, timed expiry, cross-session permissions or persistence are implied.
// Must outlive accepted actions and session tokens carrying its cleanup callback;
// Backend lifecycle closes/drains those resources before destroying orchestration.
class MutationOrchestrator {
public:
  MutationOrchestrator(core::storage::MetadataCatalog& catalog,
                       core::storage::NormalRepository& repository,
                       core::storage::CatalogCoordinator& coordinator);
  /// Mutation worker dispatch, or in-memory editing control on the coordination thread. The
  /// retained session identifies lock ownership; IPC workers must neither call this directly nor
  /// mutate the lock registry. Checks current Catalog state and prerequisites before Repository
  /// persistence.
  OperationResponse handle(const OperationRequest& request, const Session& session);
  /// Briefly installs immediate-mutation protection before dispatch, without I/O.
  /// The returned action retains protection/session through execution, including exceptions.
  std::function<OperationResponse()> prepare(OperationRequest request, Session session);

  /// Install/remove with execution drained. Sink only enqueues, never performs I/O.
  void set_lock_observer(std::function<void(const core::Id&)> sink) { lock_sink_ = std::move(sink); }

private:
  /// Called by the session release callback when its last reference is destroyed.
  /// Acquires ownership_mutex_ for registry cleanup; never performs Storage I/O.
  void cleanup_locks();
  /// Called by cleanup_locks(), handle() and prepare() with ownership_mutex_ held.
  /// Removes expired lock owners and reports each availability transition.
  void prune_locks();
  std::function<void(const core::Id&)> lock_sink_;
  /// Caller holds ownership_mutex_; required means a long-lived Edit Lock is needed.
  std::optional<ApplicationError> check_lock(const core::Id& id, const Session& session,
                                             bool required);
  /// Ownership-only, in-memory acquisition; never validates/loads the file.
  /// Same-owner reacquisition succeeds; Load failure does not release ownership.
  OperationResponse acquire_edit(const core::Id& id, const Session& session);
  OperationResponse release_edit(const core::Id& id, const Session& session);
  NormalMutationResponse mutate(const MutationRequest& request, const Session& session);
  core::storage::MetadataCatalog& catalog_;
  core::storage::NormalRepository& repository_;
  core::storage::CatalogCoordinator& coordinator_;
  /// Protects locks_ and active_mutations_ across coordination and mutation execution.
  /// Never held during filesystem work. May acquire Coordinator's publication mutex;
  /// publication callbacks never acquire this mutex. Scheduler queue locks are not held.
  std::mutex ownership_mutex_;
  std::map<std::string, std::weak_ptr<ConnectionSession>> active_mutations_;
  // Weak ownership avoids keeping disconnected sessions alive. Accepted actions
  // hold strong session references; final release triggers synchronized cleanup.
  std::map<std::string, std::weak_ptr<ConnectionSession>> locks_;
};
} // namespace folio::application::internal
