# Public Application and process design

**Status: Implemented baseline with planned extensions.** Processes, ownership,
local IPC, lifecycle, public Normal reads and shared sequential request scheduling
are implemented. Phase 4.3a adds Normal Mutations and Entry Edit Locks; Push Events remain deferred.
See [architecture](architecture.md), [decisions](design-decisions.md) and [roadmap](roadmap.md).

## Backend responsibility overview

**Implemented.** This tree groups responsibilities, not runtime order,
ownership or strict dependency layers. Parentheses identify classes/structs;
brackets identify logical modules. The executor callback routes Access requests
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
+-- [IPC and scheduling]
|   +-- Connections, admission and response delivery (IpcServer)
|   +-- Connection identity and lifetime (ConnectionSession)
|   +-- Shared bounded queue and sequential executor (RequestScheduler)
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
lifecycle transitions. IpcServer owns its RequestScheduler. Operation handlers
borrow Catalog, Repository and Coordinator through C++ references. Coordinator
borrows Catalog. Repository retains persistence sequencing and reports results
through matching Coordinator callbacks; Coordinator synchronizes Catalog.
ConnectionSession is Backend-private and is retained by workers/accepted work,
while Edit Locks reference it weakly. No additional controller or lifecycle
framework is implied.

## User configuration and executable

One OS user has one Folio data configuration with separately configurable Normal/Vault authoritative roots. OS accounts provide user separation. There is no Multi-Workspace Manager, named Workspace selection, registry, or management UUID.

The agreed direction is a single `folio` executable: default invocation launches TUI; command arguments request CLI operations such as `folio todo ...`. Exact grammar is Open/Deferred. Both modes use the same formal Public Application API. Current default `folio` shows a welcome screen; private Backend dispatch is implemented as described below.

The same executable has an internal Backend mode. Process launching and exclusive ownership are implemented in Phase 4.1a;
connection handshake and readiness in 4.1b. Default TUI startup connects to or
launches the Backend and retains its connection.

## Ownership and boundaries

```text
CLI Frontend / TUI Frontend (many)
            ↕ public requests, responses, events over local IPC
Backend (one active owner for this user's configuration)
    ├── MetadataCatalog
    ├── CatalogCoordinator (borrows that Catalog)
    ├── NormalRepository / future VaultRepository
    ├── shared request queue / one sequential executor
    └── connection-owned Entry Edit Lock registry
```

Backend owns operations, lifecycle, metadata, event publication, and later Recovery/Trash/Vault/Git integration. Frontends own navigation, tabs, focus, and editing buffers. They do not own the authoritative Catalog or invoke private Storage APIs.

Formal Application APIs begin here; private Storage classes/errors/callbacks/handles must not simply become public. Public DTOs, request/error semantics, protocol versioning, and compatibility require explicit design. Domain APIs remain independent of presentation. Do not introduce an unnecessary Repository hierarchy or expose private Catalog types.

## Lifetime and exclusivity

The lifecycle states are Bootstrap → Serve → Stopping → Report → Finish, with
ordinary failures routed to Report and mandatory Finish. Start on demand. At the
final Frontend disconnect stop admission, drain accepted operations, finalize
Catalog state and release resources. A bounded ten-second initial connection
window allows the launching Frontend to arrive; established sessions have no
arbitrary idle delay or permanent daemon lifetime.

An OS lock prevents concurrent Backend owners. File existence is not lock ownership. OS termination releases locks; stale files must not permanently block startup. Keep lock identity outside independently clearable Cache. Locking and [Session Marker](recovery-design.md) have different purposes. Startup/connect/shutdown races need tested protocol design.

The future owner supplies known UUIDs/locators, Ready-state checks, creation checks and matching result callbacks, and serializes check → persistence → Catalog synchronization. It invokes startup_load only after acquiring exclusive ownership, then finish_session after stopping/draining work during orderly shutdown, with explicit errors/retries. finish_session orders shutdown_save before durable Marker removal; shutdown_save alone leaves the Marker intact. No destructor persistence or runtime once-only save guard is used.

## IPC and operations

Local IPC implements per-user access controls, requests/responses, disconnect
detection and explicit errors using Unix sockets or Windows Named Pipes.
Push Event messages remain deferred. Callbacks in a Frontend are local dispatch mechanisms; IPC sends messages, never cross-process function pointers. An event receiver can dispatch to the UI loop.

