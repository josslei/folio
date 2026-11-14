# Phase 4 implementation record

**Status:** historical milestone narrative, consolidated during documentation
cleanup. This records how capabilities arrived, not a live feature checklist or
new implementation instructions. Current scope is in [status](../status.md);
required behavior belongs to the [contracts](../../README.md).
Older scoped verification counts remain historical evidence.

## Completed foundation

Implemented: independent domain models and Snapshots, plaintext codecs, atomic filesystem utilities, NormalRepository, typed Catalog/reverse membership, MessagePack Index, Coordinator startup fallback/full Refresh/shutdown save, and optional Repository synchronization hooks. Post-audit corrections enforce unified UUID identity, classified post-publication failure notification, and efficient managed known-locator/creation operations. Evidence: [Storage sources](../../../server/src/storage) and [tests](../../../tests/storage).

Those Storage milestones did not themselves implement Application integration or platform configuration. These subsequently landed through Phase 4.3a. Public navigation count checks, Vault, Trash, Journal and Git remain deferred; the welcome TUI is not a complete editor.

## Consolidation completion

Review linked architecture/decision documents and the separately proposed Storage amendments. Completion: current/planned distinctions, ownership boundaries, roadmap gates, and unresolved decisions are searchable; no code or schema changes.

## Storage foundation — implemented

After consolidation review, establish platform-standard configuration, data, state, cache, and runtime paths; refactor internal paths; relocate the Normal Index from State to Cache; and integrate the durable Session Marker with exclusive process/data ownership.

The effective StoragePaths/ApplicationPaths separation is implemented with explicitly injected paths. Platform default resolution and configuration defaults are implemented through Phase 4.1c. Normal Index relocation to `<application.state>/index/normal.msgpack` is implemented; former Cache indexes are ignored without migration or deletion. Durable Session Marker and retryable ordered cleanup are implemented in Phase 3.4.3, with isolated tests for abandoned sessions, failed rebuild/save/sync, and Cache loss. OS process/data ownership and private bootstrap are implemented in Phase 4.1a; Backend lifecycle integration is implemented in 4.1b; actual power-loss and cross-platform verification remain required. A Backend must not become Ready before its Marker is durable; failed persistence or Catalog inconsistency must preserve the Marker.

Authoritative roots stay configurable. One configuration per OS user; no Workspace Manager.

## Public Application and Backend — Normal operations implemented; integration remains

Private same-executable launching and exclusive ownership are implemented in 4.1a. Connections, startup handshake and lifecycle are implemented in 4.1b; public Normal Access, Mutation contracts and execution are implemented through 4.3a; fuller CLI/TUI integration remains in 4.4. No service readiness or Catalog lifecycle wiring is implied by process creation.

The public CLI/TUI-neutral Application boundary, on-demand Backend, local IPC, sequential executor and Entry Edit Locks are implemented. Push Events remain deferred. The Backend owns the Catalog, Coordinator, and Repositories. CLI operations and future navigation use the same public boundary.

Resolve launch/lock/connect races, public data and error representations, protocol versioning, request identity, unknown outcomes, revisions, event/reconnection behavior, and navigation count-check policy during design. Completion requires multiple-client and lifecycle tests, orderly persistence, and no private Storage leakage. Public request identity and unknown-outcome contracts must precede mutation retry behavior; durable Journal/draft implementation follows in Recovery. See [Application](../../contracts/backend-lifecycle.md).

## Configuration baseline — Phase 4.1c implemented

Validated built-in Normal/Vault roots and optional TOML overrides now form a
startup snapshot. Missing files are atomically created from the factory template; malformed existing
files fail explicitly. Platform paths remain separate. See
[Configuration](../../contracts/configuration.md). Logging and public Normal operations are subsequently implemented; this configuration milestone did not itself implement them.

## Process Logging — Phase 4.1d implemented

One synchronous spdlog default logger per Backend/Frontend process records local
diagnostics in platform log directories. Session files are exclusive, timestamped
and never automatically rotated/deleted. The factory template supplies Warn-level
configuration; initialization precedes TOML loading. Lifecycle error retention and
IPC behavior remain separate from logging. See [Logging](../../contracts/logging.md). Public Normal operations, the sequential request queue
and Edit Locks are now implemented separately; Push Events remain deferred.

## Phase 4.2a — Public Normal read access implemented

BackendConnectionHandle provides Root/Directory browsing, scoped Record metadata
queries, Directory listing, and typed content loads through IPC. Public errors
and metadata warnings preserve meaningful causes without exposing locators.
See [read-access contracts](../../contracts/access.md). Mutation contracts and Normal
execution are implemented subsequently through 4.3a; Recovery, Vault and events remain deferred.

## Phase 4.2b — Mutation contract foundation

Public mutation identity and result/evidence value types are implemented for
review. The identity type and response members now use `MutationId` and
`mutation_id` consistently. Concrete IPC messages, execution and finer persistence
evidence are subsequently implemented in 4.3a; persistent Recovery remains deferred. See [mutation contracts and follow-up decisions](../../contracts/mutations.md).

## Phase 4.2c — Unified IPC protocol extension implemented

