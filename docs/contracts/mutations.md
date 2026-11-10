# Mutation and editing contracts

Phase 4.3a implements Normal Create/Update/Move, Directory Create/Rename,
immediate Todo status changes, and connection-owned Entry Edit Locks through
`BackendConnectionHandle`. Public inputs live in
`folio/application/mutation.hpp`; result/evidence contracts and
`MutationId` retain their Phase 4.2b ownership. The Backend implementation is
`server/src/operations/mutation_orchestrator.cpp`. Query APIs, protocol version 1, bounded
complete-message transport, admission, and the one sequential executor remain.
See the [unified IPC protocol](ipc.md).

## Principles and invariants

Failure cause, authoritative outcome, evidence and confirmation are independent.
Preserve known facts and Draft content; no status alone authorizes replay. Check
current state and ownership before writes, then reuse the existing persistence and
Catalog synchronization path. Exact contracts and mappings follow below.

## Application Operation taxonomy — Approved

```text
Application Operation
├── Access Operation
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

These are architectural concepts, not C++ inheritance requirements. Access
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

## Mutation identity — Implemented

`MutationId` is defined with editing data and result contracts in
`folio/application/mutation.hpp`; its implementation is `lib/src/application/mutation.cpp`.
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
the old ID. The shared confirmed-response decoder now validates the echoed ID; the exchange caller invalidates the connection on mismatch.
Local SubmissionState distinguishes NotSent, MayHaveBeenSent, admission Rejected,
and Accepted without a confirmed final result. Rejected is not a final execution
failure; it is represented by NoMutationResponse with the admission error, not a
fabricated MutationFailure. Accepted is session-only responsibility. These local
states are not encoded as Backend final responses.
Neither correlation nor content equivalence proves execution. There is no receipt,
deduplication, automatic retry, or exactly-once guarantee.

Preserve buffers and evidence for failure/unconfirmed results. Applied confirms
persistence of the submitted intention, not necessarily subsequent buffer edits.
Notification dismissal, Draft deletion, and Recovery resolution are separate.
No outcome alone grants permission to retry or apply Recovery.

## Normal execution and field semantics — Implemented

Three logical property categories are explicit:

1. **Modification Context:** Frontend-supplied `TimestampContext modified`, shared
   by Update and Move; Create supplies both `created` and `modified`. The Backend
   preserves timestamp, timezone and optional location, without enforcing equality
   or monotonicity. This is not a revision token.
2. **Membership / Location:** Move manages Memo/Todo `directory_id` and the
   corresponding Normal physical location. Directory Rename changes only the
   logical name; the UUID folder and member files remain fixed.
3. **Editable Content & Metadata:** Update supplies complete LogEditableFields, MemoEditableFields
   or TodoEditableFields, excluding identity, creation, privacy, membership and locators.
   Backend loads the current authoritative Snapshot, applies those fields and
   Modification Context, then calls the existing full-file Repository Update.

Categories 2 and 3 are independent responsibilities; category 1 is shared context.
These are not three new persisted structures. Directory has no creation/modification
context in its persisted model; Directory Create/Rename do not invent one.

The Backend generates one Entry UUID for Create. Frontend-generated MutationId is
independent. Successful operations return the canonical Core metadata appropriate
to the Entry type. Full Editable Fields are not a patch API; immediate Todo status
changes read the current Snapshot and preserve every unrelated field.

Execution requires Ready, checks current Catalog existence/type/membership and
lock permission before writing, and uses current Catalog locators without UUID
discovery or implicit scanning. The same Catalog metadata validators are reused
before publication. Directory paths are fixed by kind/UUID and checked against
the Catalog locator. Repository serialization, atomic publication and matching
Coordinator callbacks remain the sole persistence/Catalog synchronization path;
Application does not update the Catalog a second time. A disagreement returns an
explicit error. No Full Refresh or automatic reconciliation is performed.

## Edit-lock ownership — Implemented

`acquire_edit(id)` checks Catalog readiness, Entry existence and connection-owned
editing rights entirely in memory. It returns empty success (`std::monostate`),
not a Snapshot; it does not read or verify the authoritative file. Same-owner
reacquisition succeeds without reloading. Another owner's lock returns
LockConflict. `release_edit(id)` is harmless if unlocked and explicitly rejects
another owner's lock.

Frontend workflows compose acquisition and existing typed Loads. Acquire then
Load is the safe editing workflow: acquiring ownership alone does not certify
that previously displayed content is current. A failed Load does not release the
lock; the Frontend explicitly releases ownership when abandoning editing, with
connection cleanup as fallback. Directories use the same ownership-only operation;
Directory metadata remains available through Browse/Directory Listing.

Acquisition and Load remain separate sequential executor requests. The
[concurrent Access plan](../development/plans/catalog-concurrency.md#filesystem-execution-and-edit-locks)
defines later scheduling changes; this split does not enable concurrent execution.

Full content Update requires an existing owned lock. Immediate operations (Move,
Directory Rename and Todo status change) check and execute adjacently on the
executor: another owner's lock rejects them; unlocked Entries need no separate
temporary lock record; the owner's existing lock remains afterward. Rename is a
logical organizational mutation, not a physical Directory relocation. It obeys
lock ownership without locking all member Records. Reads require no locks.

Workers and their accepted executor actions share a private connection-lifetime
token. The lock registry holds weak ownership. Disconnect cannot expire ownership
while an accepted action still retains that token, so a queued owner Update remains
authorized and competing work cannot bypass it. After the worker and accepted
work release the token, ownership expires; the next executor operation removes
expired entries. No cleanup request competes with a full or closed admission
queue. No worker accesses the lock registry, Catalog or Repository.

## Failure mappings and evidence — Implemented

| Boundary | Status | Evidence |
|---|---|---|
| Execution prerequisite rejection | NotApplied | Applicable steps NotAttempted |
| Atomic write fails before publication | NotApplied | WriteEntry NotCompleted |
| Publication known, durability unconfirmed | Unknown | WriteEntry Published |
| Required write durability confirmed, artifact cleanup or Catalog synchronization fails | Applied | WriteEntry DurabilityConfirmed |
| Move destination durable, source removal does not publish | PartiallyApplied | Destination DurabilityConfirmed; source NotCompleted/NotAttempted |
| Move destination durable, source removal published but sync fails | Unknown | Destination DurabilityConfirmed; source Published |
| Both Move steps durable, Catalog synchronization fails | Applied | Both steps DurabilityConfirmed |
| Transport/protocol failure without final confirmation | NoMutationResponse | Original MutationId and local SubmissionState, never fabricated NotApplied |

Storage adds internal per-step observations to RepositoryMutationError; I/O retains
`durability_confirmed` separately from `replacement_completed`. The latter still
means publication only. This records completed destination sync even if subsequent
temporary-link cleanup fails, without changing the atomic algorithm. Unexpected
writer exceptions remain uncertain. Failure cause and these execution facts are
independent; error codes alone never determine persistence status.

Directory metadata publication is its WriteEntry step. Empty bootstrap/UUID folders
left before publication do not count as applied Entry creation; the existing
Coordinator may nevertheless require reconciliation. A same-location Move remains
a no-op and does not apply modified; callback failure then reports no new write.

Durability means the existing operation's implemented synchronization boundary,
not a new whole-filesystem transaction or power-loss proof. Ancestor bootstrap
links still have existing Recovery TODOs. POSIX Move syncs source removal; Windows
currently does not establish equivalent removal durability. On Windows an otherwise
completed location-changing Move reports confirmed Unknown with durable destination
and published source removal, rather than claiming fully durable success. Stronger
Windows guarantees and native verification remain deferred.

Post-publication failures retain the original cause and mark the Coordinator
Inconsistent through existing callbacks, blocking Index publication and clean
Marker removal. Missing final delivery does not cancel accepted work. Oversized
Mutation final encoding closes that exchange without replacing its result with
an uncorrelated error or asserting NotApplied; it does not terminate the Backend.
Evidence is historical; it authorizes neither replay nor filesystem repair.
Session Marker/Index remain derived-state recovery mechanisms, not mutation receipts.

## Traceable follow-up work

| ID | Status | Missing capability, reason, and revisit point |
|---|---|---|
| MUT-01 | Open design / deferred verification | Current Normal operations use the documented existing sync boundaries. Stronger bootstrap-ancestor and Windows source-removal durability require platform/Recovery review; Windows native runtime is unverified. |
| MUT-02 | Implemented for current Normal operations | Per-step Repository evidence and I/O destination durability distinguish publication, durable writes and cleanup failure. Extend and approve evidence when new compound/Trash/Vault operations are designed; empty-folder reconciliation remains Recovery work. |
| MUT-03 | Open design | Persistent Mutation Receipts and cross-restart Mutation status lookup are absent. Both require a crash-consistent persistence design; revisit with Recovery before promising history or lookup. |
| MUT-04 | Open design | Mutation deduplication and idempotency are absent and need mutation-specific safety rules before replay is considered. |
| MUT-05 | Open design | Separate `RequestId` is not implemented because present exchanges are sequential; reconsider with concurrent requests or Push Event protocol design. |
| MUT-06 | Open design / deferred implementation | Revision checking remains undecided; session-local Edit Locks are implemented in 4.3a. Revisit stale-buffer/conflict protection and fresh validation with Recovery; locks do not prove offline drafts conflict-free. |
| MUT-07 | Deferred implementation | Shared persistent Draft Recovery and correlation using `MutationId` are not implemented. They require retained submission evidence and distinct buffer restoration/application contracts; revisit in Phases 5–6. |
| MUT-08 | Deferred implementation / security review | Vault-safe Draft and evidence persistence requires encryption and visibility review in Vault/Recovery phases; do not persist sensitive content in plaintext. |
| MUT-09 | Deferred implementation / further design | Compound Move recovery and Delete/Trash semantics require explicit partial-failure resolution in their respective design phases. |
| MUT-10 | Deferred implementation | Frontend reconnection and concrete notifications belong to Phase 4.4b / Recovery; preserve drafts and do not replay unknown outcomes. |
| MUT-11 | Not Guaranteed | Exactly-once execution, safe automatic replay of Unknown mutations, and proof of execution from MutationId alone are not provided. Correlation is not a receipt; reconsider only with mutation-specific safety evidence and justified persistence mechanisms. |
| MUT-12 | Implemented for current Normal operations | Concrete payloads, correlated final results and dispatch are implemented in 4.3a through the existing protocol/queue. Unsupported or malformed wire tags are rejected before acceptance; no fake handler or speculative Delete/Trash payload exists. |
| MUT-13 | Open design | Internal schema-versioning policy remains undecided before release. Version 1 is retained without negotiation or a separate Mutation version; revisit after Minimum TUI before defining compatibility promises. |

## Public arguments and wire requests

Public mutation methods take explicit Entry identity, destination, timestamps,
and descriptive `LogEditableFields`, `MemoEditableFields`, or
`TodoEditableFields` values. Create takes editable fields and Frontend-supplied
creation/modification contexts (plus initial membership for Memo/Todo).
Directory creation takes kind/name; rename takes Entry identity/new name.
Operation-specific request structs belong to the implementation-facing IPC
namespace in `application_protocol.hpp`; they are not public argument bundles.
The wire field ordering and execution semantics are unchanged.

## Overloaded public operations

Create and Update use overloaded `create()` and `update()` methods: editable-field
types select the Record contract and typed metadata result. Directory creation
uses its kind/name overload. The unified `move()` accepts Entry UUID, optional
destination Directory UUID (absent means Root), and Modification Context; the
Backend resolves the current Record type and validates membership compatibility.
Move returns `core::RecordMetadata`. Typed Repository handlers and their existing
persistence/evidence rules remain in place. No typed Entry IDs are introduced.


## Application orchestration boundary — reviewed

`MutationOrchestrator` borrows Repository, Catalog and Coordinator and owns the
executor-only Edit Lock map. It checks readiness, ownership and membership,
resolves current locators, prepares authoritative Snapshots and selects matching
Coordinator callbacks. The executor callback routes Access requests directly to
AccessOrchestrator::handle in server/src/operations/access_orchestrator.cpp, independently of
MutationOrchestrator.

Repository retains atomic persistence and compound Move sequencing, execution
evidence and result notification. Coordinator callbacks apply saved metadata or
mark unsafe failures Inconsistent. The handler does not perform a second Catalog
update. Backend lifecycle retains startup, draining and explicit finalization.
ConnectionSession remains a Backend-private shared lifetime token used by IPC
workers, accepted actions and the weak lock map.

The interaction review found no duplicate synchronization mechanism or need to
move persistence sequencing into MutationOrchestrator. Existing managed calls
provide matching creation checks/callbacks and current locators; ordinary
operations do not initiate scans, automatic repair, retries or shutdown.


## Operation terminology

Application Operations distinguish read-only **Access Operations** (Browse,
Metadata Query, Directory Listing and Record Loading) from **Mutation Operations**.
Metadata Query names a specific Access operation, rather than the whole category.
Edit Lock acquisition/release are explicit editing-control requests supporting
the mutation workflow. Shared IPC envelopes are OperationRequest/OperationResponse;
the Frontend invoke_operation helper serves all operations. The Backend
AccessOrchestrator class handles only Access operations and is declared in
access_orchestrator.hpp. These names do not change variant tags or wire data.

Access and Mutation orchestration are concrete peer classes. AccessOrchestrator
borrows Catalog, Repository and Coordinator without owning Storage or connection
state. Ownership-only lock acquisition stays in MutationOrchestrator; separate
Record Loads use AccessOrchestrator. No Vault behavior is implemented.