One shared queue/executor initially serializes every accepted Query and future
Mutation through completion, including required Catalog synchronization. IPC
handling runs independently. Concurrent Reads and cancellation remain open
decisions; domain/Storage classes need no speculative thread-safety changes. Acknowledgment must distinguish an unknown outcome from a confirmed failure; separate Request IDs, revision protection, and reconciliation remain open. Mutation intentions use the approved name `MutationId`; see [mutation contracts](application-mutations.md). Retry semantics must be designed with [Frontend Recovery](recovery-design.md) before exposing retry semantics.

## Editing and events

Backend enforces Frontend-requested exclusive interactive Edit Locks per Entry. Other clients may read. Disconnect releases/invalidates locks; restart/reconnect requires reacquisition before modification. CLI mutations respect applicable locks. Serialization remains necessary.

Content Update requires a long-lived owned Edit Lock. Immediate Move, Directory
Rename and Todo status changes check ownership and execute together on the single
executor: reject another owner's lock; proceed when unlocked or owned, preserving
an existing owner's lock. Create has no existing Entry to lock. Acquire obtains
the authoritative Snapshot with ownership in one action. Disconnect expires locks
only after that connection's already Accepted work settles. Update reconstructs
from current authoritative state, preserving membership/locator and immutable
fields. Revision checking remains open. See [Normal mutation contracts](application-mutations.md).

Push events are decided; event names, payloads, revisions, subscriptions, ordering, and reconnection rules are not. Notify relevant Frontends of changes without filesystem polling; keep Backend business logic out of TUI.

## Navigation and verification

Catalog queries remain memory-only. Application directory entry may count immediate formal items against Catalog expectations. Count equality is practical confidence, not a guarantee against edits or one-for-one replacement. Count units for module roots including Directory Entries, mismatch responses, and targeted reconciliation API need design. Do not implement automatic global scans from ordinary queries/navigation.

## Design gates

Before implementation resolve launcher/lock race handling, platform IPC transport/security, public types/errors, version negotiation, unknown-outcome request semantics, lock ownership, event delivery, and shutdown admission rules. Exact platform APIs must be verified during implementation. Prerequisite platform paths, Cache relocation, and durable Marker foundation are tracked in [roadmap](roadmap.md).

## Process foundation — Implemented in Phase 4.1a

Frontend launching and discovery live in `app/src/`; Backend
ownership and lifecycle live in `server/src/runtime/`. Shared IPC primitives and
protocol codecs live in `lib/`. Each module owns its exposed headers; only the
[dispatcher](../app/src/main.cpp) consumes both role entry points. Presentation
stays in `app/src/tui/`. Public read operations are implemented in 4.2a; functional
TUI/CLI integration remains 4.4b.

`app/src/main.cpp` uses a preliminary CLI11 parser to detect private `--backend-process`, allowing unrelated arguments and disabling help handling.
If present anywhere, all other arguments are ignored. The switch is absent from
help. Otherwise CLI11 parses normal Frontend arguments.
CLI11 handles help, invalid arguments and parser exit statuses; no separate
execution parser or request model is maintained. Backend launch/bootstrap lives
in `backend.cpp`, OS process mechanisms in `process.cpp`, and exclusive ownership
in `repository_ownership.cpp`. Unrecognized/incomplete arguments fail before opening
the TUI. This argument
format is private bootstrap plumbing, not a supported command/configuration API.
The shared Application function `launch_backend()` in `backend.hpp` resolves the
current executable through private OS helpers and starts it with only
`--backend-process`. Paths come from configuration, never command-line arguments.
Successful launch means process creation, not ownership or readiness.

`BackendConnectionHandle` and `connect_or_start_backend()` are shared Application
functionality in `backend_connection_handle.hpp/.cpp`, reusable by TUI, CLI, or a future
GUI. The TUI holds a connection during its lifetime; it does not implement process
launching or IPC connection policy. Generic `launch_process()` and
`current_executable()` declarations and implementations are private under
`app/src/`. Tests receive explicit private include access; no
Frontend implementation source is compiled into tests.

