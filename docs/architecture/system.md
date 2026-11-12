# System architecture

**Purpose:** human-oriented explanation of the implemented structure and its
rationale. For a compact working baseline, read [overview](overview.md); precise
requirements live in the linked [principle owners](principles.md).

```text
                         folio executable
                        (shared dispatcher)
                         /              \
               Frontend process       Backend process
                 app/                   server/
                   |                       |
                   +------ local IPC ------+
                   |                       |
                   +---- shared lib/ ------+

Runtime authority: Backend only
Persistent authority: Entry files, not Catalog or Index
```

Both roles link shared code; neither role links the other's implementation as an
API dependency. Shared deployment does not authorize direct Frontend Storage access.

## Domain baseline — Implemented

```text
Entry
├── Record
│   ├── Log
│   ├── Memo
│   └── Todo
└── Directory
```

These categories imply no C++ inheritance. All four independent classes are first-class Entries. Directory's distinct fields and layout justify specialized operations, but never a lower conceptual status. `RecordType` names only Log/Memo/Todo; `DirectoryKind` is Memo/Todo. Within a Normal Catalog all four share one UUID namespace. Normal/Vault are storage/privacy domains, not new identities; future transfers must preserve identity. Cross-domain enforcement is not implemented.

[Public headers](../../lib/include/folio/core) define:

| Model/value | Existing contract |
| --- | --- |
| Id | UUIDv4 generation with libsodium randomness and stduuid parsing; canonical strings; identity is independent of path |
| Snapshot / restore | Typed complete state; restore preserves UUID and creation data without generating another identity |
| TimestampContext | Absolute system-clock timestamp, original timezone string, optional UTF-8 location |
| DateContext | Calendar date, timezone string, optional location; distinct from an instant |
| Log | UUID, created/modified contexts, title, Markdown content, Log date, ordered tags, privacy; defaults Protected |
| Memo | UUID, created/modified contexts, title, Markdown content, tags, optional directory_id, privacy; defaults Normal |
| Todo | UUID, created/modified contexts, title, notes, status, priority, optional DueDate/directory_id, privacy, ordered embedded subtasks; defaults Normal |
| Directory | UUID, Memo/Todo kind, UTF-8 name; no body, privacy field, timestamps, parent_id, or owned member list |
| DueDate | DateContext or TimestampContext; optional on Todo |
| Subtask | Todo-owned content and completion flag; not independently persisted or indexed |

Todo status is Pending/Completed and priority None/Low/Medium/High. Privacy is Normal/Protected/Hidden and has no ordinary post-creation setter. These values alone provide no encryption. NormalRepository accepts only Normal privacy; the default Protected Log therefore needs an explicit Normal choice for current plaintext persistence.

Core validation is deliberately limited: restore validates enum values and applicable calendar dates; Directory validates kind, Todo validates due dates/status/priority and subtask indices, and Log validates Log dates. Core does not verify filesystem membership, timezone databases, or persistence consistency. Empty Todo title/notes are allowed; localized placeholders belong to Frontends. Do not infer uniform validation on all constructors/setters from restore validation. See [domain implementations](../../lib/src/core).

Logs have no user Directory. Memos/Todos support one level, optional membership on the Record, and no nested Directories. The Catalog derives reverse membership; Directory never duplicates it.

## Current component and build boundary — Implemented

[Root CMake](../../CMakeLists.txt) builds three static libraries: `folio_shared`
(`lib/`), `folio_frontend` (`app/`) and `folio_backend` (`server/`). Both roles
depend on shared code; neither role depends on the other. The single `folio`
executable is their shared dispatcher. Only lib/include propagates shared headers; app/include and server/include remain
role-private, with explicit dispatcher access; only internal tests receive private source paths.
FTXUI belongs to the Frontend and CLI11 to the dispatcher. Dependencies remain
pinned submodules; no database or JSON serialization is introduced.

Private process code uses `folio::application::internal`. Domain/Storage namespaces are `folio::core`, `folio::core::storage`, `folio::core::storage::codec`, and Storage `internal` atomic helpers. The shared core::Result<T, E> is a variant; subsystem signatures select their own error contracts.

