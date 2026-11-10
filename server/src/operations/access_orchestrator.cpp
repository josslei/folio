#include "operations/access_orchestrator.hpp"
#include "storage/catalog_coordinator.hpp"
#include <folio/logging/logging.hpp>

#include <algorithm>
#include <type_traits>

namespace folio::application::internal {
namespace storage = core::storage;
namespace {

ApplicationError invalid_query(const char* message = "Invalid or incompatible Record query") {
  return {
      ErrorCategory::InvalidRequest, ErrorReason::InvalidQuery, message, {ErrorAction::Dismiss}};
}

ApplicationError missing_target(const char* message) {
  return {ErrorCategory::TargetUnavailable,
          ErrorReason::NotFound,
          message,
          {ErrorAction::ReloadView, ErrorAction::Dismiss}};
}

ApplicationError wrong_type() {
  return {ErrorCategory::InvalidRequest,
          ErrorReason::WrongRecordType,
          "Wrong Record type",
          {ErrorAction::Dismiss}};
}

core::RecordMetadata project(const storage::RecordMetadata& value) {
  return std::visit([](const auto& located) -> core::RecordMetadata { return located.metadata; },
                    value);
}

std::vector<core::DirectoryMetadata> list_directories(const storage::MetadataCatalogView& catalog,
                                                      core::DirectoryKind kind) {
  std::vector<core::DirectoryMetadata> result;
  for (const auto& directory : catalog.list_directories(kind)) {
    result.push_back(directory.metadata);
  }
  std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
    if (left.name != right.name) return left.name < right.name;
    return left.id.to_string() < right.id.to_string();
  });
  return result;
}

template <class T> bool reversed(const InclusiveRange<T>& range) {
  return range.from && range.through && *range.through < *range.from;
}

bool valid_due_bound(const std::optional<core::DueDate>& bound, std::optional<DueKind> kind) {
  if (!bound) return true;
  if (kind && static_cast<std::size_t>(*kind) != bound->index()) return false;
  return std::visit(
      [](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, core::DateContext>)
          return value.date.ok();
        else
          return true;
      },
      *bound);
}

bool reversed_due_range(const core::DueDate& from, const core::DueDate& through) {
  if (from.index() != through.index()) return true;
  return std::visit(
      [](const auto& left, const auto& right) {
        using L = std::decay_t<decltype(left)>;
        using R = std::decay_t<decltype(right)>;
        if constexpr (!std::is_same_v<L, R>)
          return false;
        else if constexpr (std::is_same_v<L, core::DateContext>)
          return right.date < left.date;
        else
          return right.timestamp < left.timestamp;
      },
      from, through);
}

std::optional<ApplicationError> validate_query(const RecordQuery& query) {
  const auto& filters = query.filters;
  const bool log_filters = filters.log_date.from || filters.log_date.through;
  const bool todo_filters = filters.todo_status || filters.todo_priority || filters.has_due_date ||
                            filters.due_kind || filters.due_from || filters.due_through;
  // Sort fields may be inapplicable to some Records: Catalog sorts missing keys
  // last. Only contradictory filters are invalid, not those valid order choices.
  if ((query.type && static_cast<unsigned>(*query.type) > 2) ||
      static_cast<unsigned>(query.order.sort) > 9 ||
      (filters.privacy && static_cast<unsigned>(*filters.privacy) > 2) ||
      (filters.todo_status && static_cast<unsigned>(*filters.todo_status) > 1) ||
      (filters.todo_priority && static_cast<unsigned>(*filters.todo_priority) > 3) ||
      (filters.due_kind && static_cast<unsigned>(*filters.due_kind) > 1)) {
    return invalid_query();
  }
  if ((log_filters && todo_filters) || (filters.tag && todo_filters) ||
      (log_filters && query.type && *query.type != core::RecordType::Log) ||
      (todo_filters && query.type && *query.type != core::RecordType::Todo) ||
      (filters.tag && query.type && *query.type == core::RecordType::Todo)) {
    return invalid_query();
  }
  if (reversed(filters.created) || reversed(filters.modified) || reversed(filters.log_date) ||
      (filters.log_date.from && !filters.log_date.from->ok()) ||
      (filters.log_date.through && !filters.log_date.through->ok()) ||
      !valid_due_bound(filters.due_from, filters.due_kind) ||
      !valid_due_bound(filters.due_through, filters.due_kind) ||
      (filters.due_from && filters.due_through &&
       reversed_due_range(*filters.due_from, *filters.due_through))) {
    return invalid_query("Invalid Record query range");
  }
  if (filters.has_due_date && !*filters.has_due_date &&
      (filters.due_kind || filters.due_from || filters.due_through)) {
    return invalid_query();
  }
  return std::nullopt;
}