On POSIX, `launch_process()` uses a detached double-fork/exec sequence. All C++
argument preparation happens before fork; children perform only signal-safe OS
operations before exec. The intermediate is reaped by the caller, the final
process is adopted by the OS reaper, terminal streams go to `/dev/null`, and
unrelated descriptors are closed. A close-on-exec pipe reports exec errors; it
is not application IPC. The process may outlive its launcher. Environments using
an application subreaper must reap their adopted children; this utility does not
change global SIGCHLD policy. On Windows, `CreateProcessW` uses native Unicode
arguments, detached execution and disabled handle inheritance; returned process
and thread handles are closed. Process IDs are diagnostic and may be reused.

`RepositoryOwnership::acquire(paths)` is called **only in the Backend**, before any
Catalog lifecycle access. Its move-only RAII value holds nonblocking exclusive
locks until destruction or process termination. POSIX uses `flock`; Windows uses
`LockFileEx`. RAII releases handles only, never saves an Index or clears a Marker.
`AlreadyOwned` is distinct from invalid paths, permission and other I/O errors.
Partial acquisition releases every already-acquired handle.

Lock targets are centralized in the ownership implementation:

- `<application.state>/backend.lock` protects the user's configuration owner.
- `<normal_root>/.folio-backend.lock` protects the physical Normal root even when
  different State directories or filesystem aliases are supplied.
- `<vault_root>/.folio-backend.lock` protects an **existing** Vault root. An absent
  Vault is never created for locking; future Vault initialization must establish
  its ownership before use.

Missing State/Normal directories may be created during explicit acquisition.
Existing root aliases are canonicalized, with existing StoragePaths overlap
validation retained. State locking cannot live beneath Cache or Runtime. Lock
files must be regular, not symlinks/reparse points; POSIX additionally rejects
foreign-owned and hard-linked lock files. Files are never truncated or removed.
They may remain indefinitely without implying a running owner. Multiple
candidates contend on the same physical file; a loser releases its other locks.
These support files are not authoritative Entries and must be excluded from
future Git synchronization.

