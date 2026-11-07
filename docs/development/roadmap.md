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

| Stage | Remaining responsibility |
| --- | --- |
| Cleanup | Documentation structure/instructions, harness tooling, code documentation and navigation/enforcement checks |
| Remaining 4.3b | Explicit authoritative Full Refresh and Directory Count Consistency Checks; settle remaining revision/concurrency questions |
| 4.4a | Backend Push Events, ordering and recipient rules; keep Mutation final results separate |
| 4.4b | Functional Frontend integration, UI/IPC worker, bounded reconnection, buffers and outcome presentation; not the full editor/Recovery |

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

## Hardening and release — planned

Expand macOS/Linux/Windows filesystem, process, and IPC verification; complete crash/fault, migration/compatibility, performance/resource-limit, and security coverage; and provide packaging/install and user/developer documentation.

Resolve supported platforms/versions and release/migration policy first. Completion requires documented reproducible builds and validated failure behavior. Fundamental data safety remains a gate at every earlier stage, not deferred until release.

