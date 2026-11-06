#pragma once

#include <folio/application/process_error.hpp>
#include <folio/ipc/ipc.hpp>
#include "storage/common.hpp"

#include <memory>

namespace folio::application::internal {

// Exclusive access to the configured Repositories, acquired by the Backend
// before Catalog lifecycle access. Repository denotes the persistent storage
// location; NormalRepository/VaultRepository are its management classes. Move-only RAII
// releases the endpoint before lock handles, never saves an Index, finalizes
// a session or deletes lock targets.
// Callers must not unlink/replace lock files or roots during ownership.
class RepositoryOwnership {
 public:
  ~RepositoryOwnership();
  RepositoryOwnership(RepositoryOwnership&&) noexcept;
  RepositoryOwnership& operator=(RepositoryOwnership&&) noexcept;
  RepositoryOwnership(const RepositoryOwnership&) = delete;
  RepositoryOwnership& operator=(const RepositoryOwnership&) = delete;

  // Locks State/backend.lock and Normal/.folio-backend.lock; locks an existing
  // Vault root too, but never creates Vault. Missing Normal/State parents may be
  // created. Root aliases resolve to the same physical lock file. Partial
  // acquisition releases all resources. After locking, establishes the IPC
  // listener and reclaims only a stale endpoint. Permission failure is not
  // AlreadyOwned.
  [[nodiscard]] static core::Result<RepositoryOwnership, ProcessError>
  acquire(const core::storage::StoragePaths& paths);

  // Nonblocking connection acceptance while the ownership resource is active.
  core::Result<std::optional<IpcStream>, IpcError> accept();
  core::Result<bool, IpcError> wait(
      std::chrono::steady_clock::time_point deadline, std::stop_token wake = {});
  // Stop admission and remove the owned endpoint, retaining all Repo locks
  // through explicit Catalog finalization. Safe to call more than once.
  void stop_listening() noexcept;

 private:
  struct Handles;
  explicit RepositoryOwnership(std::unique_ptr<Handles> handles);
  std::unique_ptr<Handles> handles_;
};

}  // namespace folio::application::internal
