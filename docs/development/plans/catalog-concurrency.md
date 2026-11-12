# Catalog views and concurrent Access execution

**Status:** combined Steps 5–6 baseline implemented: structurally shared Catalog
views, ownership-only Edit Lock acquisition, short request coordination, concurrent
query/Load execution and one sequential Mutation worker. Current guarantees live in
[IPC](../../contracts/ipc.md), [Access](../../contracts/access.md),
[Mutations](../../contracts/mutations.md) and [Storage](../../contracts/storage.md).
Push transport handoff is implemented; UI consumption, caching and other later directions remain deferred; recording
them does not authorize implementation.

## Work sequence and scope

Agree on implementation increments and their validation/review gates. Implement
and review those increments separately, allowing redesign and corrections between
rounds; do not bundle the entire refactor into
one change. The sequence below is agreed; no new phase number is assigned.
Only the focused refactor baseline precedes remaining Phase 4.3b on the
[roadmap](../roadmap.md); this document also records later architectural direction,
which is not a prerequisite for 4.3b. Confirm the exact baseline and increment
boundaries in the implementation-sequence discussion. This plan authorizes neither implementation nor Git
staging, commits, history changes or pushes.

### Agreed implementation sequence

Steps 5 and 6 form one combined implementation-and-review stage, with incremental
reviews and verification throughout. Earlier steps were separate review rounds.
Steps are not authorization to create commits or
to implement subsequent work without the user's instruction.

| Step | Implementation and review scope |
| --- | --- |
| 1. Evaluate persistent containers | Verify C++/platform fit, structural sharing, memory and performance; select the dependency and metadata representation |
| 2. Refactor Catalog storage | Introduce immutable states, retained views and complete-state publication; preserve validation, membership, ordering and no-op behavior; keep runtime sequential |
| 3. Integrate consumers | One view per Browse/Query; adapt Coordinator, rebuild, Index encode/decode and persistence bookkeeping; preserve formats and failure semantics |
| 4. Split Edit Lock acquisition from Load — implemented | Public API, IPC and handlers return ownership-only success; accepted-request ownership/disconnect cleanup preserved; execution remains sequential |
| 5. Concurrent Access execution — implemented | Separate short in-memory coordination from a shared query/Load Access pool; retain one Mutation executor; implement conflict scheduling, queued Loads, configurable bounded Access concurrency, resource accounting and safe draining |
| 6. Integrated baseline review — combined with Step 5 | Verify queries/updates, Load/Move ordering, locks, failures and shutdown; measure retained memory/responsiveness and resolve findings before returning to the roadmap |
| 7. Remaining 4.3b | Public explicit Full Refresh and Directory-count checks integrated with immutable publication/execution coordination; revisit remaining concurrency/version questions where required |
| 8. 4.4a Push Notifications | Distribution, recipient rules, connection retention, result/event ordering, gap-free coverage, coalescing and bounded delivery state |
| 9. 4.4b Frontend integration | UI/IPC worker, event-driven view updates/requery, bounded reconnection, buffers and outcome presentation; blocking public calls stay off the UI thread |

The combined Steps 5–6 stage may be subdivided after code inspection. Do not enable concurrency before
the safety mechanisms required by that increment are complete. Reviews may lead
to corrections or redesign before the next round.

Later work is scheduled by demonstrated need and its own design/authorization:

- Load forwarding/content caching: measure duplicate reads and define eligibility
  of authoritative results before implementation; see CAT-LOAD-FORWARDING below.
