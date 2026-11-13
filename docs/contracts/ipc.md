# Unified IPC protocol and runtime

## Implemented boundary

Shared transport, codecs and wire value types are under lib/. Frontend exchanges
and BackendConnectionHandle belong to app/; server connection management, admission
and execution belong to server/. Implementation-facing protocol headers are under
`lib/include/folio/ipc/`; they depend on shared contracts and
Core values, never Backend handler or Storage headers. This physical separation
does not change messages or runtime behavior.

Access and Mutation are categories of Application Operation, not C++ base classes.
Both use the Request connection of one paired logical session, version-1 messages, positional
MessagePack encoding, bounded logical messages and sequential exchanges. The public
access point remains `BackendConnectionHandle`. Phase 4.3a adds concrete Normal
Mutation payloads/dispatch and Edit Lock requests to that same OperationRequest and
OperationResponse architecture; existing Query tags and payload arrays are unchanged.

Record Load alternative tags are retained, but their payloads now use
`Annotated<Snapshot>` (value plus warnings), exposing Backend-owned
`EntryReadOnly` status without adding compatibility claims to Mutation requests.
Protocol version remains 1; pre-release peers must use the same current schema. The former
`access_codec` implementation is now `application_codec`, backed by one shared
private `messagepack_codec.hpp`. The shared codec handles Core identifiers,
metadata, Snapshots, logical locations and Application errors alongside MutationId,
AuthoritativeChangeStatus, MutationStepEvidence, MutationEvidence, MutationFailure
and the templated confirmed MutationResponse. The template allows future approved
success types without fixing a speculative business contract today.

Mutation IDs are validated as UUIDs. Confirmed response decoding takes the expected
MutationId and reports Protocol failure on mismatch. The Frontend exchange
invalidates its connection on an ID mismatch or wrong success type, just as reads
do for protocol failures. Normal Mutations now have end-to-end exchanges. NoMutationResponse
and SubmissionState are local Frontend observations and have no wire codec.
MutationRequest contains MutationId plus a concrete, tagged payload. Confirmed
results contain canonical Core metadata or structured MutationFailure. AcquireEdit
and ReleaseEdit use the same admission/queue and return empty success
(`std::monostate`) or ApplicationError. Acquisition performs no authoritative Load;
existing typed Load requests remain separate. The obsolete EditSnapshot response
alternative has been removed under Protocol Version 1's pre-release policy. The Backend-private connection token is never encoded or supplied by a
Frontend. No opaque payload, new RequestId or parallel transport is introduced.

## Complete messages and bounded transfers

Every request or response is one length-prefixed message, regardless of size:

```text
Body length (4 bytes) | Protocol version (2 bytes) | Kind (2 bytes) | Payload
```

The length and header integers are big-endian. The message body is limited to
**64 MiB**, including the four bytes for version/kind; the maximum MessagePack
payload is therefore 64 MiB minus four bytes. The four-byte length prefix is
outside the body. This is a memory-safety policy, not an OS transfer limit.
There is no separate 64 KiB protocol limit or request/response chunk sequence.
`protocol_version` remains **1**, checked on every message. There is no
negotiation or separate Mutation schema version.

| Kind | Value | Meaning |
|---|---|---|
| Hello / Ready | 1 / 2 | Request handshake returns a one-use pairing capability; Push Ready completes pairing |
| Ping / Pong | 3 / 4 | Connection health |
| Reject | 5 | Protocol/lifecycle rejection followed by closure |
| Request / Response | 6 / 7 | Complete Application request / final result |
| Accepted | 11 | Empty acknowledgment: admitted to this Backend session |
| Rejected | 12 | ApplicationError: refused before acceptance |
| Push / PushAck | 14 / 15 | Ordered invalidation envelope / complete-message receipt |

`MessageTransport` owns partial-transfer bookkeeping. `send_message` and
`receive_message` drive it using OS waits, not recurring sleeps. Linux/macOS use
`poll()` readiness with a close-on-exec wake pipe; Windows uses overlapped Named
Pipe I/O and `WaitForMultipleObjects` on completion/wake events. Windows buffers
and OVERLAPPED objects remain alive until completion or cancellation has drained.
The 64 KiB OS-transfer buffer is not a wire boundary.