This is cooperative local-filesystem ownership, not a security boundary against
external writers. Do not unlink/replace lock targets or roots during ownership.
Cross-user shared storage is not a supported service: permissions/ACLs may deny
acquisition, and no cross-user registry or permissions provisioning is supplied.
Network-filesystem locking semantics require separate validation. Acquiring these
locks alone does not establish a durable Session Marker or application readiness.
See the OS contracts for [flock](https://man7.org/linux/man-pages/man2/flock.2.html)
and [LockFileEx](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-lockfileex).

The executable dispatches Backend mode to the single public
`folio::application::run_backend()` entry point. Backend startup resolves
configuration and effective paths internally, then acquires ownership before
initializing Storage. Tests use a private scoped configuration override with
isolated temporary paths; no Storage types appear in the public Backend header.
Configuration resolution itself does not create directories.

The following paragraph records the original Phase 4.1a integration boundary;
Phase 4.1b implementation status is described below.

Phase 4.1b must replace the bounded bootstrap with connection/handshake handling,
retain ownership for the whole Backend lifetime, call Coordinator startup before
Ready, track clients/work, drain operations, explicitly finish the Session Marker
lifecycle, and only then release ownership. It must handle launch success followed
by a losing candidate, startup failure, and connect/shutdown races. Existing Marker
and Index algorithms are unchanged in 4.1a.

## Build organization — Runtime-role layout

`folio_frontend` (app/), `folio_backend` (server/) and `folio_shared` (lib/) are
static libraries. The sole `folio` executable links both roles, and both depend
on shared code. Only lib/ exports its include tree; app/ and server/ keep their include
trees private and grant explicit access to the dispatcher/tests. Backend initialization remains `folio::application::run_backend()`;
Frontend launching is declared separately in `backend_launcher.hpp`.
BackendConnectionHandle belongs to app/, while query/error/mutation contracts
remain shared. Linking the libraries together does not allow Frontends to bypass
IPC or include private Storage. See [physical ownership](architecture.md#runtime-role-module-structure--implemented).

## Phase 4.1b connection lifecycle — Implemented

Frontend BackendConnectionHandle and launcher interfaces, shared IPC facilities,
and the Backend entry point implement on-demand connections. Default Frontend startup retains a Client through the welcome TUI;
connection loss exits the TUI with an explicit error. No automatic request replay
occurs. Public operations remain Phase 4.2.

Configuration is a shared startup snapshot. Loading reads
`<config>/folio_config.toml` (or `FOLIO_CONFIG_FILE`) and atomically creates a
missing file from the embedded `templates/folio_config.toml`. User-facing storage
policies are transformed into validated effective roots. Existing files are not
rewritten; no hot reload is implemented. See [Configuration](configuration.md)
for schema, compatibility, and validation. Logging is implemented in Phase 4.1d; see [Logging](logging.md).

Unix uses a user-owned 0700 directory and 0600 Unix socket, with OS peer UID
verification. Runtime is preferred; a private State/ipc directory is the fallback.
Windows uses duplex local Named Pipes with a current-user SID DACL and remote
clients rejected. Endpoint names are not authentication. RepositoryOwnership is
required before stale endpoint cleanup, binding, Catalog startup, or finalization.
Only stale sockets are reclaimed; live endpoints are preserved.

Messages use a big-endian 32-bit body length, 16-bit version 1 and 16-bit
kind; the maximum complete body is 64 MiB, including version/kind. Partial OS
transfers are handled internally, without protocol chunking. Hello/Ready confirm protocol compatibility and readiness with empty payloads;
Ping/Pong also carry no payload. Reject
contains an internal diagnostic. Unknown versions/kinds/invalid lengths are
rejected. This internal lifecycle protocol has no CRUD or event schemas.

The lifecycle owner accepts connections and creates bounded IPC workers (at most
64). Linux/macOS readiness waits use poll plus explicit wake pipes; Windows uses
overlapped Named Pipes and completion/wake events. There are no recurring sleep
loops for established IPC progress. Workers decode complete requests and admit
through one Backend-owned RequestScheduler. One executor performs all current
Access and Mutation execution sequentially; it alone accesses live Storage.
Accepted acknowledgment is distinct from final Response; Rejected is distinct
from execution failure. See [runtime/protocol limits](ipc-protocol.md).

Handshake and transfer-inactivity bounds remain three seconds; total request
reception is bounded to thirty seconds. A ten-second initial registration window
permits launching Frontends to connect. Acceptance is session-only and waiting in
the queue has no short operation-completion deadline. The final disconnection
stops admission and drains accepted work, including work whose connection is lost.
Admission closure wakes idle reception without closing usable connections.
Accepted work and bounded final delivery settle before Report; Finish closes
remaining connections and joins workers before Catalog finalization.

The endpoint is bound under ownership before Storage initialization so startup
errors can be returned, but no successful handshake is processed until
`CatalogCoordinator::startup_load()` succeeds. No Ready precedes the durable
Marker. Shutdown closes admission/connections, explicitly calls `finish_session()`,
and releases ownership last. Failed finalization returns failure and retains the
Marker according to Storage's existing contract. Backend crashes release locks,
break connections and leave Marker-based reconstruction for the next session.
The runtime now drains queued/active Access Operations through this lifecycle
boundary. Concrete Mutation execution remains Phase 4.3a.

### Backend module failure containment — Decided; current paths audited

**BACKEND-01 completed for current Normal Storage and Query paths:** see the
[audit findings, tests and limitations](backend-failure-containment-audit.md).
No confirmed production-code defect was found. New Phase 4.3a Mutation handlers
must still enforce pre-write readiness and operation-specific preconditions;
post-persistence callbacks cannot substitute for those checks.

Backend modules, currently `NormalRepository`, `MetadataCatalog`, and
`CatalogCoordinator`, must not terminate the process on operational failure.
If a module's internal state cannot support correct execution, it must reject
the affected operation with an explicit error rather than continue unsafely.
Errors must remain available for collection; a later error must not erase an
earlier failure. Fatal, unresolvable errors must be recognizable so the Backend
lifecycle owner can initiate termination. Modules do not terminate the process
themselves. Forced termination, crashes, and failures that make further execution
impossible are outside this ordinary error-handling guarantee.

Nonfatal operation errors must not terminate the Backend. Return them through
Application error contracts to the requesting Frontend; do not broadcast a
request-specific error to unrelated Frontends. For example, a Save failure for
Frontend B belongs to B's final response, not to Frontend A's browsing session.
The request/response path already routes current Query results to their
originating connection; Normal Mutation final results follow the same routing.

If a failure also changes shared service availability, affected Frontends may
need a separate availability notification. That notification must contain only
relevant, safe shared-state information, not another Frontend's operation error
or sensitive context. Future lifecycle/Push notification design must explicitly
define recipients. Error severity alone does not determine broadcast scope.
Fatal, unresolvable service failures are handled by the lifecycle owner;
ordinary operation rejection and lifecycle termination remain distinct.

The executor continues consuming accepted requests, invoking handlers and
retaining their actual results or exceptions. Draining guarantees an execution
attempt, not success. Modules enforce their own state and operation preconditions;
the executor does not add a general health check or classify which queued
operations may succeed after a previous failure. See the BACKEND-01 audit for
verification of existing modules against this principle.

`run_backend()` defines five lifecycle states: Bootstrap, Serve, Stopping,
Report, and Finish. Bootstrap performs configuration, ownership acquisition,
and Storage initialization. Serve serves connections. Stopping closes admission.
Unrecoverable failures in Bootstrap, Serve, or Stopping go to the same Report state;
Report always proceeds to Finish, including when reporting fails. The executor
executes Finish's final cleanup action before finishing, rather than merely
using it as a loop termination condition.

The independent private `BackendStateMachine` module only executes registered
handlers and follows the supplied transition table. It has no Storage, IPC, or
configuration dependencies. `backend_states.cpp` implements five standalone
handlers and registers them once against a shared data-only `BackendContext`.
`run_backend()` shows the transition table and assembles the machine with those
handlers. Handler implementation changes do not alter the lifecycle table.
Executor tests register synthetic handlers directly; no production test hook is
needed.

Finish closes admission even when Stopping was bypassed, attempts session finalization only
after successful initialization, and releases resources including ownership.
Failed startup cannot certify an incomplete Catalog or clear its Marker.
Finalization failure or an ordinary finalization exception still allows resource
release. Final cleanup diagnostics use the process logger; they are retained internally
without returning to Report.

The context retains ordered typed diagnostics tagged with their lifecycle phase.
Process, IPC, and Coordinator errors keep their codes, paths, system errors, and
nested causes; exceptions keep their exception pointers. The first failure is
primary and later reporting/cleanup failures append without replacing it.
Formatting occurs only for presentation. The successful-initialization flag is
a historical cleanup obligation, not an assertion of current consistency:
Coordinator retains authority to reject unsafe finalization and preserve its
Marker. Resource ownership itself determines which handles need release.

Report retains the existing bounded startup handshake rejection where an endpoint
was established, without printing internal diagnostics to the terminal. Configuration and ownership
failures before an endpoint exists still lack reliable Frontend delivery. On serving failure accepted work drains before final resource release; no final
broadcast is promised. Multiple Frontends remain independent, and the final disconnect ends
the session. Complete final Frontend reporting and pre-IPC bootstrap delivery
remain deferred pending output/protocol design. No new IPC message is introduced.

Ordinary action exceptions follow the same failure transitions. If notifying the
owner of an action exception also throws, the executor retains both exceptions
and still advances toward Finish. The owner collects those notification failures
after the lifecycle ends. Logging failure cannot change retained operational errors or prevent resource release. These are execution-path guarantees,
not guarantees against process termination or resource exhaustion that makes
further cleanup impossible.

Platform runtime verification for this phase is macOS only. Windows/Linux native
execution, access-control and shutdown behavior still require platform testing.

### Ownership and endpoint resource grouping

`RepositoryOwnership::acquire()` acquires the Repo locks and then establishes the
IPC listener. Its private implementation retains both resources. Failed endpoint
setup releases the acquired locks. Endpoint addresses are derived by configuration
bootstrap; IPC transport no longer depends on StoragePaths or Repo identities.
The duplicate private IPC header has been removed.

The Backend serves connections through `RepositoryOwnership::accept()`. Before
explicit Catalog finalization it calls `stop_listening()`, which closes admission
and removes its owned endpoint while retaining Repo locks. Destruction also closes
the listener before releasing lock handles. Neither operation saves the Index or
finalizes the Marker; those remain explicit Backend lifecycle responsibilities.

## Process diagnostic logging — Phase 4.1d implemented

The executable selects its role and initializes a default spdlog logger before
Configuration loading or Backend Bootstrap/TUI connection setup. Platform paths
alone determine the log root. Each process has a distinct role/session file.
Successful configuration loading updates its severity without reopening the file.
Backend diagnostics are logged once where retained, including cleanup failures;
logging does not alter the lifecycle table or replace Frontend error delivery.
Explicit logger flush/release follows Finish or Frontend completion. Existing
user-facing TUI stderr messages remain separate. See [Logging](logging.md).

## Public read access — Phase 4.2a implemented

Browse, scoped metadata queries, Directory listing, and typed Snapshot loading
are now available on BackendConnectionHandle through IPC. See
[Public read access](application-access.md) for current contracts, warnings,
errors, transport limits, and deferred work. This supersedes earlier statements
in this document that all Public Application operations are absent. Phase 4.3a
subsequently adds Normal Mutations and Edit Locks; Recovery, events and Vault
remain deferred.

## Phase 4.2c — unified protocol components implemented

Access and Mutation Operations share one IPC transport and MessagePack
codec architecture. Complete length-prefixed messages use shared nonblocking transfer handling
without separate protocol chunks. Mutation identity, failure/evidence and confirmed envelopes have codecs,
including ID correlation checks. Phase 4.3a subsequently supplies concrete Normal
business payloads and dispatch using those same facilities. Protocol version remains 1. See [protocol boundaries and limits](ipc-protocol.md).
Backend lifecycle, ownership, Catalog and Marker behavior are unchanged.

## Runtime refactor before Normal Mutations — Implemented

IPC connection workers and one shared sequential executor now have independent
scheduling. BackendStateMachine and explicit Coordinator finalization retain
lifecycle authority. Scheduling does not persist accepted requests, implement
Recovery, cancel work on disconnect, or guarantee execution after a crash.
See [admission and runtime contract](ipc-protocol.md#scheduling-and-lifecycle)
for limits, deadlines, shutdown and deferred Frontend integration.

### Backend lifecycle call flow — Implemented

Normal and abnormal termination retain distinct state transitions. Operational
failure does not promise successful execution of queued work: the executor
attempts each accepted action and retains its actual result or exception. Modules
remain responsible for checking their own preconditions and consistency.

```text
BACKEND MAIN THREAD                    CONNECTION WORKERS                 EXECUTOR THREAD
───────────────────                    ──────────────────                 ───────────────

run_backend()
  └─ BackendStateMachine::run()
       │
       ▼
[Bootstrap]
  Bootstrap(context)
    ├─ Load configuration
    ├─ Acquire RepositoryOwnership / listener
    ├─ Initialize Repository / Catalog / Coordinator
    └─ startup_load() → durable Session Marker
       │
       ├─ Failure ───────────────────────────────────────────────► [Report]
       ▼ Success
[Serve]
  Serve(context)
    ├─ Construct IpcServer; store operation dispatch callback
    │    └─ Construct RequestScheduler ──────────────────────────────────► Start executor
    │                                                                      └─ Wait for work
    └─ IpcServer::run() [blocks main thread]
         ├─ ownership.wait() → OS readiness wait
         ├─ ownership.accept()
         ├─ Start connection worker ──► connection(...)
         │                               ├─ Receive / validate Hello
         │                               ├─ Register connection
         │                               └─ Send Ready
         │                               │
         │                               ▼
         │                             Request loop
         │                               ├─ Wait / receive complete message
         │                               ├─ Validate framing/version/bounds
         │                               ├─ codec::decode<OperationRequest>()
         │                               └─ scheduler.admit(action)
         │                                    ├─ Reject → send Rejected
         │                                    └─ Enqueue ───────────────► Wake executor
         │                                         │                         ├─ Pop action
         │                                    Send Accepted                  ├─ handler_(request)
         │                                         │                         │   └─ access->handle(...)
         │                                    Wait for result                │       └─ Catalog / Repository
         │                                         │                         ├─ Encode response
         │                                         │                         ├─ Retain result/exception
         │                                         ◄─────────────────────────┤ Signal completion
         │                                    Send final response            └─ Consume next action
         │                                    Repeat request loop
         │
         │                             On disconnection:
         │                               Unregister; wake main thread; exit
         │                               Accepted work is NOT cancelled
         │
         └─ Detect stopping condition
              Stop admission/listening; preserve established connections
              ├─ Failure ──────────────────────────────────────► [Report]
              └─ Normal return
       │
       ▼
[Stopping] [normal path]
  Stopping(context)
    ├─ drain_requests() ─────────────────────────────────────────────────► Attempt accepted work
    │                                                                      └─ Executor exits
    ├─ Join executor and reap completed connection workers
    └─ Collect retained failures
       │ either outcome
       ▼
[Report] [also reached directly on Bootstrap / Serve failure]
  Report(context)
    ├─ drain_requests()
    │    ├─ Normal path: already drained
    │    └─ Failure path: attempt accepted work and join executor
    ├─ Collect retained failures
    │                                  Usable workers retain streams
    │                                  without admitting new requests
    ├─ Existing startup rejection where applicable
    └─ Connected-Frontend lifecycle notification: DEFERRED; no implementation
       │ regardless of outcome
       ▼
[Finish]
  Finish(context)
    ├─ Ensure requests drained [exceptional fallback]
    ├─ close_connections() ────────────► Wake workers; close streams; exit
    │    └─ Join remaining workers
    ├─ Collect retained failures
    ├─ Finalize eligible Catalog session
    │    ├─ Save Index when permitted / required
    │    └─ Clear Marker only when safety is established
    └─ Release RepositoryOwnership
       │
       ▼
     Stopped
```

`drain_requests()` is idempotent and does not close usable connections. The
repeated calls establish the prerequisite on both termination paths and provide
Finish's exceptional fallback; they do not certify clean Catalog state.
`take_failures()` transfers and clears retained failures, without logging or
sending them. Connection workers parked after admission closes are woken by
Finish's cooperative stop signal; threads are not forcibly terminated.
Actual lifecycle notification remains deferred to the output/notification design;
keeping streams alive is resource preparation, not a placeholder wire message.
Catalog finalization eligibility, error propagation, and the transition table are
unchanged.

#### Drain exception contract

Operation errors and exceptions are outcomes collected during draining, not
failures of draining itself. The executor catches action exceptions, retains
original exception pointers on tickets, publishes completion, and continues with
all remaining accepted actions. IpcServer retains handler failures for lifecycle
diagnostics without rethrowing them from `drain_requests()`. Repeated draining
is harmless. Completion means execution has stopped, not that every action
succeeded or that Catalog state is consistent.

Report also retains unexpected drain-preparation exceptions locally so they
do not skip the reporting action. Finish retries draining before finalization;
catching an exception is not evidence that execution has stopped.

Do not add `noexcept` merely to hide thread/synchronization infrastructure
failures: termination would bypass cleanup, and incomplete draining must never
be represented as complete. This operational guarantee excludes failures that
make further execution impossible. See the hung-operation boundary below.

#### Unpredictable hangs — Agreed limitation

Find and fix concrete preventable hangs in existing code: incorrect lock
ordering, missing wakeups, loops without progress, and waits with an applicable
safe termination mechanism. This does not prove the absence of overlooked
bugs, deadlocks, or stalled OS operations.

An unpredictable hang may block the sequential executor during Serve and may
also prevent draining and orderly shutdown. No operation-completion deadline
or bounded shutdown is guaranteed. Slowness is not evidence of operation failure.
Do not add watchdogs, progress tracking, automatic process termination, or new
timeout/cancellation policies solely to hide this limitation; none is authorized
by this decision. Existing bounded IPC transfer waits remain unchanged.

Do not forcibly terminate an operation thread, finalize Catalog while execution
may still access it, fabricate a final result, or automatically replay an
unconfirmed Mutation. If the user forcibly terminates the Backend, treat that
as a crash: OS ownership is released by the OS, the durable Session Marker
remains for an active session, and subsequent startup uses the existing
Marker-based recovery path. This restores Catalog consistency, not proof of a
particular Mutation's outcome, and does not promise Draft Recovery.

Revisit additional hang-handling mechanisms only for a concrete requirement
through design review. No such mechanism is currently scheduled.

## Phase 4.3a — Normal Mutations and Edit Locks implemented

The authoritative description of inputs, modification context, independent
membership/content responsibilities, lock lifetime and failure evidence is
[Normal mutation contracts](application-mutations.md#normal-execution-and-field-semantics--implemented).
MutationOrchestrator owns the executor-only lock registry and handlers. IpcServer passes
an opaque shared session token to each accepted action; workers never manipulate
locks or Storage. Weak registry ownership expires after disconnect and accepted
work completion. No extra cleanup queue or lifecycle state was added. Stopping /
Report drain accepted work; Finish closes workers before Coordinator finalization.
Backend Entry UUID generation and Frontend MutationId generation remain distinct.
Full Refresh, count checks, Push, Recovery and UI worker integration remain deferred.
