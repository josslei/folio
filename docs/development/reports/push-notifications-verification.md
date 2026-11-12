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
**458/458 passed**, using native local socket/process access outside the sandbox.
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

### Shutdown timing investigation — 2026-11-12

An earlier 458-test parallel run failed three MetadataAccessIntegration tests on
child orderly-exit deadlines; all three passed on immediate rerun. After the
reviewed ownership/callback changes, a fresh native full run passed **458/458**
in 4.00 seconds. Each affected test also passed **30 consecutive repetitions**
with the three tests running concurrently (90 successful executions):

- `LargeContentAndMetadataMessagesAllowTwoPeersAndSubsequentPing`
- `MissingWrongTypeAndStaleErrorsLeaveConnectionUsableWithoutDiscovery`
- `PrematureDisconnectAndInvalidMessagesDoNotAffectOtherConnections`

Inspection followed Frontend receiver termination, Request session cleanup,
IpcServer last-session stopping/wakeups, and the test child completion marker.
No runtime defect was confirmed. A concrete timing mismatch remains: the fixture's
`until()` budget is 8 seconds, while an unused Backend candidate may wait 10 seconds
for its first client. The fixture can launch multiple candidates and waits for all
of their completion markers. This is a possible contributor, not an established
explanation of the earlier failures; neither production deadlines nor test budgets
were changed to make this investigation pass.

**PUSH-VERIFY-01 — Deferred Investigation:** if orderly-exit failures recur, capture
the failing candidate's lifecycle progress before fixture termination, distinguish
an unused candidate's startup wait from an active Backend shutdown stall, and align
the fixture deadline with the actual lifecycle contract if confirmed. Revisit before
claiming shutdown timing reliability; passing repetitions do not prove absence of a
race. No runtime fix is claimed by this report.

A subsequent native parallel stress run repeated every regular test ten times:
**4,580 executions passed**, completing in 36.73 seconds. The earlier failures
remain unreproduced. The 8/10-second mismatch applies only to unused candidates;
it cannot explain a confirmed stall in a Backend that already served clients.
Accordingly it is not treated as the root cause or justification to relax the
shutdown test deadline. Distinguishing candidate identity/lifecycle position is
still required if the failure recurs.


Initial sandboxed full/repetition attempts failed because native local socket
access was denied; those runs are not evidence of a Backend shutdown regression.
The successful results above used native IPC access outside that restriction.

Linux, Windows, ThreadSanitizer and the opt-in resource-limit suite were not run.
macOS results do not establish runtime support on another platform.

## Deferred boundaries

Full UI application must serialize returned results before retained invalidations;
Push never overwrites Drafts or automatically queries/reloads. UI consumption and
bounded reconnection remain 4.4b. Subscriptions and coalescing remain open. Persistent
receipts, replay, cross-restart delivery, exactly-once processing and uncertain
Mutation reconciliation are not provided. See the traceable PUSH items in the
[Application plan](../plans/application.md#cross-connection-ordering-and-remaining-decisions).
