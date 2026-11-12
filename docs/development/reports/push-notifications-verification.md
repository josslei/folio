# Push notification implementation and verification

**Status:** implementation record for Phase 4.4a; the [IPC contract](../../contracts/ipc.md#push-notifications-and-session-ownership)
owns current requirements. Complete TUI integration is not implemented.

## Implementation

- Shared `application/push_notification.hpp` defines small Record, Catalog-scope,
  Edit Lock and shared availability invalidations. Metadata/content are independent
  flags; each notification carries one change notice with its own ACK. No bodies, locators,
  MutationId, owner identity or private request errors are included.
- `ipc/application_protocol.hpp`, `messagepack_codec.hpp` and the existing framed
  transport carry Push/receipt controls. Protocol Version 1, Application alternatives,
  64 MiB request/response bounds and one outstanding request remain unchanged.
- `server/src/ipc/ipc_server.cpp` pairs independent Request/Push workers through a
  single-use secure capability. Session termination wakes both paths. Per-recipient
  bounded queues and one outstanding acknowledged Push isolate slow Frontends.
  Delivery failures do not cancel accepted work or change authoritative results.
- `BackendConnectionHandle::PushNotificationReceiver` in `app/src/backend_connection_handle.cpp` continuously validates and ACKs Push without
  UI consumption. `BackendConnectionHandle` retains synchronous Application calls
  and adds event-driven wait/drain integration. Request I/O stays on its calling
  worker; a full Frontend UI Request-worker is deferred to 4.4b.
- CatalogCoordinator publication observers, CatalogMaintenance scoped publication,
  MutationOrchestrator ownership transitions and Backend Report produce events at
  actual state transitions. They do not use RequestCoordinator completion order.
  Update compares body fields before replacement; Move invalidates both listings
  while preserving content. Refresh invalidates potentially changed loaded content.
- Final session-reference cleanup releases Edit Locks after accepted work, with
  notification to remaining sessions. Report sends a sanitized fatal service error
  while sessions remain usable. Report blocks for pending receipt ACKs or failure up to the configured delivery
  timeout before Finish closes sessions (PUSH-REPORT-01).

## Verification

macOS Debug incremental parallel build and the complete regular CTest suite:
**451/451 passed**, using native local socket/process access outside the sandbox.
The sandbox itself denied listener binding with `Operation not permitted`.
Formatting, local documentation links and whitespace checks are also checked.

New coverage includes codec validation; session pairing/reused capabilities;
independent Push during incomplete large Requests and blocked execution; immediate
ACK before UI consumption; request-time retention; notification order; ACK mismatch,
timeout and overflow; accepted-work preservation; shutdown wakeups; Record metadata
versus content, membership and Refresh invalidations; disconnection lock cleanup;
request-error isolation; fresh reconnection; old immutable view survival; notification
producer failure leaving authoritative/Catalog success intact; and fatal Report
notification sanitization/delivery before Finish.

Earlier parallel native passes encountered child-process orderly-exit deadlines.
The isolated raw-connection case passed 12 repetitions and the multi-session
Mutation/lock case passed 15 repetitions; the final full run passed. The cause of
those earlier timing failures was not conclusively established, so this record does
not claim all subprocess timing sensitivity has been eliminated.

Linux, Windows, ThreadSanitizer and the opt-in resource-limit suite were not run.
macOS results do not establish runtime support on another platform.

## Deferred boundaries

Full UI application must serialize returned results before retained invalidations;
Push never overwrites Drafts or automatically queries/reloads. UI consumption and
bounded reconnection remain 4.4b. Subscriptions and coalescing remain open. Persistent
receipts, replay, cross-restart delivery, exactly-once processing and uncertain
Mutation reconciliation are not provided. See the traceable PUSH items in the
[Application plan](../plans/application.md#cross-connection-ordering-and-remaining-decisions).
