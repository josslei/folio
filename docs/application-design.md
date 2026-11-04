# Public Application and process design

**Status: Decided/Planned**, except the explicitly identified current baseline. See [architecture](architecture.md), [decisions](design-decisions.md), and [roadmap](roadmap.md). No Public Application API, daemon, IPC, queue, Edit Lock registry, or push event system exists today.

## User configuration and executable

One OS user has one Folio data configuration with separately configurable Normal/Vault authoritative roots. OS accounts provide user separation. There is no Multi-Workspace Manager, named Workspace selection, registry, or management UUID.

The agreed direction is a single `folio` executable: default invocation launches TUI; command arguments request CLI operations such as `folio todo ...`. Exact grammar is Open/Deferred. Both modes use the same formal Public Application API. Current default `folio` shows a welcome screen; private Backend dispatch is implemented as described below.

The same executable has an internal Backend mode. Process launching and exclusive ownership are implemented in Phase 4.1a; connection handshake and service readiness remain deferred to 4.1b. Default TUI invocation does not yet launch a Backend.

## Ownership and boundaries

```text
CLI Frontend / TUI Frontend (many)
            ↕ public requests, responses, events over local IPC
Backend (one active owner for this user's configuration)
    ├── MetadataCatalog
    ├── CatalogCoordinator (borrows that Catalog)
    ├── NormalRepository / future VaultRepository
    ├── serialized mutation queue
    └── Entry Edit Lock registry
```

Backend owns operations, lifecycle, metadata, event publication, and later Recovery/Trash/Vault/Git integration. Frontends own navigation, tabs, focus, and editing buffers. They do not own the authoritative Catalog or invoke private Storage APIs.

Formal Application APIs begin here; private Storage classes/errors/callbacks/handles must not simply become public. Public DTOs, request/error semantics, protocol versioning, and compatibility require explicit design. Domain APIs remain independent of presentation. Do not introduce an unnecessary Repository hierarchy or expose private Catalog types.

## Lifetime and exclusivity

Lifecycle is Starting → Ready → Stopping → Stopped. Start on demand; stay alive while clients or active operations remain. After the final disconnect and all operations finish, stop accepting work, persist orderly state, release resources, and exit. A CLI-only run may cause immediate shutdown. No permanent daemon or idle grace period is agreed.

An OS lock prevents concurrent Backend owners. File existence is not lock ownership. OS termination releases locks; stale files must not permanently block startup. Keep lock identity outside independently clearable Cache. Locking and [Session Marker](recovery-design.md) have different purposes. Startup/connect/shutdown races need tested protocol design.

The future owner supplies known UUIDs/locators, Ready-state checks, creation checks and matching result callbacks, and serializes check → persistence → Catalog synchronization. It invokes startup_load only after acquiring exclusive ownership, then finish_session after stopping/draining work during orderly shutdown, with explicit errors/retries. finish_session orders shutdown_save before durable Marker removal; shutdown_save alone leaves the Marker intact. No destructor persistence or runtime once-only save guard is used.

## IPC and operations

Local IPC needs per-user access controls, requests/responses/events, disconnect detection, and explicit connection-loss/errors. Transport and wire format are Open/Deferred. Callbacks in a Frontend are local dispatch mechanisms; IPC sends messages, never cross-process function pointers. An event receiver can dispatch to the UI loop.

Initial mutations execute through a serialized queue. This avoids requiring every domain/Storage class to be independently thread-safe. Read scheduling and cancellation are deferred. Acknowledgment must distinguish an unknown outcome from a confirmed failure; separate Request IDs, revision protection, and reconciliation remain open. Mutation intentions use the approved name `MutationId`; see [mutation contracts](application-mutations.md). Retry semantics must be designed with [Frontend Recovery](recovery-design.md) before exposing retry semantics.

## Editing and events

Backend enforces Frontend-requested exclusive interactive Edit Locks per Entry. Other clients may read. Disconnect releases/invalidates locks; restart/reconnect requires reacquisition before modification. CLI mutations respect applicable locks. Serialization remains necessary.

