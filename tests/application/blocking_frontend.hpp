#pragma once
#include "frontend/runtime/backend_connection_handle.hpp"
#include <condition_variable>
#include <mutex>
#include <optional>
#include <stdexcept>

namespace folio::test {
using namespace folio::application;
/// Test-only blocking driver for existing synchronous IPC/lifecycle scenarios.
/// Production exposes only asynchronous BackendConnectionHandle operations.
class BlockingFrontend {
public:
  explicit BlockingFrontend(std::unique_ptr<application::ConnectionSession>& session)
      : handle_(std::move(session), [this] {
          std::lock_guard lock(mutex_);
          signaled_ = true;
          changed_.notify_one();
        }) {}
  bool connected() const { return handle_.connected(); }
  application::ConnectionSession& connection_session() { return handle_.connection_session(); }
  core::Result<std::monostate, internal::IpcError> ping() {
    return run<core::Result<std::monostate, internal::IpcError>>(
        [&](auto completed) { return handle_.ping(std::move(completed)); });
  }
  AccessResult<Annotated<BrowseResult>> browse(DirectoryRef dir, RecordOrder order = {}) {
    return run<AccessResult<Annotated<BrowseResult>>>(
        [&](auto completed) { return handle_.browse(dir, order, std::move(completed)); });
  }
  AccessResult<Annotated<std::vector<core::RecordMetadata>>>
  query_records(const RecordQuery& query = {}) {
    return run<AccessResult<Annotated<std::vector<core::RecordMetadata>>>>(
        [&](auto completed) { return handle_.query_records(query, std::move(completed)); });
  }
  AccessResult<Annotated<std::vector<core::DirectoryMetadata>>>
  list_directories(core::DirectoryKind kind) {
    return run<AccessResult<Annotated<std::vector<core::DirectoryMetadata>>>>(
        [&](auto completed) { return handle_.list_directories(kind, std::move(completed)); });
  }
  AccessResult<ConsistencyCheckResult> check_consistency(DirectoryRef scope) {
    return run<AccessResult<ConsistencyCheckResult>>(
        [&](auto completed) { return handle_.check_consistency(scope, std::move(completed)); });
  }
  AccessResult<std::monostate> refresh(RefreshScope scope = {}) {
    return run<AccessResult<std::monostate>>(
        [&](auto completed) { return handle_.refresh(scope, std::move(completed)); });
  }
  AccessResult<Annotated<core::Log::Snapshot>> load_log(const core::Id& id) {
    return run<AccessResult<Annotated<core::Log::Snapshot>>>(
        [&](auto completed) { return handle_.load_log(id, std::move(completed)); });
  }
  AccessResult<Annotated<core::Memo::Snapshot>> load_memo(const core::Id& id) {
    return run<AccessResult<Annotated<core::Memo::Snapshot>>>(
        [&](auto completed) { return handle_.load_memo(id, std::move(completed)); });
  }
  AccessResult<Annotated<core::Todo::Snapshot>> load_todo(const core::Id& id) {
    return run<AccessResult<Annotated<core::Todo::Snapshot>>>(
        [&](auto completed) { return handle_.load_todo(id, std::move(completed)); });
  }
  AccessResult<std::monostate> acquire_edit(const core::Id& id) {
    return run<AccessResult<std::monostate>>(
        [&](auto completed) { return handle_.acquire_edit(id, std::move(completed)); });
  }
  AccessResult<std::monostate> release_edit(const core::Id& id) {
    return run<AccessResult<std::monostate>>(
        [&](auto completed) { return handle_.release_edit(id, std::move(completed)); });
  }
  MutationResult<core::LogMetadata> create(const MutationId& mutation_id,
                                           const LogEditableFields& fields,
                                           const core::TimestampContext& created,
                                           const core::TimestampContext& modified) {
    return run<MutationResult<core::LogMetadata>>([&](auto completed) {
      return handle_.create(mutation_id, fields, created, modified, std::move(completed));
    });
  }
  MutationResult<core::MemoMetadata> create(const MutationId& mutation_id,
                                            const MemoEditableFields& fields,
                                            const core::TimestampContext& created,
                                            const core::TimestampContext& modified,
                                            std::optional<core::Id> directory_id) {
    return run<MutationResult<core::MemoMetadata>>([&](auto completed) {
      return handle_.create(mutation_id, fields, created, modified, directory_id,
                            std::move(completed));
    });
  }
  MutationResult<core::TodoMetadata> create(const MutationId& mutation_id,
                                            const TodoEditableFields& fields,
                                            const core::TimestampContext& created,
                                            const core::TimestampContext& modified,
                                            std::optional<core::Id> directory_id) {
    return run<MutationResult<core::TodoMetadata>>([&](auto completed) {
      return handle_.create(mutation_id, fields, created, modified, directory_id,
                            std::move(completed));
    });
  }
  MutationResult<core::LogMetadata> update(const MutationId& mutation_id, const core::Id& entry_id,
                                           const LogEditableFields& fields,
                                           const core::TimestampContext& modified) {
    return run<MutationResult<core::LogMetadata>>([&](auto completed) {
      return handle_.update(mutation_id, entry_id, fields, modified, std::move(completed));
    });
  }
  MutationResult<core::MemoMetadata> update(const MutationId& mutation_id, const core::Id& entry_id,
                                            const MemoEditableFields& fields,
                                            const core::TimestampContext& modified) {
    return run<MutationResult<core::MemoMetadata>>([&](auto completed) {
      return handle_.update(mutation_id, entry_id, fields, modified, std::move(completed));
    });
  }
  MutationResult<core::TodoMetadata> update(const MutationId& mutation_id, const core::Id& entry_id,
                                            const TodoEditableFields& fields,
                                            const core::TimestampContext& modified) {
    return run<MutationResult<core::TodoMetadata>>([&](auto completed) {
      return handle_.update(mutation_id, entry_id, fields, modified, std::move(completed));
    });
  }
  MutationResult<core::RecordMetadata> move(const MutationId& mutation_id, const core::Id& entry_id,
                                            std::optional<core::Id> destination,
                                            const core::TimestampContext& modified) {
    return run<MutationResult<core::RecordMetadata>>([&](auto completed) {
      return handle_.move(mutation_id, entry_id, destination, modified, std::move(completed));
    });
  }
  MutationResult<core::DirectoryMetadata>
  create(const MutationId& mutation_id, core::DirectoryKind kind, const std::string& name) {
    return run<MutationResult<core::DirectoryMetadata>>([&](auto completed) {
      return handle_.create(mutation_id, kind, name, std::move(completed));
    });
  }
  MutationResult<core::DirectoryMetadata> rename_directory(const MutationId& mutation_id,
                                                           const core::Id& entry_id,
                                                           const std::string& new_name) {
    return run<MutationResult<core::DirectoryMetadata>>([&](auto completed) {
      return handle_.rename_directory(mutation_id, entry_id, new_name, std::move(completed));
    });
  }
  MutationResult<core::TodoMetadata> set_todo_status(const MutationId& mutation_id,
                                                     const core::Id& entry_id,
                                                     core::TodoStatus status,
                                                     const core::TimestampContext& modified) {
    return run<MutationResult<core::TodoMetadata>>([&](auto completed) {
      return handle_.set_todo_status(mutation_id, entry_id, status, modified, std::move(completed));
    });
  }

private:
  template <class Result, class Submit> Result run(Submit submit) {
    std::optional<Result> result;
    if (!submit([&](Result value) { result.emplace(std::move(value)); }))
      throw std::logic_error("Test attempted overlapping Frontend requests");
    while (!result) {
      {
        std::unique_lock lock(mutex_);
        changed_.wait(lock, [&] { return signaled_; });
        signaled_ = false;
      }
      if (handle_.take_pending_work().completion) handle_.process_completion();
    }
    return std::move(*result);
  }
  std::mutex mutex_;
  std::condition_variable changed_;
  bool signaled_ = false;
  application::BackendConnectionHandle handle_;
};
} // namespace folio::test
