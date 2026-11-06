# Mutation contracts — Phase 4.2b

Implemented for review: public value contracts in `folio/application/mutation/mutation_result.hpp`
and the strong `MutationId` type in `folio/application/mutation/mutation_id.hpp`, with its
matching implementation at `lib/src/application/mutation/mutation_id.cpp`. These are
Application concepts; Core supplies secure UUID generation but does not depend
on mutation submission or Recovery. This contract step introduced no mutation methods, handlers, or execution changes.
Phase 4.2c adds shared codec support and bounded complete-message transport, described in the
[unified IPC protocol](ipc-protocol.md); concrete Mutation exchanges remain deferred.
Read API signatures remain unchanged. The subsequent runtime refactor adds
session-only Accepted/Rejected admission and a shared sequential executor. Accepted
is not a final Mutation response, durable receipt or promise of cross-restart
execution. Mutation-specific admission/result integration remains MUT-12 / 4.3a;
transport failure must not fabricate NotApplied. See [runtime rules](ipc-protocol.md#scheduling-and-lifecycle).

## Application Operation taxonomy — Decided/Planned

```text
Application Operation
├── Query Operation
│   ├── Browse
│   ├── Metadata Query
│   ├── Directory Listing
│   └── Record Loading
└── Mutation Operation
    ├── Create
    ├── Update
    ├── Move
    └── Delete / Trash (future)
```

These are architectural concepts, not C++ inheritance requirements. Query
Operations retrieve information without changing authoritative business data.
After a lost response, a Frontend can generally issue a fresh query after
reconnecting; its results may reflect newer state. Mutation Operations may have
changed authoritative data despite a lost response, so blind replay is unsafe.

A `MutationId` identifies one mutation intention, independently of an Entry UUID
or Draft identity. The Frontend generates it before submission, associates it
with the request and confirmed result, and retains it with submission evidence
for future Recovery. Reconnection does not replace it; a genuinely new intention
gets a new ID. This stable logical identity does not imply a persistent Backend
receipt or automatic persistence of Frontend recovery information.

| Identifier | Meaning | Scope / current status |
|---|---|---|
| `MutationId` | One mutation intention across submission and future Recovery | Mutation Operations only; implemented strong type |
| `RequestId` | One IPC request/response exchange | Potentially any Application Operation; not implemented or required by current synchronous sequential exchanges |

Mutation IDs do not guarantee deduplication, idempotency, exactly-once execution,
or cross-restart status lookup. Concurrent requests or future Push Events may
justify a separate Request ID; neither is introduced by this naming decision.

## Mutation identity in code — Implemented for review

`MutationId` is defined in `mutation_id.hpp` with matching `mutation_id.cpp`.
`MutationResponse<T>` and `NoMutationResponse` carry a `mutation_id` member.
The draft mutation-specific naming has been updated in tests and CMake without
changing UUID generation, result/evidence semantics, or any wire format. No
compatibility alias is retained for the earlier, unfinalized draft name; source
consumers of that draft must update their includes, type names, and members.

## Results and evidence

`MutationResult<T>` distinguishes a trustworthy `MutationResponse<T>` from the
Frontend's `NoMutationResponse` observation. A confirmed response contains the
echoed Mutation ID and either success data or `MutationFailure`. The latter
combines the existing `ApplicationError`, `AuthoritativeChangeStatus`, and
`MutationEvidence`. Backend-reported Unknown is a confirmed failure response;
connection loss without a trustworthy response is a separate alternative.

NotApplied means the requested change was not published; incidental temporary
artifacts may remain. Applied requires the operation's specified durability steps,
even if later Catalog synchronization fails. PartiallyApplied means confirmed
durable partial effects; Unknown retains known publication facts without claiming
durability. Success additionally requires necessary Application synchronization.

Evidence identifies the Entry, optional logical source/destination, and relevant
write/destination/source-removal observations. NotCompleted means that step did
not publish its change; Published does not confirm durability. Evidence describes
execution history, not current state. Future operations must specify applicable
steps/order and validate evidence combinations before returning them. No physical
locators, temporary filenames, Record contents, or Vault secrets belong here.

The Frontend generates a Mutation ID for one intention before submission. A
distinct intention gets a new ID; investigation after connection loss retains
the old ID. The shared confirmed-response decoder now validates the echoed ID; its future
exchange caller must invalidate the connection on mismatch.
NotSent confirms no transmission; MayHaveBeenSent does not imply Backend acceptance.
Neither correlation nor content equivalence proves execution. There is no receipt,
deduplication, automatic retry, or exactly-once guarantee.

Preserve buffers and evidence for failure/unconfirmed results. Applied confirms
persistence of the submitted intention, not necessarily subsequent buffer edits.
Notification dismissal, Draft deletion, and Recovery resolution are separate.
No outcome alone grants permission to retry or apply Recovery.

## Future execution boundary

Application validates preconditions, supplies the known Catalog locator and
matching Coordinator callback to Repository operations, then translates their
result/evidence. The Repository already invokes that callback after persistence;
Application must not update Catalog a second time. Storage retains Repository
progress; I/O retains publication/durability facts. Neither depends on these
public Application contracts. The current failure stages and publication flag
alone cannot determine every outcome. Session Marker/Index provide no operation
history and failed Index saving does not undo authoritative writes.

## Traceable follow-up work

| ID | Status | Missing capability, reason, and revisit point |
|---|---|---|
| MUT-01 | Open design | Define each operation's durability boundary, including new ancestor links and platform guarantees, before mutation execution is exposed. |
| MUT-02 | Open design / deferred implementation | Define valid per-operation evidence and capture finer internal progress before implementing mutations. Current failure stages cannot separate durable creation with cleanup failure from uncertain publication. Directory creation needs folder/metadata treatment. |
| MUT-03 | Open design | Persistent Mutation Receipts and cross-restart Mutation status lookup are absent. Both require a crash-consistent persistence design; revisit with Recovery before promising history or lookup. |
| MUT-04 | Open design | Mutation deduplication and idempotency are absent and need mutation-specific safety rules before replay is considered. |
| MUT-05 | Open design | Separate `RequestId` is not implemented because present exchanges are sequential; reconsider with concurrent requests or Push Event protocol design. |
| MUT-06 | Open design / deferred implementation | Revision checking remains undecided; Edit Lock integration belongs to Phase 4.3b. Both affect fresh validation for Recovery. |
| MUT-07 | Deferred implementation | Shared persistent Draft Recovery and correlation using `MutationId` are not implemented. They require retained submission evidence and distinct buffer restoration/application contracts; revisit in Phases 5–6. |
| MUT-08 | Deferred implementation / security review | Vault-safe Draft and evidence persistence requires encryption and visibility review in Vault/Recovery phases; do not persist sensitive content in plaintext. |
| MUT-09 | Deferred implementation / further design | Compound Move recovery and Delete/Trash semantics require explicit partial-failure resolution in their respective design phases. |
| MUT-10 | Deferred implementation | Frontend reconnection and concrete notifications belong to Phase 4.4b / Recovery; preserve drafts and do not replay unknown outcomes. |
| MUT-11 | Not Guaranteed | Exactly-once execution, safe automatic replay of Unknown mutations, and proof of execution from MutationId alone are not provided. Correlation is not a receipt; reconsider only with mutation-specific safety evidence and justified persistence mechanisms. |
| MUT-12 | Deferred implementation | Concrete Mutation Request/Success payloads, the complete request envelope, Backend dispatch/execution and unsupported-Mutation responses belong to Phase 4.3a. They require business preconditions and approved payloads; 4.2c supplies common codecs/framing only, without fake handlers. |
| MUT-13 | Open design | Internal schema-versioning policy remains undecided before release. Version 1 is retained without negotiation or a separate Mutation version; revisit after Minimum TUI before defining compatibility promises. |
