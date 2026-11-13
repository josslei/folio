#pragma once
#include "storage/normal_repository.hpp"
#include "storage/catalog_coordinator.hpp"
#include "storage/index_codec.hpp"
#include <folio/io/filesystem.hpp>
#include <folio/ipc/application_protocol.hpp>
#include <map>
#include <exception>

namespace folio::application::internal {
/// One maintenance module for Check/Refresh, Index and Session Marker I/O.
/// Owns no active Catalog/readiness state; delegates managed publication to
/// Coordinator. Callers coordinate writers and lifecycle ordering.
class CatalogMaintenance {
public:
  explicit CatalogMaintenance(core::storage::StoragePaths paths)
      : paths_(std::move(paths)), repository_(paths_.normal_root) {}
  using CoordinatorError = core::storage::CoordinatorError;
  using CatalogCandidate = core::storage::CatalogCandidate;
  using CatalogCoordinator = core::storage::CatalogCoordinator;
  using MetadataCatalog = core::storage::MetadataCatalog;
  using OperationResponse = application::internal::OperationResponse;
  using DirectoryRef = application::DirectoryRef;
  using RefreshScope = application::RefreshScope;
  /// Startup preparation result; no active Catalog or session has been changed.
  struct PreparedInitialization {
    MetadataCatalog catalog;
    core::storage::InitializationOutcome outcome;
  };
  /// Inspect the previous Marker, load the Index when eligible, otherwise reconstruct.
  /// Returns an isolated candidate and diagnostics; does not establish a session or publish.
  ::folio::core::Result<PreparedInitialization, CoordinatorError> prepare_initialization() const;
  /// Check once per scope/session; caller holds the maintenance reservation.
  OperationResponse check(CatalogCoordinator& coordinator, const DirectoryRef& scope);
  /// Reconcile selected scope; Coordinator handles managed publication/readiness.
  OperationResponse refresh(CatalogCoordinator& coordinator, const RefreshScope& scope) const;
  /// Install/remove with execution drained. Sink enqueues only, never performs I/O.
  void set_refresh_observer(std::function<void(const RefreshScope&)> observer) {
    refreshed_ = std::move(observer);
  }
  ::folio::core::Result<bool, CoordinatorError> previous_marker() const;
  ::folio::core::Result<std::monostate, CoordinatorError> establish_marker() const;
  ::folio::core::Result<MetadataCatalog, CoordinatorError> load_index() const;
  ::folio::core::Result<std::monostate, CoordinatorError>
  save_index(const MetadataCatalog& catalog) const;
  ::folio::core::Result<std::monostate, CoordinatorError> sync_index() const;
  ::folio::core::Result<std::monostate, CoordinatorError> remove_marker() const;
  ::folio::core::Result<std::monostate, CoordinatorError> validate_paths() const;
  ::folio::core::Result<CatalogCandidate, CoordinatorError> reconstruct_full() const;

private:
  OperationResponse perform_check(CatalogCoordinator& coordinator, const DirectoryRef& scope) const;
  struct CheckOutcome {
    std::optional<OperationResponse> result;
    std::exception_ptr exception;
  };
  // Serialized by maintenance reservations; history lasts for this session.
  std::map<std::string, CheckOutcome> checked_;
  std::function<void(const RefreshScope&)> refreshed_;
  core::storage::StoragePaths paths_;
  core::storage::NormalRepository repository_;
};
} // namespace folio::application::internal
