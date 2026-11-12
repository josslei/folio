#include <tuple>
#include "operations/catalog_maintenance.hpp"
#include <folio/logging/logging.hpp>
#include <set>
#include <type_traits>

namespace folio::application::internal {
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

OperationResponse CatalogMaintenance::check(const DirectoryRef& scope) {
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
      outcome.result = perform_check(scope);
    } catch (...) {
      outcome.exception = std::current_exception();
    }
  }
  if (outcome.exception) std::rethrow_exception(outcome.exception);
  return *outcome.result;
}

OperationResponse CatalogMaintenance::perform_check(const DirectoryRef& scope) const {
  const auto state = coordinator_.read_state();
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

OperationResponse CatalogMaintenance::refresh(const RefreshScope& scope) const {
  if (std::holds_alternative<RepositoryScope>(scope)) {
    auto result = coordinator_.refresh_all();
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
  const auto state = coordinator_.read_state();
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
  auto published = coordinator_.publish_scoped_refresh(std::move(candidate), [&] {
    if (refreshed_) refreshed_(scope);
  });
  if (std::holds_alternative<storage::CoordinatorError>(published)) return unavailable();
  return std::monostate{};
}
} // namespace folio::application::internal