| Component | Responsibility / evidence |
| --- | --- |
| [NormalRepository](../../server/include/storage/normal_repository.hpp) | Authoritative Normal Entry operations; targeted identity verification, explicit UUID discovery, scoped/full scan and direct-entry counts |
| [MetadataCatalog](../../server/include/storage/metadata_catalog.hpp) | Four typed maps, derived UUID membership sets, non-recursive listing and Record metadata search; no filesystem I/O |
| [CatalogCoordinator](../../server/include/storage/catalog_coordinator.hpp) | Borrows owner-supplied Catalog; startup, full Refresh, Index shutdown persistence, stored synchronization callbacks and consistency status |
| [CatalogMaintenance](../../server/include/operations/catalog_maintenance.hpp) | Backend Check/Refresh workflow; logical scope resolution, scan reconciliation and candidate publication through Coordinator |
| [IndexCodec](../../server/include/storage/index_codec.hpp) | Pure deterministic MessagePack encode/decode; temporary validated Catalog, no file I/O |
| [I/O](../../lib/include/folio/io/filesystem.hpp) | Generic byte reads, atomic create/replace, and durability operations |
| [Storage common types](../../server/include/storage/common.hpp) | Storage-specific errors, mixed Repository results, and effective-path validation |

There is no separate BinaryIO, IndexStore or IndexBuilder. Frontend launching
lives under `app/src/`; Backend ownership/lifecycle under
`server/src/runtime/`. The [dispatcher](../../app/src/main.cpp) selects the role;
only the Backend initializes Catalog lifecycle and finalizes its Index/Marker.
The historical `binary_io_test.cpp` filename does not denote a production module.

## Backend responsibility overview

**Implemented.** This tree groups responsibilities, not runtime order,
ownership or strict dependency layers. Parentheses identify classes/structs;
brackets identify logical modules. Prepared operation dispatch routes Access requests
to AccessOrchestrator::handle() and mutation/editing requests to MutationOrchestrator.

```text
[Backend]
|
+-- [Lifecycle and exclusive ownership]
|   +-- Backend entry: run_backend() -- free function
|   +-- Lifecycle transition enforcement (BackendStateMachine)
|   +-- [Lifecycle state actions]
|   |     Bootstrap / Serve / Stopping / Report / Finish
|   +-- Resource ownership and lifecycle data (BackendContext)
|   +-- Exclusive Repo and IPC listener ownership (RepositoryOwnership)
|
+-- [IPC]
|   +-- Connections, admission and response delivery (IpcServer)
|   +-- Connection identity and lifetime (ConnectionSession)
|
+-- [Execution]
|   +-- Shared bounded admission, scheduling and completion (RequestCoordinator)
|   +-- Shared bounded Access workers and one Mutation worker (ExecutionPool)
|
+-- [Operation handling]
|   +-- [Operation dispatch]
|   |     Routes to the appropriate peer below; no dispatcher class required
|   +-- Access orchestration (AccessOrchestrator)
|   |     Browse / Metadata Query / Directory Listing / Record Loading
|   +-- Mutation orchestration and editing (MutationOrchestrator)
|         Create / Update / Move / Directory Rename / Todo status changes
|         Edit Lock acquisition, release and ownership checks
|         Owns Edit Lock map; selects Repository calls/Coordinator callbacks
|
+-- [Storage]
    +-- Authoritative Normal Entry persistence (NormalRepository)
    +-- Metadata projection and queries (MetadataCatalog)
    +-- Catalog/session consistency coordination (CatalogCoordinator)
    +-- [Persistence support]
          Entry codecs / Index codec / locators / path validation
```

Access and Mutation handling are peers and both use Storage. AccessOrchestrator borrows its Storage dependencies and owns no mutable editing
state; MutationOrchestrator owns editing state.
Edit Lock requests are editing-control operations supporting mutation workflows,
rather than authoritative data mutations or ordinary read-only Access.

BackendContext owns the resource instances; BackendStateMachine enforces
lifecycle transitions. BackendContext owns IpcServer and RequestCoordinator as peers. Operation handlers
borrow Catalog, Repository and Coordinator through C++ references. Coordinator
borrows Catalog. Repository retains persistence sequencing and reports results
through matching Coordinator callbacks; Coordinator synchronizes Catalog.
ConnectionSession is Backend-private and is retained by workers/accepted work,
while Edit Locks reference it weakly. No additional controller or lifecycle
framework is implied.

