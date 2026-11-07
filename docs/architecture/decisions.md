# Design decision register

Status distinguishes implemented contracts from agreed future direction. This register records rationale, not new APIs. See [architecture](system.md), [roadmap](../development/roadmap.md), [Application](../contracts/backend-lifecycle.md), [Recovery](../development/plans/recovery.md), [Vault](../development/plans/vault.md), and [Git](../development/plans/git-sync.md).

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
- **Consequences:** BackendContext owns instances; peer orchestrators borrow them and matching Repository/Coordinator hooks synchronize saved metadata.
- **Open/Deferred:** Vault and later operation/error extensions; Normal public contracts are implemented.

## D03 — Single-user configuration

- **Status:** Implemented baseline; future extensions remain open.
- **Decision:** One configuration per OS user with configurable Normal/Vault roots; no Multi-Workspace Manager.
- **Motivation:** Personal application; OS accounts separate users.
- **Alternatives considered:** Named Workspace registry, selector, management UUIDs.
- **Consequences:** One Backend ownership scope for that configuration.
- **Open/Deferred:** Interactive configuration editing and migration; factory-template format and restart-based startup snapshots are implemented.

## D04 — One Backend and multiple Frontends

- **Status:** Implemented baseline; future extensions remain open.
- **Decision:** On-demand separate Backend owns shared state; multiple Frontends connect through IPC.
- **Motivation:** One coordinated authority avoids divergent state.
- **Alternatives considered:** One Backend per Frontend or global daemon spanning OS users.
- **Consequences:** OS ownership and per-user IPC are implemented; native Linux/Windows runtime verification remains pending.
- **Open/Deferred:** Native platform access-control and launch/shutdown verification; see IPC-08.

## D05 — Single executable

- **Status:** Implemented baseline; future extensions remain open.
- **Decision:** Prefer folio CLI/TUI entry point plus internal Backend mode; default launches TUI.
- **Motivation:** Share deployment and public operations.
- **Alternatives considered:** Independent business logic per interface; running forked initialized Frontend as Backend.
- **Consequences:** Use executable process launch; no private Storage calls from UI.
- **Open/Deferred:** Fuller CLI grammar and TUI operation integration; internal mode and launcher handshake are implemented.

## D06 — Backend lifetime

- **Status:** Implemented baseline; future extensions remain open.
- **Decision:** Stop admission after the last Frontend disconnects, drain accepted work, then finalize and release resources.
- **Motivation:** On-demand personal application without permanent background service.
- **Alternatives considered:** Permanent daemon or idle grace period.
- **Consequences:** CLI-only invocation may start and stop Backend immediately.
- **Open/Deferred:** Request cancellation and arbitrary operation-completion bounds; no cancellation guarantee exists.

## D07 — Serialized mutations

- **Status:** Implemented baseline; future extensions remain open.
- **Decision:** Backend queue serializes shared mutations including creation check through Catalog update.
- **Motivation:** Preserve Repo/Catalog consistency without global class thread safety.
- **Alternatives considered:** Independent simultaneous mutation of Storage objects.
- **Consequences:** CreationCheck is availability only, not reservation.
- **Open/Deferred:** Concurrent Read execution and cancellation; all current Access and Mutation execution is sequential.

## D08 — Exclusive interactive Entry editing

