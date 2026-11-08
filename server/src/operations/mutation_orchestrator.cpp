#include "operations/mutation_orchestrator.hpp"
#include "operations/access_orchestrator.hpp"
#include <folio/ipc/messagepack_codec.hpp>
#include <folio/logging/logging.hpp>

#include <algorithm>
#include <type_traits>
#include <stdexcept>

namespace folio::application::internal {
namespace storage = core::storage;
namespace {

ApplicationError problem(ErrorReason reason, const char* message,
                         ErrorCategory category = ErrorCategory::InvalidRequest) {
  return {category, reason, message, {ErrorAction::Dismiss}};
}

ApplicationError repository_problem(const storage::RepositoryError& error) {
  return std::visit(
      [](const auto& cause) {
        using T = std::decay_t<decltype(cause)>;
        if constexpr (std::is_same_v<T, storage::StorageError>) {
          switch (cause.code) {
          case storage::StorageErrorCode::PostPersistFailure:
            return problem(ErrorReason::CatalogSynchronizationFailed,
                           "Authoritative operation completed but Catalog synchronization failed",
                           ErrorCategory::ApplicationUnavailable);
          case storage::StorageErrorCode::StaleLocator:
            return problem(ErrorReason::StaleLocation, "Entry location is stale",
                           ErrorCategory::AuthoritativeDataProblem);
          case storage::StorageErrorCode::DuplicateId:
            return problem(ErrorReason::IdentityConflict, "Entry identity conflict",
                           ErrorCategory::AuthoritativeDataProblem);
          case storage::StorageErrorCode::CatalogUnavailable:
            return problem(ErrorReason::MetadataUnavailable, "Catalog is unavailable",
                           ErrorCategory::ApplicationUnavailable);
          case storage::StorageErrorCode::OperationFailure:
            return problem(ErrorReason::UnexpectedFailure, "Unexpected Repository failure",
                           ErrorCategory::InternalFailure);
          default:
            return problem(ErrorReason::InvalidData, "Entry state or supplied fields are invalid",
                           ErrorCategory::AuthoritativeDataProblem);
          }
        } else {
          if (cause.code == io::FileErrorCode::InvalidPath)
            return problem(ErrorReason::UnsafePath, "Entry path or layout is invalid",
                           ErrorCategory::AuthoritativeDataProblem);
          if (cause.system_error == std::errc::permission_denied ||
              cause.system_error == std::errc::operation_not_permitted)
            return problem(ErrorReason::AccessDenied, "Filesystem access was denied",
                           ErrorCategory::FilesystemFailure);
          return problem(ErrorReason::IoFailure, "Authoritative filesystem operation failed",
                         ErrorCategory::FilesystemFailure);
        }
      },
      error);
}

StepObservation observation(storage::PersistenceObservation value) {
  switch (value) {
  case storage::PersistenceObservation::NotAttempted:
    return StepObservation::NotAttempted;
  case storage::PersistenceObservation::NotCompleted:
    return StepObservation::NotCompleted;
  case storage::PersistenceObservation::Published:
    return StepObservation::Published;
  case storage::PersistenceObservation::DurabilityConfirmed:
    return StepObservation::DurabilityConfirmed;
  case storage::PersistenceObservation::Unknown:
    return StepObservation::Unknown;
  }
  return StepObservation::Unknown;
}

MutationFailure mutation_problem(const storage::RepositoryMutationError& error,
                                 MutationEvidence evidence, bool move) {
  const auto& facts = error.evidence;
  evidence.steps =
      move
          ? std::vector<MutationStepEvidence>{{MutationStep::CreateDestination,
                                               observation(facts.destination)},
                                              {MutationStep::RemoveSource,
                                               observation(facts.source_removal)}}
          : std::vector<MutationStepEvidence>{{MutationStep::WriteEntry, observation(facts.write)}};
  auto status = AuthoritativeChangeStatus::NotApplied;
  bool durable = false;
  for (const auto& step : evidence.steps) {
    if (step.observation == StepObservation::Published ||
        step.observation == StepObservation::Unknown) {
      status = AuthoritativeChangeStatus::Unknown;
      break;
    }
    durable |= step.observation == StepObservation::DurabilityConfirmed;
  }
  if (status != AuthoritativeChangeStatus::Unknown && durable) {
    status = move && facts.source_removal != storage::PersistenceObservation::DurabilityConfirmed
                 ? AuthoritativeChangeStatus::PartiallyApplied
                 : AuthoritativeChangeStatus::Applied;
  }
  try {
    SPDLOG_ERROR("Normal mutation failed (stage={}, authoritative_status={})",
                 static_cast<int>(error.stage), static_cast<int>(status));
  } catch (...) {
  }
  return {repository_problem(error.cause), status, std::move(evidence)};
}

void apply(core::Log::Snapshot& snapshot, const LogEditableFields& fields) {
  snapshot.title = fields.title;
  snapshot.content = fields.content;
  snapshot.log_date = fields.log_date;
  snapshot.tags = fields.tags;
}
void apply(core::Memo::Snapshot& snapshot, const MemoEditableFields& fields) {
  snapshot.title = fields.title;
  snapshot.content = fields.content;
  snapshot.tags = fields.tags;
}
void apply(core::Todo::Snapshot& snapshot, const TodoEditableFields& fields) {
  snapshot.title = fields.title;
  snapshot.notes = fields.notes;
  snapshot.status = fields.status;
  snapshot.priority = fields.priority;
  snapshot.due_date = fields.due_date;
  snapshot.subtasks = fields.subtasks;
}

std::filesystem::path directory_path(const core::DirectoryMetadata& metadata) {
  return std::filesystem::path(metadata.kind == core::DirectoryKind::Memo ? "memos" : "todos") /
         metadata.id.to_string();
}

DirectoryRef location(core::RecordType type, const std::optional<core::Id>& directory) {
  return directory ? DirectoryRef{*directory} : DirectoryRef{type};
}
} // namespace

MutationOrchestrator::MutationOrchestrator(storage::MetadataCatalog& catalog,
                                           storage::NormalRepository& repository,
                                           storage::CatalogCoordinator& coordinator)
    : catalog_(catalog), repository_(repository), coordinator_(coordinator) {}

std::optional<ApplicationError>
MutationOrchestrator::check_lock(const core::Id& id, const Session& session, bool required) {
  const auto found = locks_.find(id.to_string());
  if (found != locks_.end()) {
    if (found->second.lock() != session)
      return problem(ErrorReason::LockConflict, "Entry is being edited by another Frontend",
                     ErrorCategory::TargetUnavailable);
    return std::nullopt;
  }
  if (required)
    return problem(ErrorReason::EditLockRequired,
                   "This connection must acquire the Entry edit lock");
  return std::nullopt;
}

OperationResponse MutationOrchestrator::acquire_edit(const core::Id& id, const Session& session) {
  if (coordinator_.status() != storage::CatalogStatus::Ready)
    return problem(ErrorReason::MetadataUnavailable, "Editing requires a Ready Catalog",
                   ErrorCategory::ApplicationUnavailable);
  if (auto error = check_lock(id, session, false)) return *error;
  std::optional<EditSnapshot> snapshot;
  if (const auto directory = catalog_.find_directory(id)) {
    if (directory->relative_path != directory_path(directory->metadata))
      return problem(ErrorReason::StaleLocation, "Directory location disagrees with Catalog",
                     ErrorCategory::AuthoritativeDataProblem);
    auto loaded = repository_.load_directory(directory->metadata.kind, id);
    if (auto* error = std::get_if<storage::StorageError>(&loaded))
      return repository_problem(*error);
    if (auto* error = std::get_if<io::FileError>(&loaded)) return repository_problem(*error);
    auto value = std::get<core::Directory::Snapshot>(std::move(loaded));
    if (value.name != directory->metadata.name)
      return problem(ErrorReason::InvalidData, "Authoritative Directory disagrees with Catalog",
                     ErrorCategory::AuthoritativeDataProblem);
    snapshot = std::move(value);
  } else {
    const auto metadata = catalog_.find_record(id);
    if (!metadata)
      return problem(ErrorReason::NotFound, "Entry not found", ErrorCategory::TargetUnavailable);
    OperationRequest request = LoadLogRequest{id};
    switch (storage::record_type(*metadata)) {
    case core::RecordType::Log:
      request = LoadLogRequest{id};
      break;
    case core::RecordType::Memo:
      request = LoadMemoRequest{id};
      break;
    case core::RecordType::Todo:
      request = LoadTodoRequest{id};
      break;
    }
    auto loaded = AccessOrchestrator{catalog_, repository_, coordinator_}.handle(request);
    if (auto* error = std::get_if<ApplicationError>(&loaded)) return *error;
    bool matches = false;
    std::visit(
        [&](auto&& value) {
          using T = std::decay_t<decltype(value)>;
          if constexpr (std::is_same_v<T, core::Log::Snapshot> ||
                        std::is_same_v<T, core::Memo::Snapshot> ||
                        std::is_same_v<T, core::Todo::Snapshot>) {
            const auto locator =
                std::visit([](const auto& item) { return item.relative_path; }, *metadata);
            matches = storage::metadata_matches(storage::metadata_from(value, locator), *metadata);
            snapshot = std::move(value);
          }
        },
        std::move(loaded));
    if (!matches)
      return problem(ErrorReason::InvalidData, "Authoritative metadata disagrees with Catalog",
                     ErrorCategory::AuthoritativeDataProblem);
  }
  OperationResponse response = std::move(*snapshot);
  if (!codec::fits_message(response))
    return problem(ErrorReason::ResourceLimit, "Entry exceeds the edit Snapshot response limit",
                   ErrorCategory::ApplicationUnavailable);
  locks_.insert_or_assign(id.to_string(), session);
  return response;
}

OperationResponse MutationOrchestrator::release_edit(const core::Id& id, const Session& session) {
  if (auto error = check_lock(id, session, false)) return *error;
  locks_.erase(id.to_string());
  return std::monostate{};
}

OperationResponse MutationOrchestrator::handle(const OperationRequest& request,
                                               const Session& session) {
  // Weak ownership expires only after the worker AND all accepted actions leave.
  std::erase_if(locks_, [](const auto& item) { return item.second.expired(); });
  if (!session)
    return problem(ErrorReason::UnexpectedFailure, "Missing connection session",
                   ErrorCategory::InternalFailure);
  if (const auto* mutation = std::get_if<MutationRequest>(&request))
    return mutate(*mutation, session);
  if (const auto* acquire = std::get_if<AcquireEditRequest>(&request))
    return acquire_edit(acquire->id, session);
  if (const auto* release = std::get_if<ReleaseEditRequest>(&request))
    return release_edit(release->id, session);
  return problem(ErrorReason::UnexpectedFailure, "Access request routed to mutation orchestration",
                 ErrorCategory::InternalFailure);
}

NormalMutationResponse MutationOrchestrator::mutate(const MutationRequest& request,
                                                    const Session& session) {
  return std::visit(
      [&](const auto& operation) -> NormalMutationResponse {
        using T = std::decay_t<decltype(operation)>;
        if constexpr (std::is_same_v<T, MoveRecordRequest>) {
          const auto metadata = catalog_.find_record(operation.id);
          if (metadata && std::holds_alternative<storage::Located<core::MemoMetadata>>(*metadata))
            return mutate(
                {request.mutation_id,
                 MoveMemoRequest{operation.id, operation.directory_id, operation.modified}},
                session);
          if (metadata && std::holds_alternative<storage::Located<core::TodoMetadata>>(*metadata))
            return mutate(
                {request.mutation_id,
                 MoveTodoRequest{operation.id, operation.directory_id, operation.modified}},
                session);
          return {request.mutation_id,
                  MutationFailure{
                      problem(metadata || catalog_.contains_entry(operation.id)
                                  ? ErrorReason::WrongRecordType
                                  : ErrorReason::NotFound,
                              "Requested Entry cannot be moved", ErrorCategory::TargetUnavailable),
                      AuthoritativeChangeStatus::NotApplied,
                      MutationEvidence{
                          operation.id,
                          {},
                          {},
                          {{MutationStep::CreateDestination, StepObservation::NotAttempted},
                           {MutationStep::RemoveSource, StepObservation::NotAttempted}}}}};
        } else {
          constexpr bool create_log = std::is_same_v<T, CreateLogRequest>;
          constexpr bool create_memo = std::is_same_v<T, CreateMemoRequest>;
          constexpr bool create_todo = std::is_same_v<T, CreateTodoRequest>;
          constexpr bool create_directory = std::is_same_v<T, CreateDirectoryRequest>;
          constexpr bool create = create_log || create_memo || create_todo || create_directory;
          constexpr bool update = std::is_same_v<T, UpdateLogRequest> ||
                                  std::is_same_v<T, UpdateMemoRequest> ||
                                  std::is_same_v<T, UpdateTodoRequest>;
          constexpr bool move =
              std::is_same_v<T, MoveMemoRequest> || std::is_same_v<T, MoveTodoRequest>;
          constexpr bool directory = create_directory || std::is_same_v<T, RenameDirectoryRequest>;
          constexpr bool memo = create_memo || std::is_same_v<T, UpdateMemoRequest> ||
                                std::is_same_v<T, MoveMemoRequest>;
          constexpr bool todo = create_todo || std::is_same_v<T, UpdateTodoRequest> ||
                                std::is_same_v<T, MoveTodoRequest> ||
                                std::is_same_v<T, SetTodoStatusRequest>;
          constexpr auto type = memo   ? core::RecordType::Memo
                                : todo ? core::RecordType::Todo
                                       : core::RecordType::Log;
          const auto id = [&] {
            if constexpr (create)
              return core::Id::generate();
            else
              return operation.id;
          }();
          MutationEvidence evidence{id, {}, {}, {}};
          if constexpr (move)
            evidence.steps = {{MutationStep::CreateDestination, StepObservation::NotAttempted},
                              {MutationStep::RemoveSource, StepObservation::NotAttempted}};
          else
            evidence.steps = {{MutationStep::WriteEntry, StepObservation::NotAttempted}};
          auto reject = [&](ApplicationError error) -> NormalMutationResponse {
            return {
                request.mutation_id,
                MutationFailure{std::move(error), AuthoritativeChangeStatus::NotApplied, evidence}};
          };
          if (coordinator_.status() != storage::CatalogStatus::Ready)
            return reject(problem(ErrorReason::MetadataUnavailable,
                                  "Mutation requires a Ready Catalog",
                                  ErrorCategory::ApplicationUnavailable));
          std::optional<storage::RecordMetadata> metadata;
          std::optional<storage::DirectoryMetadata> dir_metadata;
          std::filesystem::path locator;
          if constexpr (!create) {
            if constexpr (directory) {
              dir_metadata = catalog_.find_directory(id);
              if (!dir_metadata)
                return reject(problem(catalog_.contains_entry(id) ? ErrorReason::WrongRecordType
                                                                  : ErrorReason::NotFound,
                                      "Requested Directory is unavailable",
                                      ErrorCategory::TargetUnavailable));
              if (dir_metadata->relative_path != directory_path(dir_metadata->metadata))
                return reject(problem(ErrorReason::StaleLocation,
                                      "Directory location disagrees with Catalog",
                                      ErrorCategory::AuthoritativeDataProblem));
              auto current = repository_.load_directory(dir_metadata->metadata.kind, id);
              if (auto* error = std::get_if<storage::StorageError>(&current))
                return reject(repository_problem(*error));
              if (auto* error = std::get_if<io::FileError>(&current))
                return reject(repository_problem(*error));
              if (std::get<core::Directory::Snapshot>(current).name != dir_metadata->metadata.name)
                return reject(problem(ErrorReason::InvalidData,
                                      "Authoritative Directory disagrees with Catalog",
                                      ErrorCategory::AuthoritativeDataProblem));
            } else {
              metadata = catalog_.find_record(id);
              if (!metadata)
                return reject(problem(catalog_.contains_entry(id) ? ErrorReason::WrongRecordType
                                                                  : ErrorReason::NotFound,
                                      "Requested Record is unavailable",
                                      ErrorCategory::TargetUnavailable));
              if (storage::record_type(*metadata) != type)
                return reject(problem(ErrorReason::WrongRecordType, "Wrong Record type"));
              locator =
                  std::visit([](const auto& value) { return value.relative_path; }, *metadata);
            }
          }
          if constexpr (!create) {
            if (auto error = check_lock(id, session, update)) return reject(*error);
          }
          if constexpr (create_memo || create_todo || move) {
            evidence.destination = location(type, operation.directory_id);
            if (operation.directory_id) {
              auto target = catalog_.find_directory(*operation.directory_id);
              if (!target)
                return reject(problem(ErrorReason::NotFound, "Target Directory not found",
                                      ErrorCategory::TargetUnavailable));
              if (target->metadata.kind !=
                  (memo ? core::DirectoryKind::Memo : core::DirectoryKind::Todo))
                return reject(
                    problem(ErrorReason::WrongRecordType, "Target Directory has the wrong kind"));
            }
          }
          using Snapshot =
              std::conditional_t<directory, core::Directory::Snapshot,
                                 std::conditional_t<memo, core::Memo::Snapshot,
                                                    std::conditional_t<todo, core::Todo::Snapshot,
                                                                       core::Log::Snapshot>>>;
          std::optional<Snapshot> snapshot;
          if constexpr (create_directory) {
            if (operation.kind != core::DirectoryKind::Memo &&
                operation.kind != core::DirectoryKind::Todo)
              return reject(problem(ErrorReason::InvalidMutation, "Invalid Directory kind"));
            snapshot.emplace(Snapshot{id, operation.kind, operation.name});
          } else if constexpr (create_log) {
            snapshot.emplace(Snapshot{id, operation.created, operation.modified,
                                      operation.fields.title, operation.fields.content,
                                      operation.fields.log_date, operation.fields.tags,
                                      core::Privacy::Normal});
          } else if constexpr (create_memo) {
            snapshot.emplace(Snapshot{id, operation.created, operation.modified,
                                      operation.fields.title, operation.fields.content,
                                      operation.fields.tags, operation.directory_id,
                                      core::Privacy::Normal});
          } else if constexpr (create_todo) {
            snapshot.emplace(Snapshot{id, operation.created, operation.modified,
                                      operation.fields.title, operation.fields.notes,
                                      operation.fields.status, operation.fields.priority,
                                      operation.fields.due_date, operation.directory_id,
                                      core::Privacy::Normal, operation.fields.subtasks});
          } else if constexpr (!directory) {
            auto loaded = [&] {
              if constexpr (memo)
                return repository_.load_memo(id, locator);
              else if constexpr (todo)
                return repository_.load_todo(id, locator);
              else
                return repository_.load_log(id, locator);
            }();
            if (auto* error = std::get_if<storage::StorageError>(&loaded))
              return reject(repository_problem(*error));
            if (auto* error = std::get_if<io::FileError>(&loaded))
              return reject(repository_problem(*error));
            snapshot.emplace(std::get<Snapshot>(std::move(loaded)));
            if (!storage::metadata_matches(storage::metadata_from(*snapshot, locator), *metadata))
              return reject(problem(ErrorReason::InvalidData,
                                    "Authoritative metadata disagrees with Catalog",
                                    ErrorCategory::AuthoritativeDataProblem));
            if constexpr (memo || todo) {
              evidence.source = location(type, snapshot->directory_id);
            }
            if constexpr (update) apply(*snapshot, operation.fields);
            if constexpr (std::is_same_v<T, SetTodoStatusRequest>)
              snapshot->status = operation.status;
            // Move uses this Snapshot only for validation; Repository reloads and
            // preserves its content. No second full-body copy is needed for context checks.
            snapshot->modified = operation.modified;
          }
          // The same validators used by Catalog insertion/update reject malformed
          // submitted metadata before persistence, without a second Catalog/database.
          if constexpr (directory) {
            const auto candidate = [&] {
              if constexpr (create_directory)
                return storage::DirectoryMetadata{
                    core::DirectoryMetadata{id, snapshot->kind, snapshot->name},
                    directory_path({id, snapshot->kind, snapshot->name})};
              else
                return storage::DirectoryMetadata{
                    core::DirectoryMetadata{id, dir_metadata->metadata.kind, operation.name},
                    dir_metadata->relative_path};
            }();
            if (auto checked = storage::validate_metadata(candidate);
                std::holds_alternative<storage::StorageError>(checked))
              return reject(problem(ErrorReason::InvalidMutation, "Invalid Directory fields"));
          } else {
            auto candidate_path = locator;
            if constexpr (create) {
              auto name = storage::normal_record_filename(type, id, snapshot->created.timestamp);
              if (std::holds_alternative<storage::StorageError>(name))
                return reject(problem(ErrorReason::InvalidMutation, "Invalid creation timestamp"));
              candidate_path = type == core::RecordType::Log ? "logs" : memo ? "memos" : "todos";
              if constexpr (memo || todo)
                if (snapshot->directory_id) candidate_path /= snapshot->directory_id->to_string();
              candidate_path /= std::get<std::string>(name);
            }
            if (auto checked =
                    storage::validate_metadata(storage::metadata_from(*snapshot, candidate_path));
                std::holds_alternative<storage::StorageError>(checked))
              return reject(
                  problem(ErrorReason::InvalidMutation, "Invalid Record fields or context"));
          }
          auto result = [&]() -> core::Result<std::monostate, storage::RepositoryMutationError> {
            if constexpr (create_directory)
              return repository_.create_directory(operation.kind, *snapshot,
                                                  coordinator_.directory_creation_callback(),
                                                  coordinator_.creation_check());
            else if constexpr (std::is_same_v<T, RenameDirectoryRequest>)
              return repository_.rename_directory(dir_metadata->metadata.kind, id, operation.name,
                                                  coordinator_.directory_update_callback());
            else if constexpr (create_log)
              return repository_.create_log(*snapshot, coordinator_.log_creation_callback(),
                                            coordinator_.creation_check());
            else if constexpr (create_memo)
              return repository_.create_memo(*snapshot, coordinator_.memo_creation_callback(),
                                             coordinator_.creation_check());
            else if constexpr (create_todo)
              return repository_.create_todo(*snapshot, coordinator_.todo_creation_callback(),
                                             coordinator_.creation_check());
            else if constexpr (std::is_same_v<T, MoveMemoRequest>)
              return repository_.move_memo(id, operation.directory_id, operation.modified,
                                           coordinator_.memo_update_callback(), locator);
            else if constexpr (std::is_same_v<T, MoveTodoRequest>)
              return repository_.move_todo(id, operation.directory_id, operation.modified,
                                           coordinator_.todo_update_callback(), locator);
            else if constexpr (memo)
              return repository_.update_memo(*snapshot, locator,
                                             coordinator_.memo_update_callback());
            else if constexpr (todo)
              return repository_.update_todo(*snapshot, locator,
                                             coordinator_.todo_update_callback());
            else
              return repository_.update_log(*snapshot, locator, coordinator_.log_update_callback());
          }();
          if (auto* error = std::get_if<storage::RepositoryMutationError>(&result))
            return {request.mutation_id, mutation_problem(*error, evidence, move)};
#ifdef _WIN32
          // The existing Repository cannot confirm durable source removal on Windows.
          // Do not upgrade a published removal into Applied or confirmed success.
          if constexpr (move) {
            if (evidence.source != evidence.destination) {
              evidence.steps = {
                  {MutationStep::CreateDestination, StepObservation::DurabilityConfirmed},
                  {MutationStep::RemoveSource, StepObservation::Published}};
              return {request.mutation_id,
                      MutationFailure{
                          problem(ErrorReason::IoFailure,
                                  "Move completed but source-removal durability is unconfirmed",
                                  ErrorCategory::FilesystemFailure),
                          AuthoritativeChangeStatus::Unknown, evidence}};
            }
          }
#endif
          if constexpr (directory) {
            const auto saved = catalog_.find_directory(id);
            if (saved) return {request.mutation_id, MutationValue{saved->metadata}};
          } else {
            const auto saved = catalog_.find_record(id);
            if (saved)
              return std::visit(
                  [&](const auto& value) -> NormalMutationResponse {
                    return {request.mutation_id, MutationValue{value.metadata}};
                  },
                  *saved);
          }
          // Matching Coordinator callbacks guarantee a Catalog result on success.
          // A breach is a service failure, never a fabricated NotApplied response.
          throw std::logic_error("Successful mutation missing from Catalog");
        }
      },
      request.payload);
}
} // namespace folio::application::internal
