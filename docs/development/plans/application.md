# Remaining Application design

**Status:** Push transport implemented; remaining UI work is approved direction, not implementation authorization.
Normal Access, Mutations and Edit Locks already exist. Their current contracts are
[Access](../../contracts/access.md), [Mutations](../../contracts/mutations.md),
[IPC](../../contracts/ipc.md) and [lifecycle](../../contracts/backend-lifecycle.md).

## Frontend integration and events

Public calls remain synchronous. The Frontend session has continuous Push reception; the future UI IPC worker owns Request I/O
and returns completion events and notifications to the UI. Preserve buffers on disconnection; suspend
Backend-dependent actions, use bounded reconnection and initialize current state
on the new connection. Do not replay unconfirmed Mutations or require a separate
manual Refresh solely because reconnection occurred. Full CLI grammar and editor
presentation remain deferred.

Push invalidation transport, publication hooks and bounded receipt delivery are
implemented. The [IPC contract](../../contracts/ipc.md#push-notifications-and-session-ownership)
is authoritative for pairing, payloads, ACK, handoff, limits and Report behavior.
The [Catalog/concurrent Access plan](catalog-concurrency.md#query-results-and-push-handoff)
explains the result/event requirement. Request-specific errors stay with their
originating Frontend; shared availability uses sanitized notifications.

### Paired Push and request connections — implemented

One logical session owns identity, lifecycle and Edit Locks. Independent Request
and Push I/O prevents either stream's transfers from occupying the other worker.
The Frontend's synchronous caller performs Request I/O; a dedicated receiver ACKs
Push even when no Application request or UI consumer is active. The Backend has
two dedicated workers per paired session. See the
[thread diagram and limits](../../contracts/ipc.md#push-notifications-and-session-ownership).

### Receipt ACK and session failure — implemented

ACK confirms complete valid message reception, before UI work. No retransmission;
timeout, invalid receipt or overflow fails the logical session. Accepted actions
retain identity and execute under existing draining rules. No replay or guarantee
of UI processing, future delivery or Mutation success is implied.

### Cross-connection ordering and remaining decisions

Session-wide coverage starts before read capture. Request-time invalidations are
ACKed immediately and retained. The future UI must apply the returned result before
consuming retained invalidations; streams themselves have no shared arrival order.
The Frontend provides event-driven wait/drain APIs without implementing view refresh.

| ID | Status | Remaining work / revisit condition |
| --- | --- | --- |
| PUSH-01 | Implemented baseline | One-use secure pairing, bounded initialization, sequence receipts, timeout, independent I/O and resource limits are implemented. Revisit concrete defaults if operational measurements justify changing them. |
| PUSH-02 | Open Design Decision / Deferred Implementation | Recipient filtering/subscriptions and coalescing remain absent; reconsider when real UI traffic demonstrates need. Initially broadcast safe invalidations including to the initiator. No generic event bus. |
| PUSH-03 | Deferred Implementation | Complete serialized UI result/event handling, refresh decisions, Draft preservation, bounded reconnection and current-view initialization in 4.4b. The transport coverage/retention boundary is implemented; it must not be defeated by unordered UI callbacks. |
| PUSH-04 | Not Guaranteed | Report delivery has one bounded best-effort window. No persistent delivery, cross-restart replay, UI processing acknowledgment or exactly-once guarantee; revisit only with a separately justified persistence design. |
| PUSH-05 | Open Design Decision | Edit Lock invalidations expose an Entry ID without owner identity. Decide any additional read-only lock-status presentation/API when 4.4b requires it; acquiring still requires an explicit operation. |

## Navigation and verification

Catalog queries remain memory-only. The implemented Backend
[4.3b navigation and Consistency Check contract](catalog-refresh.md#consistency-check-and-navigation)
separates Browse/display from a navigation-triggered Check and user-authorized
Refresh. Its generic Backend Check initially counts all direct entries without
classifying files. That plan owns scope, count overhead and mismatch behavior;
do not add automatic scans to Browse or repeat the contract here.

## Query responsibility follow-up

**TODO APP-QUERY-SEPARATION — Open Design Decision / Deferred Implementation:**
Revisit separating metadata filtering, sorting and query execution from
MetadataCatalog after the [Catalog concurrency design](catalog-concurrency.md) is
elaborated.
Catalog would retain metadata, membership/index invariants and state publication;
a query component could consume a stable view. The boundary, types and any
reuse with future content search remain undecided, so do not extract a generic
query framework now. Revisit when Catalog view APIs are settled or content-search
design establishes concrete shared requirements. Full-text search remains outside
the current refactor; do not duplicate Catalog data or assume both search kinds
need the same evaluation engine.

## Design gates

Before implementing the corresponding feature, settle its open contracts: event
recipients/order, UI worker ownership/reconnection, revision checking and Recovery
reconciliation and the remaining [maintenance follow-up decisions](catalog-refresh.md#traceable-follow-ups). Existing
ownership, protocol-v1 and lock contracts are not reopened merely by moving this
plan. Native platform behavior still needs platform-specific verification.

See [roadmap](../roadmap.md), [IPC follow-ups](../../contracts/ipc.md#deferred-decisions)
and [Mutation follow-ups](../../contracts/mutations.md#traceable-follow-up-work).
