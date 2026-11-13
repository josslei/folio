#include "storage/catalog_coordinator.hpp"
#include "operations/catalog_maintenance.hpp"
namespace folio::core::storage {
namespace {
CoordinatorError error(CoordinatorErrorCode code, const std::filesystem::path& path,
                       std::string message) {
  return {code, path, std::move(message), {}, {}, {}, {}};
}
} // namespace
CatalogCoordinator::CatalogCoordinator(StoragePaths paths, MetadataCatalog& catalog)
    : paths_(std::move(paths)), catalog_(catalog),
      maintenance_(std::make_unique<application::internal::CatalogMaintenance>(paths_)),
      creation_check_(
          [this](const core::Id& id) -> ::folio::core::Result<std::monostate, StorageError> {
            std::lock_guard lock(publication_mutex_);
            if (status_ != CatalogStatus::Ready)
              return StorageError{StorageErrorCode::CatalogUnavailable,
                                  paths_.normal_root,
                                  "Creation requires an initialized, consistent Catalog",
                                  {},
                                  false};
            if (catalog_.read_view().contains_entry(id))
              return StorageError{StorageErrorCode::DuplicateId,
                                  paths_.normal_root,
                                  "Entry UUID already exists in Catalog",
                                  {},
                                  false};
            return std::monostate{};
          }),
      log_creation_callback_(wrap_catalog_update<LogMetadata>(
          [this](const LogMetadata& metadata) { return catalog_.insert_record(metadata); })),
      log_update_callback_(wrap_catalog_update<LogMetadata>(
          [this](const LogMetadata& metadata) { return catalog_.update_record(metadata); })),
      memo_creation_callback_(wrap_catalog_update<MemoMetadata>(
          [this](const MemoMetadata& metadata) { return catalog_.insert_record(metadata); })),
      memo_update_callback_(wrap_catalog_update<MemoMetadata>(
          [this](const MemoMetadata& metadata) { return catalog_.update_record(metadata); })),
      todo_creation_callback_(wrap_catalog_update<TodoMetadata>(
          [this](const TodoMetadata& metadata) { return catalog_.insert_record(metadata); })),
      todo_update_callback_(wrap_catalog_update<TodoMetadata>(
          [this](const TodoMetadata& metadata) { return catalog_.update_record(metadata); })),
      directory_creation_callback_(
          wrap_catalog_update<DirectoryMetadata>([this](const DirectoryMetadata& metadata) {
            return catalog_.insert_directory(metadata);
          })),
      directory_update_callback_(
          wrap_catalog_update<DirectoryMetadata>([this](const DirectoryMetadata& metadata) {
            return catalog_.update_directory(metadata);
          })) {}

CatalogCoordinator::~CatalogCoordinator() = default;
application::internal::CatalogMaintenance& CatalogCoordinator::maintenance() noexcept {
  return *maintenance_;
}

::folio::core::Result<InitializationOutcome, CoordinatorError> CatalogCoordinator::initialize() {
  if (load_attempted_)
    return error(CoordinatorErrorCode::Lifecycle, {}, "Catalog initialization already attempted");
  load_attempted_ = true;
  status_ = CatalogStatus::Unavailable;
  auto prepared = maintenance_->prepare_initialization();
  if (auto* problem = std::get_if<CoordinatorError>(&prepared)) return *problem;
  auto candidate = std::get<application::internal::CatalogMaintenance::PreparedInitialization>(
      std::move(prepared));
  if (candidate.outcome.previous_session_incomplete || candidate.outcome.index_error) {
    auto published = publish_full_refresh(std::move(candidate.catalog));
    if (auto* problem = std::get_if<CoordinatorError>(&published)) return *problem;
  } else {
    auto marker_ready = maintenance_->establish_marker();
    if (auto* problem = std::get_if<CoordinatorError>(&marker_ready)) return *problem;
    session_active_ = true;
    catalog_ = std::move(candidate.catalog);
    status_ = CatalogStatus::Ready;
  }
  return std::move(candidate.outcome);
}

::folio::core::Result<std::vector<ExcludedObject>, CoordinatorError>
CatalogCoordinator::refresh_all() {
  if (status_ == CatalogStatus::Stopped)
    return error(CoordinatorErrorCode::Lifecycle, {}, "Session already finalized");
  auto built = maintenance_->reconstruct_full();
  if (auto* problem = std::get_if<CoordinatorError>(&built)) return *problem;
  auto candidate = std::get<CatalogCandidate>(std::move(built));
  auto published = publish_full_refresh(std::move(candidate.catalog));
  if (auto* problem = std::get_if<CoordinatorError>(&published)) return *problem;
  return std::move(candidate.diagnostics);
}

::folio::core::Result<std::monostate, CoordinatorError>
CatalogCoordinator::publish_full_refresh(MetadataCatalog candidate) {
  auto validated = maintenance_->validate_paths();
  if (auto* problem = std::get_if<CoordinatorError>(&validated)) return *problem;
  if (status_ == CatalogStatus::Stopped)
    return error(CoordinatorErrorCode::Lifecycle, {}, "Session already finalized");
  if (!session_active_) {
    auto marker_ready = maintenance_->establish_marker();
    if (auto* problem = std::get_if<CoordinatorError>(&marker_ready)) return *problem;
    session_active_ = true;
  }
  {
    std::lock_guard lock(publication_mutex_);
    const auto previous_status = status_.load();
    marker_cleanup_pending_ = false;
    catalog_ = std::move(candidate);
    status_ = CatalogStatus::Ready;
    load_attempted_ = true;
    notify_observer([] { return Change{std::monostate{}, false}; });
    if (previous_status != CatalogStatus::Ready)
      notify_observer([] { return Change{CatalogStatus::Ready, false}; });
  }
  return std::monostate{};
}

::folio::core::Result<std::monostate, CoordinatorError>
CatalogCoordinator::publish_scoped_refresh(MetadataCatalog candidate,
                                           std::function<void()> published) {
  // Preparation/scan run without this lock. Publication and status are indivisible
  // to Access captures; scheduling excludes other writers and lifecycle finalization.
  std::lock_guard lock(publication_mutex_);
  if (!session_active_ ||
      (status_ != CatalogStatus::Ready && status_ != CatalogStatus::Inconsistent))
    return error(CoordinatorErrorCode::Unavailable, {}, "Catalog session cannot publish Refresh");
  catalog_ = std::move(candidate);
  if (published) {
    try {
      published();
    } catch (...) {
      notification_failed();
    }
  } else
    notify_observer([] { return Change{std::monostate{}, false}; });
  return std::monostate{};
}

::folio::core::Result<SaveOutcome, CoordinatorError> CatalogCoordinator::shutdown_save() {
  if (status_ == CatalogStatus::Inconsistent)
    return error(CoordinatorErrorCode::Unavailable, {},
                 "Catalog requires successful Refresh before Index persistence");
  if (status_ != CatalogStatus::Ready)
    return error(CoordinatorErrorCode::Unavailable, {},
                 "No valid restored catalog is available to save");
  if (!catalog_.unpersisted()) return SaveOutcome::SkippedPersisted;
  auto written = maintenance_->save_index(catalog_);
  if (auto* problem = std::get_if<CoordinatorError>(&written)) return *problem;
  catalog_.mark_persisted();
  return SaveOutcome::Saved;
}

::folio::core::Result<SaveOutcome, CoordinatorError> CatalogCoordinator::finish_session() {
  // No once-only guard on saving: explicit retries remain possible.
  ::folio::core::Result<SaveOutcome, CoordinatorError> saved = SaveOutcome::SkippedPersisted;
  if (!marker_cleanup_pending_) saved = shutdown_save();
  if (auto* problem = std::get_if<CoordinatorError>(&saved)) return *problem;
  auto synced = maintenance_->sync_index();
  if (auto* problem = std::get_if<CoordinatorError>(&synced)) return *problem;
  auto marker = maintenance_->previous_marker();
  if (auto* problem = std::get_if<CoordinatorError>(&marker)) {
    session_active_ = false;
    status_ = CatalogStatus::Unavailable;
    return *problem;
  }
  auto removed = maintenance_->remove_marker();
  if (auto* problem = std::get_if<CoordinatorError>(&removed)) {
    // unlink may already have succeeded. The Index is safe, but disallow any
    // further managed work until a retry completes or Refresh reestablishes the
    // Marker. Preserve the original removal/sync diagnostic.
    marker_cleanup_pending_ = true;
    session_active_ = false;
    status_ = CatalogStatus::Unavailable;
    return *problem;
  }
  session_active_ = false;
  marker_cleanup_pending_ = false;
  status_ = CatalogStatus::Stopped;
  return std::get<SaveOutcome>(saved);
}

CatalogStatus CatalogCoordinator::status() const noexcept {
  return status_.load();
}
CatalogCoordinator::ReadState CatalogCoordinator::read_state() const {
  std::lock_guard lock(publication_mutex_);
  return {status_.load(), catalog_.read_view()};
}
void CatalogCoordinator::mark_needs_refresh() noexcept {
  if (status_ == CatalogStatus::Ready) {
    status_ = CatalogStatus::Inconsistent;
    notify_observer([] { return Change{CatalogStatus::Inconsistent, true}; });
  }
}

} // namespace folio::core::storage
