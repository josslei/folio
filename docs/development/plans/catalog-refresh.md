# Phase 4.3b — Consistency Checking & Catalog Refresh

**Status:** implemented Normal Check/Refresh contracts; later/open capabilities are
marked below. Current execution behavior also follows the
[IPC contract](../../contracts/ipc.md#scheduling-and-lifecycle). Frontend navigation
and prompt integration remain later work; Push transport and Refresh invalidations are implemented.

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
expose a Count-specific public API or build a checker registration framework.
Concrete interfaces are summarized below. Normal initially uses counts; Vault or later Normal checks
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

## Implemented interfaces and limits

- Shared [maintenance types](../../../lib/include/folio/application/maintenance.hpp):
  `DirectoryRef` selects Check locations; `RefreshScope` selects `RepositoryScope`
  or `DirectoryScanScope` (a logical `DirectoryRef` plus Immediate/Recursive
  `RefreshTraversal`). Repository scope always reconstructs the complete Catalog
  and has no traversal field. Logs and user Directories have no recursive children.
- Check returns `AccessResult<ConsistencyCheckResult>`: the two successful
  observations are `NoAnomalyDetected` and `AnomalyDetected`; inability to complete
  is an ApplicationError. Membership counts use immutable reverse indices without
  copying/filtering Record metadata; the physical count is raw iteration.
- Refresh returns `AccessResult<std::monostate>`. `FullRefreshRequired`
  identifies scoped identity/location ambiguity; `ErrorAction::Refresh` is an
  explicit user choice, never automatic execution. Stale Loads advertise it.
- Repository `scan(type, directory, recursive)` reuses the full-scan parser/validation path;
  scoped scans do not read unselected modules or member bodies. Scoped candidates
  retain unscanned entries and validate complete UUID/membership/path invariants.
- Scoped publication preserves global readiness, including Inconsistent. Full
  successful reconstruction can restore Ready. Neither writes the Index or
  removes the Session Marker. Failed scans/candidates retain the previous state.

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
  reconciliation and publication: `CatalogMaintenance` in
  `server/include/operations/catalog_maintenance.hpp`. The single instance owns
  maintenance state and Repository access, and receives Coordinator explicitly
  for coherent capture/publication; worker execution owns its complete workflow.
- A future consistency-policy owner may decide whether an Access anomaly warrants
  maintenance, user action or no additional action. This mechanism is deferred;
  scheduling must not become a second implementation of consistency policy.

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
scope-based exclusions are a later design decision. Maintenance executes on the shared Access pool; no dedicated maintenance thread
is introduced. Checks and Refreshes do not overlap one another, protecting the
comparison interval against Catalog replacement as well as authoritative writes.

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
views. Full rebuild now publishes under the Coordinator publication mutex. Scoped
reconciliation uses `publish_scoped_refresh()` at the same boundary; no mutex spans
Repository scanning or query evaluation. Scheduling excludes other writers;
startup/finalization still require drained execution.

## Queue state, wakeups and dispatch precedence

The current RequestCoordinator already owns pending `queue_`, worker-completion
`completed_` and separate active-worker/Entry-reservation state. Running work is
not left at the head of the pending queue. Refresh adds eligibility/reservation
state; it does not require another request queue or persistent acceptance journal.

Workers append completion under the queue mutex, release it and notify the
coordinator's condition variable. Predicate-based waiting under the same mutex
prevents lost wakeups. Completion notification does not interrupt a coordinator
action already executing: all coordination actions must remain short.

The dispatch loop preserves completion-first scheduling:

```text
Process available completions; release finished reservations
                         |
                         v
Select next ordinary eligible queued request
  - pending Refresh prevents new Mutation dispatch
  - active maintenance reservations remain enforced
                         |
                         v
Return to completion processing before selecting more work
```

Latest priority clarification: do not add a dedicated Check/Refresh priority branch
now. Ordinary eligible selection applies; a pending Refresh still defers Mutations
as required by its execution contract. If a Mutation finishes during one short
ordinary dispatch, that dispatch may finish before completion is observed. The
coordinator never joins, waits for results or performs filesystem work.

**Important implementation follow-up — CONS-03:** add a code TODO at this selection
point for the future priority system. Refresh must have highest priority in that
system; lower-priority Batch work and fairness/starvation must be reconciled there.
This makes priority-system design increasingly necessary as maintenance cases grow.
Do not implement a generic priority mechanism or a temporary special priority rule.

## Access anomalies while Refresh is in progress

An Access operation can fail, including with StaleLocator, during Refresh. Return
the actual failure to its originating Frontend; do not turn it into success or
broadcast request-specific details. Do not automatically forward it into another
Refresh, reinterpret it as success or retry the originating operation.

General anomaly forwarding, Catalog-version coverage and retention are deferred,
not immediate implementation requirements. The initial flow returns StaleLocator
and lets the Frontend offer explicit Full Refresh. Concurrent Full Refresh requests
share execution as described below; future Push updates obsolete prompts.

### Full Refresh coalescing — approved direction

Keep the unified scope-based Refresh API. RequestCoordinator recognizes Full
Refresh by its scope. If one is pending/running, attach the new accepted request
to that execution instead of starting another scan. Otherwise schedule a new
Full Refresh. Scoped Refresh merging is not implied.

The coordinator owns the association between one execution and its waiting request
tickets. Each ticket represents that request's completion state/result and remains
connected to its own connection worker. The execution worker does not own or mutate
the subscriber list: it reports one outcome; coordination completes all associated
tickets from that outcome. Later requests can therefore join while execution runs
without editing the worker's captures. Preserve per-request admission/resource
accounting, request-specific delivery and accepted-work lifetime after disconnect.
The shared execution owns its outcome; the coordinator owns the subscriber list.
Ordinary results move to their sole ticket; additional Refresh subscribers receive
the same small encoded result. Every request retains admission/resource accounting.

Use a uniform execution-to-ticket association: an execution completes one or more
request tickets, with ordinary operations retaining exactly one. Normal admission,
resource accounting, completion and connection delivery remain the common paths.
Do not add another queue, thread, delivery mechanism or a Refresh-specific completion
branch. Only the compatibility/join decision is operation-specific and belongs
alongside Refresh scheduling requirements. Joining adds the newly admitted ticket
to the association rather than modifying the running worker's captures. This is
shared completion support, not a generic macro-operation framework or permission
to coalesce arbitrary operations.

Successful Refresh publication produces a Push invalidation so
Frontends can update/dismiss obsolete Refresh prompts while still listening for
events. Full Refresh completion means another Full Refresh is not required solely
because of the earlier stale-location prompt; it does not prove an uncertain
Mutation executed. Push delivery is implemented; prompt integration remains 4.4b.

**Accepted race / Not Guaranteed:** a user can submit another Full Refresh after
the previous execution completes but before receiving its Push notification. That
request can start a redundant Full Refresh. This rare overlap is accepted; do not
add historical result caching, Catalog-version tracking or a deduplication journal
solely to eliminate it. During implementation, put a comment at the Full Refresh
join-or-start decision documenting this exact boundary: only pending/running work
is coalesced; completed work is not reused, and late requests may scan again.

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

This settles execution safety, not dispatch precedence. Until a priority system is
implemented, Checks use ordinary eligible-request selection; do not add a special
Check-versus-Refresh priority branch. An already running Check retains its
reservation until completion. The future priority system must give Refresh its
highest priority. Implementing that system is an important follow-up: maintenance
reservations make dispatch policy increasingly consequential, not merely an
optional performance optimization.

Frontend navigation and explicit Refresh behavior are defined above. Track related
scheduling cases together below.

## Collected special dispatch cases

**Status:** two concrete cases identified. This is the shared review point for
operations that need additional eligibility/reservation or precedence beyond
ordinary Access/Mutation dispatch. Add future similar cases here so a general
policy can be designed from actual requirements rather than scattered exceptions.

| Case | Settled execution requirement | Dispatch policy / unresolved interaction |
| --- | --- | --- |
| Refresh (Full and Scoped workflows) | Wait for active Mutation completion; exclude new Mutations while pending/running; allow Access; execute off the coordinator thread. | Completion-first ordinary eligible selection; pending Refresh defers Mutations. No dedicated Check/Refresh priority branch until the priority system exists. General coalescing/fairness policy remains open. |
| Consistency Check (initial Normal count strategy) | Reserve against Mutations before Catalog capture; hold through physical counting/comparison and completion; allow Access; execute off the coordinator thread. | Ordinary eligible selection initially; no dedicated Check priority rule. General fairness/coalescing policy remains open. |

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
overwrite drafts or justify finalizing Storage under active execution. Lifecycle draining includes pending, active and joined maintenance requests;
finalization follows worker completion. No new Catalog state machine is introduced.

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
| CONS-01 | Implemented | `BackendConnectionHandle::check_consistency(DirectoryRef)` and `refresh(RefreshScope)` use the shared IPC/Access pool. `CatalogMaintenance` performs scanning/reconciliation, `CatalogCoordinator` publishes, and RequestCoordinator reserves/drains maintenance and shares pending/running Full Refresh executions. |
| CONS-02 | Deferred Implementation / Open Design Decision | General consistency-policy ownership and anomaly/view/scope coverage tracking are outside immediate scope. Revisit only if explicit errors, Full Refresh coalescing and future Push leave a concrete need; preserve originating failures. |
| CONS-03 | Approved direction / Open Design Decision | Coalesce pending/running Full Refresh requests; accept the documented late-request race. Scoped merging and general fairness remain open at dispatch selection. Check/Refresh initially use ordinary eligible selection without special priority handling. The priority system is an important next design task: Refresh must have highest priority, while Batch work motivates lower priorities. Current ordinary eligible selection is not a complete priority/fairness guarantee. |
| CONS-04 | Open Design Decision | Decide if and how scoped reconciliation may restore globally Inconsistent readiness; successful local repair alone is insufficient evidence. Revisit before approving state transitions. |
| CONS-05 | Deferred Implementation / Open Design Decision | Choose Vault low-cost checking strategy in Vault design; Normal filesystem counts do not establish Vault layout or privacy rules. |
| CONS-06 | Not Guaranteed | Count equality is not full verification; Refresh is not authoritative repair or proof of Mutation execution. Preserve these limits in APIs and future UI. |

Push transport distribution/retention is implemented; complete UI application remains later work under the [handoff contract](catalog-concurrency.md#query-results-and-push-handoff);
Frontend navigation presentation and UI/IPC worker integration remain 4.4b.
[Recovery](recovery.md), uncertain Mutation resolution and compound-operation
recovery remain separate later work. Undo/Redo and cancellation are deferred/open
in their respective future designs; this phase introduces none. Existing
[Mutation](../../contracts/mutations.md#traceable-follow-up-work) and
[IPC](../../contracts/ipc.md#deferred-decisions) TODOs retain ownership of receipts,
replay/deduplication, cancellation and related guarantees.

## Session-local navigation Check suppression

Automatic navigation Checks execute at most once per logical scope per Backend
session, across all Frontends. CatalogMaintenance owns the once-per-scope policy and historical outcomes.
RequestCoordinator only associates duplicate pending/running requests with one
execution: pending/running duplicates attach result tickets through the existing
shared-completion path. Once complete, later requests receive the retained
historical outcome without another filesystem count. The ordinary short maintenance reservation
still applies; there is no coordinator-side completed-result cache.
Failures also count as attempted; navigation must not cause repeated retries.
Only the result/exception is retained, not request captures or subscriber tickets.
Backend restart resets this tracking. Mutation and Refresh do not reset it.

This avoids repeated navigation overhead, not an upper bound on an individual
filesystem operation. Results are historical observations, not current consistency
certificates; external changes after checking can remain undetected. Browse still
runs independently and remains Catalog-only.

**Deferred Implementation:** explicit user-requested rechecking/bypass semantics
must be discussed with TUI integration; the present API has no force-check flag.

Check and Refresh share the RequestCoordinator Maintenance lane: both execute on
Access workers with the same active maintenance/Mutation exclusion. Operation
identity remains separate: pending Refresh defers new Mutations, pending Check
does not. Lane is not priority; the future priority system must select Refresh
ahead of Check despite their shared lane. Ticket sharing remains operation-specific.

### Observer initialization follow-up

**CONS-06 — Deferred Implementation / Open Design Decision:** review constructor
support alongside `CatalogMaintenance::set_refresh_observer()` and choose the
appropriate Backend wiring time during the later initialization/API refactor.
The setter remains available; this naming cleanup does not change initialization
order. Review other setters only within that future refactor's agreed scope.
