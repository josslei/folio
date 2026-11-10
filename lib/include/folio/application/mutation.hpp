#pragma once

#include <folio/core/types.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <folio/core/directory.hpp>
#include <folio/core/log.hpp>
#include <folio/core/memo.hpp>
#include <folio/core/todo.hpp>
#include <variant>
#include <vector>
#include <folio/application/error.hpp>
#include <folio/application/query.hpp>

namespace folio::application {

// Mutation identity

// Identifies one mutation intention, not an Entry or Draft. Correlation alone
// provides no deduplication, receipt, or cross-restart execution guarantee.
class MutationId {
public:
  /// Generates a secure UUID for one Frontend mutation intention before submission.
  /// Retain it across investigation of an unconfirmed result; never assume replay
  /// is safe merely because the same ID can be submitted again.
  [[nodiscard]] static MutationId generate();
  [[nodiscard]] static std::optional<MutationId> parse(std::string_view text);
  [[nodiscard]] std::string to_string() const;

  bool operator==(const MutationId&) const = default;

private:
  explicit MutationId(core::Id value) : value_(value) {}

  core::Id value_;
};

// Editing data

// Full editable fields. Identity, creation, privacy and membership are excluded.
struct LogEditableFields {
  std::string title;
  std::string content;
  core::DateContext log_date;
  std::vector<core::Tag> tags;
};
struct MemoEditableFields {
  std::string title;
  std::string content;
  std::vector<core::Tag> tags;
};
struct TodoEditableFields {
  std::string title;
  std::string notes;
  core::TodoStatus status;
  core::TodoPriority priority;
  std::optional<core::DueDate> due_date;
  std::vector<core::Subtask> subtasks;
};

// Mutation outcomes and evidence

// Describes the intended authoritative change, independently of failure cause.
enum class AuthoritativeChangeStatus {
  NotApplied,       // Intended change was not published; artifacts may remain.
  Applied,          // Required authoritative durability steps completed.
  PartiallyApplied, // Confirmed durable partial effects of a compound mutation.
  Unknown           // Evidence does not establish the final authoritative outcome.
};

enum class MutationStep { WriteEntry, CreateDestination, RemoveSource };

enum class StepObservation {
  NotAttempted,
  NotCompleted, // This step is confirmed not to have published its change.
  Published,    // Publication known, required durability not confirmed.
  DurabilityConfirmed,
  Unknown
};

struct MutationStepEvidence {
  MutationStep step;
  StepObservation observation;
};

// Historical execution observations, not current-state guarantees or permission
// to retry/repair. Each future operation defines its applicable steps and order.
// Logical locations contain no private filesystem locators or Record contents.
struct MutationEvidence {
  core::Id entry_id;
  std::optional<DirectoryRef> source;
  std::optional<DirectoryRef> destination;
  std::vector<MutationStepEvidence> steps;
};

struct MutationFailure {
  ApplicationError error;
  AuthoritativeChangeStatus status;
  MutationEvidence evidence;
};

// A trustworthy Backend response. Success requires both authoritative durability
// and necessary Application synchronization. The echoed ID must match the request.
template <class T> struct MutationResponse {
  MutationId mutation_id;
  core::Result<T, MutationFailure> result;
};

enum class SubmissionState {
  NotSent,
  MayHaveBeenSent, // Does not establish Backend receipt or acceptance.
  Rejected,        // Admission explicitly rejected; no execution was accepted.
  Accepted         // Session-only admission, still no confirmed final outcome.
};

// Frontend observation only: never fabricate a Backend MutationFailure from a
// lost/malformed response. Retain the original intention and ID; do not replay.
struct NoMutationResponse {
  MutationId mutation_id;
  ApplicationError error;
  SubmissionState submission;
};

/// Outer error means no confirmed final outcome; inner response.result may still
/// be a confirmed MutationFailure, including Applied or Unknown. Acceptance alone
/// is not final confirmation. See docs/contracts/mutations.md for recovery limits.
template <class T> using MutationResult = core::Result<MutationResponse<T>, NoMutationResponse>;

} // namespace folio::application
