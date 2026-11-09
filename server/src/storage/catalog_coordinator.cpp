#include "storage/catalog_coordinator.hpp"

#include <system_error>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace folio::core::storage {
namespace {
namespace fs = std::filesystem;
constexpr std::size_t kMaxIndexBytes = 64u * 1024u * 1024u; // IndexCodec v1 limit.
fs::path marker_path(const StoragePaths& paths) {
  return paths.application.state / "session.marker";
}
fs::path index_relative_path() {
  return fs::path{"index"} / "normal.msgpack";
}

bool contains_path(const fs::path& parent, const fs::path& child) {
  auto p = parent.begin();
  auto c = child.begin();
  for (; p != parent.end(); ++p, ++c) {
    if (c == child.end()) return false;
#ifdef _WIN32
    if (CompareStringOrdinal(p->c_str(), -1, c->c_str(), -1, TRUE) != CSTR_EQUAL) return false;
#else
    if (*p != *c) return false;
#endif
  }
  return true;
}

CoordinatorError error(CoordinatorErrorCode code, const fs::path& path, std::string message) {
  return {code, path, std::move(message), std::nullopt, std::nullopt, std::nullopt};
}
CoordinatorError storage_error(StorageError source) {
  return {CoordinatorErrorCode::StorageFailure,
          source.path,
          source.message,
          std::move(source),
          std::nullopt,
          std::nullopt,
          std::nullopt};
}
CoordinatorError storage_error(io::FileError source) {
  const auto code =
      source.code == io::FileErrorCode::InvalidPath
          ? CoordinatorErrorCode::InvalidPath
          : (source.code == io::FileErrorCode::IoError ? CoordinatorErrorCode::IoError
                                                       : CoordinatorErrorCode::StorageFailure);
  return {code,         source.path,  source.message,   std::nullopt,
          std::nullopt, std::nullopt, std::move(source)};
}
CoordinatorError io_error(const fs::path& path, std::string message, std::error_code ec) {
  return storage_error(
      io::FileError{io::FileErrorCode::IoError, path, std::move(message), ec, false});
}
CoordinatorError codec_error(const fs::path& path, index_codec::IndexCodecError source,
                             bool loading) {
  CoordinatorErrorCode code;
  switch (source.code) {
  case index_codec::IndexCodecErrorCode::UnsupportedVersion:
    code = CoordinatorErrorCode::UnsupportedVersion;
    break;
  case index_codec::IndexCodecErrorCode::LimitExceeded:
    code = CoordinatorErrorCode::LimitExceeded;
    break;
  case index_codec::IndexCodecErrorCode::Malformed:
  case index_codec::IndexCodecErrorCode::InvalidMetadata:
    code = loading ? CoordinatorErrorCode::Corrupt : CoordinatorErrorCode::CodecError;
    break;
  }
  return {code, path, source.message, std::nullopt, std::move(source), std::nullopt};
}
CoordinatorError binary_error(io::ReadError source) {
  const auto code = source.code == io::ReadErrorCode::LimitExceeded
                        ? CoordinatorErrorCode::LimitExceeded
                        : CoordinatorErrorCode::IoError;
  return {code, source.path, source.message, std::nullopt, std::nullopt, std::move(source)};
}

struct Locations {
  fs::path cache_root;
  fs::path parent;
  fs::path file;
};
::folio::core::Result<Locations, CoordinatorError> locations(const StoragePaths& paths) {
  auto checked = validate_storage_paths(paths);
  if (auto* problem = std::get_if<StorageError>(&checked)) return storage_error(*problem);
  if (auto* problem = std::get_if<io::FileError>(&checked)) return storage_error(*problem);
  std::error_code ec;
  const auto root = fs::weakly_canonical(paths.application.cache, ec);
  if (ec) return io_error(paths.application.cache, "Could not resolve cache root", ec);
  const auto relative = index_relative_path();
  return Locations{root, root / relative.parent_path(), root / relative};
}
::folio::core::Result<fs::file_status, CoordinatorError> inspect(const fs::path& path) {
  std::error_code ec;
  const auto status = fs::symlink_status(path, ec);
  if (ec == std::errc::no_such_file_or_directory) return fs::file_status{fs::file_type::not_found};
  if (ec) return io_error(path, "Could not inspect lifecycle path", ec);
  if (fs::is_symlink(status))
    return error(CoordinatorErrorCode::InvalidPath, path, "Symlink in lifecycle path");
  return status;
}
::folio::core::Result<bool, CoordinatorError> directory_ready(const fs::path& path) {
  auto inspected = inspect(path);
  if (auto* problem = std::get_if<CoordinatorError>(&inspected)) return *problem;
  const auto status = std::get<fs::file_status>(inspected);
  if (status.type() == fs::file_type::not_found) return false;
  if (!fs::is_directory(status))
    return error(CoordinatorErrorCode::InvalidPath, path, "Lifecycle parent is not a directory");
  return true;
}
::folio::core::Result<bool, CoordinatorError> file_ready(const fs::path& path) {
  auto inspected = inspect(path);
  if (auto* problem = std::get_if<CoordinatorError>(&inspected)) return *problem;
  const auto status = std::get<fs::file_status>(inspected);
  if (status.type() == fs::file_type::not_found) return false;
  if (!fs::is_regular_file(status))
    return error(CoordinatorErrorCode::InvalidPath, path, "Lifecycle target is not a regular file");
  return true;
}
} // namespace