### Backend module responsibilities

Application operation categories and execution resources are different dimensions.
Access versus Mutation describes business effects, not a required worker hierarchy.
Edit Lock acquisition/release is editing control. Worker allocation and conflict
scheduling are execution policy; they do not define new domain operation types.

| Directory under `server/include/` and `server/src/` | Responsibility and placement rule |
| --- | --- |
| `runtime/` | Backend lifecycle, resource assembly and lifetime, Repo ownership and safe finalization; not a container for all threaded code |
| `ipc/` | Connections, protocol handling, admission acknowledgments and response delivery |
| `execution/` | RequestCoordinator, scheduling eligibility, execution capacity and worker/completion coordination; one shared queue, short coordination and bounded execution workers |
| `operations/` | Application business rules and orchestration through AccessOrchestrator, MutationOrchestrator and CatalogMaintenance: prerequisites, lower-level call sequencing and public result interpretation |
| `storage/` | Authoritative persistence, Catalog projections and Coordinator consistency mechanisms |

Put a future operation helper under `operations/` when it implements Application
semantics by coordinating lower-level components. CatalogMaintenance follows this boundary. Delete or Trash orchestration could fit
there once separately authorized. One operation does not
require one class/file. Worker pools, queue accounting, codecs, filesystem
primitives and Core models have other owners. Repository still owns persistence
sequencing and invokes Coordinator callbacks; operation orchestration must not
duplicate that synchronization path.

```text
runtime/       Assembles components; owns startup, draining and finalization
    |
    +-- ipc/ --> execution/ --> operations/ --> storage/
                 When/where     What rules       Persistence and
                 work runs      work follows     Catalog mechanisms
```

