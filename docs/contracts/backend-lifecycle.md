# Backend lifecycle and ownership contract

**Status:** implemented for the current Normal Backend. This is the authoritative
contract for lifecycle sequencing, resource ownership, cleanup and module failure
containment. [IPC](ipc.md) owns transport/admission details; [Storage](storage.md)
owns Catalog/Index/Marker safety. The [system overview](../architecture/system.md)
explains component relationships.

## Principles and invariants

The lifecycle owner controls termination; modules return errors. Applicable cleanup
obligations survive ordinary operational failure. Ownership outlives explicit
Catalog finalization. Stop admission and settle accepted execution before closing
workers and finalizing Storage. Reporting does not certify consistency or replace
finalization; connected-Frontend lifecycle delivery is still deferred.

## User configuration and executable

One OS user has one Folio data configuration with separately configurable Normal/Vault authoritative roots. OS accounts provide user separation. There is no Multi-Workspace Manager, named Workspace selection, registry, or management UUID.

The agreed direction is a single `folio` executable: default invocation launches TUI; command arguments request CLI operations such as `folio todo ...`. Exact grammar is Open/Deferred. Both modes use the same formal Public Application API. Current default `folio` shows a welcome screen; private Backend dispatch is implemented as described below.

The same executable has an internal Backend mode. Process launching and exclusive ownership are implemented in Phase 4.1a;
connection handshake and readiness in 4.1b. Default TUI startup connects to or
launches the Backend and retains its connection.

## Lifetime and exclusivity

The lifecycle states are Bootstrap → Serve → Stopping → Report → Finish, with
ordinary failures routed to Report and mandatory Finish. Start on demand. At the
final Frontend disconnect stop admission, drain accepted operations, finalize
Catalog state and release resources. A bounded ten-second initial connection
window allows the launching Frontend to arrive; established sessions have no
arbitrary idle delay or permanent daemon lifetime.

An OS lock prevents concurrent Backend owners. File existence is not lock ownership. OS termination releases locks; stale files must not permanently block startup. Keep lock identity outside independently clearable Cache. Locking and [Session Marker](../development/plans/recovery.md) have different purposes. Startup/connect/shutdown races need tested protocol design.

The Backend supplies known UUIDs/locators, Ready-state checks, creation checks and matching result callbacks, and serializes check → persistence → Catalog synchronization. It invokes startup_load only after acquiring exclusive ownership, then finish_session after stopping/draining work during orderly shutdown, with explicit errors/retries. finish_session orders shutdown_save before durable Marker removal; shutdown_save alone leaves the Marker intact. No destructor persistence or runtime once-only save guard is used.

## Process startup and ownership

Frontend launching and discovery live in `app/src/`; Backend
ownership and lifecycle live in `server/src/runtime/`. Shared IPC primitives and
protocol codecs live in `lib/`. Each module owns its headers; only the
[dispatcher](../../app/src/main.cpp) consumes both role entry points. Presentation
stays in `app/src/tui/`. Public read operations are implemented in 4.2a; functional
TUI/CLI integration remains 4.4b.

`app/src/main.cpp` uses a preliminary CLI11 parser to detect private `--backend-process`, allowing unrelated arguments and disabling help handling.
If present anywhere, all other arguments are ignored. The switch is absent from
help. Otherwise CLI11 parses normal Frontend arguments.
CLI11 handles help, invalid arguments and parser exit statuses; no separate
execution parser or request model is maintained. Frontend launch lives in app/src/backend_launcher.cpp, OS process mechanisms in
app/src/process.cpp, Backend entry in server/src/backend.cpp, and exclusive
ownership in server/src/runtime/repository_ownership.cpp. Unrecognized/incomplete arguments fail before opening
the TUI. This argument
format is private bootstrap plumbing, not a supported command/configuration API.
The Frontend function `launch_backend()` in `backend_launcher.hpp` resolves the
current executable through private OS helpers and starts it with only
`--backend-process`. Paths come from configuration, never command-line arguments.
Successful launch means process creation, not ownership or readiness.

`BackendConnectionHandle` and `connect_or_start_backend()` are Frontend Application
functionality in `backend_connection_handle.hpp/.cpp`, reusable by TUI, CLI, or a future
GUI. The TUI holds a connection during its lifetime; it does not implement process
launching or IPC connection policy. Generic `launch_process()` and
`current_executable()` declarations and implementations are role-private in app/include/process.hpp and app/src/process.cpp. Tests receive explicit private include access; no
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
initializing Storage. Tests inject effective startup paths explicitly with
isolated temporary paths; no Storage types appear in the public Backend header.
Platform path resolution alone creates nothing. Configuration loading may create
missing configuration from the factory template; it does not initialize Storage.

## Ownership and endpoint resource grouping

`RepositoryOwnership::acquire()` acquires the Repo locks and then establishes the
IPC listener. Its private implementation retains both resources. Failed endpoint
setup releases the acquired locks. Endpoint addresses are derived by the shared IPC settings helper from resolved
application paths; IPC transport no longer depends on StoragePaths or Repo identities.
Endpoint derivation is not a second configuration loader.

The Backend serves connections through `RepositoryOwnership::accept()`. Before
explicit Catalog finalization it calls `stop_listening()`, which closes admission
and removes its owned endpoint while retaining Repo locks. Destruction also closes
the listener before releasing lock handles. Neither operation saves the Index or
finalizes the Marker; those remain explicit Backend lifecycle responsibilities.

## Module failure containment

**BACKEND-01 completed for current Normal Storage and Query paths:** see the
[audit findings, tests and limitations](../development/reports/backend-failure-containment-audit.md).
No confirmed production-code defect was found. Phase 4.3a Mutation handlers
enforce pre-write readiness and operation-specific preconditions;
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
Report always proceeds to Finish, including when reporting fails. BackendStateMachine
executes Finish's final cleanup action before finishing, rather than merely
using it as a loop termination condition.

The independent private `BackendStateMachine` module only executes registered
handlers and follows the supplied transition table. It has no Storage, IPC, or
configuration dependencies. `backend_states.cpp` implements five standalone
handlers and registers them once against a shared data-only `BackendContext`.
`run_backend()` shows the transition table and assembles the machine with those
handlers. Handler implementation changes do not alter the lifecycle table.
State-machine tests register synthetic handlers directly; no production test hook is
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
Formatting occurs only for presentation. The successful-initialization flag records
a cleanup obligation established during startup, not current consistency:
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

## Backend lifecycle call flow — Implemented

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

## Drain exception contract

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

## Unpredictable hangs — Agreed limitation

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

