# Architectural baseline

**Purpose:** compact shared context for humans and agents. Read this before
structural or behavioral work, then follow the affected contracts. A text-label
change does not require loading every design document. When impact is uncertain,
broaden the review before implementation.

## Critical boundaries

- One executable dispatches Frontend and Backend processes. `app/` and `server/`
  depend on shared `lib/`, never on each other; `app/src/main.cpp` is the deliberate
  dispatcher exception. Frontends use IPC, never Backend-private Storage.
- Entry means Log, Memo, Todo or Directory; Record excludes Directory. These
  concepts imply no common base class. Logical Roots have no fabricated UUID;
  Memo/Todo Directories form one level, not a nested hierarchy.
- Entry files are authoritative. MetadataCatalog and Index are projections.
  `unpersisted` means metadata is not saved to Index, not that user content is
  unsaved. Inconsistent Catalog state cannot certify a clean Index/session.
- BackendContext owns resources. AccessOrchestrator and MutationOrchestrator
  borrow Storage; Repository persistence invokes matching Coordinator callbacks.
  Do not add a second Catalog synchronization path or implicit repair scan.
- IPC workers admit and deliver; one bounded shared queue and one executor run
  Application operations. Workers must not access live Catalog/Repository state.
- Accepted is session-only responsibility, not execution success or durability.
  Disconnecting does not cancel accepted work. No automatic Mutation replay.
- BackendStateMachine enforces Bootstrap → Serve → Stopping → Report → Finish.
  Failure may bypass Stopping into Report. Drain accepted work and close/join
  workers before eligible Catalog finalization; retain RepositoryOwnership until
  afterward. Cleanup must be attempted on ordinary failure paths.
- Modules return operational errors instead of terminating the process. Unsafe
  state rejects affected work; recognizable fatal service failures reach the
  lifecycle owner. Draining collects failures and attempts remaining accepted
  work; it does not prove that operations succeeded or that state is healthy.
- Failure cause, authoritative outcome, execution evidence and confirmation are
  separate. Publication is not durability; lost confirmation is not NotApplied.
  MutationId correlates an intention, not a receipt or exactly-once execution.
- Update applies Full Editable Fields to current authoritative state; Move owns
  membership/location. Frontend Modification Context is shared, not a version.
  Connection-owned Edit Locks remain alive while accepted actions retain their
  session token. Immediate mutations respect another owner's lock.
- Logging records diagnostics without controlling errors, lifecycle or IPC.
  Never log Entry bodies, raw payloads or secrets. Future Vault-sensitive durable
  drafts require encryption; permitted in-memory plaintext is a different concern.

## Current scope and limits

Normal Access operations, Normal Mutations, Entry Edit Locks, Configuration,
Logging and event-driven Backend IPC are implemented. The welcome TUI is not a
complete editor. Push notifications, Full Refresh/count-check public integration,
Recovery, Vault and Git Sync remain deferred. Concurrent execution, cancellation,
revision checking and mutation receipts require further decisions.

No bounded operation completion or orderly shutdown is guaranteed for arbitrary
hangs. Do not forcibly terminate operation threads or finalize Storage beneath
active work. Native runtime verification must be reported per platform; macOS
results do not establish Linux or Windows support.

## Read next

- [Principles index](principles.md): precise contracts, code and verification entry points.
- [System explanation and Backend hierarchy](system.md#backend-responsibility-overview): detailed diagrams and rationale.
- [Documentation index](../README.md): task-based navigation and source authority.
- [Roadmap](../development/roadmap.md): sequencing and open/deferred obligations, not authorization.
