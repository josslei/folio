#pragma once

#include <folio/application/error.hpp>
#include <folio/application/query.hpp>
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

using AccessRequest = std::variant<
    BrowseRequest, QueryRecordsRequest,
    ListDirectoriesRequest, LoadLogRequest, LoadMemoRequest, LoadTodoRequest>;

using AccessResponse = std::variant<
    ApplicationResponse<BrowseResult>,
    ApplicationResponse<std::vector<core::RecordMetadata>>,
    ApplicationResponse<std::vector<core::DirectoryMetadata>>,
    core::Log::Snapshot, core::Memo::Snapshot, core::Todo::Snapshot,
    ApplicationError>;

}  // namespace folio::application::internal
