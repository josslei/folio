# Unified IPC protocol and runtime

## Implemented boundary

Shared transport, codecs and wire value types are under lib/. Frontend exchanges
and BackendConnectionHandle belong to app/; server connection management, admission
and execution belong to server/. Implementation-facing protocol headers are under
`lib/include/folio/ipc/`; they depend on shared contracts and
Core values, never Backend handler or Storage headers. This physical separation
does not change messages or runtime behavior.

Access and Mutation are categories of Application Operation, not C++ base classes.
Both use the existing per-user IPC connection, version-1 messages, positional
MessagePack encoding, bounded logical messages and sequential exchanges. The public
access point remains `BackendConnectionHandle`. Phase 4.3a adds concrete Normal
Mutation payloads/dispatch and Edit Lock requests to that same OperationRequest and
OperationResponse architecture; existing Query tags and payload arrays are unchanged.

The existing read payload arrays and alternative tags are unchanged. The former
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
and ReleaseEdit use the same admission/queue and return EditSnapshot or an empty
success. The Backend-private connection token is never encoded or supplied by a
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
| Hello / Ready | 1 / 2 | Readiness handshake |
| Ping / Pong | 3 / 4 | Connection health |
| Reject | 5 | Protocol/lifecycle rejection followed by closure |
| Request / Response | 6 / 7 | Complete Application request / final result |
| Accepted | 11 | Empty acknowledgment: admitted to this Backend session |
| Rejected | 12 | ApplicationError: refused before acceptance |

`MessageTransport` owns partial-transfer bookkeeping. `send_message` and
`receive_message` drive it using OS waits, not recurring sleeps. Linux/macOS use
`poll()` readiness with a close-on-exec wake pipe; Windows uses overlapped Named
Pipe I/O and `WaitForMultipleObjects` on completion/wake events. Windows buffers
and OVERLAPPED objects remain alive until completion or cancellation has drained.
The 64 KiB OS-transfer buffer is not a wire boundary.

Headers are validated before payload accumulation. At most 64 connection workers
exist, with one stream owner per worker. The Backend accepts no more than four
buffered Application exchanges concurrently: each reserves one maximum-sized
64 MiB exchange **before** accumulating a Request body, and holds that reservation
through queuing, execution and final delivery. An abandoned connection transfers
its reservation obligation to its accepted job until execution completes. This is
a conservative 256 MiB logical-buffer allowance, not an exact process-RSS cap:
MessagePack/domain objects, transient encoding copies, executor state and thread
stacks also use bounded resources. Lifecycle messages must be empty and cannot
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
per-operation semantic combinations await execution design. Publication is never
interpreted as durability.

The former chunk producer/assembler and all chunk/end message kinds remain
removed. No RequestId, multiplexing, replay or automatic reconnection is added.
Protocol version remains 1; admission changes this pre-release internal protocol,
so running Frontends and Backend must come from the same build. Future streaming
or incremental editing needs explicit operation semantics.

## Scheduling and lifecycle

```text
Frontend connections -> bounded IPC workers -> admission -> one shared FIFO
                                                        -> one executor
                         originating connection <- final result/error
```

The complete request is decoded/validated before queue admission. A single
`RequestScheduler` owns execution, with capacity 32 including its active job;
the stricter exchange-buffer reservations may reject earlier. Both current Queries
and Mutations use this scheduling boundary. No connection worker accesses
the Catalog. All operation execution is sequential for now; idle workers and the
executor block on OS events or condition variables, independently of each other.
A slow operation cannot prevent another connection's handshake, admission or Ping.

Admission atomically refuses or enqueues work. Accepted is sent before the final
response, even if execution finishes immediately. It promises responsibility only
within this session, not durable queuing, execution after a crash, or delivery.
The blocking Public API consumes this stage internally; no UI callback is added.
The Backend records Queued/Executing/Finished facts for delivery diagnostics;
these are historical scheduling facts, not authoritative persistence evidence.

A connection that disconnects after acceptance loses delivery, not its queued
work. Last-Frontend disconnection stops admission. Explicit shutdown/failure also
stops admission; every accepted job drains before Catalog finalization. Idle reception wakes on admission closure; usable connections remain available
through Report. Busy workers attempt final delivery with bounded deadlines.
Finish cooperatively closes connections and joins remaining workers. No worker outlives Storage resources.
The existing Bootstrap -> Serve -> Stopping -> Report -> Finish state machine
remains the sole lifecycle controller. Report drains if a failure bypassed Stopping; Finish retains an exceptional
drain fallback if earlier cleanup failed. See the detailed
[Backend lifecycle call flow](backend-lifecycle.md#backend-lifecycle-call-flow--implemented). Only then may `finish_session()` run, under RepositoryOwnership.
No Marker/Index algorithm or finalization eligibility rule changes.

Scheduling is in-memory session execution, **not Recovery**. Future Batch
operations must durably prepare required Recovery information during execution,
before their first dependent authoritative change; it need not precede Accepted.
Frontend Draft Persistence is independent. No Recovery mechanism is implemented.

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

## Deferred decisions

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
| IPC-03 | Deferred Implementation | Edit Locks are implemented in 4.3a; Push/lifecycle notifications in 4.4a and Frontend handling in 4.4b. Define recipients explicitly: request errors go to their originating Frontend; shared availability changes may notify affected Frontends without disclosing another Frontend's operation details. Scheduling does not implement these notification protocols. |
| IPC-04 | Deferred Implementation | Frontend UI/IPC Worker ownership and completion events, connection monitor replacement, bounded reconnection and ordinary state initialization on a new connection: 4.4b. Blocking methods must run off the UI thread; no extra manual Refresh is required solely by reconnecting. |
| IPC-05 | Deferred Implementation | Durable Batch Recovery preparation before protected authoritative changes; revisit Batch/Recovery design. Acceptance does not require persistence. Frontend Draft Persistence remains separate in Recovery phases. |
| IPC-06 | Open Design Decision | Concurrent Read execution requires a demonstrated need and Catalog access policy; initially all execution is sequential. |
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
`CatalogCoordinator::startup_load()` succeeds. No Ready precedes the durable
Marker. Shutdown closes admission/connections, explicitly calls `finish_session()`,
and releases ownership last. Failed finalization returns failure and retains the
Marker according to Storage's existing contract. Backend crashes release locks,
break connections and leave Marker-based reconstruction for the next session.