The arrows summarize responsibilities, not mandatory class dependencies. IpcServer
borrows RequestCoordinator; BackendContext owns both runtime services.
The [combined Steps 5–6 plan](../development/plans/catalog-concurrency.md#execution-modules-and-combined-steps-56)
separates short coordination from costly execution. Concurrent worker groups are implemented; business categories and execution
resources remain separate dimensions.

## Ownership and boundaries

```text
CLI Frontend / TUI Frontend (many)
            ↕ public requests, responses, events over local IPC
Backend (one active owner for this user's configuration)
    ├── MetadataCatalog
    ├── CatalogCoordinator (borrows that Catalog)
    ├── NormalRepository / future VaultRepository
    ├── shared request queue / concurrent Access / sequential Mutations
    └── connection-owned Entry Edit Lock registry
```

Backend owns operations, lifecycle, metadata, event publication, and later Recovery/Trash/Vault/Git integration. Frontends own navigation, tabs, focus, and editing buffers. They do not own the authoritative Catalog or invoke private Storage APIs.

Formal Application APIs begin here; private Storage classes/errors/callbacks/handles must not simply become public. Public DTOs, request/error semantics, protocol versioning, and compatibility require explicit design. Domain APIs remain independent of presentation. Do not introduce an unnecessary Repository hierarchy or expose private Catalog types.

## Runtime-role module structure — Implemented

```text
app/                         Frontend-only code
  include/                   BackendConnectionHandle, launcher and TUI entry
  src/                       Frontend implementations
  src/main.cpp               Shared dispatcher (explicit ownership exception)
server/                      Backend-only code
  include/                   Role-private headers; matching responsibility dirs
  src/                       Backend entry point and implementations
  src/runtime/               Lifecycle, state machine and ownership implementations
  src/ipc/                   Backend connection management
  src/execution/             Request scheduling and execution coordination
  src/operations/            Access and Mutation business orchestration
  src/storage/               Storage implementations (headers in include/storage)
lib/                         Shared code; no dependency on either runtime role
  include/folio/             Core, Config, Logging, I/O and shared contracts
  src/                       Implementations and private helpers/test seams
```

`folio -> {folio_frontend, folio_backend} -> folio_shared`. There is still one
executable and separate Frontend/Backend processes at runtime. Presentation never
calls Storage directly. Frontend reuse across TUI, CLI or GUI does not make a
component shared between runtime roles: connection/launch code stays in app/.

Each module owns its headers. include/ separates declarations from sources; it does not itself promise an exposed API. The folio/ include prefix is reserved for lib/include/folio; app/include and server/include have no folio/ wrapper. Only lib/include is shared between roles; app/include and server/include are private to their owners, with explicit dispatcher/test access. Shared codec/transport headers under
`lib/include/folio/ipc/` are implementation-facing cross-module
interfaces, not a second business API. I/O filesystem/path interfaces are shared
through `lib/include/folio/io/`; atomic-write internals and test hooks stay private.
Wire value types no longer require a Backend handler header. Process error values
are shared; Frontend launching and Backend execution have separate headers.
No production target receives another module's private source path. Internal
tests explicitly receive the private paths they exercise. CMake visibility is a
build boundary, not language-level access control.

The earlier Phase 4.1a.5 physical layout is superseded. This role-based refactor
preserves the runtime responsibilities described in the [IPC contract](../contracts/ipc.md).

## Catalog-centric flow

```text
MutationOrchestrator: preconditions + current authoritative Snapshot
  → NormalRepository: authoritative persistence
    → matching CatalogCoordinator callback
      → MetadataCatalog: metadata + locator + reverse membership

AccessOrchestrator → Catalog metadata / targeted authoritative content reads
Lifecycle owner → CatalogCoordinator → Index save and eligible Marker cleanup
```

The [Storage contract](../contracts/storage.md#catalog-centric-flows--implemented-internal-capabilities)
defines callback ordering, consistency state and persistence guarantees.

## Runtime and future boundaries

See the [lifecycle call flow](../contracts/backend-lifecycle.md#backend-lifecycle-call-flow--implemented)
for thread creation, connection workers, executor draining, reporting and finalization.
The [IPC contract](../contracts/ipc.md#scheduling-and-lifecycle) defines admission and
execution; the [mutation contract](../contracts/mutations.md) defines field ownership,
Edit Locks and evidence. [Current status](../development/status.md) distinguishes
implemented capabilities from the [roadmap](../development/roadmap.md).

## Paired IPC session — implemented

```text
Frontend session                         Backend session
  Request caller / future UI worker        Request I/O worker
          |                                       |
          +----------- Request connection --------+
          |                                       +--> RequestCoordinator
          |                                              | Access pool
          |                                              | Sequential Mutation worker
  Push receiver                            Push I/O worker
          +------------- Push / ACK -------------+
          |                                       ^
  Retained invalidations                    Bounded recipient queue
          |                                       ^
  Future serialized UI consumer             Catalog / Edit Lock / lifecycle changes
```

Both connections share one lifetime and editing identity. Push I/O and receipt ACKs
remain independent of Request transfers and UI processing. Request-time events are
retained until result delivery; UI integration must apply the result before retained
invalidations. Report attempts sanitized fatal availability delivery with a bounded
window; Finish terminates sessions before Catalog finalization. The
[IPC contract](../contracts/ipc.md#push-notifications-and-session-ownership) owns
pairing, payload, bounds, ordering and failure details.

### Runtime ownership and handoff

```text
BackendContext (lifecycle resource owner)
  |-- FatalErrorReporter                 retained causes + lifecycle wake signal
  |-- RequestCoordinator                 coordination thread + execution pools
  |     |-- PrepareOperation callback --> operation orchestrators / maintenance
  |     `-- fatal-report callback ------> FatalErrorReporter
  `-- IpcServer                          Request / Push connection workers
        |-- borrowed RequestCoordinator -> admit_operation() / ticket result
        `-- borrowed FatalErrorReporter -> fatal report / wait wakeup
```

Serve constructs RequestCoordinator before IpcServer; the coordinator starts its
own threads. IpcServer has no business Handler or Preparer dependency. It supplies
response encoding while the coordinator owns operation preparation/dispatch.
Stopping/Report close IPC admission, drain the coordinator, then wait for final
response delivery. Report resolves Push delivery before Finish closes IPC workers.
The coordinator is drained even if IPC initialization fails; Storage finalization
follows execution termination. Destruction order keeps borrowed services alive.
