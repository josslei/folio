# Backend working constraints

Applies to server/. Consult these contract owners before changing their behavior:

- Lifecycle/ownership: [Backend contract](../docs/contracts/backend-lifecycle.md).
  Keep the state-machine module independent of state actions; run_backend shows
  transitions. Preserve the thread/drain/report/finalization ordering.
- Scheduling: [IPC contract](../docs/contracts/ipc.md#scheduling-and-lifecycle).
  Connection workers never access live Storage/editing state. RequestCoordinator
  prepares bounded Access work and one sequential Mutation stream. Preserve conflict
  reservations and the synchronized Catalog capture/publication boundary.
- Storage: [Storage contract](../docs/contracts/storage.md). Keep Repository
  persistence and matching Coordinator callbacks as the synchronization path;
  preserve metadata coverage guards that require explicit approval to change.
- Mutation handling: [Mutation contract](../docs/contracts/mutations.md). Resolve
  current locators and check readiness/lock/membership before writes. Keep
  AccessOrchestrator and MutationOrchestrator peers borrowing Storage; preparation
  stays in memory, with filesystem work confined to execution workers.
  The weak lock map belongs to MutationOrchestrator; accepted actions retain session
  ownership. Do not let worker disconnection race ahead of accepted work.

Ordinary request errors return to their originating connection; shared lifecycle
notifications are separate deferred work, not a reason to broadcast request details.