Shared Access/Mutation codec components and bounded complete-message transport are
implemented with Protocol Version 1. Read payloads/API semantics are preserved.
This phase introduced no concrete Mutation request/success payload, dispatch or
execution. These are now implemented in 4.3a using the shared sequential executor,
with Edit Locks brought forward into that approved scope. Push Events remain
4.4a, and Frontend integration/reconnection 4.4b.
See [IPC implementation scope](../../contracts/ipc.md) and traceable
[MUT follow-ups](../../contracts/mutations.md#traceable-follow-up-work), including the
internal schema-versioning review after Minimum TUI.

## Runtime architecture refactor before Normal Mutations — Implemented

IPC-01 replaces established-connection progress polling with OS waits. Bounded
connection workers admit into one shared in-memory FIFO; one executor processes
all accepted operations sequentially. Accepted/Rejected and final Response are
distinct stages. Orderly shutdown drains accepted work before existing Catalog
finalization and ownership release. macOS is verified; Linux/Windows execution
remains IPC-08 native verification work.

This supplies scheduling infrastructure before 4.3a, without implementing
Mutations in that refactor. Concrete payloads, handlers, operation-specific
preconditions and Edit Locks are subsequently implemented in 4.3a. Push Notifications
remain 4.4a; Frontend UI/IPC Worker and
reconnection integration 4.4b. Batch/Draft Recovery remain independent later work.
Concurrent Reads and cancellation are open decisions. See
[IPC-01 through IPC-08](../../contracts/ipc.md#deferred-decisions) and existing
[MUT follow-ups](../../contracts/mutations.md#traceable-follow-up-work); no persistent
queue, receipt, replay, RequestId or exactly-once guarantee is implemented.

### BACKEND-01 — Backend module failure-containment audit completed

**Completed:** Reviewed `NormalRepository`, `MetadataCatalog`, `CatalogCoordinator`,
Application Query handlers and runtime/lifecycle containment against the
[containment principle](../../contracts/backend-lifecycle.md#module-failure-containment).
No confirmed production-code defect was found. Added three tests and extended
the two-Frontend integration coverage; all 378 normal tests passed on macOS.
See the [audit report](backend-failure-containment-audit.md) for reviewed state
transitions, exception boundaries and limitations. Persistence and Session Marker
algorithms are unchanged; no generic executor health check was introduced.

**Implemented — Phase 4.3a:** Normal Mutation handlers apply pre-write readiness,
current-state, membership and lock checks and structured failure/evidence mapping.
Post-persistence callbacks do not replace these preconditions. New operations must
extend the same failure-containment checks and tests.

## Runtime-role project structure — Implemented

Frontend-only implementation and headers now belong to app/, Backend-only code
and private Storage to server/, and shared Core/Config/Logging/I/O/contracts/IPC
to lib/. Each owns include/ and src/; app/src/main.cpp remains the deliberate
shared dispatcher. Static targets are folio_frontend, folio_backend and
folio_shared; one executable still supports both runtime processes. Shared wire
values and process error contracts no longer depend on role-owned headers.
This is a structural refactor preserving pending runtime work; it authorizes no
new behavior or later Phase 4 operations. See [module ownership](../../architecture/system.md#runtime-role-module-structure--implemented).

## Phase 4.3a — Normal Mutations and Edit Locks implemented

This approved scope brings Edit Locks forward from the earlier 4.3b plan. Normal
Create/Update/Move, Directory Create/Rename and immediate Todo status changes now
execute through the shared bounded queue. Full Editable Fields preserve current
identity, creation, privacy and membership; Move manages membership/location;
Frontend-supplied Modification Context is shared operation context. Backend creates
Entry UUIDs; Frontend supplies MutationId. Confirmed successes return Core metadata.

Connection-owned Edit Locks and authoritative acquisition share the executor.
Accepted requests retain ownership after disconnect; cleanup cannot jump ahead of
queued work. Per-step persistence evidence distinguishes NotApplied, Applied,
PartiallyApplied and Unknown. Catalog/Marker finalization and runtime states are
unchanged. See [contracts and remaining TODOs](../../contracts/mutations.md).

- **Deferred Implementation — remaining 4.3b:** explicit Full Refresh and Directory
  Count Consistency Checks; locks themselves are now implemented, without authorizing
  the rest of that phase.
- **Deferred Implementation — 4.4a/4.4b:** Push/shared lifecycle notifications and
  Frontend UI/IPC worker integration, bounded reconnection and presentation.
- **Deferred Implementation — Recovery/Trash phases:** persistent drafts, compound
  Move resolution, bootstrap artifact reconciliation, Delete/Trash and Vault-safe
  Recovery. Scheduling and weak lock lifetime are not persistent Recovery.
- **Open Design Decision:** revision checking, concurrent execution, mutation
  receipts/cross-restart lookup, deduplication/idempotency and stronger platform
  durability remain tracked by MUT/IPC TODOs; no new guarantees are implied.
- **Not Guaranteed:** exactly-once execution, automatic replay safety or proof of
  execution from MutationId. Windows location-changing Moves cannot currently
  confirm source-removal durability and report Unknown rather than durable success.
