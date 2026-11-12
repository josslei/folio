#pragma once

#include <folio/ipc/application_protocol.hpp>
#include "storage/catalog_coordinator.hpp"
#include <map>
#include <exception>

namespace folio::application::internal {

/// Executes maintenance on a reserved Access worker. Borrows session Storage;
/// caller excludes Mutations through completion, but concurrent Access may retain
/// old views. Never repairs Entries, retries requests or writes the Index.
class CatalogMaintenance {
public:
  CatalogMaintenance(core::storage::NormalRepository& repository,
                     core::storage::CatalogCoordinator& coordinator)
      : repository_(repository), coordinator_(coordinator) {}
  /// Once per scope per session; later calls return the historical outcome.
  OperationResponse check(const DirectoryRef& scope);
  OperationResponse refresh(const RefreshScope& scope) const;

  /// Optional Application sink, called inside the scoped publication boundary.
  void set_refresh_observer(std::function<void(const RefreshScope&)> observer) {
    refreshed_ = std::move(observer);
  }

private:
  std::function<void(const RefreshScope&)> refreshed_;
  OperationResponse perform_check(const DirectoryRef& scope) const;
  struct CheckOutcome {
    std::optional<OperationResponse> result;
    std::exception_ptr exception;
  };
  // Check executions are serialized by maintenance reservations, not a mutex.
  // Retained until this Backend session ends, including unsuccessful attempts.
  std::map<std::string, CheckOutcome> checked_;
  core::storage::NormalRepository& repository_;
  core::storage::CatalogCoordinator& coordinator_;
};

} // namespace folio::application::internal
