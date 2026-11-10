#include "execution/request_coordinator.hpp"
#include <stdexcept>
#include <type_traits>

namespace folio::application::internal {
namespace {
std::size_t checked_workers(std::size_t workers) {
  if (workers == 0) throw std::invalid_argument("Access workers must be positive");
  return workers;
}
std::size_t lane_index(RequestCoordinator::Lane lane) {
  return static_cast<std::size_t>(lane);
}
} // namespace
RequestCoordinator::Plan RequestCoordinator::plan(const OperationRequest& request) {
  return std::visit(
      [](const auto& operation) -> Plan {
        using T = std::decay_t<decltype(operation)>;
        if constexpr (std::is_same_v<T, MutationRequest>) {
          return std::visit(
              [](const auto& value) -> Plan {
                using M = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<M, CreateLogRequest> ||
                              std::is_same_v<M, CreateMemoRequest> ||
                              std::is_same_v<M, CreateTodoRequest> ||
                              std::is_same_v<M, CreateDirectoryRequest>)
                  return {Lane::Mutation, {}};
                else if constexpr (std::is_same_v<M, UpdateLogRequest> ||
                                   std::is_same_v<M, UpdateMemoRequest> ||
                                   std::is_same_v<M, UpdateTodoRequest> ||
                                   std::is_same_v<M, MoveMemoRequest> ||
                                   std::is_same_v<M, MoveTodoRequest> ||
                                   std::is_same_v<M, RenameDirectoryRequest> ||
                                   std::is_same_v<M, SetTodoStatusRequest> ||
                                   std::is_same_v<M, MoveRecordRequest>)
                  return {Lane::Mutation, value.id.to_string()};
                else
                  static_assert(sizeof(M) == 0,
                                "Define new mutation conflict scope before dispatch");
              },
              operation.payload);
        } else if constexpr (std::is_same_v<T, AcquireEditRequest> ||
                             std::is_same_v<T, ReleaseEditRequest>) {
          return {Lane::Control, operation.id.to_string()};
        } else if constexpr (std::is_same_v<T, LoadLogRequest> ||
                             std::is_same_v<T, LoadMemoRequest> ||
                             std::is_same_v<T, LoadTodoRequest>) {
          return {Lane::Load, operation.id.to_string()};
        } else if constexpr (std::is_same_v<T, BrowseRequest> ||
                             std::is_same_v<T, QueryRecordsRequest> ||
                             std::is_same_v<T, ListDirectoriesRequest>) {
          return {Lane::Query, {}};
        } else {
          static_assert(sizeof(T) == 0, "Define execution requirements before dispatch");
        }
      },
      request);
}
RequestCoordinator::RequestCoordinator(std::size_t capacity, std::size_t access_workers)
    : capacity_(capacity), completed_([&] {
        std::vector<Work> result;
        result.reserve(capacity);
        return result;
      }()),
      access_workers_(checked_workers(access_workers)), access_(access_workers_),
      coordination_([this] { coordinate(); }) {}
RequestCoordinator::~RequestCoordinator() {
  drain();
}

std::shared_ptr<RequestCoordinator::Ticket> RequestCoordinator::admit(Action action) {
  // Explicit generic actions retain sequential semantics, including test/control callers.
  return admit({}, [action = std::move(action)]() mutable { return std::move(action); });
}
std::shared_ptr<RequestCoordinator::Ticket> RequestCoordinator::admit(Plan plan,
                                                                      Preparation prepare) {
  auto ticket = std::make_shared<Ticket>();
  {
    std::lock_guard lock(mutex_);
    if (!accepting_ || outstanding_ >= capacity_) return {};
    queue_.push_back({std::move(plan), std::move(prepare), ticket});
    ++outstanding_;
  }
  changed_.notify_one();
  return ticket;
}
void RequestCoordinator::stop_admission() {
  {
    std::lock_guard lock(mutex_);
    accepting_ = false;
  }
  changed_.notify_one();
}
void RequestCoordinator::drain() {
  stop_admission();
  if (coordination_.joinable()) coordination_.join();
  access_.drain();
  mutations_.drain();
}

