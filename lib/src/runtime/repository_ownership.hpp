#pragma once

#include "runtime/process.hpp"
#include "storage/filesystem.hpp"

#include <memory>

namespace folio::application::internal {

// Exclusive access to the configured Repositories, acquired by the Backend
// before Catalog lifecycle access. Repository denotes the persistent storage
// location; NormalRepository/VaultRepository are its management classes. Move-only RAII
// releases handles, never saves an Index, finalizes a session or deletes locks.
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
  // acquisition releases all handles. Permission failure is not AlreadyOwned.
  [[nodiscard]] static core::internal::Result<RepositoryOwnership, ProcessError>
  acquire(const core::storage::StoragePaths& paths);

 private:
  struct Handles;
  explicit RepositoryOwnership(std::unique_ptr<Handles> handles);
  std::unique_ptr<Handles> handles_;
};

}  // namespace folio::application::internal
