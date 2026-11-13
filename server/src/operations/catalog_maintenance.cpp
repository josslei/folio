#include "operations/catalog_maintenance.hpp"
#include "storage/catalog_coordinator.hpp"
#include <folio/logging/logging.hpp>
#include <tuple>
#include <set>
#include <system_error>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
namespace folio::application::internal {
using namespace ::folio::core::storage;
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
  fs::path state_root;
  fs::path parent;
  fs::path file;
};
::folio::core::Result<Locations, CoordinatorError> locations(const StoragePaths& paths) {
  auto checked = validate_storage_paths(paths);
  if (auto* problem = std::get_if<StorageError>(&checked)) return storage_error(*problem);
  if (auto* problem = std::get_if<io::FileError>(&checked)) return storage_error(*problem);
  std::error_code ec;
  const auto root = fs::weakly_canonical(paths.application.state, ec);
  if (ec) return io_error(paths.application.state, "Could not resolve state root", ec);
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
::folio::core::Result<bool, CoordinatorError> CatalogMaintenance::previous_marker() const {
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

::folio::core::Result<std::monostate, CoordinatorError>
CatalogMaintenance::establish_marker() const {
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
  return std::monostate{};
}

::folio::core::Result<MetadataCatalog, CoordinatorError> CatalogMaintenance::load_index() const {
  auto resolved = locations(paths_);
  if (auto* problem = std::get_if<CoordinatorError>(&resolved)) return *problem;
  const auto& path = std::get<Locations>(resolved);
  for (const auto& directory : {path.state_root, path.parent}) {
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
  return std::get<MetadataCatalog>(std::move(decoded));
}
::folio::core::Result<std::monostate, CoordinatorError>
CatalogMaintenance::save_index(const MetadataCatalog& catalog) const {
  auto encoded = index_codec::encode_index(catalog);
  if (auto* problem = std::get_if<index_codec::IndexCodecError>(&encoded))
    return codec_error(paths_.application.state / index_relative_path(), *problem, false);
  auto resolved = locations(paths_);
  if (auto* problem = std::get_if<CoordinatorError>(&resolved)) return *problem;
  const auto& path = std::get<Locations>(resolved);
  for (const auto& directory : {path.state_root, path.parent}) {
    auto found = directory_ready(directory);
    if (auto* problem = std::get_if<CoordinatorError>(&found)) return *problem;
  }
  auto parents = io::create_directories_durable(path.parent);
  if (auto* problem = std::get_if<io::FileError>(&parents)) return storage_error(*problem);
  for (const auto& directory : {path.state_root, path.parent}) {
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
  return std::monostate{};
}
::folio::core::Result<std::monostate, CoordinatorError> CatalogMaintenance::sync_index() const {
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
  return std::monostate{};
}
::folio::core::Result<std::monostate, CoordinatorError> CatalogMaintenance::remove_marker() const {
  auto removed = io::remove_file_durable(marker_path(paths_));
  if (auto* problem = std::get_if<io::FileError>(&removed)) return storage_error(*problem);
  return std::monostate{};
}
::folio::core::Result<std::monostate, CoordinatorError> CatalogMaintenance::validate_paths() const {
  auto checked = validate_storage_paths(paths_);
  if (auto* problem = std::get_if<StorageError>(&checked)) return storage_error(*problem);
  if (auto* problem = std::get_if<io::FileError>(&checked)) return storage_error(*problem);
  return std::monostate{};
}
::folio::core::Result<CatalogCandidate, CoordinatorError>
CatalogMaintenance::reconstruct_full() const {
  auto valid = validate_paths();
  if (auto* problem = std::get_if<CoordinatorError>(&valid)) return *problem;
  auto scanned = repository_.scan();
  if (auto* problem = std::get_if<StorageError>(&scanned))
    return CoordinatorError{CoordinatorErrorCode::StorageFailure,
                            problem->path,
                            problem->message,
                            *problem,
                            {},
                            {},
                            {}};
  if (auto* problem = std::get_if<folio::io::FileError>(&scanned)) {
    const auto code =
        problem->code == folio::io::FileErrorCode::InvalidPath ? CoordinatorErrorCode::InvalidPath
        : problem->code == folio::io::FileErrorCode::IoError ? CoordinatorErrorCode::IoError
                                                             : CoordinatorErrorCode::StorageFailure;
    return CoordinatorError{code, problem->path, problem->message, {}, {}, {}, *problem};
  }
  auto inventory = std::get<NormalRepositoryScanResult>(std::move(scanned));
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
  if (auto* problem = std::get_if<StorageError>(&adopted))
    return CoordinatorError{CoordinatorErrorCode::StorageFailure,
                            problem->path,
                            problem->message,
                            *problem,
                            {},
                            {},
                            {}};
  return CatalogCandidate{std::move(candidate), std::move(inventory.diagnostics)};
}

} // namespace folio::application::internal

namespace folio::application::internal {
using namespace ::folio::core::storage;
using namespace ::folio::application;
using namespace ::folio::application::internal;
namespace storage = core::storage;
namespace {
constexpr std::size_t root_overhead = 0;
constexpr std::size_t directory_overhead = 1; // .folio-dir.toml
ApplicationError error(ErrorReason reason, const char* message) {
  return {ErrorCategory::AuthoritativeDataProblem,
          reason,
          message,
          {ErrorAction::Refresh, ErrorAction::Dismiss}};
}
ApplicationError unavailable() {
  return {ErrorCategory::ApplicationUnavailable,
          ErrorReason::MetadataUnavailable,
          "Application metadata unavailable",
          {ErrorAction::Dismiss}};
}
ApplicationError full_required() {
  return error(ErrorReason::FullRefreshRequired,
               "Reconciliation requires an explicit Full Refresh");
}
ApplicationError scan_error(const storage::StorageError& value) {
  try {
    SPDLOG_WARN("Catalog maintenance failed (StorageError code={})", static_cast<int>(value.code));
  } catch (...) {
    // Diagnostics must not replace the operation's structured failure.
  }
  return error(value.code == storage::StorageErrorCode::DuplicateId ? ErrorReason::IdentityConflict
                                                                    : ErrorReason::InvalidData,
               "Authoritative metadata could not be reconciled");
}
ApplicationError scan_error(const io::FileError& value) {
  try {
    SPDLOG_WARN("Catalog maintenance failed (FileError code={})", static_cast<int>(value.code));
  } catch (...) {
    // Diagnostics must not replace the operation's structured failure.
  }
  if (value.code == io::FileErrorCode::InvalidPath)
    return {ErrorCategory::AuthoritativeDataProblem,
            ErrorReason::UnsafePath,
            "The selected Repository scope has an unsafe layout",
            {ErrorAction::ResolveExternally, ErrorAction::Dismiss}};
  if (value.system_error == std::errc::permission_denied ||
      value.system_error == std::errc::operation_not_permitted)
    return {ErrorCategory::FilesystemFailure,
            ErrorReason::AccessDenied,
            "Access to the selected Repository scope was denied",
            {ErrorAction::ResolveExternally, ErrorAction::Retry, ErrorAction::Dismiss}};
  return {ErrorCategory::FilesystemFailure,
          ErrorReason::IoFailure,
          "Unable to read the selected Repository scope",
          {ErrorAction::Retry, ErrorAction::Dismiss}};
}
using ResolvedDirectory = std::tuple<core::RecordType, std::optional<core::Id>>;
core::Result<ResolvedDirectory, ApplicationError>
resolve(const DirectoryRef& scope, const storage::MetadataCatalogView& view) {
  if (auto* type = std::get_if<core::RecordType>(&scope)) {
    if (static_cast<unsigned>(*type) <= 2) return ResolvedDirectory{*type, {}};
  } else {
    const auto& id = std::get<core::Id>(scope);
    if (auto dir = view.find_directory(id))
      return ResolvedDirectory{dir->metadata.kind == core::DirectoryKind::Memo
                                   ? core::RecordType::Memo
                                   : core::RecordType::Todo,
                               id};
    return ApplicationError{ErrorCategory::TargetUnavailable,
                            ErrorReason::NotFound,
                            "Directory not found",
                            {ErrorAction::ReloadView, ErrorAction::Dismiss}};
  }
  return ApplicationError{ErrorCategory::InvalidRequest,
                          ErrorReason::InvalidQuery,
                          "Invalid logical scope",
                          {ErrorAction::Dismiss}};
}
} // namespace

OperationResponse CatalogMaintenance::check(CatalogCoordinator& coordinator,
                                            const DirectoryRef& scope) {
  const auto key = std::visit(
      [](const auto& value) -> std::string {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, core::RecordType>)
          return "root:" + std::to_string(static_cast<unsigned>(value));
        else
          return "directory:" + value.to_string();
      },
      scope);
  auto [entry, first] = checked_.try_emplace(key);
  auto& outcome = entry->second;
  if (first) {
    try {
      outcome.result = perform_check(coordinator, scope);
    } catch (...) {
      outcome.exception = std::current_exception();
    }
  }
  if (outcome.exception) std::rethrow_exception(outcome.exception);
  return *outcome.result;
}

OperationResponse CatalogMaintenance::perform_check(CatalogCoordinator& coordinator,
                                                    const DirectoryRef& scope) const {
  const auto state = coordinator.read_state();
  if (state.status != storage::CatalogStatus::Ready &&
      state.status != storage::CatalogStatus::Inconsistent)
    return unavailable();
  auto selection = resolve(scope, state.view);
  if (auto* failure = std::get_if<ApplicationError>(&selection)) return *failure;
  const auto [type, directory] = std::get<ResolvedDirectory>(selection);
  auto expected = state.view.record_count(type, directory);
  if (directory)
    expected += directory_overhead;
  else {
    expected += root_overhead;
    if (type != core::RecordType::Log)
      expected += state.view.directory_count(
          type == core::RecordType::Memo ? core::DirectoryKind::Memo : core::DirectoryKind::Todo);
  }
  auto count = repository_.count_entries(type, directory);
  if (auto* failure = std::get_if<storage::StorageError>(&count)) return scan_error(*failure);
  if (auto* failure = std::get_if<io::FileError>(&count)) return scan_error(*failure);
  return std::get<std::size_t>(count) == expected ? ConsistencyCheckResult::NoAnomalyDetected
                                                  : ConsistencyCheckResult::AnomalyDetected;
}

OperationResponse CatalogMaintenance::refresh(CatalogCoordinator& coordinator,
                                              const RefreshScope& scope) const {
  if (std::holds_alternative<RepositoryScope>(scope)) {
    auto result = coordinator.refresh_all();
    if (auto* failure = std::get_if<storage::CoordinatorError>(&result)) {
      if (failure->storage_error) return scan_error(*failure->storage_error);
      if (failure->file_error) return scan_error(*failure->file_error);
      if (failure->code == storage::CoordinatorErrorCode::Lifecycle ||
          failure->code == storage::CoordinatorErrorCode::Unavailable)
        return unavailable();
      return error(ErrorReason::InvalidData, "Full Refresh could not rebuild Catalog");
    }
    return std::monostate{};
  }
  const auto& directory_scope = std::get<DirectoryScanScope>(scope);
  if (static_cast<unsigned>(directory_scope.traversal) > 1)
    return ApplicationError{ErrorCategory::InvalidRequest,
                            ErrorReason::InvalidQuery,
                            "Invalid Refresh traversal",
                            {ErrorAction::Dismiss}};
  const auto state = coordinator.read_state();
  if (state.status != storage::CatalogStatus::Ready &&
      state.status != storage::CatalogStatus::Inconsistent)
    return unavailable();
  auto selection = resolve(directory_scope.directory, state.view);
  if (auto* failure = std::get_if<ApplicationError>(&selection)) return *failure;
  const auto [type, directory] = std::get<ResolvedDirectory>(selection);
  const bool recursive = directory_scope.traversal == RefreshTraversal::Recursive;
  auto scanned = repository_.scan(type, directory, recursive);
  if (auto* failure = std::get_if<storage::StorageError>(&scanned)) return scan_error(*failure);
  if (auto* failure = std::get_if<io::FileError>(&scanned)) return scan_error(*failure);
  auto inventory = std::get<storage::NormalRepositoryScanResult>(std::move(scanned));
  std::vector<storage::LogMetadata> logs;
  std::vector<storage::MemoMetadata> memos;
  std::vector<storage::TodoMetadata> todos;
  std::vector<storage::DirectoryMetadata> directories;
  std::set<std::string> outside;
  auto keep_record = [&](const auto& value) {
    using T = std::decay_t<decltype(value)>;
    if constexpr (std::is_same_v<T, storage::LogMetadata>)
      logs.push_back(value);
    else if constexpr (std::is_same_v<T, storage::MemoMetadata>)
      memos.push_back(value);
    else
      todos.push_back(value);
    outside.insert(value.metadata.id.to_string());
  };
  for (const auto& record : state.view.query()) {
    std::visit(
        [&](const auto& value) {
          bool replace = storage::record_type(record) == type;
          if constexpr (!std::is_same_v<std::decay_t<decltype(value)>, storage::LogMetadata>) {
            if (directory)
              replace = replace && value.metadata.directory_id == directory;
            else if (!recursive)
              replace = replace && !value.metadata.directory_id;
          }
          if (!replace) keep_record(value);
        },
        record);
  }
  std::set<std::string> scanned_directories;
  for (const auto& dir : inventory.directories)
    scanned_directories.insert(dir.metadata.id.to_string());
  for (auto kind : {core::DirectoryKind::Memo, core::DirectoryKind::Todo}) {
    for (const auto& dir : state.view.list_directories(kind)) {
      const auto directory_type =
          kind == core::DirectoryKind::Memo ? core::RecordType::Memo : core::RecordType::Todo;
      // A Directory scan reconciles members only; its metadata remains outside.
      if (directory || directory_type != type) {
        directories.push_back(dir);
        outside.insert(dir.metadata.id.to_string());
      } else if (!recursive && !scanned_directories.contains(dir.metadata.id.to_string())) {
        if (state.view.record_count(directory_type, dir.metadata.id) != 0) return full_required();
      }
    }
  }
  auto append = [&](auto& target, auto& source) -> bool {
    for (auto& value : source) {
      if (outside.contains(value.metadata.id.to_string())) return false;
      target.push_back(std::move(value));
    }
    return true;
  };
  if (!append(logs, inventory.logs) || !append(memos, inventory.memos) ||
      !append(todos, inventory.todos))
    return full_required();
  if (!directory && !append(directories, inventory.directories)) return full_required();
  storage::MetadataCatalog candidate;
  auto built = candidate.rebuild(std::move(logs), std::move(memos), std::move(todos),
                                 std::move(directories));
  if (auto* failure = std::get_if<storage::StorageError>(&built)) return scan_error(*failure);
  auto published = coordinator.publish_scoped_refresh(std::move(candidate), [&] {
    if (refreshed_) refreshed_(scope);
  });
  if (std::holds_alternative<storage::CoordinatorError>(published)) return unavailable();
  return std::monostate{};
}
} // namespace folio::application::internal
