# Recovery architecture

**Status: Decided/Planned.** Single-file atomic writing, mutation failure classification, and internal Session Marker lifecycle are Implemented; durable Journals, Trash, Recovery Service, and Frontend draft recovery remain planned. See [Storage](../../contracts/storage.md) for current durability and partial-failure behavior.

## Separate mechanisms

| Mechanism | Purpose | Planned delivery |
| --- | --- | --- |
| OS lock | One active authoritative owner; OS releases ownership after process death | Implemented with Backend lifecycle integration |
| Durable Session Marker | Previous session may have left stale Index | Implemented internally in Phase 3.4.3 |
| Recovery Journal | Durable intent/progress for compound operations | Normal Recovery foundation; extended for Vault |
| Frontend draft / pending request | Preserve user edits and uncertain acknowledged outcomes | Public API contracts, then Recovery and TUI integration |

None replaces Catalog synchronization or another mechanism. Marker/Journal/drafts belong in durable state, never independently clearable Cache. A lock file's existence alone proves neither a live owner nor a previous clean exit.

## Existing session-safety foundation

OS ownership and the durable Session Marker are implemented with Backend lifecycle
integration. They are distinct: locks provide live exclusivity, while a previous
Marker causes Catalog reconstruction. Neither provides per-mutation receipts or
repairs compound authoritative conflicts. The authoritative details are
[Storage session safety](../../contracts/storage.md#session-lifecycle--implemented-internal-support),
[platform durability limits](../../contracts/storage.md#durability-scope), and
[Backend sequencing](../../contracts/backend-lifecycle.md#backend-lifecycle-call-flow--implemented).
Future Recovery must preserve these contracts rather than redefine Marker cleanup.

## Normal Trash and compound recovery — Decided/Planned

Implement Trash/Restore before Minimal TUI. Plan compound operations before executing; durably journal intent and progress, represent RecoveryCandidates, and provide Recovery Service contracts. Single-file atomic writes do not automatically require a transaction Journal. Moves and Directory operations already expose partial completion; do not silently roll back or repair them.

Nonempty Directory deletion/restore semantics, Journal format, idempotency, interruption points, and user-resolution rules remain Open/Deferred. Recovery data must survive Cache cleanup. No current code implements Trash or permanent deletion.

## Frontend drafts and pending operations

Preserve three distinct states: unsubmitted local edits; submitted requests without acknowledged outcome; acknowledged authoritative persistence. Backend may publish and then disconnect before acknowledgment. Absence of acknowledgment is not failure.

Reconciliation should be largely automatic and safe: no blind mutation retries, duplicate creation, or silent overwrite. Prevent conflicts using edit locks, serialized mutations, and disabled editing while Backend is unavailable. Reacquire valid permissions after reconnect and consult authoritative state. Avoid user prompts for conflicts caused solely by internal failures where safe resolution is possible.

MutationId and confirmed-versus-unconfirmed result semantics are implemented; see the [Mutation contract](../../contracts/mutations.md#results-and-evidence). Separate Request IDs, revisions, retention and the persistent reconciliation algorithm remain open. Restoring a Draft to a buffer does not apply it to authoritative data; unpersisted content is not automatically conflict-free. Current-state equivalence does not prove that a particular Mutation executed. Durable drafts must remain writable while Backend is unavailable and live in the Recovery area, not Cache.

## Vault extension and security

Vault-related drafts must be encrypted before durable persistence. A Frontend may hold displayed/edited plaintext but must not possess Vault decryption keys. It must be able to encrypt recovery drafts without decryption capabilities. Mechanism, key distribution, and recovery decryption protocol are deliberately deferred; do not pick a cryptosystem here.

Vault-aware Recovery extends the Normal Journal architecture. Cross-storage operations follow: prepare Normal side → prepare Vault side → both durable → begin coordinated mutation. Do not put sensitive Vault plaintext in an unencrypted Journal. Vault lock state, encrypted candidates, and two-sided failure handling require later design with [Vault](vault.md).

## Entry Migration boundary

[ENTRY-MIG-01/02](../../contracts/storage.md#compatibility-follow-up-work) track
required historical and Vault-aware Migration. Persistent conversion will need
explicit data-preservation and failure-recovery rules; those rules are not yet
designed. Migration is distinct from rebuilding Catalog/Index, replaying an
unconfirmed Mutation, or restoring a Frontend Draft. Compatible reading may avoid
conversion, but does not establish safe editing or remove the Migration obligation
for incompatible historical data.