// Marker location must survive independent Cache/Runtime deletion. Application
// categories can otherwise share parents; only this durable file is constrained.
::folio::core::Result<bool, CoordinatorError> CatalogCoordinator::previous_marker() const {
  auto validated = validate_storage_paths(paths_);
  if (auto* problem = std::get_if<StorageError>(&validated)) return storage_error(*problem);
  if (auto* problem = std::get_if<io::FileError>(&validated)) return storage_error(*problem);
  std::error_code ec;
  const auto marker = fs::weakly_canonical(marker_path(paths_), ec);
  if (ec) return io_error(marker_path(paths_), "Could not resolve Session Marker", ec);
  for (const auto& transient :
       {std::optional<fs::path>{paths_.application.cache}, paths_.application.runtime}) {
    if (!transient) continue;
    const auto root = fs::weakly_canonical(*transient, ec);
    if (ec) return io_error(*transient, "Could not resolve transient directory", ec);
    if (contains_path(root, marker))
      return error(CoordinatorErrorCode::InvalidPath, marker,
                   "Session Marker must be outside Cache and Runtime");
  }
  // Inspect the original path, not its canonical alias: reject symlinks rather
  // than following a Marker or State-directory link.
  auto current = paths_.application.state.root_path();
  for (const auto& component : paths_.application.state.relative_path()) {
    current /= component;
    auto found = directory_ready(current);
    if (auto* problem = std::get_if<CoordinatorError>(&found)) return *problem;
    if (!std::get<bool>(found)) return false;
  }
  return file_ready(marker_path(paths_));
}

::folio::core::Result<std::monostate, CoordinatorError> CatalogCoordinator::establish_marker() {
  auto present = previous_marker();
  if (auto* problem = std::get_if<CoordinatorError>(&present)) return *problem;
  auto parents = io::create_directories_durable(paths_.application.state);
  if (auto* problem = std::get_if<io::FileError>(&parents)) return storage_error(*problem);
  const auto marker = marker_path(paths_);
  // Never remove/recreate an existing Marker during startup. Sync it and its
  // ancestry, including when a previous process failed during publication sync.
  auto durable = std::get<bool>(present) ? io::sync_regular_file(marker)
                                         : io::create_empty_file_durable(marker);
  if (auto* problem = std::get_if<io::FileError>(&durable)) return storage_error(*problem);
  session_active_ = true;
  return std::monostate{};
}

