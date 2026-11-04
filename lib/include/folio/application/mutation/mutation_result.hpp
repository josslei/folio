#pragma once

#include <folio/application/error.hpp>
#include <folio/application/mutation/mutation_id.hpp>
#include <folio/application/query.hpp>

#include <optional>
#include <vector>

namespace folio::application {

// Describes the intended authoritative change, independently of failure cause.
enum class AuthoritativeChangeStatus {
  NotApplied,       // Intended change was not published; artifacts may remain.
  Applied,          // Required authoritative durability steps completed.
  PartiallyApplied, // Confirmed durable partial effects of a compound mutation.
  Unknown          // Evidence does not establish the final authoritative outcome.
};

enum class MutationStep { WriteEntry, CreateDestination, RemoveSource };

enum class StepObservation {
  NotAttempted,
  NotCompleted,        // This step is confirmed not to have published its change.
  Published,           // Publication known, required durability not confirmed.
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
template<class T>
struct MutationResponse {
  MutationId mutation_id;
  core::Result<T, MutationFailure> result;
};

enum class SubmissionState {
  NotSent,
  MayHaveBeenSent  // Does not establish Backend receipt or acceptance.
};

// Frontend observation only: never fabricate a Backend MutationFailure from a
// lost/malformed response. Retain the original intention and ID; do not replay.
struct NoMutationResponse {
  MutationId mutation_id;
  ApplicationError error;
  SubmissionState submission;
};

template<class T>
using MutationResult = core::Result<MutationResponse<T>, NoMutationResponse>;

}  // namespace folio::application
