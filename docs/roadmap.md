# Development roadmap

Status labels follow [architecture](architecture.md). Stage numbers are planning references, not persistence schema versions. All existing persisted schemas remain version 1. No dates or estimates are implied. This roadmap records stage-level scope and dependencies. Detailed subtasks and their ordering are intentionally omitted; define and review them when each stage begins.

| Stage | Purpose | Status / dependency |
| --- | --- | --- |
| Phase 1–3.3.7 | Core and Normal Storage foundation | Implemented; not a complete application |
| Phase D | Architecture documentation consolidation | This documentation delivery |
| Phase 3.4 | Platform paths and Session Marker | Internal effective paths, Cache relocation, and Marker implemented; OS defaults deferred |
| Phase 4 | Public Application, Backend daemon, IPC | In progress: private process launch/OS ownership implemented; connections and public APIs pending |
| Phase 5 | Normal Trash and Recovery foundation | Planned; after Public API contracts |
| Phase 6 | Minimal Normal TUI/editor | Planned; after Recovery foundation |
| Phase 7 | Vault/encryption | Planned; after Normal workflow |
| Phase 8 | Vault-aware Recovery | Planned; extends Normal Recovery and Vault |
| Phase 9 | Complete TUI | Planned; after Vault-aware Recovery |
| Phase 10 | Git Sync | Planned; after complete application foundations |
| Phase 11 | Hardening and release | Planned; safety testing also occurs throughout |

## Completed foundation

Implemented: independent domain models and Snapshots, plaintext codecs, atomic filesystem utilities, NormalRepository, typed Catalog/reverse membership, MessagePack Index, Coordinator startup fallback/full Refresh/shutdown save, and optional Repository synchronization hooks. Post-audit corrections enforce unified UUID identity, classified post-publication failure notification, and efficient managed known-locator/creation operations. Evidence: [Storage sources](../lib/src/storage/) and [tests](../tests/storage/).

Not completed by those milestones: Application integration, navigation count checks, configuration/platform defaults, daemon service/IPC, Vault, Trash, Journal, and Git. Marker-based unclean-session detection is implemented internally. The welcome TUI does not wire Storage lifecycle.

## Consolidation completion

Review linked architecture/decision documents and the separately proposed Storage amendments. Completion: current/planned distinctions, ownership boundaries, roadmap gates, and unresolved decisions are searchable; no code or schema changes.

## Storage foundation — internal lifecycle implemented; platform defaults deferred

After consolidation review, establish platform-standard configuration, data, state, cache, and runtime paths; refactor internal paths; relocate the Normal Index from State to Cache; and integrate the durable Session Marker with exclusive process/data ownership.

The effective StoragePaths/ApplicationPaths separation is implemented with explicitly injected paths. Platform default resolution is deferred until configuration setup near application completion. Normal Index relocation to `<application.cache>/index/normal.msgpack` is implemented; former State indexes are ignored without migration or deletion. Durable Session Marker and retryable ordered cleanup are implemented in Phase 3.4.3, with isolated tests for abandoned sessions, failed rebuild/save/sync, and Cache loss. OS process/data ownership and private bootstrap are implemented in Phase 4.1a; Backend lifecycle integration belongs to 4.1b; actual power-loss and cross-platform verification remain required. A Backend must not become Ready before its Marker is durable; failed persistence or Catalog inconsistency must preserve the Marker.

Authoritative roots stay configurable. One configuration per OS user; no Workspace Manager.

## Public Application and Backend — process foundation implemented; integration planned

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
