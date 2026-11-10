# Design principles and contract index

**Purpose:** preserve the decisions accumulated during implementation and make
high-risk constraints easy to reach. This is an index and summary, not a second
independent definition of each contract. Follow the linked owner for full semantics.
Implementation evidence below is navigation, not a claim that tests were rerun
or a proof against every failure.

## Architecture and authoritative state

| Principle / status | Contract owner | Implementation and verification |
| --- | --- | --- |
| Runtime-role separation; shared binary does not authorize bypassing IPC. Implemented. | [Module boundaries](system.md#runtime-role-module-structure--implemented) | [CMake targets](../../CMakeLists.txt), [dispatcher](../../app/src/main.cpp) |
| Directory is a first-class Entry; Record means Log/Memo/Todo only. Logical Root is not a fabricated Directory Entry. Implemented. | [Domain model](system.md#domain-baseline--implemented), [navigation](../contracts/access.md#operations) | [Core models](../../lib/include/folio/core), [Access tests](../../tests/application/metadata_access_test.cpp) |
| Entry files are authoritative; Catalog/Index are projections; unpersisted is Index state. Inconsistent blocks publication. Implemented. | [Catalog coordination](../contracts/storage.md#catalog-centric-flows--implemented-internal-capabilities) | [Coordinator](../../server/include/storage/catalog_coordinator.hpp), [tests](../../tests/storage/catalog_coordinator_test.cpp) |
| Peer Access/Mutation orchestrators borrow Storage; Repository plus matching Coordinator callbacks owns persistence/synchronization sequencing. Implemented. | [Orchestration boundary](../contracts/mutations.md#application-orchestration-boundary--reviewed) | [Orchestrators](../../server/include/operations), [mutation tests](../../tests/application/mutation_orchestrator_test.cpp) |
| Ordinary access uses current known locators, not implicit discovery/Full Refresh. Missing Catalog identity differs from stale authoritative location; usable inconsistent metadata may carry a warning. Implemented. | [Access errors](../contracts/access.md#errors-and-consistency) | [AccessOrchestrator](../../server/src/operations/access_orchestrator.cpp), [integration tests](../../tests/application/metadata_access_integration_test.cpp) |

## Consistency Checking and Refresh — approved, not implemented

The [4.3b plan](../development/plans/catalog-refresh.md) owns the detailed contract:
Browse remains Catalog-only; navigation requests a separate low-cost Check; anomalies
offer explicit Refresh rather than implicit repair. Scoped/Full Refresh reconciles
only scanned scope using a validated candidate. Refresh excludes Mutations while
Access continues; coordination never waits synchronously for operations. Check
reservations and the two special dispatch cases are tracked together there.
Refresh is projection reconstruction, not Draft/uncertain-Mutation Recovery.

## Lifecycle and failure containment

| Principle / status | Contract owner | Implementation and verification |
| --- | --- | --- |
| State machine controls transitions independently of state actions; run_backend shows the lifecycle. Implemented. | [Lifecycle responsibilities](../contracts/backend-lifecycle.md#module-failure-containment) | [Transition table](../../server/src/backend.cpp), [state-machine tests](../../tests/application/backend_lifecycle_test.cpp) |
| Acquire ownership before initialization; attempt applicable cleanup after ordinary failure; finalize only when eligible; release ownership last. Implemented. | [Lifecycle call flow](../contracts/backend-lifecycle.md#backend-lifecycle-call-flow--implemented), [Marker safety](../contracts/storage.md#session-lifecycle--implemented-internal-support) | [State handlers](../../server/src/runtime/backend_states.cpp), [state tests](../../tests/application/backend_states_test.cpp) |
| Stopping handles normal draining; Report also handles failures that bypass it; Finish remains mandatory. Usable streams survive until reporting opportunity; actual lifecycle notification is deferred. | [Lifecycle call flow](../contracts/backend-lifecycle.md#backend-lifecycle-call-flow--implemented) | [IpcServer](../../server/src/ipc/ipc_server.cpp), [runtime tests](../../tests/application/runtime_test.cpp) |
| Operational module errors are collected, not process termination. Nonfatal errors stay request-specific; fatal service failures reach lifecycle owner. Modules enforce safety, not a generic executor health test. Approved; current paths audited. | [Containment contract](../contracts/backend-lifecycle.md#module-failure-containment) | [Audit scope and evidence](../development/reports/backend-failure-containment-audit.md), [mutation tests](../../tests/application/mutation_orchestrator_test.cpp) |
| Drain attempts remaining accepted actions after errors/exceptions; completion is not operational success. Do not hide infrastructure failure with noexcept or claim incomplete draining is complete. Implemented. | [Drain exception contract](../contracts/backend-lifecycle.md#drain-exception-contract) | [RequestCoordinator::drain](../../server/include/execution/request_coordinator.hpp), [request coordination tests](../../tests/application/runtime_test.cpp) |
| Fix concrete preventable hangs; arbitrary hang freedom/bounded shutdown is not guaranteed. Never terminate operation threads or finalize beneath active work. Approved limitation. | [Hang boundary](../contracts/backend-lifecycle.md#unpredictable-hangs--agreed-limitation) | [Review record](../development/status.md#backend-02--preventable-hang-review-and-agreed-limitation) |

## Scheduling, mutation and editing

| Principle / status | Contract owner | Implementation and verification |
| --- | --- | --- |
| One shared bounded queue, short request coordination and independent connection handling. Queries/Loads use bounded workers; Mutations remain sequential. Connection workers never access live Storage. Implemented. | [Scheduling](../contracts/ipc.md#scheduling-and-lifecycle) | [RequestCoordinator](../../server/src/execution/request_coordinator.cpp), [runtime tests](../../tests/application/runtime_test.cpp) |
| Admission, execution and final confirmation differ. Acceptance is session-only; disconnection does not cancel accepted actions. Implemented. | [Scheduling](../contracts/ipc.md#scheduling-and-lifecycle) | [IpcServer](../../server/src/ipc/ipc_server.cpp), [runtime tests](../../tests/application/runtime_test.cpp) |
| Access and Mutation share bounded complete-message IPC, protocol v1; no chunks, multiplexing or separate Mutation transport. Implemented. | [IPC](../contracts/ipc.md#complete-messages-and-bounded-transfers) | [Shared IPC](../../lib/include/folio/ipc), [transport tests](../../tests/application/message_transport_test.cpp) |
| Errors answer what failed, what happened and available user choices. Cause/status/evidence/confirmation stay distinct; user actions are not automatic retries. Implemented contracts; Recovery UI deferred. | [Application errors](../contracts/access.md#errors-and-consistency), [mutation results](../contracts/mutations.md#results-and-evidence) | [ApplicationError](../../lib/include/folio/application/error.hpp), [mutation contracts](../../lib/include/folio/application/mutation.hpp) |
| Publication differs from durability; Applied can accompany later Catalog failure. Evidence is historical, not recovery authorization. Implemented for current operations. | [Failure mapping](../contracts/mutations.md#failure-mappings-and-evidence--implemented) | [I/O evidence](../../lib/include/folio/io/filesystem.hpp), [fault tests](../../tests/application/mutation_orchestrator_test.cpp) |
| Frontend generates MutationId; Backend generates Entry UUID. Identity does not imply deduplication, receipts or execution proof; missing response never fabricates NotApplied. Implemented; stronger guarantees absent. | [Results and identity](../contracts/mutations.md#results-and-evidence) | [Mutation contracts](../../lib/include/folio/application/mutation.hpp), [correlation tests](../../tests/application/runtime_test.cpp) |
| Update owns Full Editable Fields; Move owns membership/location; Frontend context is shared and not a revision token. Resolve current state before writing; no duplicate Catalog update. Implemented. | [Field model](../contracts/mutations.md#normal-execution-and-field-semantics--implemented) | [MutationOrchestrator](../../server/src/operations/mutation_orchestrator.cpp), [mutation tests](../../tests/application/mutation_orchestrator_test.cpp) |
| Acquire lock with authoritative Snapshot; full Update requires ownership. Immediate Move/Rename/status respects ownership without temporary lock records. Accepted actions retain session ownership after disconnect. Implemented. | [Lock lifetime](../contracts/mutations.md#edit-lock-ownership--implemented) | [ConnectionSession](../../server/include/ipc/connection_session.hpp), [lock and disconnect tests](../../tests/application/mutation_orchestrator_test.cpp) |

## Supporting systems and future boundaries

| Principle / status | Contract owner | Evidence / next decision |
| --- | --- | --- |
| Shared Configuration loader, authoritative readable factory template, atomic missing-file creation, no silent overwrite or migration. Implemented. | [Configuration](../contracts/configuration.md) | [Template](../../templates/folio_config.toml), [tests](../../tests/application/configuration_test.cpp) |
| Logging is diagnostic only; failures do not change lifecycle or original errors. No raw/sensitive payloads or terminal disruption. Implemented. | [Logging](../contracts/logging.md) | [Logging code](../../lib/src/logging), [tests](../../tests/application/logging_test.cpp) |
| I/O transfers bytes without domain semantics; Atomic Creation and Replacement have distinct preconditions and publication guarantees. Implemented. | [I/O](../contracts/io.md) | [I/O code](../../lib/src/io), [atomic tests](../../tests/storage/storage_test.cpp) |
| Scheduling is not Recovery. Restore-to-buffer differs from authoritative application; unpersisted drafts are not automatically conflict-free. Approved; Recovery deferred. | [Recovery](../development/plans/recovery.md#frontend-drafts-and-pending-operations) | [Mutation follow-ups](../contracts/mutations.md#traceable-follow-up-work) |
| Vault is a privacy domain, not a new Record type. Authorized plaintext may exist in memory; sensitive durable drafts require encryption. Locked metadata must not be invented with defaults. Approved; Vault deferred. | [Vault](../development/plans/vault.md), [Recovery security](../development/plans/recovery.md#vault-extension-and-security) | Directory visibility, restricted metadata, encryption and recovery protocols still require design. |
| Verification is platform-specific. macOS tests do not verify Linux/Windows; omit Windows release support if unverified. Approved policy; native verification pending. | [Release policy](../development/status.md#platform-release-verification-policy) | [IPC-08](../contracts/ipc.md#deferred-decisions) |

## Documentation and harness decisions

The [documentation/harness plan](../development/documentation-harness-plan.md)
records the approved organization, audiences, instruction discovery requirements
and implementation sequence. Formatting, selected static analysis and explicit
validation/fix commands are implemented; see the [harness inventory](../engineering/harness.md)
for their coverage and remaining verification obligations.

Existing MUT, IPC and BACKEND identifiers remain the follow-up references; this
index introduces no parallel TODO register. Design conflicts must be reviewed,
not resolved by automatically treating either documentation or code as correct.
