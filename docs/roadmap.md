# Development roadmap

Status labels follow [architecture](architecture.md). Stage numbers are planning references, not persistence schema versions. All existing persisted schemas remain version 1. No dates or estimates are implied. This roadmap records stage-level scope and dependencies. Detailed subtasks and their ordering are intentionally omitted; define and review them when each stage begins.

| Stage | Purpose | Status / dependency |
| --- | --- | --- |
| Phase 1–3.3.7 | Core and Normal Storage foundation | Implemented; not a complete application |
| Phase D | Architecture documentation consolidation | This documentation delivery |
| Phase 3.4 | Platform paths and Session Marker | Effective paths, Cache relocation, and Marker implemented; platform/configuration defaults completed in 4.1c |
| Phase 4 | Public Application, Backend daemon, IPC | Process ownership, IPC/lifecycle, Configuration and Logging implemented; public operations pending |
| Phase 5 | Normal Trash and Recovery foundation | Planned; after Public API contracts |
| Phase 6 | Minimal Normal TUI/editor | Planned; after Recovery foundation |
| Phase 7 | Vault/encryption | Planned; after Normal workflow |
| Phase 8 | Vault-aware Recovery | Planned; extends Normal Recovery and Vault |
| Phase 9 | Complete TUI | Planned; after Vault-aware Recovery |
| Phase 10 | Git Sync | Planned; after complete application foundations |
| Phase 11 | Hardening and release | Planned; safety testing also occurs throughout |

## Phase 4 completion gate — Pending Backend safety audit

**BACKEND-01 is NOT completed. Phase 4 must not be marked complete until this
audit and any confirmed corrective work are finished. Schedule it before Normal
Mutation execution (Phase 4.3a), as already required.**

- [ ] Audit NormalRepository, MetadataCatalog, CatalogCoordinator, and their
  Application handling boundaries for failure containment.
- [ ] Verify nonfatal operation errors never terminate the Backend and are
  routed to the requesting Frontend, not broadcast to unrelated Frontends.
- [ ] Verify unsafe module state rejects affected operations and fatal,
  unresolvable service failures are recognizable by the lifecycle owner.
- [ ] Correct confirmed violations, add appropriate regression coverage, and
  record the audit findings and actual verification results.