Headers are validated before payload accumulation. At most 64 logical sessions
(128 physical connection workers, including incomplete handshakes) exist, with
one stream owner per worker. The Backend accepts no more than four
buffered Application exchanges concurrently: each reserves one maximum-sized
64 MiB exchange **before** accumulating a Request body, and holds that reservation
through queuing, execution and final delivery. An abandoned connection transfers
its reservation obligation to its accepted job until execution completes. This is
a conservative 256 MiB logical-buffer allowance, not an exact process-RSS cap:
MessagePack/domain objects, transient encoding copies, executor state and thread
stacks also use bounded resources. Ordinary lifecycle messages are empty; pairing carries exactly a UUIDv4 capability. They cannot
bypass this allowance with large payloads. No unbounded per-connection queues exist.

A capacity rejection before the body can be buffered sends Rejected and closes
that connection; an entirely received request rejected by queue admission keeps
its connection usable. Malformed complete Query payloads also receive Rejected
with InvalidQuery and remain usable. Invalid headers, lifecycle sequencing and
pipelined requests receive Reject/closure, never a fabricated execution failure.

Handshakes and stalled transfers have three-second bounds. Complete request
reception has a thirty-second total bound. Idle established connections sleep
until OS readiness or shutdown; there is no periodic IPC wakeup. A successful
Accepted acknowledgment ends the Frontend's ordinary admission deadline: waiting
for the final result has no short completion deadline, because earlier queued
operations may legitimately take time. Once a final transfer begins, inactivity
and complete-transfer bounds apply. EOF still reports connection loss. No
liveness guarantee, heartbeat during an outstanding call, or cancellation is
implied by acceptance. Startup discovery retains bounded launch/connect retries;
those retries are separate from established connection progress.

