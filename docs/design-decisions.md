# Design decision register

Status distinguishes implemented contracts from agreed future direction. This register records rationale, not new APIs. See [architecture](architecture.md), [roadmap](roadmap.md), [Application](application-design.md), [Recovery](recovery-design.md), [Vault](vault-design.md), and [Git](git-sync-design.md).

## D01 — Entry terminology

- **Status:** Implemented conceptual alignment.
- **Decision:** Entry includes Record (Log/Memo/Todo) and Directory; independent classes, shared UUID identity.
- **Motivation:** Avoid conceptual hierarchy while retaining genuine field/layout differences.
- **Alternatives considered:** Common base classes and treating Directory as a Storage helper.
- **Consequences:** Keep RecordType narrow; no schema change for naming.
- **Open/Deferred:** Cross-Normal/Vault identity enforcement and Vault Directory representation.

## D02 — Catalog-centric architecture

- **Status:** Implemented internal foundation.
- **Decision:** Files are authoritative; Catalog projects metadata, reverse membership and queries. Coordinator borrows owner-supplied Catalog.
- **Motivation:** Fast queries without making a cache authoritative.
- **Alternatives considered:** Repository-owned Catalog, duplicate active Catalogs, CRUD wrapper/service hierarchy.
- **Consequences:** Future Backend owns instances; explicit Repository hooks synchronize saved metadata.
- **Open/Deferred:** Public operation/error representations.

## D03 — Single-user configuration

- **Status:** Decided/Planned.
- **Decision:** One configuration per OS user with configurable Normal/Vault roots; no Multi-Workspace Manager.
- **Motivation:** Personal application; OS accounts separate users.
- **Alternatives considered:** Named Workspace registry, selector, management UUIDs.
- **Consequences:** One Backend ownership scope for that configuration.
- **Open/Deferred:** Config file format and path-change workflow.

## D04 — One Backend and multiple Frontends

- **Status:** Decided/Planned.
- **Decision:** On-demand separate Backend owns shared state; many CLI/TUI clients.
- **Motivation:** One coordinated authority avoids divergent state.
- **Alternatives considered:** One Backend per Frontend or global daemon spanning OS users.
- **Consequences:** OS lock and local per-user IPC required.
- **Open/Deferred:** Transport, authorization and launch/shutdown races.

## D05 — Single executable

- **Status:** Decided/Planned.
- **Decision:** Prefer folio CLI/TUI entry point plus internal Backend mode; default launches TUI.
- **Motivation:** Share deployment and public operations.
- **Alternatives considered:** Independent business logic per interface; running forked initialized Frontend as Backend.
- **Consequences:** Use executable process launch; no private Storage calls from UI.
- **Open/Deferred:** CLI grammar, internal mode and launcher handshake.

## D06 — Backend lifetime

- **Status:** Decided/Planned.
- **Decision:** Exit after last client disconnect and active work finishes.
- **Motivation:** On-demand personal application without permanent background service.
- **Alternatives considered:** Permanent daemon or idle grace period.
- **Consequences:** CLI-only invocation may start and stop Backend immediately.
- **Open/Deferred:** Admission races and active-operation cancellation.

## D07 — Serialized mutations

- **Status:** Decided/Planned.
- **Decision:** Backend queue serializes shared mutations including creation check through Catalog update.
- **Motivation:** Preserve Repo/Catalog consistency without global class thread safety.
- **Alternatives considered:** Independent simultaneous mutation of Storage objects.
- **Consequences:** CreationCheck is availability only, not reservation.
- **Open/Deferred:** Read scheduling and cancellation.

## D08 — Exclusive interactive Entry editing

- **Status:** Decided/Planned.
- **Decision:** Frontend requests Backend-owned per-Entry edit permission; readers continue.
- **Motivation:** Prevent competing interactive edits.
- **Alternatives considered:** Frontend-only lock state or making every mutation require edit locks.
- **Consequences:** Disconnect invalidates permissions; CLI respects applicable locks; move/rename/create need not inherently lock.
- **Open/Deferred:** Scoped edits, revision protection, exact protocol.

## D09 — Push events

- **Status:** Decided/Planned.
- **Decision:** Backend sends events over IPC; Frontend locally dispatches to UI.
- **Motivation:** React without filesystem polling.
- **Alternatives considered:** Cross-process callback pointers or Frontend filesystem watchers.
- **Consequences:** Receiver/event loop integration required.
- **Open/Deferred:** Payloads, revisions, subscriptions and reconnect delivery.

## D10 — Platform-standard paths

