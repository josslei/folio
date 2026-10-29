# Recovery architecture

**Status: Decided/Planned.** Single-file atomic writing, mutation failure classification, and internal Session Marker lifecycle are Implemented; durable Journals, Trash, Recovery Service, and Frontend draft recovery remain planned. See [Storage](storage-design.md) for current durability and partial-failure behavior.

## Separate mechanisms

| Mechanism | Purpose | Planned delivery |
| --- | --- | --- |
| OS lock | One active authoritative owner; OS releases ownership after process death | Phase 4; not implemented |
| Durable Session Marker | Previous session may have left stale Index | Implemented internally in Phase 3.4.3 |
| Recovery Journal | Durable intent/progress for compound operations | Normal Recovery foundation; extended for Vault |
| Frontend draft / pending request | Preserve user edits and uncertain acknowledged outcomes | Public API contracts, then Recovery and TUI integration |

None replaces Catalog synchronization or another mechanism. Marker/Journal/drafts belong in durable state, never independently clearable Cache. A lock file's existence alone proves neither a live owner nor a previous clean exit.

## Session Marker protocol — Internal support Implemented; Backend wiring planned

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

Ready requires durable Marker establishment. This is a session-wide Marker, not a lazy first-mutation marker. A read-only crash can cause unnecessary reconstruction; accepted. Previous Marker forces reconstruction even if Index decodes. Failed rebuild must not publish an incomplete Catalog. Startup failures preserve the active Catalog and existing Marker. Marker establishment must succeed before a decoded or rebuilt candidate becomes Ready.

Failed Index persistence leaves Marker. Inconsistent Catalog cannot authorize Marker removal. Marker does not cause periodic saves. Cache disappearance and a clean unpersisted flag cannot alone certify a safe shutdown: finish_session() requires a regular persisted Index and completes its synchronization before Marker cleanup. Missing Cache prevents finalization without deleting the Marker.

Current Coordinator manages `<application.state>/session.marker` and requires durable establishment for Ready. It has no OS lock or daemon. The future Backend supplies exclusive ownership and calls startup_load(), then finish_session() after draining work. shutdown_save() alone saves without removing the Marker. Cleanup failures remain explicit and retryable; see [durability scope](architecture.md#durability-scope), including Windows and macOS limits. The Marker is independent of unpersisted and Inconsistent, and does not replace runtime synchronization.

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