Creating an independent Record, moving a Record, or renaming a Directory does not inherently require an Edit Lock. Content editing must not overwrite newer membership/locator changes; design scoped updates/revision protection before implementing saves. Exact lock API is deferred.

Push events are decided; event names, payloads, revisions, subscriptions, ordering, and reconnection rules are not. Notify relevant Frontends of changes without filesystem polling; keep Backend business logic out of TUI.

## Navigation and verification

Catalog queries remain memory-only. Application directory entry may count immediate formal items against Catalog expectations. Count equality is practical confidence, not a guarantee against edits or one-for-one replacement. Count units for module roots including Directory Entries, mismatch responses, and targeted reconciliation API need design. Do not implement automatic global scans from ordinary queries/navigation.

## Design gates

Before implementation resolve launcher/lock race handling, platform IPC transport/security, public types/errors, version negotiation, unknown-outcome request semantics, lock ownership, event delivery, and shutdown admission rules. Exact platform APIs must be verified during implementation. Prerequisite platform paths, Cache relocation, and durable Marker foundation are tracked in [roadmap](roadmap.md).

## Process foundation — Implemented in Phase 4.1a

Private implementation lives in [src/application](../lib/src/application/internal/), compiled
as `folio_application` with a private dependency on the existing `folio_backend`.
The executable entry point is [app/src/main.cpp](../app/src/main.cpp); presentation stays
in `app/src/tui/`. No Application headers are public yet. Public Application APIs
belong to 4.2; TUI/CLI operations through them belong to 4.4.

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
`application/internal/`. Tests receive explicit private include access; no
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

## Build organization — Phase 4.1a.5

`folio_backend` is the static Backend library under `lib/`; `folio` is the sole
executable under `app/`. Same-executable internal Backend dispatch uses the narrow
public `folio/application/backend.hpp` header. Private Storage and runtime headers
remain under `lib/src/`, available explicitly to internal tests only. Frontend
operations must retain their IPC path when the separately stashed 4.1b work is
restored; linking the Backend library does not authorize bypassing IPC.
Phase 4.1b exists but awaits review/acceptance; this refactor adds no functionality.


## Phase 4.1b connection lifecycle — Implemented

Private `application/client.hpp`, `ipc.hpp`, and `backend.hpp` implement on-demand
connections. Default Frontend startup retains a Client through the welcome TUI;
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

One Backend loop owns lifecycle and all connection state, capped at 64 peers.
Handshake, incomplete-message inactivity, and output inactivity deadlines are
three seconds; total request reception is bounded to thirty seconds. A ten-second initial
registration window permits the launching Client to arrive. Once Clients have
registered, the final disconnect stops admission immediately. Pending handshakes
may retry against the next owner; startup retries are bounded to fifteen seconds.
The five-millisecond transport poll interval is not a persistence timer.

The endpoint is bound under ownership before Storage initialization so startup
errors can be returned, but no successful handshake is processed until
`CatalogCoordinator::startup_load()` succeeds. No Ready precedes the durable
Marker. Shutdown closes admission/connections, explicitly calls `finish_session()`,
and releases ownership last. Failed finalization returns failure and retains the
Marker according to Storage's existing contract. Backend crashes release locks,
break connections and leave Marker-based reconstruction for the next session.
There are no application operations to drain yet; Phase 4.3 must extend this
single lifecycle boundary to account for queued/active work.

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
failures before an endpoint exists still lack reliable Frontend delivery. On
serving-loop failure its connections close before Report; no final broadcast is
promised. Multiple Frontends remain independent, and the final disconnect ends
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
in this document that all Public Application operations are absent. Mutations,
Recovery, Edit Locks, events, and Vault remain deferred.

## Phase 4.2c — unified protocol components implemented

Query and future Mutation Operations share one IPC transport and MessagePack
codec architecture. Complete length-prefixed messages use shared nonblocking transfer handling
without separate protocol chunks. Mutation identity, failure/evidence and confirmed envelopes have codecs,
including ID correlation checks; complete business payloads and dispatch remain
deferred to 4.3a. Protocol version remains 1. See [protocol boundaries and limits](ipc-protocol.md).
Backend lifecycle, ownership, Catalog and Marker behavior are unchanged.
