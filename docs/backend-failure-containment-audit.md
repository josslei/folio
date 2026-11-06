# Backend failure-containment audit — BACKEND-01

Completed against the implemented Normal Storage and Query paths on 2026-11-06.
No confirmed production-code violation was found; additional tests cover the
gaps identified during review. This closes BACKEND-01 for the current code, not
for future Mutation handlers or arbitrary failures that prevent execution.

## Scope and findings

| Boundary | Finding |
| --- | --- |
| NormalRepository | Validation, filesystem and codec failures return typed errors. Mutation execution catches standard and nonstandard exceptions, tracks possible publication before later steps can fail, and retains the original operation error if failure notification also fails. Reads/scans build local results; failed scans are not adopted as complete inventories. |
| MetadataCatalog | Validation rejects invalid metadata, UUID/locator collisions and invalid membership before committing. Insertions roll back membership if insertion throws; updates prepare potentially allocating membership changes before nonthrowing swaps. Rebuild validates an isolated candidate and swaps only on success. Lookup/query results are copies. |
| CatalogCoordinator | Startup failure remains Unavailable. Failed Refresh preserves the previous snapshot/status. Post-publication or Catalog-update failure marks Inconsistent, which blocks Index publication and managed creation. Save failure leaves metadata unpersisted; Marker cleanup is attempted only after Index safety is established. Marker-cleanup uncertainty makes the session Unavailable and supports explicit retry. |
| Application Query handler | Unloaded/Unavailable/Stopped sessions return MetadataUnavailable. Inconsistent metadata remains readable with the established warning. Missing, malformed or stale Records return request-specific Application errors without automatic scans. Unexpected read exceptions are caught at this read-only boundary and return a safe InternalFailure. |
| IPC/executor/lifecycle | Application errors are final responses to the originating connection, not service-failure signals or broadcasts. Exceptions escaping the handler/encoding boundary are retained as service failures and wake the lifecycle owner. The executor continues attempting accepted work. Bootstrap and finalization failures retain typed diagnostics; secondary lifecycle failures do not overwrite the primary error. |

Reviewed implementations: `server/src/storage/normal_repository.cpp`,
`metadata_catalog.cpp`, `catalog_coordinator.cpp`, the callback template in
`server/include/storage/catalog_coordinator.hpp`, and
`server/src/metadata_access.cpp`, `request_scheduler.cpp`, `ipc/ipc_server.cpp`,
`runtime/backend_states.cpp` and `runtime/backend_state_machine.cpp`.
The reviewed module implementations contain no explicit process-exit, abort or
terminate calls. This observation alone is not the containment guarantee:
state preservation and the handling boundaries above are also required.

## State and exception boundaries

An Inconsistent Catalog is not globally unusable: established Query behavior
permits reading its snapshot with a warning, while unsafe Index publication and
managed creation are rejected. Unavailable metadata rejects Application access.
Ordinary request failures do not initiate Backend shutdown. Failure fatality is
contextual: inability to bootstrap or an escaped runtime service exception enters
the existing lifecycle termination path; a malformed selected Record does not.
No blanket classification of every Storage error as fatal was added.

Unexpected allocation/library exceptions can still escape individual low-level
methods. Catalog insertion/rebuild ordering preserves the existing snapshot;
Coordinator update callbacks mark Inconsistent before rethrowing to Repository
mutation containment. Request and lifecycle boundaries provide the applicable
outer handling. This audit does not promise successful error allocation or
cleanup when further execution is impossible, nor prove the absence of hangs.
It does not introduce universal `noexcept` annotations or catch-and-continue
behavior for an arbitrary corrupted process.

## Verification

Existing tests exercise Catalog rejection/rebuild preservation, pre/post-publication
Repository failures, throwing callbacks, primary-error preservation, partial
Moves, Inconsistent save refusal, failed Index/Marker synchronization, lifecycle
diagnostic ordering, ownership release and executor continuation after throws.

Additional coverage:

- `NormalRepositoryCallbacksTest.NonstandardPreflightExceptionDoesNotPoisonSession`:
  a nonstandard pre-write exception returns OperationFailure without publication;
  the session remains Ready and a subsequent valid creation succeeds.
- `MetadataAccess.MalformedRecordFailureDoesNotPoisonOtherReadsOrFinalizeSession`:
  a malformed selected Record returns InvalidData; unrelated reads and metadata
  queries work, and the session Marker remains active.
- `MetadataAccess.FinalizedSessionRejectsAccessAndManagedCreation`: a finalized
  session rejects Application access and managed creation before publication.
- Extended `MetadataAccessIntegration.MissingWrongTypeAndStaleErrorsLeaveConnectionUsableWithoutDiscovery`:
  a second connected Frontend still receives its own valid browse response and
  Ping after the first Frontend's request-specific failures.

Parallel Debug build and all **378 normal CTest tests passed on macOS**.
Resource-limit tests were not rerun for this audit; Linux and Windows runtime
verification remains pending under the platform release policy.

## Required follow-through in later phases

- **Deferred Implementation — Phase 4.3a:** Mutation handlers must check session
  and operation preconditions before authoritative writes, then map actual
  persistence evidence. Repository is intentionally independent of Coordinator
  ownership; a post-persistence callback cannot prevent a write. Existing
  `creation_check()` covers managed creation, not every future Update/Move.
- **Deferred Implementation — Phase 4.4a/4.4b:** shared availability/lifecycle
  notifications need explicit recipient rules. Current request failures already
  return only to their requesting connection. No temporary broadcast was added.

These are integration obligations for unimplemented features, not evidence that
the current Query API can mutate data or that future handlers have been audited.