storage::RecordQuery storage_query(const RecordQuery& query) {
  storage::RecordQuery result;
  const auto& filters = query.filters;
  result.type = query.type;
  result.privacy = filters.privacy;
  result.text = filters.title_contains;
  result.tag = filters.tag;
  result.created = {filters.created.from, filters.created.through};
  result.modified = {filters.modified.from, filters.modified.through};
  result.log_date = {filters.log_date.from, filters.log_date.through};
  result.todo_status = filters.todo_status;
  result.todo_priority = filters.todo_priority;
  result.has_due_date = filters.has_due_date;
  if (filters.due_kind) result.due_kind = static_cast<storage::DueKind>(*filters.due_kind);
  result.due_from = filters.due_from;
  result.due_through = filters.due_through;
  result.sort = static_cast<storage::MetadataSort>(query.order.sort);
  result.descending = query.order.descending;
  return result;
}

OperationResponse query_records(RecordQuery query, const storage::MetadataCatalogView& catalog,
                                const std::vector<ApplicationWarning>& warnings) {
  if (auto invalid = validate_query(query)) return *invalid;
  auto native = storage_query(query);
  if (std::holds_alternative<RootScope>(query.scope)) {
    native.directory = std::optional<core::Id>{};
  } else if (auto* scope = std::get_if<DirectoryScope>(&query.scope)) {
    const auto directory = catalog.find_directory(scope->id);
    if (!directory) return missing_target("Directory not found");
    const auto type = directory->metadata.kind == core::DirectoryKind::Memo
                          ? core::RecordType::Memo
                          : core::RecordType::Todo;
    if (query.type && *query.type != type) return invalid_query("Directory type mismatch");
    query.type = type;
    if (auto invalid = validate_query(query)) return *invalid;
    native.type = type;
    native.directory = std::optional<core::Id>{scope->id};
  }
  std::vector<core::RecordMetadata> records;
  for (const auto& metadata : catalog.query(native))
    records.push_back(project(metadata));
  return ApplicationResponse<std::vector<core::RecordMetadata>>{std::move(records), warnings};
}

ApplicationError map_load_error(const storage::StorageError& failure) {
  try {
    SPDLOG_ERROR("Normal read failed: StorageError code={}", static_cast<int>(failure.code));
  } catch (...) {
  }
  ErrorReason reason = ErrorReason::InvalidData;
  const char* message = "Persisted Record is invalid";
  if (failure.code == storage::StorageErrorCode::StaleLocator) {
    reason = ErrorReason::StaleLocation;
    message = "Record location is stale";
  } else if (failure.code == storage::StorageErrorCode::DuplicateId) {
    reason = ErrorReason::IdentityConflict;
    message = "Entry identity conflict";
  }
  // TODO(Application): expose explicit Refresh separately before advertising it
  // as an action. It is not part of the approved read-access operation inventory.
  return {ErrorCategory::AuthoritativeDataProblem,
          reason,
          message,
          {ErrorAction::ResolveExternally, ErrorAction::Dismiss}};
}

