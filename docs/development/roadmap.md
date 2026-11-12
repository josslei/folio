# Development roadmap

**Purpose:** future sequencing, scope and completion gates. A roadmap does not
authorize implementation. See [current status](status.md), [completed milestone
record](reports/phase-4-implementation.md) and the [cleanup plan](documentation-harness-plan.md).
Stage numbers are planning references, not persistence or IPC schema versions.
No dates or estimates are implied. Define detailed subtasks when each phase begins.

## Remaining Phase 4

Documentation and harness cleanup precedes the next feature subphase. Normal
Mutations and Edit Locks are implemented in 4.3a; do not schedule locks again as
unimplemented 4.3b work.

The focused baseline of the
[Catalog/concurrent Access refactor](plans/catalog-concurrency.md#implementation-timing)
precedes remaining 4.3b. The plan also records later features; completing the entire
document is not a gate for 4.3b. Push delivery remains in 4.4a and Frontend
integration in 4.4b; optimizations and other deferred features keep their own
follow-ups. Follow the [agreed implementation/review sequence](plans/catalog-concurrency.md#agreed-implementation-sequence),
allowing corrections between rounds. No new subphase number is assigned. Recording
the design does not authorize code changes, new dependencies or Git operations.

| Stage | Remaining responsibility |
| --- | --- |
| Cleanup | Documentation structure/instructions, harness tooling, code documentation and navigation/enforcement checks |
| Pre-4.3b refactor — implemented baseline | Immutable Catalog views, ownership-only acquisition and concurrent Access with short RequestCoordinator scheduling; one Mutation worker, bounded Loads and integrated conflict/drain verification. Push handoff remains 4.4a. |
| 4.3b — Consistency Checking & Catalog Refresh | Implemented Backend Consistency Check, unified explicit Scoped/Full Refresh and safe reconciliation; navigation UI/Push remain 4.4 work |
| 4.4a — implemented transport | Paired-session Push/ACK, bounded safe broadcast invalidations, request-time retention, lock cleanup and Report delivery; Mutation final results remain separate; complete UI handling belongs to 4.4b |
| 4.4b | Functional Frontend integration, UI/IPC worker, bounded reconnection, buffers and outcome presentation; not the full editor/Recovery |

The [approved Consistency Checking & Catalog Refresh design](plans/catalog-refresh.md) keeps Access
running while Refresh excludes Mutations, with nonblocking coordination and
completion-first dispatch. Scope semantics, navigation triggers and explicit user
Refresh are implemented. Scoped publication preserves global Inconsistent state.
**Important next scheduling design: CONS-03 priority system, with Refresh at highest
priority**; combined fairness/starvation policy remains open. Checks initially
use ordinary eligible selection without a special priority rule.

## Deferred obligations and guarantees

[MUT follow-ups](../contracts/mutations.md#traceable-follow-up-work) own MutationId,
receipts/status lookup, deduplication/idempotency, RequestId, revision, evidence and
Draft/compound Recovery obligations. [IPC follow-ups](../contracts/ipc.md#deferred-decisions)
own UI workers, notification recipients, Batch Recovery preparation, concurrent
Reads, cancellation and native verification. Keep these identifiers; do not create
parallel TODO definitions here.

Scheduling remains session-only, not persistent Recovery. Required future Batch
Recovery preparation belongs inside execution before its dependent authoritative
changes, not before Accepted. No exactly-once or safe automatic replay guarantee
exists. [Hang limits and platform release policy](status.md) remain explicit gates.

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

Resolve concurrent-operation/Recovery coordination, conflict rules, key availability, interrupted-sync recovery, and compatibility before implementation. Completion requires synthetic-repository tests preserving UUID identity and encryption boundaries. See [Git design](plans/git-sync.md).

## Entry compatibility and Migration — required future work

Follow the [Entry compatibility contract](../contracts/storage.md#entry-compatibility-and-version-domains).
Best-effort bidirectional reading is approved; current codecs still require version
1 and do not provide general safe round-trip editing of extensions. Address
ENTRY-COMPAT-01/02 in a focused compatibility change, independently of IPC versions.
Historical Migration (ENTRY-MIG-01) is required when compatible decoding is
insufficient, not an optional optimization. Vault-aware Migration (ENTRY-MIG-02)
requires its access/protection design. Establish concrete supported schemas,
preservation and conversion-failure recovery before implementing those paths;
this roadmap does not authorize implementation.

## Hardening and release — planned

Expand macOS/Linux/Windows filesystem, process, and IPC verification; complete crash/fault, migration/compatibility, performance/resource-limit, and security coverage; and provide packaging/install and user/developer documentation.

Resolve supported platforms/versions and release/migration policy first. Completion requires documented reproducible builds and validated failure behavior. Fundamental data safety remains a gate at every earlier stage, not deferred until release.

