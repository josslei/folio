# Recovery architecture

**Status: Decided/Planned.** Existing single-file atomic writing and mutation failure classification are implemented; Session Marker, durable Journals, Trash, Recovery Service, and Frontend draft recovery are not. See [Storage](storage-design.md) for current durability and partial-failure behavior.

## Separate mechanisms

| Mechanism | Purpose | Planned delivery |
| --- | --- | --- |
| OS lock | One active authoritative owner; OS releases ownership after process death | Storage lifecycle foundation, Backend wiring later |
| Durable Session Marker | Previous session may have left stale Index | Storage lifecycle foundation |
| Recovery Journal | Durable intent/progress for compound operations | Normal Recovery foundation; extended for Vault |
| Frontend draft / pending request | Preserve user edits and uncertain acknowledged outcomes | Public API contracts, then Recovery and TUI integration |

None replaces Catalog synchronization or another mechanism. Marker/Journal/drafts belong in durable state, never independently clearable Cache. A lock file's existence alone proves neither a live owner nor a previous clean exit.

## Session Marker protocol — Decided/Planned

```text
Acquire exclusive process/data lock
  → inspect previous durable Marker
  → present: distrust cached Index, rebuild authoritative Catalog
    absent: ordinary one-attempt Index load / fallback rebuild
  → durably establish Marker
  → expose Backend Ready and serve requests
  → stop admissions and finish active work
  → persist Index if required and consistent
  → remove Marker only when Index state is safe (durable cleanup)
  → release lock and exit
```

Ready requires durable Marker establishment. This is a session-wide Marker, not a lazy first-mutation marker. A read-only crash can cause unnecessary reconstruction; accepted. Previous Marker forces reconstruction even if Index decodes. Failed rebuild must not publish an incomplete Catalog. Startup failures and uncertain durability must conservatively preserve evidence; exact cleanup/error states need design.

Failed Index persistence leaves Marker. Inconsistent Catalog cannot authorize Marker removal. Marker does not cause periodic saves. Cache disappearance and a clean unpersisted flag cannot alone certify a safe shutdown: define the complete safe-Index predicate and Cache-loss races during lifecycle implementation.

Current Coordinator has no Marker or OS lock; its internal Ready currently means initialized/reconciled metadata, not future process readiness. Foundation tasks establish lifecycle capabilities; actual daemon ownership arrives later.

## Normal Trash and compound recovery — Decided/Planned

Implement Trash/Restore before Minimal TUI. Plan compound operations before executing; durably journal intent and progress, represent RecoveryCandidates, and provide Recovery Service contracts. Single-file atomic writes do not automatically require a transaction Journal. Moves and Directory operations already expose partial completion; do not silently roll back or repair them.

Nonempty Directory deletion/restore semantics, Journal format, idempotency, interruption points, and user-resolution rules remain Open/Deferred. Recovery data must survive Cache cleanup. No current code implements Trash or permanent deletion.

## Frontend drafts and pending operations

Preserve three distinct states: unsubmitted local edits; submitted requests without acknowledged outcome; acknowledged authoritative persistence. Backend may publish and then disconnect before acknowledgment. Absence of acknowledgment is not failure.

Reconciliation should be largely automatic and safe: no blind mutation retries, duplicate creation, or silent overwrite. Prevent conflicts using edit locks, serialized mutations, and disabled editing while Backend is unavailable. Reacquire valid permissions after reconnect and consult authoritative state. Avoid user prompts for conflicts caused solely by internal failures where safe resolution is possible.

Operation/request identity, acknowledgment semantics, revisions, retention, and the reconciliation algorithm are not frozen. Public API design must establish them before TUI pending-operation behavior. Durable drafts must remain writable while Backend is unavailable and live in the Recovery area, not Cache.

## Vault extension and security

Vault-related drafts must be encrypted before durable persistence. A Frontend may hold displayed/edited plaintext but must not possess Vault decryption keys. It must be able to encrypt recovery drafts without decryption capabilities. Mechanism, key distribution, and recovery decryption protocol are deliberately deferred; do not pick a cryptosystem here.

Vault-aware Recovery extends the Normal Journal architecture. Cross-storage operations follow: prepare Normal side → prepare Vault side → both durable → begin coordinated mutation. Do not put sensitive Vault plaintext in an unencrypted Journal. Vault lock state, encrypted candidates, and two-sided failure handling require later design with [Vault](vault-design.md).