The agreed principles are documented; implementation compliance has **not** been
established. Passing current tests and completing runtime refactors do not close
this gate. See [BACKEND-01](#todo-backend-01--backend-module-failure-containment-audit)
for scope. Deferred notification delivery remains in its planned phases.

## Completed foundation

Implemented: independent domain models and Snapshots, plaintext codecs, atomic filesystem utilities, NormalRepository, typed Catalog/reverse membership, MessagePack Index, Coordinator startup fallback/full Refresh/shutdown save, and optional Repository synchronization hooks. Post-audit corrections enforce unified UUID identity, classified post-publication failure notification, and efficient managed known-locator/creation operations. Evidence: [Storage sources](../server/src/storage/) and [tests](../tests/storage/).

Not completed by those milestones: Application integration, navigation count checks, configuration/platform defaults, daemon service/IPC, Vault, Trash, Journal, and Git. Marker-based unclean-session detection is implemented internally. The welcome TUI does not wire Storage lifecycle.

## Consolidation completion

Review linked architecture/decision documents and the separately proposed Storage amendments. Completion: current/planned distinctions, ownership boundaries, roadmap gates, and unresolved decisions are searchable; no code or schema changes.

## Storage foundation — implemented

After consolidation review, establish platform-standard configuration, data, state, cache, and runtime paths; refactor internal paths; relocate the Normal Index from State to Cache; and integrate the durable Session Marker with exclusive process/data ownership.

The effective StoragePaths/ApplicationPaths separation is implemented with explicitly injected paths. Platform default resolution and configuration defaults are implemented through Phase 4.1c. Normal Index relocation to `<application.cache>/index/normal.msgpack` is implemented; former State indexes are ignored without migration or deletion. Durable Session Marker and retryable ordered cleanup are implemented in Phase 3.4.3, with isolated tests for abandoned sessions, failed rebuild/save/sync, and Cache loss. OS process/data ownership and private bootstrap are implemented in Phase 4.1a; Backend lifecycle integration belongs to 4.1b; actual power-loss and cross-platform verification remain required. A Backend must not become Ready before its Marker is durable; failed persistence or Catalog inconsistency must preserve the Marker.

Authoritative roots stay configurable. One configuration per OS user; no Workspace Manager.

## Public Application and Backend — lifecycle and configuration implemented; public operations planned

Private same-executable launching and exclusive ownership are implemented in 4.1a. Connections, startup handshake and lifecycle follow in 4.1b; public APIs in 4.2; CLI/TUI operation integration in 4.4. No service readiness or Catalog lifecycle wiring is implied by process creation.

After the Storage lifecycle foundation, establish the public CLI/TUI-neutral Application boundary, on-demand Backend process, local IPC, serialized mutations, exclusive interactive edit locks, and push events. The Backend owns the Catalog, Coordinator, and Repositories. CLI operations and future navigation use the same public boundary.

Resolve launch/lock/connect races, public data and error representations, protocol versioning, request identity, unknown outcomes, revisions, event/reconnection behavior, and navigation count-check policy during design. Completion requires multiple-client and lifecycle tests, orderly persistence, and no private Storage leakage. Public request identity and unknown-outcome contracts must precede mutation retry behavior; durable Journal/draft implementation follows in Recovery. See [Application](application-design.md).

## Normal Recovery — planned

After Public Application contracts, implement Normal Trash/Restore, compound-operation planning, a durable Recovery Journal, RecoveryCandidate/Service support, and Frontend pending-operation and draft recovery.

Resolve nonempty Directory semantics, identity/path conflicts, intent/progress states, acknowledgment reconciliation, retention, and durable recovery locations. Completion requires fault-injection coverage and safe handling of unsubmitted edits, unknown outcomes, and acknowledged persistence without blind retries or ambiguous automatic repair. Simple single-file atomic writes do not require a blanket Journal.

## Minimal Normal TUI/editor — planned

After Normal Recovery, provide Directory navigation, Catalog search/filter/sort, editing contexts, saving, moves, Trash/Restore, and draft/reconnection recovery through the Public Application API.

Resolve the editor model, tabs, and external-editor policy before implementation. Completion requires a usable Normal workflow with isolated buffers and permission/revision-safe saves. The TUI must not call private Storage or claim Vault security before Vault exists.

## Vault/encryption — planned

After the Normal workflow, implement Vault initialization and key protection, authenticated Entry persistence, Public/Private indexes, lock/unlock and privacy visibility, and Frontend encrypt-only recovery drafts.

Review Argon2id/master-key protection, envelopes/versioning, key lifecycle, authoritative Directory representation, locked metadata exposure, and Index synchronization before implementation. The draft-encryption mechanism and key distribution remain deferred. Completion requires authenticated persistence for all Entry types, the agreed locked visibility, and no sensitive plaintext in durable recovery state or logs. Frontends must not possess Vault decryption keys.

## Vault-aware Recovery — planned

Extend Normal Recovery after Vault implementation with encrypted recovery data, cross-Normal/Vault operations, and pending Vault draft reconciliation.

Resolve lock-state/key availability and two-sided preparation/reconciliation contracts. Both sides must be durably prepared before coordinated mutation begins. Completion requires fault-injection coverage across mutation boundaries and safe reconnect/unlock recovery without plaintext Journal leakage or duplicate operations.

## Complete TUI — planned

After Vault-aware Recovery, integrate Vault lock/unlock, privacy placeholders, complete Recovery UI, full editing behavior, and search/filter/sort/navigation refinements. Review presentation and Public API requirements before implementation. Completion means end-to-end workflows reuse Backend rules rather than duplicating them.

## Git Sync — planned

After the complete application foundations, implement single-repository Normal/Vault branches and worktrees, local-only exclusions, status/sync, semantic conflict handling, and approved Vault Index coherence and history maintenance/compaction.

Resolve concurrent-operation/Recovery coordination, conflict rules, key availability, interrupted-sync recovery, and compatibility before implementation. Completion requires synthetic-repository tests preserving UUID identity and encryption boundaries. See [Git design](git-sync-design.md).

## Hardening and release — planned

Expand macOS/Linux/Windows filesystem, process, and IPC verification; complete crash/fault, migration/compatibility, performance/resource-limit, and security coverage; and provide packaging/install and user/developer documentation.

Resolve supported platforms/versions and release/migration policy first. Completion requires documented reproducible builds and validated failure behavior. Fundamental data safety remains a gate at every earlier stage, not deferred until release.

Phase 4 status: 4.1a is complete; 4.1a.5 reorganizes physical files and targets
without behavioral changes. The separately preserved 4.1b implementation awaits
review/acceptance and restoration after the structural commit. Phase 4.2 is not
started.


Phase 4 progress: private process ownership and local IPC connection/lifecycle
support are implemented through 4.1b. Public Application contracts and operations
remain the next design boundary; no CRUD, queue, Edit Locks or events exist yet.

## Configuration baseline — Phase 4.1c implemented

Validated built-in Normal/Vault roots and optional TOML overrides now form a
startup snapshot. Missing files are atomically created from the factory template; malformed existing
files fail explicitly. Platform paths remain separate. See
[Configuration](configuration.md). Phase 4.1d Logging is implemented after this baseline; Phase 4.2 public operations
remain unimplemented.

## Process Logging — Phase 4.1d implemented

One synchronous spdlog default logger per Backend/Frontend process records local
diagnostics in platform log directories. Session files are exclusive, timestamped
and never automatically rotated/deleted. The factory template supplies Warn-level
configuration; initialization precedes TOML loading. Lifecycle error retention and
IPC behavior remain separate from logging. See [Logging](logging.md). Public
operations, mutation queue, Edit Locks and Push Events remain deferred.

## Phase 4.2a — Public Normal read access implemented

BackendConnectionHandle provides Root/Directory browsing, scoped Record metadata
queries, Directory listing, and typed content loads through IPC. Public errors
and metadata warnings preserve meaningful causes without exposing locators.
See [read-access contracts](application-access.md). Phase 4.2b operation outcomes
and later mutation, Recovery, Vault, and event work remain deferred.

## Phase 4.2b — Mutation contract foundation

Public mutation identity and result/evidence value types are implemented for
review. The identity type and response members now use `MutationId` and
`mutation_id` consistently. Mutation execution, IPC messages, and finer internal persistence evidence
remain deferred. See [mutation contracts and follow-up decisions](application-mutations.md).

## Phase 4.2c — Unified IPC protocol extension implemented

Shared Query/Mutation codec components and bounded complete-message transport are
implemented with Protocol Version 1. Read payloads/API semantics are preserved.
No concrete Mutation request/success payload, dispatch or execution is enabled;
these belong to 4.3a using the shared sequential executor. Edit Locks remain 4.3b,
Push Events 4.4a, and Frontend integration/reconnection 4.4b.
See [IPC implementation scope](ipc-protocol.md) and traceable
[MUT follow-ups](application-mutations.md#traceable-follow-up-work), including the
internal schema-versioning review after Minimum TUI.

## Runtime architecture refactor before Normal Mutations — Implemented

IPC-01 replaces established-connection progress polling with OS waits. Bounded
connection workers admit into one shared in-memory FIFO; one executor processes
all accepted operations sequentially. Accepted/Rejected and final Response are
distinct stages. Orderly shutdown drains accepted work before existing Catalog
finalization and ownership release. macOS is verified; Linux/Windows execution
remains IPC-08 native verification work.

This supplies scheduling infrastructure before 4.3a, without implementing
Mutations. Concrete payloads, handlers and operation-specific admission remain
4.3a; Edit Locks 4.3b; Push Notifications 4.4a; Frontend UI/IPC Worker and
reconnection integration 4.4b. Batch/Draft Recovery remain independent later work.
Concurrent Reads and cancellation are open decisions. See
[IPC-01 through IPC-08](ipc-protocol.md#deferred-decisions) and existing
[MUT follow-ups](application-mutations.md#traceable-follow-up-work); no persistent
queue, receipt, replay, RequestId or exactly-once guarantee is implemented.

### TODO BACKEND-01 — Backend module failure-containment audit

**Deferred Implementation:** Schedule a small focused subphase before Normal
Mutation execution (Phase 4.3a) to audit `NormalRepository`, `MetadataCatalog`,
and `CatalogCoordinator` against the
[Backend module failure-containment principle](application-design.md#backend-module-failure-containment--decided-audit-deferred).
Existing compliance has not yet been established. Check that operational errors
do not terminate the process, invalid internal state rejects further affected
operations explicitly, errors remain collectable, and fatal unresolvable failures
are recognizable by the lifecycle owner. Confirm that nonfatal operation errors
remain request-scoped and do not abort service. If classification depends on
module state and its existing contract is ambiguous, bring the concrete case
back for design review rather than inventing a blanket error-code rule.
Review exception boundaries and add
focused failure-path tests; correct confirmed violations within that subphase.
The audit must preserve persistence and Session Marker safety and must not add a
generic executor health check. This TODO records future work, not a claim that
the current modules already satisfy every requirement.

## Runtime-role project structure — Implemented

Frontend-only implementation and headers now belong to app/, Backend-only code
and private Storage to server/, and shared Core/Config/Logging/I/O/contracts/IPC
to lib/. Each owns include/ and src/; app/src/main.cpp remains the deliberate
shared dispatcher. Static targets are folio_frontend, folio_backend and
folio_shared; one executable still supports both runtime processes. Shared wire
values and process error contracts no longer depend on role-owned headers.
This is a structural refactor preserving pending runtime work; it authorizes no
new behavior or later Phase 4 operations. See [module ownership](architecture.md#runtime-role-module-structure--implemented).

### BACKEND-02 — Preventable-hang review and agreed limitation

**Review performed / Not Guaranteed:** Inspection of current executor, IPC,
shutdown, Storage loops, and I/O progress handling found no confirmed
preventable-hang defect. Existing scheduler/IPC/lifecycle tests passed on macOS;
this is not proof that all hangs are excluded or native Linux/Windows verification.
Fix concrete preventable hangs when identified. Follow the
[agreed unpredictable-hang boundary](application-design.md#unpredictable-hangs--agreed-limitation):
operation completion and shutdown duration are not bounded, no unsafe thread
termination or finalization is permitted, and unconfirmed Mutations are not
replayed. Watchdogs, progress tracking, automatic termination, and new timeout
policies are not scheduled. This supersedes the earlier TODO proposing stall
detection in the next runtime subphase; reopen only for a concrete requirement
and design review.

### Platform release verification policy

Linux native runtime verification remains pending and should run when an
environment is available. Windows implementation may remain runtime-unverified
until release preparation. Native process/IPC/security/shutdown verification is
a gate for claiming Windows support, not a requirement to delay every platform's
first release. If Windows cannot be verified and corrected in time, omit Windows
support from the first release. No platform code is removed by this decision;
macOS results and cross-compilation do not establish native Windows correctness.