CatalogCoordinator::CatalogCoordinator(StoragePaths paths, MetadataCatalog& catalog)
    : paths_(std::move(paths)), catalog_(catalog),
      creation_check_(
          [this](const core::Id& id) -> ::folio::core::Result<std::monostate, StorageError> {
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

::folio::core::Result<InitializationOutcome, CoordinatorError> CatalogCoordinator::startup_load() {
  if (load_attempted_)
    return error(CoordinatorErrorCode::Lifecycle, {}, "Startup load already attempted");
  load_attempted_ = true;
  status_ = CatalogStatus::Unavailable;
  auto marker = previous_marker();
  if (auto* problem = std::get_if<CoordinatorError>(&marker)) return *problem;
  if (std::get<bool>(marker)) {
    auto rebuilt = rebuild();
    if (auto* problem = std::get_if<CoordinatorError>(&rebuilt)) return *problem;
    return InitializationOutcome{std::nullopt,
                                 std::get<std::vector<ExcludedObject>>(std::move(rebuilt)), true};
  }
  // Keep cache-read errors distinct so only unusable caches trigger a rebuild.
  auto load_index = [&]() -> ::folio::core::Result<std::monostate, CoordinatorError> {
    auto resolved = locations(paths_);
    if (auto* problem = std::get_if<CoordinatorError>(&resolved)) return *problem;
    const auto& path = std::get<Locations>(resolved);
    for (const auto& directory : {path.cache_root, path.parent}) {
      auto ready = directory_ready(directory);
      if (auto* problem = std::get_if<CoordinatorError>(&ready)) return *problem;
      if (!std::get<bool>(ready))
        return error(CoordinatorErrorCode::Missing, path.file, "Normal Index is missing");
    }
    auto ready = file_ready(path.file);
    if (auto* problem = std::get_if<CoordinatorError>(&ready)) return *problem;
    if (!std::get<bool>(ready))
      return error(CoordinatorErrorCode::Missing, path.file, "Normal Index is missing");
    auto bytes = io::read_file(path.file, kMaxIndexBytes);
    if (auto* problem = std::get_if<io::ReadError>(&bytes)) return binary_error(*problem);
    auto decoded = index_codec::decode_index(std::get<std::string>(bytes));
    if (auto* problem = std::get_if<index_codec::IndexCodecError>(&decoded))
      return codec_error(path.file, *problem, true);
    auto marker_ready = establish_marker();
    if (auto* problem = std::get_if<CoordinatorError>(&marker_ready)) return *problem;
    catalog_ = std::get<MetadataCatalog>(std::move(decoded));
    status_ = CatalogStatus::Ready;
    return std::monostate{};
  };
  auto loaded = load_index();
  if (const auto* problem = std::get_if<CoordinatorError>(&loaded)) {
    switch (problem->code) {
    case CoordinatorErrorCode::Missing:
    case CoordinatorErrorCode::Corrupt:
    case CoordinatorErrorCode::UnsupportedVersion:
    case CoordinatorErrorCode::LimitExceeded:
      break;
    default:
      return *problem;
    }
    CoordinatorError index_error = *problem;
    auto rebuilt = rebuild();
    if (const auto* failure = std::get_if<CoordinatorError>(&rebuilt)) return *failure;
    return InitializationOutcome{std::move(index_error),
                                 std::get<std::vector<ExcludedObject>>(std::move(rebuilt))};
  }
  return InitializationOutcome{};
}

::folio::core::Result<std::vector<ExcludedObject>, CoordinatorError> CatalogCoordinator::rebuild() {
  auto paths = validate_storage_paths(paths_);
  if (auto* problem = std::get_if<StorageError>(&paths)) return storage_error(*problem);
  if (auto* problem = std::get_if<io::FileError>(&paths)) return storage_error(*problem);
  NormalRepository repository(paths_.normal_root);
  auto scanned = repository.scan();
  if (auto* problem = std::get_if<StorageError>(&scanned)) return storage_error(*problem);
  if (auto* problem = std::get_if<io::FileError>(&scanned)) return storage_error(*problem);
  auto inventory = std::get<NormalRepositoryScanResult>(std::move(scanned));
  // Allocate each typed vector once and move records without combining types.
  auto to_vector = []<class T>(std::deque<T>& records) {
    std::vector<T> result;
    result.reserve(records.size());
    for (auto& record : records)
      result.push_back(std::move(record));
    return result;
  };
  MetadataCatalog candidate;
  auto adopted = candidate.rebuild(to_vector(inventory.logs), to_vector(inventory.memos),
                                   to_vector(inventory.todos), to_vector(inventory.directories));
  if (auto* problem = std::get_if<StorageError>(&adopted)) return storage_error(*problem);
  if (!session_active_) {
    auto marker_ready = establish_marker();
    if (auto* problem = std::get_if<CoordinatorError>(&marker_ready)) return *problem;
  }
  marker_cleanup_pending_ = false;
  catalog_ = std::move(candidate);
  status_ = CatalogStatus::Ready;
  load_attempted_ = true;
  return std::move(inventory.diagnostics);
}

::folio::core::Result<std::vector<ExcludedObject>, CoordinatorError>
CatalogCoordinator::refresh_all() {
  if (status_ == CatalogStatus::Stopped)
    return error(CoordinatorErrorCode::Lifecycle, {}, "Session already finalized");
  load_attempted_ = true;
  return rebuild();
}

::folio::core::Result<SaveOutcome, CoordinatorError> CatalogCoordinator::shutdown_save() {
  if (status_ == CatalogStatus::Inconsistent)
    return error(CoordinatorErrorCode::Unavailable, {},
                 "Catalog requires successful Refresh before Index persistence");
  if (status_ != CatalogStatus::Ready)
    return error(CoordinatorErrorCode::Unavailable, {},
                 "No valid restored catalog is available to save");
  if (!catalog_.unpersisted()) return SaveOutcome::SkippedPersisted;
  auto encoded = index_codec::encode_index(catalog_);
  if (auto* problem = std::get_if<index_codec::IndexCodecError>(&encoded))
    return codec_error(paths_.application.cache / index_relative_path(), *problem, false);
  auto resolved = locations(paths_);
  if (auto* problem = std::get_if<CoordinatorError>(&resolved)) return *problem;
  const auto& path = std::get<Locations>(resolved);
  for (const auto& directory : {path.cache_root, path.parent}) {
    auto found = directory_ready(directory);
    if (auto* problem = std::get_if<CoordinatorError>(&found)) return *problem;
  }
  auto parents = io::create_directories_durable(path.parent);
  if (auto* problem = std::get_if<io::FileError>(&parents)) return storage_error(*problem);
  for (const auto& directory : {path.cache_root, path.parent}) {
    auto found = directory_ready(directory);
    if (auto* problem = std::get_if<CoordinatorError>(&found)) return *problem;
    if (!std::get<bool>(found))
      return error(CoordinatorErrorCode::IoError, directory,
                   "Index parent is missing after creation");
  }
  auto found = file_ready(path.file);
  if (auto* problem = std::get_if<CoordinatorError>(&found)) return *problem;
  const auto& bytes = std::get<std::string>(encoded);
  auto written = std::get<bool>(found) ? io::replace_file_atomic(path.file, bytes)
                                       : io::create_file_atomic(path.file, bytes);
  if (auto* problem = std::get_if<io::FileError>(&written)) return storage_error(*problem);
  catalog_.mark_persisted();
  return SaveOutcome::Saved;
}

::folio::core::Result<SaveOutcome, CoordinatorError> CatalogCoordinator::finish_session() {
  // No once-only guard on saving: explicit retries remain possible.
  ::folio::core::Result<SaveOutcome, CoordinatorError> saved = SaveOutcome::SkippedPersisted;
  if (!marker_cleanup_pending_) saved = shutdown_save();
  if (auto* problem = std::get_if<CoordinatorError>(&saved)) return *problem;
  auto resolved = locations(paths_);
  if (auto* problem = std::get_if<CoordinatorError>(&resolved)) return *problem;
  const auto& path = std::get<Locations>(resolved);
  auto exists = file_ready(path.file);
  if (auto* problem = std::get_if<CoordinatorError>(&exists)) return *problem;
  if (!std::get<bool>(exists))
    return error(CoordinatorErrorCode::Missing, path.file,
                 "Cannot finalize without a persisted Index");
  // Also establish durability for a loaded, unchanged Index before certifying
  // clean shutdown, without rewriting it. This is shutdown-only work.
  auto parents = io::create_directories_durable(path.parent);
  if (auto* problem = std::get_if<io::FileError>(&parents)) return storage_error(*problem);
  auto synced = io::sync_regular_file(path.file);
  if (auto* problem = std::get_if<io::FileError>(&synced)) return storage_error(*problem);
  auto marker = previous_marker();
  if (auto* problem = std::get_if<CoordinatorError>(&marker)) {
    session_active_ = false;
    status_ = CatalogStatus::Unavailable;
    return *problem;
  }
  auto removed = io::remove_file_durable(marker_path(paths_));
  if (auto* problem = std::get_if<io::FileError>(&removed)) {
    // unlink may already have succeeded. The Index is safe, but disallow any
    // further managed work until a retry completes or Refresh reestablishes the
    // Marker. Preserve the original removal/sync diagnostic.
    marker_cleanup_pending_ = true;
    session_active_ = false;
    status_ = CatalogStatus::Unavailable;
    return storage_error(*problem);
  }
  session_active_ = false;
  marker_cleanup_pending_ = false;
  status_ = CatalogStatus::Stopped;
  return std::get<SaveOutcome>(saved);
}

CatalogStatus CatalogCoordinator::status() const noexcept {
  return status_;
}
void CatalogCoordinator::mark_needs_refresh() noexcept {
  if (status_ == CatalogStatus::Ready) status_ = CatalogStatus::Inconsistent;
}

} // namespace folio::core::storage
