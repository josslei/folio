# Phase 4.3b — Consistency Checking & Catalog Refresh

**Status:** approved design, not implemented. Public contracts and scope direction
are settled below; concrete APIs, scheduling details and listed follow-ups remain
open. This plan does not authorize implementation. Current execution behavior
remains defined by the [IPC contract](../../contracts/ipc.md#scheduling-and-lifecycle).

## Terminology

- **Consistency Check:** a low-cost, potentially heuristic test for discrepancies
  between authoritative Storage and derived Catalog state.
- **Consistency Anomaly:** evidence of a possible discrepancy, not necessarily a
  fatal Backend failure.
- **Refresh:** a Backend workflow scanning authoritative data and reconstructing
  the relevant Catalog projection. Scoped Refresh selects a limited scope; Full
  Refresh covers the entire applicable Repository domain.
- **Catalog Reconciliation:** construction and validation of a replacement Catalog
  candidate from authoritative scan results and the applicable replacement scope.
- **Recovery:** handling interrupted or uncertain authoritative operations. It is
  not an automatic responsibility of Refresh.

## Consistency Check and navigation

Provide one generic Application-level Consistency Check: the Frontend identifies
logical scope, and the Backend chooses the applicable low-cost strategy. Do not
freeze C++ names/signatures, expose a Count-specific public API or build a checker
registration framework. Normal initially uses counts; Vault or later Normal checks
may use other strategies.

Distinguish three results: no anomaly detected by the performed checks, anomaly
detected, and check unable to complete. No detected anomaly does not certify full
consistency. Check does not modify Catalog, initiate Refresh or retry another
operation.

Normal counts direct filesystem entries without decoding or classifying files:

| Logical scope | Expected clean-layout direct-entry count |
| --- | --- |
| Log Root | Root-level Logs |
| Memo Root | Root-level Memos + Memo Directories |
| Todo Root | Root-level Todos + Todo Directories |
| User Memo/Todo Directory | Direct Records + 1 (`.folio-dir.toml`) |

Use appropriate compile-time layout-overhead constants in implementation: 0 for
module Roots, 1 for user Directories. Unrelated files and temporary artifacts may
cause mismatches; content edits or one-for-one replacements may preserve counts.
There is no periodic monitoring or protection against external changes during a
check/scan.

Frontend navigation composes separate requests:

```text
Enter Record Root / Directory
              |
              v
Browse(scope) -> Display Catalog result
              |
              v
Consistency Check(scope)
  |-- No anomaly -> no UI action
  |-- Anomaly ----> notify user; offer explicit applicable Refresh
  `-- Check error -> report check failure
```

Entering a location is not synonymous with calling Browse. Sorting, filtering,
re-rendering and repeated Browse calls do not inherently trigger checks. Browse
remains Catalog-only; the separate navigation-triggered Check is not a prerequisite
for displaying results. Mismatch is exceptional, not an extra waiting branch on
normal navigation. Do not automatically Refresh on mismatch. Precise presentation
belongs to later Frontend integration.

## Unified Refresh scope and reconciliation

Full and Scoped Refresh share one Application-level mechanism with selectable
logical scope: the entire applicable Repository, a Record Root or a user Directory,
with Immediate or Recursive traversal as applicable. No artificial persisted parent
Directory, arbitrary integer depth or unrestricted filesystem traversal is implied.

**Absence from a scan implies absence only within the scope actually scanned.**
Scan and replacement scopes must agree:

| Scan scope | Reconciliation responsibility |
| --- | --- |
| One user Directory | Its immediate Record members |
| Memo/Todo Root, Immediate | Root-level Records and immediate Directory metadata; preserve unscanned Directory members |
| Memo/Todo Root, Recursive | The Root, its Directories and their members |
| Full Repository | Complete Catalog reconstruction |

Build and validate a private candidate before atomic publication. Failed validation
must retain the previous Catalog rather than publish partial results.

Two scoped anomalies require explicit failure identifying Full Refresh as the
required next Refresh scope:

- An immediate Root scan finds a previously cataloged Directory missing and its
  cached members would be affected: do not silently discard unscanned members.
- A scanned UUID is represented outside the scanned scope: do not assume relocation
  or overwrite the existing locator.

Do not silently broaden the scan or force Full Refresh without user authorization.
Do not offer an unsupported partial repair alternative. Full Refresh can itself
fail on duplicate UUIDs or malformed authoritative data; it is not a guarantee of
resolution and never silently repairs authoritative files.

## Responsibility and extension boundaries

Refresh is one externally observable Application Operation with internal workflow
steps. Full Refresh and Scoped Refresh are Backend workflows: scan the selected scope,
construct and validate a candidate Catalog, then publish on success. They are not
Frontend collections of independent requests. Refresh changes a derived projection;
it does not silently repair authoritative files or resolve uncertain Mutations.

Keep three responsibilities separate:

- RequestCoordinator owns dispatch eligibility, reservations, completions and
  response routing through the existing connection machinery.
- A Backend-private Refresh component owns scope-specific scanning, candidate
  reconciliation and publication. Its precise API and file placement remain open.
- A consistency-policy owner decides whether an Access anomaly warrants Refresh,
  user action or no additional action. Its placement remains open; scheduling must
  not become a second implementation of consistency policy.

Explicit Refresh requests and later consistency-policy decisions use the same
Refresh scheduling path. A Check only reports its result; it does not submit Refresh.
Scoped failures may require Full Refresh as described above, but never execute
that broader scope automatically.

Do not build a generic macro-operation engine now. Multiple internal steps alone
do not require multiple queued requests or staged cross-worker continuations.
Keep workflow scope separate from scheduling requirements so later maintenance
operations can reuse reservations without placing business logic in coordination.
Batch, Recovery and Git Sync workflows remain subject to their own future designs.

## Mutation exclusion with concurrent Access

Once Full Refresh is pending, defer new Mutations. Finish the active Mutation,
then dispatch Refresh on an execution worker. Access remains eligible during both
the pending and running phases, subject to existing capacity and Entry-conflict
rules. Resume Mutations after Refresh completes, applying the resulting Catalog
safety state rather than assuming Refresh succeeded.

Initially apply the same Mutation-wide exclusion to Scoped Refresh. Narrower
scope-based exclusions are a later design decision. A dedicated maintenance
thread is not required by this design; worker placement remains to be determined.

```text
CONNECTION WORKERS        REQUEST COORDINATOR          EXECUTION WORKERS
------------------        -------------------          -----------------
Submit Refresh ---------> Record pending Refresh       Active Mutation
Send Accepted             Defer new Mutations               |
                          Dispatch eligible Access ------> Access pool
                          Process completions <----------- Mutation finishes
                          Dispatch eligible Refresh -----> Refresh workflow
                          Continue Access/admission        Scan / candidate
                          Process completion <------------ Publish / result
Receive final result <--- Resume eligible Mutations
```

Refresh performs no scan or blocking operation on the coordinator thread. Building
the candidate happens privately; complete publication and status capture must use
the synchronized Catalog boundary. Existing Access operations retain immutable
views. Current `CatalogCoordinator::refresh_all()` assumes drained execution and
must be adapted before concurrent Refresh is enabled; immutable views alone do
not establish thread safety for that method.

## Queue state, wakeups and dispatch precedence

The current RequestCoordinator already owns pending `queue_`, worker-completion
`completed_` and separate active-worker/Entry-reservation state. Running work is
not left at the head of the pending queue. Refresh adds eligibility/reservation
state; it does not require another request queue or persistent acceptance journal.

Workers append completion under the queue mutex, release it and notify the
coordinator's condition variable. Predicate-based waiting under the same mutex
prevents lost wakeups. Completion notification does not interrupt a coordinator
action already executing: all coordination actions must remain short.

The dispatch loop must preserve this order:

```text
Process available completions; release finished reservations
                         |
                         v
Refresh pending and now eligible? -- yes --> Dispatch Refresh
                         |
                         no
                         v
Dispatch one eligible ordinary request
                         |
                         v
Return to completion processing before selecting more work
```

If a Mutation finishes during one short ordinary dispatch, that dispatch may
finish before its completion is observed. No new Mutation can slip ahead because
pending Refresh already prevents Mutation dispatch. An Access dispatched in this
interval is permitted to overlap Refresh. The coordinator must not join, wait for
an operation result or execute filesystem work to establish this ordering.

**Implementation obligation:** at the dispatch-selection branch corresponding to
“Refresh pending and now eligible?”, add a code comment explaining that Refresh
precedence is a specific initial policy, not a complete priority system. Future
rules, such as lower-priority Batch work, must be reconciled at this point while
preserving conflict ordering and considering starvation. Do not add a generic
priority mechanism now.

## Access anomalies while Refresh is in progress

An Access operation can fail, including with StaleLocator, during Refresh. Return
the actual failure to its originating Frontend; do not turn it into success or
broadcast request-specific details. Separately pass the anomaly to consistency
policy. If the active Refresh covers it, suppress duplicate recovery work, not the
original error. Other policy choices remain open.

Associate anomalies with the Catalog version/view used and Refresh completion
with its covered scope and published Catalog version. An Access using an older
view can finish after publication; recognize coverage by a completed Refresh as
well as an active one. Exact identity representation, coverage semantics and
bounded retention remain implementation/design work; no wire-format version or
Entry revision token is implied.

## Count Check execution reservation — approved

An immutable view alone cannot make Catalog and physical-directory counts coherent.
Schedule a Count Check only after the active Mutation finishes, and reserve against
new Mutations before capturing the current Catalog view. An execution worker then
captures that view, counts direct filesystem entries and compares the counts. Hold
the reservation through completion; release it on success or failure. Access remains
allowed, and the coordinator continues processing events without filesystem work or
blocking waits. Initially exclude all Mutations; narrower exclusions are deferred.
This is an execution reservation, not an Edit Lock. Coherence assumes the agreed
absence of concurrent external filesystem modifications; equal counts remain only
a heuristic consistency indication.

This settles execution safety, not dispatch precedence. Frontend navigation and explicit Refresh behavior are defined above. Track related
scheduling cases together below.

## Collected special dispatch cases

**Status:** two concrete cases identified. This is the shared review point for
operations that need additional eligibility/reservation or precedence beyond
ordinary Access/Mutation dispatch. Add future similar cases here so a general
policy can be designed from actual requirements rather than scattered exceptions.

| Case | Settled execution requirement | Dispatch policy / unresolved interaction |
| --- | --- | --- |
| Refresh (Full and Scoped workflows) | Wait for active Mutation completion; exclude new Mutations while pending/running; allow Access; execute off the coordinator thread. | Eligible Refresh takes precedence over ordinary dispatch after completion processing. Ordering/coalescing with Count Checks and starvation policy remain open. |
| Consistency Check (initial Normal count strategy) | Reserve against Mutations before Catalog capture; hold through physical counting/comparison and completion; allow Access; execute off the coordinator thread. | Ordering relative to Refresh and waiting Mutations, coalescing and starvation policy remain open. Do not assume Refresh precedence automatically applies to Count Checks. |

Reservation answers what may overlap; precedence answers what is dispatched next.
Reconcile both cases at the dispatch-selection point described above, including
its required future-priority code comment, rather than independently accumulating
hardcoded priority branches. Lower-priority Batch work is a future motivating
example, not a third settled case or an approved priority mechanism.

## Catalog safety, lifecycle and ownership

Preserve existing CatalogCoordinator states: Unloaded, Ready, Unavailable,
Inconsistent and Stopped. An operation error, uncertain authoritative changes,
Catalog inconsistency and availability are distinct conditions. `unpersisted` is
normal Index persistence bookkeeping, not unsaved user content or a failure.

Failed Refresh preserves the prior candidate/publication state under existing
rebuild invariants; it must not certify a clean Catalog. Successful Full Refresh
may restore Inconsistent to Ready when complete rebuild invariants are satisfied.
Successful Scoped Refresh does not prove global consistency; whether it can restore
global readiness remains open.

Keep Session Marker, Index durability, connection-owned Edit Locks, safe shutdown
finalization and RepositoryOwnership guarantees. Refresh does not clear locks,
overwrite drafts or justify finalizing Storage under active execution. Exact runtime
integration remains to be designed; do not introduce another Catalog state machine.

The Frontend retains unsaved/unconfirmed content and submission evidence under the
[Mutation result contract](../../contracts/mutations.md#results-and-evidence) and
[Draft Recovery principles](recovery.md#frontend-drafts-and-pending-operations).
Backend acceptance is session-only, not durable Recovery. Refresh cannot prove an
uncertain Mutation applied or recover a lost Frontend Draft. No implicit retry,
replay or authoritative-file repair is authorized.

## Traceable follow-ups

These local identifiers own remaining Phase 4.3b details; linked existing owners
continue to own later features. They do not authorize implementation.

| ID | Status | Missing decision/capability and revisit point |
| --- | --- | --- |
| CONS-01 | Deferred Implementation / Open Design Decision | Implement the approved Check/Refresh contracts in 4.3b after reviewing concrete private Refresh APIs, worker placement, reconciliation details, bounded resources and shutdown integration. Current internal full rebuild requires drained execution. |
| CONS-02 | Open Design Decision | Choose consistency-policy ownership and bounded anomaly/view/scope coverage tracking during 4.3b design; avoid redundant work from old-view errors without suppressing originating failures. |
| CONS-03 | Open Design Decision | Set Refresh/Check coalescing, mutual ordering and fairness at dispatch selection before implementing their interaction. Refresh precedence is not a general priority policy; Batch priority is a future extension. |
| CONS-04 | Open Design Decision | Decide if and how scoped reconciliation may restore globally Inconsistent readiness; successful local repair alone is insufficient evidence. Revisit before approving state transitions. |
| CONS-05 | Deferred Implementation / Open Design Decision | Choose Vault low-cost checking strategy in Vault design; Normal filesystem counts do not establish Vault layout or privacy rules. |
| CONS-06 | Not Guaranteed | Count equality is not full verification; Refresh is not authoritative repair or proof of Mutation execution. Preserve these limits in APIs and future UI. |

Push distribution/ordering remains [4.4a work](catalog-concurrency.md#query-results-and-push-handoff);
Frontend navigation presentation and UI/IPC worker integration remain 4.4b.
[Recovery](recovery.md), uncertain Mutation resolution and compound-operation
recovery remain separate later work. Undo/Redo and cancellation are deferred/open
in their respective future designs; this phase introduces none. Existing
[Mutation](../../contracts/mutations.md#traceable-follow-up-work) and
[IPC](../../contracts/ipc.md#deferred-decisions) TODOs retain ownership of receipts,
replay/deduplication, cancellation and related guarantees.