- **Status:** Effective-path model Implemented; platform defaults Decided/Planned.
- **Decision:** Separate config/data/state/cache/runtime using platform semantics.
- **Motivation:** Respect OS lifecycle and cleanup rules.
- **Alternatives considered:** Uniform forced folder hierarchy or configurable internal paths everywhere.
- **Consequences:** Implemented StoragePaths separates configurable authoritative roots from explicitly supplied ApplicationPaths; tests inject temporary paths.
- **Open/Deferred:** Exact platform paths and resolver APIs are deferred until configuration setup; the effective-path representation is implemented.

## D11 — Normal Index in Cache

- **Status:** Implemented (Phase 3.4.2); platform defaults remain deferred.
- **Decision:** Relocate rebuildable Normal Index to Cache; keep local-only, outside Git roots.
- **Motivation:** Index loss is recoverable; durable recovery data is not.
- **Alternatives considered:** Retaining rebuildable cache in persistent State.
- **Consequences:** Current location is `<application.cache>/index/normal.msgpack`. Former State indexes are not read, migrated, or deleted. Missing/unusable Cache triggers authoritative rebuilding, with persistence deferred until orderly shutdown.
- **Open/Deferred:** OS-specific Cache defaults are deferred to configuration setup; callers currently supply effective paths.

## D12 — Durable Session Marker

- **Status:** Internal Storage support Implemented (Phase 3.4.3); Backend/OS lock wiring deferred to Phase 4.
- **Decision:** Mark every Backend session durably before Ready; previous Marker forces rebuild.
- **Motivation:** Crash safety independent of enumerating every Catalog mutation path.
- **Alternatives considered:** Lazy first-mutation invalidation marker or trusting decoded Index after unclean exit.
- **Consequences:** Read-only crash may cause rebuild; failed save/inconsistency keeps Marker; lock distinct. Marker is application.state/session.marker; finish_session orders Index saving/sync before durable cleanup. shutdown_save alone retains the Marker. Failed cleanup can be retried; successful finalization stops the session.
- **Open/Deferred:** Backend/OS lock integration and platform power-loss validation; see [durability limits](architecture.md#durability-scope).

## D13 — Frontend recovery persistence

- **Status:** Decided/Planned.
- **Decision:** Preserve unsubmitted drafts and unknown-outcome submitted work in durable Recovery area.
- **Motivation:** Acknowledgment loss does not prove persistence failure.
- **Alternatives considered:** Cache drafts, blind retry, or treating disconnect as failed write.
- **Consequences:** Public API must support safe authoritative reconciliation.
- **Open/Deferred:** Operation IDs, revisions, acknowledgment algorithm and retention.

## D14 — Vault Frontend encrypt-only recovery

- **Status:** Decided/Planned security requirement.
- **Decision:** Frontend can encrypt durable Vault drafts without owning decryption keys.
- **Motivation:** Preserve drafts during Backend loss without expanding decryption authority.
- **Alternatives considered:** Plaintext durable drafts or sharing Vault decryption keys with UI.
- **Consequences:** Displayed plaintext may exist in Frontend memory; encrypt before persistence.
- **Open/Deferred:** Reviewed asymmetric mechanism, distribution and recovery protocol.

## D15 — Recovery before Minimal TUI

- **Status:** Decided/Planned.
- **Decision:** Normal Trash/Journal/Recovery foundation precedes usable editor; Vault-aware Recovery extends it.
- **Motivation:** Avoid shipping editing workflows without pending/partial failure safety.
- **Alternatives considered:** Postponing fundamental recovery until final hardening.
- **Consequences:** Public API recovery contracts first; implementation before Minimal TUI.
- **Open/Deferred:** Detailed Journal and Directory Trash semantics.

## D16 — Explicit internal Index shutdown saving

- **Status:** Implemented primitive; future owner wiring planned.
- **Decision:** shutdown_save is explicitly called normally once at orderly shutdown, with error handling/retry.
- **Motivation:** Saving needs observable errors; encoding alone is not persistence.
- **Alternatives considered:** Throwing destructor/RAII save or runtime once-only shutdown guard.
- **Consequences:** Successful atomic persistence acknowledges unpersisted; Inconsistent blocks save.
- **Open/Deferred:** Backend lifecycle/Marker integration; no periodic or per-Entry writes.

## D17 — Vault Index and Git domains

- **Status:** Decided/Planned.
- **Decision:** One Git repository, separate Normal/Vault branches/worktrees; Vault indexes synchronize, Normal Index does not.
- **Motivation:** Private Index avoids decoding all Entries after sync; Public Index permits locked visibility.
- **Alternatives considered:** Treating all indexes identically or paths as identities.
- **Consequences:** Indexes remain rebuildable; Vault Directory data needs independent authority.
- **Open/Deferred:** Semantic conflicts, encrypted index coherence and history compaction.
