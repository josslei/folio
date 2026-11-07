#pragma once

#include <folio/application/error.hpp>
#include <folio/application/query.hpp>
#include <folio/application/mutation.hpp>
#include <folio/core/log.hpp>
#include <folio/core/memo.hpp>

// Shared wire value types, not Backend handlers or the public business API.
namespace folio::application::internal {

struct BrowseRequest {
  DirectoryRef dir;
  RecordOrder order;
};

struct QueryRecordsRequest { RecordQuery query; };
struct ListDirectoriesRequest { core::DirectoryKind kind; };
struct LoadLogRequest { core::Id id; };
struct LoadMemoRequest { core::Id id; };
struct LoadTodoRequest { core::Id id; };

struct AcquireEditRequest { core::Id id; };
struct ReleaseEditRequest { core::Id id; };
// Wire requests stay separate from public method arguments.
// Modification Context is shared operation context, supplied by the Frontend;
// it is neither an editable field nor a version/concurrency token.
struct CreateLogRequest {
  LogEditableFields fields;
  core::TimestampContext created;
  core::TimestampContext modified;
};
struct CreateMemoRequest {
  MemoEditableFields fields;
  core::TimestampContext created;
  core::TimestampContext modified;
  std::optional<core::Id> directory_id;
};
struct CreateTodoRequest {
  TodoEditableFields fields;
  core::TimestampContext created;
  core::TimestampContext modified;
  std::optional<core::Id> directory_id;
};
struct UpdateLogRequest {
  core::Id id;
  LogEditableFields fields;
  core::TimestampContext modified;
};
struct UpdateMemoRequest {
  core::Id id;
  MemoEditableFields fields;
  core::TimestampContext modified;
};
struct UpdateTodoRequest {
  core::Id id;
  TodoEditableFields fields;
  core::TimestampContext modified;
};
struct MoveMemoRequest {
  core::Id id;
  std::optional<core::Id> directory_id;
  core::TimestampContext modified;
};
struct MoveTodoRequest {
  core::Id id;
  std::optional<core::Id> directory_id;
  core::TimestampContext modified;
};
struct CreateDirectoryRequest {
  core::DirectoryKind kind;
  std::string name;
};
// An immediate mutation: ownership is checked and the rename executes as one
// executor action. Directory has no persisted Modification Context.
struct RenameDirectoryRequest {
  core::Id id;
  std::string name;
};
struct SetTodoStatusRequest {
  core::Id id;
  core::TodoStatus status;
  core::TimestampContext modified;
};

struct MoveRecordRequest {
  core::Id id;
  std::optional<core::Id> directory_id;
  core::TimestampContext modified;
};
using MutationPayload = std::variant<CreateLogRequest, CreateMemoRequest, CreateTodoRequest,
    UpdateLogRequest, UpdateMemoRequest, UpdateTodoRequest, MoveMemoRequest, MoveTodoRequest,
    CreateDirectoryRequest, RenameDirectoryRequest, SetTodoStatusRequest, MoveRecordRequest>;
struct MutationRequest { MutationId mutation_id; MutationPayload payload; };
using MutationValue = std::variant<core::LogMetadata, core::MemoMetadata,
                                    core::TodoMetadata, core::DirectoryMetadata>;
using NormalMutationResponse = MutationResponse<MutationValue>;

using OperationRequest = std::variant<
    BrowseRequest, QueryRecordsRequest,
    ListDirectoriesRequest, LoadLogRequest, LoadMemoRequest, LoadTodoRequest,
    MutationRequest, AcquireEditRequest, ReleaseEditRequest>;

using OperationResponse = std::variant<
    ApplicationResponse<BrowseResult>,
    ApplicationResponse<std::vector<core::RecordMetadata>>,
    ApplicationResponse<std::vector<core::DirectoryMetadata>>,
    core::Log::Snapshot, core::Memo::Snapshot, core::Todo::Snapshot,
    ApplicationError, NormalMutationResponse, EditSnapshot, std::monostate>;

}  // namespace folio::application::internal