ApplicationError map_load_error(const io::FileError& failure) {
  try {
    SPDLOG_ERROR("Normal read failed: FileError code={} system_error={}",
                 static_cast<int>(failure.code), failure.system_error.value());
  } catch (...) {
  }
  if (failure.code == io::FileErrorCode::InvalidPath) {
    return {ErrorCategory::AuthoritativeDataProblem,
            ErrorReason::UnsafePath,
            "Persisted Record has an unsafe location",
            {ErrorAction::ResolveExternally, ErrorAction::Dismiss}};
  }
  if (failure.system_error == std::errc::permission_denied ||
      failure.system_error == std::errc::operation_not_permitted) {
    return {ErrorCategory::FilesystemFailure,
            ErrorReason::AccessDenied,
            "Access to persisted Record was denied",
            {ErrorAction::ResolveExternally, ErrorAction::Retry, ErrorAction::Dismiss}};
  }
  return {ErrorCategory::FilesystemFailure,
          ErrorReason::IoFailure,
          "Unable to read persisted Record",
          {ErrorAction::Retry, ErrorAction::Dismiss}};
}

template <class Request>
std::function<OperationResponse()> prepare_load(const Request& request,
                                                const storage::MetadataCatalogView& catalog,
                                                storage::NormalRepository& repository) {
  constexpr auto type = std::is_same_v<Request, LoadLogRequest>    ? core::RecordType::Log
                        : std::is_same_v<Request, LoadMemoRequest> ? core::RecordType::Memo
                                                                   : core::RecordType::Todo;
  const auto metadata = catalog.find_record(request.id);
  if (!metadata) {
    if (catalog.contains_entry(request.id)) return [] { return OperationResponse{wrong_type()}; };
    return [] { return OperationResponse{missing_target("Requested Record not found")}; };
  }
  if (storage::record_type(*metadata) != type)
    return [] { return OperationResponse{wrong_type()}; };
  const auto locator = std::visit([](const auto& value) { return value.relative_path; }, *metadata);
  return [id = request.id, locator, &repository]() -> OperationResponse {
    auto loaded = [&] {
      if constexpr (std::is_same_v<Request, LoadLogRequest>)
        return repository.load_log(id, locator);
      else if constexpr (std::is_same_v<Request, LoadMemoRequest>)
        return repository.load_memo(id, locator);
      else
        return repository.load_todo(id, locator);
    }();
    return std::visit(
        [](auto&& value) -> OperationResponse {
          using T = std::decay_t<decltype(value)>;
          if constexpr (std::is_same_v<T, storage::StorageError> ||
                        std::is_same_v<T, io::FileError>) {
            return map_load_error(value);
          } else {
            return std::move(value);
          }
        },
        std::move(loaded));
  };
}

OperationResponse browse(RecordQuery query, const storage::MetadataCatalogView& catalog,
                         const std::vector<ApplicationWarning>& warnings,
                         std::optional<core::RecordType> root_type) {
  auto response = query_records(std::move(query), catalog, warnings);
  if (auto* failure = std::get_if<ApplicationError>(&response)) return *failure;
  BrowseResult result;
  result.records =
      std::move(std::get<ApplicationResponse<std::vector<core::RecordMetadata>>>(response).value);
  if (root_type && *root_type != core::RecordType::Log) {
    result.directories =
        list_directories(catalog, *root_type == core::RecordType::Memo ? core::DirectoryKind::Memo
                                                                       : core::DirectoryKind::Todo);
  }
  return ApplicationResponse<BrowseResult>{std::move(result), warnings};
}