- Query extraction/full-text search: separate design; retain
  [APP-QUERY-SEPARATION](application.md#query-responsibility-follow-up).
- Batch scheduling/Recovery: define safe execution boundaries and durable Recovery
  prerequisites before implementing Batches.
- Parallel Mutations: conflict/thread-safety review first.
- Asynchronous public submission: optional separate API decision.

These optional items are not automatic prerequisites for later roadmap stages.
After Phase 4, follow the existing roadmap: Normal Recovery → Minimal Normal
TUI/editor → Vault → Vault-aware Recovery → Complete TUI → Git Sync → release
hardening. Preserve each stage's own design gates and authorization.

### Implementation timing

| Scope | Placement |
| --- | --- |
| Catalog state/view refactor, execution/ownership coordination and split lock API | Focused refactor before remaining 4.3b; agree exact increments and their correctness/validation requirements separately |
| Query/notification ordering, distribution, connection-side retention and delivery | Preserve the agreed design; implement Push functionality in 4.4a, with Frontend consumption in 4.4b; not a 4.3b gate |
| Frontend UI/IPC worker, event-driven requery and reconnect presentation | 4.4b; not a prerequisite for the Backend refactor or 4.3b |
| Forwarding, content caching, query extraction, Batch/Recovery and parallel Mutations | Deferred or open under their follow-ups; not prerequisites for 4.3b |

Catalog publication and view ownership must leave room for later Push integration,
but do not implement notification registration/retention, event payloads or a
temporary protocol merely to complete the refactor. Until Push is implemented,
do not claim that query responses are automatically kept current on Frontends.
Required safety within each implementation increment cannot be deferred merely
because the complete architecture spans later phases.

## Motivation and terminology

The previous shared sequential executor safely serialized all operations, but a
slow filesystem operation can delay unrelated browsing or starting an editor.
Separate operation scheduling from the resources each operation accesses:

- **Catalog query:** Browse, Metadata Query and Directory Listing inspect metadata
  in memory, without authoritative file reads or implicit scans/Refresh.
- **Authoritative Load:** a Frontend supplies a UUID; the Backend resolves its
  private Catalog locator and reads the authoritative Entry file.
- **Mutation:** changes authoritative data and subsequently synchronizes Catalog
  through the established Repository/Coordinator path.

Catalog queries remain a kind of Access Operation; this does not rename the
implemented operation taxonomy or introduce C++ base types. Metadata stays
locator-free in public Core/Application results. Internal Catalog records retain
`Located<Metadata>` composition.

## Rules applying to every mechanism

- Edit Lock ownership dominates scheduling priority. Read-only viewing remains
  possible while another connection owns editing rights.
- Long-lived editing ownership differs from short-lived execution conflicts.
  Scheduling may track active readers/writers without introducing another public
  lock API or a named short-lived lock subsystem.
- Unrelated eligible work should progress independently. IPC/admission and short
  in-memory coordination must not perform filesystem work. In-memory query
  evaluation can itself be expensive and must not monopolize that coordination.
- Prevent starvation of ready conflicting work. Do not implicitly reserve every
  future Batch target just because the Batch is queued.
- Bound workers, queues, buffers, retained views and notification state. Do not
  claim arbitrary operation-completion or shutdown bounds.
- Preserve Accepted versus final confirmation, authoritative outcome/evidence,
  request-specific error routing and no automatic Mutation replay. Scheduling is
  session-local and is not Recovery.
- Preserve Bootstrap → Serve → Stopping → Report → Finish. Drain accepted work
  and end execution before eligible Catalog finalization and ownership release.
- Frontend state stays current through Backend events, not periodic freshness
  polling. Actual Push delivery is not implemented by recording this plan.

## Responsibility overview

```text
Frontend workflows compose micro operations
             │
             ▼
Existing IPC connection workers
  Receive/admit requests; own outgoing response/event ordering
             │
             ▼
Shared Backend scheduling / in-memory coordination
  Ownership decisions, conflicts, bounded dispatch, completion accounting
             │
             ├── Shared Access workers
             │     ├── Query: capture immutable view → evaluate
             │     └── Load: capture current locator → authoritative read
             └── Sequential Mutation executor
                       └── Repository → Coordinator → Catalog publication

Catalog changes → shared notification distribution
                       └── originating/affected connection outgoing handling
                           retains or sends events relative to its response
                           [later Push integration: 4.4a / 4.4b]
```

This defines responsibilities, not a mandatory class/thread for each box.
No dedicated main notification thread is required. RequestCoordinator needs completion
facts to release workers/resources; it need not route result bodies or own
notification retention. Keep short in-memory coordination separate from query
evaluation and filesystem execution. Filesystem execution worker count will be
configurable, within bounded resource accounting. Exact classes, queue plumbing,
worker counts/defaults and configuration representation are implementation details
for the staged review, not new operation semantics. Do not expose the internal
container library or worker machinery through the public Application API.

## Execution modules and combined Steps 5–6

The [Backend module boundaries](../../architecture/system.md#backend-module-responsibilities)
separate business semantics from runtime execution policy:

- `operations/` retains AccessOrchestrator and MutationOrchestrator. They define
  prerequisites, business sequencing and results; MutationOrchestrator retains
  editing ownership rules. They do not own worker pools or scheduling policy.
- `execution/` contains RequestCoordinator and any justified private worker support.
  It owns scheduling, conflict reservations, capacity and completion accounting.
  Moving RequestCoordinator here is implemented without changing execution behavior.
- `runtime/` continues to assemble Backend resources and control lifecycle,
  draining and finalization; `ipc/` owns connection handling and delivery.

Access/Mutation classifies Application behavior. Execution groups classify resource
and concurrency requirements, so they need not map one-to-one. Catalog queries
and authoritative Loads are both Access operations but have different resource
needs. A Mutation may read files, persist changes and synchronize in-memory state.
Do not add a generic operation hierarchy or a class for each worker category.

The combined stage evolves RequestCoordinator into one short in-memory
coordination thread with shared bounded execution capacity for Catalog queries
and authoritative Loads, plus one sequential Mutation stream. These are initial
execution policies, not permanent business modules. The request coordination thread must not do
filesystem work or long query evaluation. ExecutionPool provides private worker support: one shared Access pool (default 4 workers, positive worker count) and one sequential Mutation worker.

Coordination reserves access before dispatch. AccessOrchestrator on the Access
worker then captures a consistent view for a query or resolves the current locator
for a Load; both preparation and execution run on that same worker. Shared Catalog publication, Coordinator
status and editing ownership require explicit synchronization before concurrent
dispatch. Existing orchestrator methods must not simply be called concurrently.
Business validation stays in operation handling; scheduling requirements and
reservations must not become a second implementation of those rules.

Workers report completion to release reservations/capacity and wake eligible work;
connection workers retain response delivery. Conflicting Loads wait without
occupying filesystem workers, and Mutation execution remains sequential. Complete
failure paths and draining across all execution groups before safe finalization.
No general pipeline, forwarding, cache or Push protocol is introduced here.

Step 6 verification is part of each Step 5 increment, not a postponed final pass:
test concurrent views/publications, query responsiveness during slow filesystem
work, Load/Move ordering, editing ownership and disconnect lifetime, resource
limits, failures and shutdown. Measure retained memory and responsiveness before
returning to the roadmap. See the current contracts for implemented boundaries and the verification report
for actual platform coverage; later roadmap work still requires authorization.

## Implemented thread and request flows

### Names and connection ownership

RequestCoordinator owns admission, scheduling and completion/drain coordination.
One request coordination thread performs short preparation and dispatches execution
workers. CatalogCoordinator remains the separate Storage consistency component.
The generic internal handler seam stays sequential; Backend Serve supplies prepared
concurrent operations.

Keep one bounded connection worker per connection. It receives/decodes, submits,
sends Accepted/Rejected, waits for completion/disconnection and sends the final
response. It owns no execution queue or conflict policy and does not occupy an
execution worker while waiting. A shared transport event loop remains unnecessary
for this baseline. Push outgoing delivery during waits is a 4.4a follow-up.

### Lifecycle and thread ownership

The authoritative current [thread/state flow](../../contracts/backend-lifecycle.md#backend-lifecycle-call-flow--implemented)
shows construction, normal/failure draining, reporting and finalization. It replaces
the earlier proposed diagram here. All execution ends before eligible Catalog
finalization; usable connections remain available through Report.

### Request dispatch and completion

The authoritative [request/thread diagram](../../contracts/ipc.md#current-thread-and-request-flow)
shows connection admission, preparation, workers and completion. Its
[execution safety rules](../../contracts/ipc.md#execution-safety-and-bounds) describe
capacity, fairness and reservations. No per-connection executor, new public lock,
Busy response, cancellation or second ownership registry was introduced.

## Immutable Catalog design

### Ownership and view lifetime

Keep MetadataCatalog as the owner of current metadata, invariants and publication.
Introduce a small Backend-private read view retaining one immutable state.
Names such as `CatalogState`, `CatalogView` and `acquire_view()` are explanatory;
exact declarations await implementation review.

```text
MetadataCatalog ── current ──► S2
Query A ────────────────────► S1
Query B ────────────────────► S2

S1 and S2 share unchanged container structure.
Publishing S2 does not modify S1; Query A finishes normally.
```

One Application query/Browse uses one view throughout all its lookups, filtering,
sorting and result assembly. Root browsing must not obtain Records from one
state and Directories from another. Convenience Catalog methods may acquire a
view internally, but multi-read callers must explicitly retain a common view.
Query functionality stays in the Catalog module for now; see
[APP-QUERY-SEPARATION](application.md#query-responsibility-follow-up).

### State and containers

Publish a single state containing all four typed metadata maps, three Root
membership sets and Directory-to-member sets. Preserve UUID uniqueness across
Entry types, valid locators, Directory-kind checks and membership invariants.
Metadata/locator changes and reverse membership changes become visible together.

Replace mutable containers with persistent immutable containers using structural
sharing: a new state copies small root handles; updates allocate affected paths
and values while sharing unchanged branches. Wrapping the existing mutable maps
in `shared_ptr` alone does not provide this behavior. Do not deep-copy the whole
Catalog for every Mutation or implement a custom MVCC/search engine.

The implemented baseline uses Immer v0.9.1 maps/sets with boxed metadata values.
Its persistent containers preserve in-memory versions, not durable files. See the
[implemented publication walkthrough](../../contracts/storage.md#how-a-metadata-change-is-published)
for candidate construction, structural sharing, publication and failure diagrams.
Platform coverage and memory/performance evaluation remain scoped to actual
verification; choosing the dependency does not establish those results.

Preserve existing observable UUID ordering and query tie-breaking explicitly if
the chosen persistent containers are unordered. Existing listing and deterministic
Index encoding behavior must not depend accidentally on container iteration order.

### Construction and publication

Retain one writer initially. Validate against the current state, construct a
private candidate, then publish its complete state reference once. Validation or
allocation failure before publication preserves the previous current state.
Identical updates remain successful no-ops and preserve `unpersisted` behavior.

Use CatalogCoordinator::read_state() to capture status and roots under the same
short publication mutex used by managed callbacks. One Mutation writer owns changes. No query-long or per-Entry Catalog
read lock is required. Do not claim lock-free operation.

Internal versions identify Catalog states, not Entry revisions, timestamps,
Mutation receipts or execution proofs. Numeric generation identifiers are not
required merely to retain views; their use for notification handoff is undecided.

Queries release views after constructing owned result metadata, rather than
retaining whole Catalog states in connection sessions. Reference ownership reclaims
obsolete structures when no longer needed. Long readers can still retain old
changed data; measure this and bound concurrent work. No arbitrary expiration or
forced destruction of an active reader is introduced.

### Coordinator, status and persistence

Preserve Repository persistence followed by the matching stored Coordinator
callback; do not add a second Catalog synchronization path. If authoritative
persistence succeeded but Catalog construction/publication fails, retain the old
state and mark Coordinator Inconsistent under existing failure semantics. A
structurally valid old view does not prove agreement with authoritative files.

Concurrent reads also require synchronized Coordinator status handling. Capture
a view and applicable availability/consistency assessment under a coordinated
read-start boundary. Coordinator remains the status owner; do not independently
duplicate readiness in every state. Implementation must preserve a consistent
publication/status/notification ordering. Retaining a view must not hide subsequent availability
failures or silently authorize unsafe writes.

Rebuild and Index decode construct and validate a complete candidate before
adoption. Failed reconstruction preserves current contents and applicable existing
status. Index encoding uses one retained view. Keep Index schema/ordering,
authoritative Entry formats, session marker eligibility and shutdown save rules.
`unpersisted` means Index projection changes, not unsaved Entry content; writer-owned
bookkeeping may remain serialized with shutdown saving. Concurrent background
Index saving is not introduced. Copy/move/adoption semantics of MetadataCatalog
must be reviewed because Coordinator currently adopts decoded Catalog objects.

### Why this supersedes earlier traversal proposals

Per-Entry or batched reader–writer locking with writer preference, resumable UUID
cursors and full-input copying were considered earlier. Immutable views replace
that proposed Catalog query mechanism. Stable readers no longer require iterator
repair, writer-driven pausing, coroutines or full-Catalog copying. This does not
remove authoritative filesystem coordination or scheduling fairness rules.

## Filesystem execution and Edit Locks

Editing APIs now provide ownership-only acquisition/release and separate existing
Loads. This replaces the earlier combined `acquire_edit()` plus authoritative
Snapshot behavior. Frontends compose workflows:
acquire then load, or view first and acquire later. Acquiring ownership does not
certify that previously displayed content is current; reloading under ownership
is the safe editing workflow. Load failure does not implicitly release ownership;
the composing Frontend must decide/release it, with connection cleanup as fallback.

The revised ownership-only contract is:

- Acquisition returns identifiable success or an Application error, with no
  Snapshot or authoritative file read.
- Same-owner reacquisition succeeds without changing ownership or reloading data.
- Another owner's lock produces Lock Conflict, not temporary Busy or a queued
  ownership request.
- Release preserves existing ownership checks and harmless unlocked release.
- Acquisition/release consult synchronized in-memory ownership state. An executing
  Mutation must already have its required exclusive protection; acquisition
  observes that state rather than separately checking or queuing behind a
  filesystem operation. Required immediate-operation protection brackets execution
  without adding leases or persistent locks. Read-only Loads do not own editing
  rights and do not prevent acquisition.

`BackendConnectionHandle::acquire_edit(id)` returns
`AccessResult<std::monostate>` through the shared IPC protocol. This split is
implemented together with the concurrent coordination described below.

Concurrent Loads may proceed. A conflicting Move/Mutation and Load must be ordered
across current-locator lookup and authoritative access; a retained old Catalog
view does not preserve an old file location. No implicit UUID discovery/Refresh
repairs such conflicts. A Load encountering an actively mutating Record waits in
Backend scheduling, without occupying a filesystem execution worker while waiting.
After the Mutation resolves, it obtains the current locator and performs its normal
authoritative read. It returns a normal final result/error; no Busy, freshness
polling, resubmission or implicit cancellation is required. Later conflicting Loads
must not indefinitely overtake a ready waiting Mutation. If the Mutation fails,
the subsequent Load follows the resulting Repository/Coordinator safety state;
waiting itself does not certify successful persistence or repair inconsistency.

```text
Mutation A: persistence → Catalog synchronization / failure handling → release
                                                                       │
Load A:     pending in coordination ───────────────────────────────────────┘
              → current locator → authoritative read → final response
```

This replaces the earlier Busy proposal for both lock acquisition versus active
Mutation and Load versus active Mutation. Ownership conflicts remain explicit
Application failures; ordinary bounded admission rejection remains separate.

**TODO CAT-LOAD-FORWARDING — Deferred Optimization / Open Design Details:**
Consider serving a pending Load from the preceding Mutation's in-memory result
instead of rereading the file. This is not needed for correctness of the scheduled
waiting baseline. Revisit after that baseline is implemented and duplicate-load
cost is measured. Define eligibility, publication/durability and failure behavior,
content/locator identity and lifetime before forwarding; prepared content must not
be silently presented as confirmed authoritative data. This TODO does not
authorize a general content cache or speculative read protocol.

Keep long-lived Edit Locks connection-owned. Accepted actions retain the necessary
ownership after disconnection, preventing cleanup from racing ahead of queued
work. Owning a lock does not monopolize a worker while the user edits. Another
connection cannot bypass ownership by choosing a higher priority operation.

Keep sequential authoritative Mutations initially. Future independent parallel
Mutations may still serialize brief Catalog publications, but the Backend must
verify independence and Repository/Coordinator thread safety; a request's promise
is insufficient. Directory UUID identity means Rename alone is not grounds to
lock every member; inspect concrete conflicts before adding broad reservations.

## Query results and Push handoff

**Timing:** Push coverage/retention is implemented in 4.4a; Frontend
integration in 4.4b. This section does not expand the pre-4.3b refactor scope.

A query uses one coherent captured Catalog view; it does not chase every change
until completion. This replaces the earlier requirement that the initial result
itself include every update completed before returning. Later publications must
be covered by ordered notifications, including changes made while it executes.
No Frontend freshness polling is required.

Shared notification distribution routes safe relevant events to connections;
connection outgoing handling retains them until its query result is queued first.
Events arriving during the handoff must follow the same ordering. Registration/
view acquisition and notification coverage need a gap-free boundary. A dedicated
notification thread, generation counter or per-Entry event journal is not yet
required. Notifications do not belong in RequestCoordinator's resource accounting path.

A Catalog-changed invalidation recommending an event-driven requery is an accepted
simple initial payload direction; precise deltas/filter-aware subscriptions remain
open. Changes can make previously absent Entries match, so relevance cannot be
limited to the returned UUIDs. Determine coalescing, slow-connection bounds and
how to avoid excessive repeated requeries. No Push wire protocol or new IPC
functionality is approved by this document.

Keep synchronous final-result APIs and one outstanding request per connection.
Future Push reception must work independently of waiting for those responses,
with result-before-retained-event ordering. The subsequently approved
[paired-session design](application.md#paired-push-and-request-connections--implemented)
uses separate Request and Push connections with dedicated I/O workers under one
session. Cross-stream arrival order is not guaranteed: receipt ACK remains prompt
while event application may need Frontend retention; the implemented receiver retains request-time events; 4.4b must serialize UI result/event application.
One connection is not synonymous with one Frontend process. Separate submission/completion APIs remain an optional open
decision, not a required part of this refactor.

## Implementation review gates and deferred work

The architectural choices above are settled for this refactor. The following
table distinguishes implementation checks from work deliberately left for later;
neither this table nor recording the design authorizes implementation. Raise a
design question only if code inspection or measurements expose a concrete conflict
or an actual change to these agreed rules.

| Area | Status / next decision |
| --- | --- |
| Persistent container and metadata representation | Implementation evaluation: library, immutable values, deterministic ordering and allocation/performance costs |
| View/update interfaces | Implementation review: exact declarations, common-view callers, Catalog copy/move/adoption and failed-candidate tests |
| Publication/status boundary | Refactor implementation review: coordinate view acquisition and readiness changes; preserve a future notification integration boundary |
| Push notification handoff | Later Push integration (4.4a/4.4b): registration and ordered delivery without gaps; not a pre-4.3b prerequisite |
| Runtime execution | Implemented baseline under IPC-06: independent query/Load execution, short in-memory coordination, a configurable bounded Access pool, shared scheduling and memory budgets |
| Filesystem conflicts | Apply agreed scheduling/ownership rules and queued Loads; verify handling after persistence failure without replacing it with generic Busy |
| Directory interactions | Inspect existing operations for concrete conflicts; apply the same scheduling rules, raising only previously uncovered semantic cases |
| View retention | Refactor implementation evaluation: concurrent query limits and retained-memory measurements |
| Push notification retention | Later Push integration: coalescing, bounded state and slow-consumer handling; not a pre-4.3b prerequisite |
| Lifecycle integration | Implementation review: drain all accepted lanes, retain ownership and end workers before safe finalization |
| Query extraction | APP-QUERY-SEPARATION remains deferred/open; no separate query engine now |
| Load forwarding | CAT-LOAD-FORWARDING above; scheduled waiting is the complete baseline |
| Content cache / availability subscriptions | Deferred optimization/open details: measure duplicate Loads; queued waiting does not require a ready-notification subscription |
| Batch execution | Deferred design: safe step boundaries, admission/resource scope and Recovery prerequisites; not implemented here |
| Parallel Mutations / asynchronous public submission | Open future decisions; retain one Mutation executor and blocking calls initially |

Validation must include old-view stability while new state is published,
metadata/membership atomic visibility, unchanged query ordering and Index round
trips, no-op/failed-update behavior, failure containment, actual concurrent-reader
tests, long-reader/frequent-update retained memory, and query/update performance.
Measurements and native platform tests must be reported as executed, not inferred
from the architecture. Do not weaken existing tests or change persistence formats.

The application, IPC and Mutation plans continue to own existing Recovery,
receipts, RequestId/multiplexing, deduplication, exactly-once, Push and UI-worker
follow-ups. This plan introduces no competing receipt/recovery mechanism.