- **Status:** Implemented baseline; future extensions remain open.
- **Decision:** Frontend requests Backend-owned per-Entry edit permission; readers continue.
- **Motivation:** Prevent competing interactive edits.
- **Alternatives considered:** Frontend-only lock state or making every mutation require edit locks.
- **Consequences:** Accepted actions retain connection ownership after disconnect. Full Update requires an owned lock; immediate Move/Rename/status rejects another owner but needs no temporary lock when unlocked. See the [lock contract](../contracts/mutations.md#edit-lock-ownership--implemented).
- **Open/Deferred:** Revision protection and future mutation-specific concurrency rules; current lock protocol is implemented.

## D09 — Push events

- **Status:** Decided/Planned.
- **Decision:** Backend sends events over IPC; Frontend locally dispatches to UI.
- **Motivation:** React without filesystem polling.
- **Alternatives considered:** Cross-process callback pointers or Frontend filesystem watchers.
- **Consequences:** Receiver/event loop integration required.
- **Open/Deferred:** Payloads, revisions, subscriptions and reconnect delivery.

## D10 — Platform-standard paths

- **Status:** Implemented effective paths and platform defaults.
- **Decision:** Separate config/data/state/cache/logs/runtime using platform semantics.
- **Motivation:** Respect OS lifecycle and cleanup rules.
- **Alternatives considered:** Uniform forced folder hierarchy or configurable internal paths everywhere.
- **Consequences:** Implemented StoragePaths separates configurable authoritative roots from explicitly supplied ApplicationPaths; tests inject temporary paths.
- **Open/Deferred:** Native platform verification; exact paths and resolver APIs are documented in [Configuration](../contracts/configuration.md).

## D11 — Normal Index in Cache

- **Status:** Implemented, including platform defaults.
- **Decision:** Relocate rebuildable Normal Index to Cache; keep local-only, outside Git roots.
- **Motivation:** Index loss is recoverable; durable recovery data is not.
- **Alternatives considered:** Retaining rebuildable cache in persistent State.
- **Consequences:** Current location is `<application.cache>/index/normal.msgpack`. Former State indexes are not read, migrated, or deleted. Missing/unusable Cache triggers authoritative rebuilding, with persistence deferred until orderly shutdown.
- **Open/Deferred:** Native platform verification; current Cache location, defaults and injected effective paths are implemented.

## D12 — Durable Session Marker

- **Status:** Implemented Storage support and Backend/OS ownership integration.
- **Decision:** Mark every Backend session durably before Ready; previous Marker forces rebuild.
- **Motivation:** Crash safety independent of enumerating every Catalog mutation path.
- **Alternatives considered:** Lazy first-mutation invalidation marker or trusting decoded Index after unclean exit.
- **Consequences:** Read-only crash may cause rebuild; failed save/inconsistency keeps Marker; lock distinct. Marker is application.state/session.marker; finish_session orders Index saving/sync before durable cleanup. shutdown_save alone retains the Marker. Failed cleanup can be retried; successful finalization stops the session.
- **Open/Deferred:** Platform power-loss validation; see [durability limits](../contracts/storage.md#durability-scope).

## D13 — Frontend recovery persistence

- **Status:** Decided/Planned.
- **Decision:** Preserve unsubmitted drafts and unknown-outcome submitted work in durable Recovery area.
- **Motivation:** Acknowledgment loss does not prove persistence failure.
- **Alternatives considered:** Cache drafts, blind retry, or treating disconnect as failed write.
- **Consequences:** Public API must support safe authoritative reconciliation.
- **Open/Deferred:** Draft Recovery correlation using `MutationId`, revisions, acknowledgment algorithm and retention. Mutation identity naming is decided; persistent receipts and cross-restart lookup remain open. See [mutation follow-up work](../contracts/mutations.md#traceable-follow-up-work).

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

- **Status:** Implemented primitive and explicit Backend finalization wiring.
- **Decision:** shutdown_save is explicitly called normally once at orderly shutdown, with error handling/retry.
- **Motivation:** Saving needs observable errors; encoding alone is not persistence.
- **Alternatives considered:** Throwing destructor/RAII save or runtime once-only shutdown guard.
- **Consequences:** Successful atomic persistence acknowledges unpersisted; Inconsistent blocks save.
- **Open/Deferred:** Stronger platform durability verification; no periodic or per-Entry Index writes are introduced.

## D17 — Vault Index and Git domains

- **Status:** Decided/Planned.
- **Decision:** One Git repository, separate Normal/Vault branches/worktrees; Vault indexes synchronize, Normal Index does not.
- **Motivation:** Private Index avoids decoding all Entries after sync; Public Index permits locked visibility.
- **Alternatives considered:** Treating all indexes identically or paths as identities.
- **Consequences:** Indexes remain rebuildable; Vault Directory data needs independent authority.
- **Open/Deferred:** Semantic conflicts, encrypted index coherence and history compaction.