OperationResponse dispatch(const OperationRequest& request,
                           const storage::MetadataCatalogView& catalog,
                           storage::NormalRepository& repository,
                           const std::vector<ApplicationWarning>& warnings) {
  return std::visit(
      [&](const auto& value) -> OperationResponse {
        using R = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<R, QueryRecordsRequest>) {
          return query_records(value.query, catalog, warnings);
        } else if constexpr (std::is_same_v<R, ListDirectoriesRequest>) {
          if (static_cast<unsigned>(value.kind) > 1) return invalid_query("Invalid Directory kind");
          return ApplicationResponse<std::vector<core::DirectoryMetadata>>{
              list_directories(catalog, value.kind), warnings};
        } else if constexpr (std::is_same_v<R, BrowseRequest>) {
          RecordQuery query;
          query.order = value.order;
          std::optional<core::RecordType> root_type;
          if (const auto* root = std::get_if<core::RecordType>(&value.dir)) {
            root_type = *root;
            query.type = *root;
            query.scope = RootScope{};
          } else {
            query.scope = DirectoryScope{std::get<core::Id>(value.dir)};
          }
          return browse(query, catalog, warnings, root_type);
        } else if constexpr (std::is_same_v<R, LoadLogRequest> ||
                             std::is_same_v<R, LoadMemoRequest> ||
                             std::is_same_v<R, LoadTodoRequest>) {
          return prepare_load(value, catalog, repository)();
        } else {
          return ApplicationError{ErrorCategory::InvalidRequest,
                                  ErrorReason::NotImplemented,
                                  "Not an Access Operation",
                                  {ErrorAction::Dismiss}};
        }
      },
      request);
}
} // namespace

AccessOrchestrator::AccessOrchestrator(storage::MetadataCatalog& catalog,
                                       storage::NormalRepository& repository,
                                       storage::CatalogCoordinator& coordinator)
    : catalog_(catalog), repository_(repository), coordinator_(coordinator) {}

OperationResponse AccessOrchestrator::handle(const OperationRequest& request) const {
  return prepare(request)();
}

std::function<OperationResponse()> AccessOrchestrator::prepare(OperationRequest request) const {
  try {
    const auto state = coordinator_.read_state();
    if (state.status != storage::CatalogStatus::Ready &&
        state.status != storage::CatalogStatus::Inconsistent) {
      return []() -> OperationResponse {
        return ApplicationError{ErrorCategory::ApplicationUnavailable,
                                ErrorReason::MetadataUnavailable,
                                "Application metadata unavailable",
                                {ErrorAction::Dismiss}};
      };
    }
    std::vector<ApplicationWarning> warnings;
    if (state.status == storage::CatalogStatus::Inconsistent)
      warnings.push_back({WarningReason::CatalogInconsistent,
                          "Metadata may disagree with persisted Entries",
                          {ErrorAction::Dismiss}});
    auto run = std::visit(
        [&](const auto& value) -> std::function<OperationResponse()> {
          using T = std::decay_t<decltype(value)>;
          if constexpr (std::is_same_v<T, LoadLogRequest> || std::is_same_v<T, LoadMemoRequest> ||
                        std::is_same_v<T, LoadTodoRequest>)
            return prepare_load(value, state.view, repository_);
          else
            return {};
        },
        request);
    if (!run)
      run = [request = std::move(request), view = state.view, warnings = std::move(warnings),
             this]() { return dispatch(request, view, repository_, warnings); };
    return [run = std::move(run)]() -> OperationResponse {
      try {
        return run();
      } catch (...) {
        try {
          SPDLOG_CRITICAL("Unexpected Normal read-access exception");
        } catch (...) {
        }
        return ApplicationError{ErrorCategory::InternalFailure,
                                ErrorReason::UnexpectedFailure,
                                "Unexpected read-access failure",
                                {ErrorAction::Dismiss}};
      }
    };
  } catch (...) {
    try {
      SPDLOG_CRITICAL("Unexpected Normal read preparation exception");
    } catch (...) {
    }
    return []() -> OperationResponse {
      return ApplicationError{ErrorCategory::InternalFailure,
                              ErrorReason::UnexpectedFailure,
                              "Unexpected read-access failure",
                              {ErrorAction::Dismiss}};
    };
  }
}
} // namespace folio::application::internal