MessagePack collections are limited to 100,000 items, nesting to 32, and exact
field counts, types, enum values, UUIDs and variant tags are checked. Truncation
and trailing MessagePack content are rejected. Evidence validation is structural;
current Normal handlers construct operation-specific evidence as specified in the
[Mutation contract](mutations.md#failure-mappings-and-evidence--implemented). New
operations require their own approved combinations. Publication is never
interpreted as durability.

The former chunk producer/assembler and all chunk/end message kinds remain
removed. No RequestId, multiplexing, replay or automatic reconnection is added.
Protocol version remains 1; admission changes this pre-release internal protocol,
so running Frontends and Backend must come from the same build. Future streaming
or incremental editing needs explicit operation semantics.

## Scheduling and lifecycle

Automatic navigation Checks share pending/running execution by logical scope and
retain the outcome for the Backend session, including failure. Repeated completed
requests use the ordinary short reservation but CatalogMaintenance does not count
files again. Completed outcomes are owned by CatalogMaintenance, not scheduling. Explicit rechecking is a
later TUI design decision, not a new protocol flag here.

The implemented [Consistency Check/Refresh extension](../development/plans/catalog-refresh.md)
uses the same OperationRequest/Response codecs, admission and final-result path.
Maintenance runs on the Access pool with Mutation exclusion; Refresh pending state
blocks new Mutations without blocking coordination or ordinary Access. Checks also
exclude Catalog publication while comparing counts. Full Refresh requests join
pending/running execution through the common one-execution/multiple-ticket
completion association; each request keeps its own resource accounting and reply.
Completed executions are not cached. Protocol version remains 1.

**Important follow-up — CONS-03:** implement a coherent priority system; Refresh
must have highest priority. Use ordinary eligible selection for Check/Refresh for now, without a special
priority path; pending Refresh still defers Mutations as an execution constraint.
Fairness/starvation and Batch priority need design at the dispatch-selection TODO.

### Queued work and scheduling plans

Each pending queue item is a `RequestCoordinator::Work`, keeping scheduling
information and executable work together:

```text
Work
 +-- plan       Scheduling requirements (Plan)
 +-- prepare    Callback retaining the request and preparing its action
 +-- execution  Shared result state and subscribing request tickets
```

`plan(request)` classifies a decoded Application request without executing it or
looking up Catalog/Repository state. Its `Plan` identifies the execution lane,
optional Entry conflict identity and maintenance scheduling/duplicate-sharing
information. It contains neither the business payload nor its result; the request
is retained by the preparation callback rather than stored as another queue item.

```text
Decoded request + preparation callback
                  |
                  v
       plan(request) + callback + execution
                  |
                  v
            Pending Work queue
                  |
          eligible + reserve
                  |
                  v
       Prepare and execute action
                  |
                  v
          Completion inbox
                  |
       release reservations; complete tickets
                  |
                  v
       Originating connection(s) deliver results
```

Preparation and execution run on an Access worker for Access/Maintenance work;
short Mutation protection is prepared on coordination before dispatch to the
Mutation worker. Control work stays on coordination. A running action is removed
from the pending queue; the coordinator continues dispatching eligible requests.
Workers post completion and wake coordination, which processes completions before
selecting further work.

Lane describes execution/reservation requirements, not priority. Check and Refresh
share Maintenance but retain their different pending-Mutation exclusion and
duplicate-sharing information in `Plan`. A shared execution can have multiple
tickets: each accepted request receives its own final response, while only one
action runs. This is scheduling bookkeeping, not persistent acceptance or Recovery.

See [RequestCoordinator declarations](../../server/include/execution/request_coordinator.hpp)
and [implementation](../../server/src/execution/request_coordinator.cpp) for the
actual fields and scheduling rules.

### Current thread and request flow

BackendContext owns one IpcServer and one RequestCoordinator as peers.
IpcServer owns the connection workers and borrows the RequestCoordinator. Connection
workers only receive/decode, admit, acknowledge and deliver. Admission remains a
short synchronized call on the connection worker; it does not access business
state. One request coordination thread handles eligibility/reservations/completions,
with one shared Access pool (default 4, positive worker count), and one
Mutation worker. No periodic progress polling or per-connection execution queue
is introduced. Blocking Public APIs and one outstanding request per connection
are unchanged.

```text
CONNECTION WORKER        REQUEST COORD. THREAD          SHARED ACCESS WORKERS          MUTATION WORKER
----------------------   ---------------------------   ----------------------------   -----------------------
Receive / decode
admit(plan, prepare) ---> Shared bounded queue
Send Accepted            Select eligible work
Wait / disconnect        |
                         +-- Editing control: check / acquire / release in memory
                         |
                         +-- Catalog query ----------> Capture status + immutable view
                         |                             Filter / sort / build result
                         |                             Encode response
                         |
                         +-- Load: reserve Entry ----> Capture status + current locator
                         |                             Read / validate authoritative file
                         |                             Encode response
                         |
                         +-- Mutation: reserve Entry
                              Prepare protection ------------------------------------> Check / persist / synchronize
                                                                                       Encode response
                         <---------------------------- Report completion <------------- Completion
                         Release capacity/reservations
Read ticket <----------- Signal completion
Send final response
```

The rows are alternative paths, not steps of every request. Preparation/action
callbacks run without queue or ticket mutexes. Access preparation runs on the same
Access worker as execution; Catalog capture and locator interpretation belong to
AccessOrchestrator, not scheduling. Only editing control and immediate Mutation
protection are prepared on coordination. Large result construction and
encoding run on execution workers; editing-control results remain small. Workers
publish ticket results and bounded completion records; coordination releases
reservations/capacity before signaling delivery. It need not route result bodies
through another queue. Accepted is always sent before the final response, even
if execution finishes before the acknowledgment is delivered.

### Execution safety and bounds

RequestCoordinator retains one shared bounded pending queue; capacity 32 includes
queued, preparing and executing requests across all groups. Existing exchange
reservations (default 4) remain held from reception through delivery/execution,
bounding large request/result exchanges; messages remain limited to 64 MiB.
ExecutionPool contains no scheduling/business policy: coordination dispatches at
most its worker count. Query and Load requests share this capacity: if all Access
workers are occupied, accepted Access work remains pending. Filesystem saturation
can delay queries; no reserved query capacity or priority scheme is introduced. Completion storage is reserved to request capacity. Queries
retain views only while preparing/running; Loads retain a resolved locator rather
than a whole Catalog version. Result construction may allocate owned metadata;
these bounds are concurrency limits, not a hard total-RSS guarantee for arbitrary
Catalog or Entry sizes.

Loads of the same Entry may run together. A Mutation waits for active Loads of
its target UUID and excludes later conflicting Loads through persistence, Catalog
synchronization/failure handling and completion. A Load queued behind a Mutation
captures its locator only after that reservation is released, without consuming a
filesystem worker while waiting. Later conflicting Loads cannot overtake an earlier
waiting Mutation. Mutation FIFO and exactly one executing Mutation are preserved;
unrelated Loads, Catalog queries and editing control can progress independently.
No Busy, implicit scan, forwarding, retry or cancellation is added.

Current Directory Rename rewrites the UUID folder's metadata without moving member
files. Loads do not depend on that name, so Rename does not reserve every member.
Directory creation, target membership validation and Record moves remain serialized
with other Mutations. New Delete/Batch operations must define additional
conflict scope before dispatch; a UUID reservation is not a universal future rule.

CatalogCoordinator::read_state() captures readiness and immutable roots under the
same publication mutex used by managed Repository callbacks. No filesystem work or
query evaluation holds that mutex. MutationOrchestrator separately synchronizes
long-lived ownership and immediate-mutation protection. Other Frontends cannot
acquire editing rights during a protected mutation; existing ownership still
wins. Query results describe one coherent captured state, not every publication
before delivery. [Push notification retention](#push-notifications-and-session-ownership) covers concurrent publications without making the query restart.

Generic internal IpcServer handlers without a preparer retain serialized execution;
normal Backend Serve supplies the prepared concurrent path. This compatibility
seam does not make arbitrary handlers thread-safe.

### Disconnection and lifecycle

Accepted is session-only responsibility, not durable queuing, execution success or
a crash/liveness guarantee. A connection loss cancels delivery, not accepted work.
Accepted captures retain the originating session until execution/protection cleanup,
so Edit Locks cannot expire ahead of accepted work. Error routing remains specific
to the initiating connection.

Last disconnection or explicit shutdown/failure closes admission. Draining attempts
all accepted work across every execution group; operation errors/exceptions are
retained and do not abandon subsequent requests. RequestCoordinator joins its
coordination thread and drains/joins all pools before Catalog finalization. Usable
connections survive draining for Report; Finish closes/joins remaining workers.
A hung action may still prevent draining. No forced thread termination or new
completion deadline is introduced.

See the [Backend lifecycle flow](backend-lifecycle.md#backend-lifecycle-call-flow--implemented).
Scheduling is in-memory session execution, **not Recovery**. Future Batch operations
must durably prepare required Recovery information before their first dependent
authoritative change during execution, not before Accepted. Frontend Draft
Persistence remains independent and unimplemented.

Implementation: [RequestCoordinator](../../server/include/execution/request_coordinator.hpp),
[execution workers](../../server/include/execution/execution_pool.hpp),
[IpcServer](../../server/src/ipc/ipc_server.cpp),
[Access preparation](../../server/src/operations/access_orchestrator.cpp),
[editing/Mutation preparation](../../server/src/operations/mutation_orchestrator.cpp).
The [combined Steps 5–6 plan](../development/plans/catalog-concurrency.md)
records design rationale and later work.

## Results and errors

Admission Rejected carries ApplicationError but is not a final execution failure.
After Accepted, Response carries the existing confirmed Application result/error.
Valid final Application errors leave a connection usable. Transport errors, malformed responses
and response-type mismatches invalidate the public connection handle.

Mutation exchanges keep confirmed success, confirmed MutationFailure
(including a possible authoritative Unknown), and a locally unconfirmed response
separate. An ID mismatch is not a confirmed Mutation failure. Missing confirmation
must never fabricate NotApplied. A MutationId provides correlation, not execution
proof, safe replay, deduplication or exactly-once execution.

## Push notifications and session ownership

**Implemented:** one logical session owns a Request connection and an independent
Push/ACK connection. Backend workers are dedicated to their streams. Frontend
Request I/O stays on the synchronous caller (the future UI IPC worker); a dedicated
`PushNotificationReceiver` continuously receives Push. No UI thread, Request transfer,
query evaluation or notification consumer is needed to issue ACK.

```text
FRONTEND                                   BACKEND
Logical session                            Logical session / Edit Lock identity
  Request caller/worker <---- Request ---->   Request I/O worker
    blocking Public API                      admission / final-result delivery
  Push receiver         <---- Push ------>   Push I/O worker
    validate / ACK                           ordered send / receipt deadline
    bounded retained events                  bounded recipient queue
          |                                        ^
    future UI consumer                  publication / lock / lifecycle producers
```

### Pairing and lifetime

Hello carries a typed MessagePack `HelloPayload`: `RequestHello` establishes the
Request role, while `PushHello` carries the pairing token. Request Hello returns Ready with a cryptographically random
UUIDv4 capability generated through Core's existing randomness facility. Push Hello
on a second connection consumes that capability exactly once. Unknown/reused
capabilities are rejected. Pairing must finish within three seconds; no Application
operation executes before pairing. Push notification coverage is installed before Push
Ready and before Request execution can capture a Catalog view. Endpoint user/OS
access restrictions remain in force; capabilities are never logged.

A terminal connection failure, invalid Push/ACK, notification overflow or missing
ACK terminates the logical session and wakes both I/O paths. Each stream is closed
by its owning worker/caller; termination tokens interrupt blocked transfers.
Accepted work retains its session identity independently and is not cancelled.
Lock cleanup runs only when the final accepted/session reference is released.
The Push transport does not create a second editing identity.

### Payload and production

`PushNotification` is a variant of the four change-notice types, without an
additional payload wrapper. Each notification is sent and
ACKed separately; one operation may publish several ordered notifications:

| Notice alternative | Safe fields / meaning |
| --- | --- |
| Record | Entry UUID, metadata/content possibly changed flags |
| Catalog | Logical RefreshScope, whether loaded content within scope may also be stale |
| Edit Lock | Entry UUID; ownership/availability must be reconsidered |
| Availability | Ready/Inconsistent/Unavailable/Stopping/Fatal, optional sanitized shared ApplicationError |

No Entry bodies, metadata values, physical locators, session identities, MutationId
or request-specific errors are replicated. All connected paired sessions receive
events, including the initiator. No subscriptions or coalescing are implemented.

Managed Catalog callbacks enqueue events inside the publication boundary. Update
compares authoritative content/notes/subtasks with submitted fields, so metadata-only
edits need not trigger a full Load. Move preserves body content and invalidates both
listings. Directory changes invalidate the kind's Root listing. Successful Refresh
invalidates its scope including possibly changed loaded content; scanning metadata
does not certify unchanged bodies. Uncertain authoritative changes invalidate
shared Catalog state conservatively, without claiming that the Mutation succeeded.

Actual Edit Lock acquire/release and final disconnected-owner cleanup generate
availability invalidations. Same-owner reacquisition is not a transition. Changes
to Catalog readiness generate shared availability events; `unpersisted` alone does
not. RequestCoordinator completion order is not used as publication order.

### Delivery, bounds and handoff

The Push worker sends a MessagePack `[sequence, notification]` envelope, with a
strictly increasing, nonzero connection-local unsigned sequence. One Push is
outstanding at a time. PushAck echoes only that sequence, immediately after complete
valid reception. It confirms neither UI processing nor authoritative persistence.
No retransmission or replay occurs. Invalid, missing or mismatched receipts fail
the session; the send/ACK exchange has a thirty-second deadline by default.
A timeout is session failure, not proof that the OS connection was physically lost.

Each Backend recipient queue and Frontend retained queue defaults to at most 256 events (independently configurable through
[IPC resource settings](configuration.md#push-delivery-resource-settings));
an outstanding Push still occupies its Backend queue slot. Producers never wait
for network I/O. Overflow fails the affected session rather than silently losing
freshness. Each notification contains one notice and at most 4096 encoded bytes;
Push envelopes are rejected above 8192 bytes and ACK messages above 32 body bytes.
These smaller bounds do not change the 64 MiB Application message limit.

Coverage lasts for the whole paired session. The receiver ACKs immediately but
retains invalidations while a synchronous Application request is outstanding.
`take_push_notifications()` returns none during that interval. The Frontend integration
must apply each completed synchronous result before consuming subsequent retained
invalidations on its serialized UI/event path. `wait_push_notifications()` provides an
event-driven off-UI-thread wait without a deadline; a stop token cancels only
the wait, and session closure also wakes it. Receipt does not depend on calling it. This closes
the transport handoff without a shared cross-stream arrival order or RequestId.
An old immutable query view may complete; retained invalidations then invalidate
that result. Phase 4.4b must preserve result-before-event application rather than
posting unrelated result/event callbacks in arbitrary UI order. Push never writes
Buffers or initiates Query/Load automatically.

### Report and Finish

Stopping closes admission and drains accepted work, retaining usable sessions.
Report publishes a sanitized Fatal availability error when lifecycle diagnostics
exist, retaining usable sessions until Finish. Report then waits for Push delivery with the configured delivery timeout.
Delivery/ACK failure cannot change a Mutation result. Receipt is not established merely by enqueueing;
an unreachable Frontend cannot hold shutdown indefinitely. Finish terminates
sessions, wakes and joins I/O workers, then performs eligible Catalog finalization
and releases RepositoryOwnership. Failures first encountered during finalization
after session closure remain in local diagnostics; Finish does not reenter Report.
Report resolves pending delivery before Finish, subject to receipt failures/timeouts (PUSH-REPORT-01).
Arbitrarily hung operations remain outside shutdown guarantees. Normal final disconnection needs no delivery to departed
Frontends. Reconnection starts fresh; normal Query/Load initialization restores
current views without event replay or an automatic manual Catalog Refresh.

## Deferred decisions

The implemented [Push session design](../development/plans/application.md#paired-push-and-request-connections--implemented)
pairs Request and Push connections under one lifecycle/ownership identity, with
dedicated I/O workers for independent progress. Each complete Push requires an
immediate receipt ACK; ACK timeout fails and closes the entire paired session,
without resend or cancellation of accepted operations. This contract owns the implemented mechanism; the plan tracks remaining UI and optional delivery enhancements.

### IPC-01 — Event-driven waiting (Implemented; native verification incomplete)

Periodic Backend progress and Frontend response polling have been removed.
macOS runtime tests cover readiness, disconnection, timeout and shutdown wakeups;
Linux and Windows native execution remains required before claiming verified
support for each platform. Windows may be omitted from the first release if
verification and fixes are not completed; it does not block other platforms.
The current welcome TUI's existing connection monitor remains separate from this
transport; its replacement belongs to Frontend UI/IPC Worker integration below.

| ID | Status | Missing capability / revisit condition |
|---|---|---|
| IPC-02 | Implemented for current Normal operations | Phase 4.3a adds execution, current-state/lock preconditions, final evidence and MutationId validation. Admission rejection remains local SubmissionState::Rejected; Accepted without final confirmation remains distinct. Future operations extend the same infrastructure. |
| IPC-03 | Implemented transport; Deferred Implementation | Paired Push/ACK and shared lifecycle notifications are implemented. Complete Frontend UI consumption/reconnection remains 4.4b; preserve request-specific error isolation. |
| IPC-04 | Deferred Implementation | Frontend UI/IPC Worker ownership and completion events, connection monitor replacement, bounded reconnection and ordinary state initialization on a new connection: 4.4b. Blocking methods must run off the UI thread; no extra manual Refresh is required solely by reconnecting. |
| IPC-05 | Deferred Implementation | Durable Batch Recovery preparation before protected authoritative changes; revisit Batch/Recovery design. Acceptance does not require persistence. Frontend Draft Persistence remains separate in Recovery phases. |
| IPC-06 | Implemented transport handoff; UI deferred | Concurrent Catalog queries and bounded authoritative Loads use RequestCoordinator with one sequential Mutation worker; ownership, conflict reservations and draining are integrated. [Design and review](../development/plans/catalog-concurrency.md) records the boundary. Paired-session coverage and request-time event retention are implemented; serialized UI application remains 4.4b. |
| IPC-07 | Open Design Decision | Request cancellation semantics need operation-specific safety rules. Ceasing to wait or disconnecting does not cancel accepted work. |
| IPC-08 | Deferred Verification | Linux/Windows runtime, access controls, overlapped I/O cancellation and shutdown need native verification before claiming support for that platform is verified. Linux testing awaits an environment; Windows may be deferred to release preparation and omitted from the first release if not verified. |

The existing MUT TODOs in [mutation contracts](mutations.md#traceable-follow-up-work)
remain authoritative for RequestId/multiplexing, schema-versioning review after
Minimum TUI, receipts/cross-restart lookup, deduplication/idempotency, revision
checking, Draft Recovery and compound-operation Recovery. Exactly-once execution,
safe automatic replay and execution proof from MutationId alone are **Not
Guaranteed**. A hung operation can delay orderly draining indefinitely; Accepted
is not a liveness guarantee and there is no safe forced-operation cancellation.

See the [agreed unpredictable-hang boundary](backend-lifecycle.md#unpredictable-hangs--agreed-limitation).
A stalled executor must not be bypassed by unsafe Catalog finalization or forced
thread termination. No watchdog, progress tracking, automatic termination, or
new operation timeout policy is introduced; existing transport deadlines remain.

Request and response payloads use shared `codec::encode<T>()` and
`codec::decode<T>()` directly. Their distinct types enforce message direction;
there are no separate access encode/decode wrappers. Mutation response correlation
validation remains separate from structural decoding.
## Connection readiness and startup

Frontend discovery/launching and Backend readiness are separate: successful process
creation does not prove ownership or Ready. [Process startup and ownership](backend-lifecycle.md#process-startup-and-ownership)
defines launching and locking. Frontend BackendConnectionHandle owns one established
connection; current welcome-TUI disconnection handling is not the future UI worker
or reconnection design.

Unix uses a user-owned 0700 directory and 0600 Unix socket, with OS peer UID
verification. Runtime is preferred; a private State/ipc directory is the fallback.
Windows uses duplex local Named Pipes with a current-user SID DACL and remote
clients rejected. Endpoint names are not authentication. RepositoryOwnership is
required before stale endpoint cleanup, binding, Catalog startup, or finalization.
Only stale sockets are reclaimed; live endpoints are preserved.

Handshake and transfer-inactivity bounds remain three seconds; total request
reception is bounded to thirty seconds. A ten-second initial registration window
permits launching Frontends to connect. Acceptance is session-only and waiting in
the queue has no short operation-completion deadline. The final disconnection
stops admission and drains accepted work, including work whose connection is lost.
Admission closure wakes idle reception without closing usable connections.
On normal shutdown, accepted work and bounded final delivery settle before Report;
a failure that bypasses Stopping drains accepted work in Report; Finish closes
remaining connections and joins workers before Catalog finalization.

The endpoint is bound under ownership before Storage initialization so startup
errors can be returned, but no successful handshake is processed until
`CatalogCoordinator::initialize()` succeeds. No Ready precedes the durable
Marker. Shutdown closes admission/connections, explicitly calls `finish_session()`,
and releases ownership last. Failed finalization returns failure and retains the
Marker according to Storage's existing contract. Backend crashes release locks,
break connections and leave Marker-based reconstruction for the next session.

Check and Refresh share the RequestCoordinator Maintenance lane: both execute on
Access workers with the same active maintenance/Mutation exclusion. Operation
identity remains separate: pending Refresh defers new Mutations, pending Check
does not. Lane is not priority; the future priority system must select Refresh
ahead of Check despite their shared lane. Ticket sharing remains operation-specific.

**PUSH-REPORT-01 — Implemented:** after draining accepted work and publishing
fatal notices, Report blocks until pending Push messages are ACKed, their sessions
fail, or one shared configured `ipc.push_ack_timeout_ms` deadline expires. A
timed-out recipient is terminated independently. Only then may Finish close the
remaining sessions. ACK proves receipt, not UI processing. Fatal producer failures
reach the lifecycle owner; an unusable transport cannot guarantee receipt.

Fatal infrastructure and Push production failures use the Backend-owned
[FatalErrorReporter](backend-lifecycle.md#shared-fatal-error-reporting), not an
IpcServer-owned failure service. Operation modules retain injected callback
boundaries; RequestCoordinator reports unexpected execution exceptions through
the same Backend sink.