bool RequestCoordinator::eligible(std::size_t index) const {
  const auto& plan = queue_[index].plan;
  const auto lane = plan.lane;
  if (lane == Lane::Control) return true;
  const bool access = lane == Lane::Query || lane == Lane::Load;
  if (access) {
    if (active_[lane_index(Lane::Query)] + active_[lane_index(Lane::Load)] >= access_workers_)
      return false;
  } else if (active_[lane_index(lane)] >= 1)
    return false;
  if (lane == Lane::Mutation) {
    // Preserve Mutation FIFO even when the first writer awaits an active reader.
    for (std::size_t i = 0; i < index; ++i)
      if (queue_[i].plan.lane == Lane::Mutation) return false;
    return !plan.entry || !readers_.contains(*plan.entry);
  }
  if (lane == Lane::Load && plan.entry) {
    if (writer_ == plan.entry) return false;
    // Later readers cannot repeatedly overtake an earlier conflicting writer.
    for (std::size_t i = 0; i < index; ++i)
      if (queue_[i].plan.lane == Lane::Mutation && queue_[i].plan.entry == plan.entry) return false;
  }
  return true;
}
void RequestCoordinator::reserve(const Work& work) {
  ++active_[lane_index(work.plan.lane)];
  if (work.plan.lane == Lane::Load && work.plan.entry) ++readers_[*work.plan.entry];
  if (work.plan.lane == Lane::Mutation) writer_ = work.plan.entry;
}
void RequestCoordinator::release(const Work& work) {
  --active_[lane_index(work.plan.lane)];
  if (work.plan.lane == Lane::Load && work.plan.entry) {
    const auto found = readers_.find(*work.plan.entry);
    if (--found->second == 0) readers_.erase(found);
  }
  if (work.plan.lane == Lane::Mutation) writer_.reset();
}
void RequestCoordinator::execute(Work work, Action action) {
  Result result;
  std::exception_ptr exception;
  try {
    result = action();
  } catch (...) {
    exception = std::current_exception();
    result = IpcError{IpcErrorCode::IoError, "Backend operation raised an unexpected exception"};
  }
  {
    std::lock_guard lock(work.ticket->mutex);
    work.ticket->result.emplace(std::move(result));
    work.ticket->exception = exception;
    work.ticket->stage = ExecutionStage::Finished;
  }
  // Release request/session captures before completion makes another request eligible.
  action = {};
  work.prepare = {};
  {
    std::lock_guard lock(mutex_);
    completed_.push_back(std::move(work));
  }
  changed_.notify_one();
}
void RequestCoordinator::coordinate() {
  for (;;) {
    Work work;
    {
      std::unique_lock lock(mutex_);
      changed_.wait(lock, [&] {
        if (!completed_.empty() || (!accepting_ && outstanding_ == 0)) return true;
        for (std::size_t i = 0; i < queue_.size(); ++i)
          if (eligible(i)) return true;
        return false;
      });
      while (!completed_.empty()) {
        auto finished = std::move(completed_.back());
        completed_.pop_back();
        release(finished);
        --outstanding_;
        // Waking connection workers must not invoke their callbacks under queue lock.
        lock.unlock();
        finished.ticket->completed.request_stop();
        lock.lock();
      }
      if (!accepting_ && outstanding_ == 0) return;
      std::size_t index = 0;
      while (index < queue_.size() && !eligible(index))
        ++index;
      if (index == queue_.size()) continue;
      work = std::move(queue_[index]);
      queue_.erase(queue_.begin() + index);
      reserve(work);
    }
    {
      std::lock_guard lock(work.ticket->mutex);
      work.ticket->stage = ExecutionStage::Executing;
    }
    // Reserve before dispatch. Access workers capture their own business state;
    // only short editing control / mutation protection runs on coordination.
    try {
      const auto lane = work.plan.lane;
      const bool access = lane == Lane::Query || lane == Lane::Load;
      Action action;
      if (access) {
        action = [prepare = std::move(work.prepare)]() mutable {
          auto run = prepare();
          prepare = {};
          return run();
        };
      } else {
        action = work.prepare();
        work.prepare = {};
      }
      if (lane == Lane::Control) {
        execute(std::move(work), std::move(action));
      } else {
        auto& pool = access ? access_ : mutations_;
        // Keep a copy until submission succeeds, so dispatch allocation failures
        // still complete the accepted ticket and release its reservation.
        pool.submit([this, work, action = std::move(action)]() mutable {
          execute(std::move(work), std::move(action));
        });
      }
    } catch (...) {
      auto exception = std::current_exception();
      execute(std::move(work), [exception]() -> Result { std::rethrow_exception(exception); });
    }
  }
}
} // namespace folio::application::internal
