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

Initial mutations execute through a serialized queue. This avoids requiring every domain/Storage class to be independently thread-safe. Read scheduling and cancellation are deferred. Acknowledgment must distinguish an unknown outcome from a confirmed failure; request identity, operation IDs, revision protection, and reconciliation must be designed with [Frontend Recovery](recovery-design.md) before exposing retry semantics.

## Editing and events

Backend enforces Frontend-requested exclusive interactive Edit Locks per Entry. Other clients may read. Disconnect releases/invalidates locks; restart/reconnect requires reacquisition before modification. CLI mutations respect applicable locks. Serialization remains necessary.

Creating an independent Record, moving a Record, or renaming a Directory does not inherently require an Edit Lock. Content editing must not overwrite newer membership/locator changes; design scoped updates/revision protection before implementing saves. Exact lock API is deferred.

Push events are decided; event names, payloads, revisions, subscriptions, ordering, and reconnection rules are not. Notify relevant Frontends of changes without filesystem polling; keep Backend business logic out of TUI.

## Navigation and verification

Catalog queries remain memory-only. Application directory entry may count immediate formal items against Catalog expectations. Count equality is practical confidence, not a guarantee against edits or one-for-one replacement. Count units for module roots including Directory Entries, mismatch responses, and targeted reconciliation API need design. Do not implement automatic global scans from ordinary queries/navigation.

## Design gates

Before implementation resolve launcher/lock race handling, platform IPC transport/security, public types/errors, version negotiation, unknown-outcome request semantics, lock ownership, event delivery, and shutdown admission rules. Exact platform APIs must be verified during implementation. Prerequisite platform paths, Cache relocation, and durable Marker foundation are tracked in [roadmap](roadmap.md).

## Process foundation — Implemented in Phase 4.1a

Private implementation lives in [src/application](../src/application/), compiled
as `folio_application` with a private dependency on the existing `folio_core`.
The executable entry point is [src/main.cpp](../src/main.cpp); presentation stays
in `src/tui/`. No Application headers are public yet. Public Application APIs
belong to 4.2; TUI/CLI operations through them belong to 4.4.

`src/main.cpp` uses a preliminary CLI11 parser to detect private `--backend-process`, allowing unrelated arguments and disabling help handling.
If present anywhere, all other arguments are ignored. The switch is absent from
help. Otherwise CLI11 parses normal Frontend arguments.
CLI11 handles help, invalid arguments and parser exit statuses; no separate
execution parser or request model is maintained. Backend launch/bootstrap lives
in `backend.cpp`, OS process mechanisms in `process.cpp`, and exclusive ownership
in `repository_ownership.cpp`. Unrecognized/incomplete arguments fail before opening
the TUI. This argument
format is private bootstrap plumbing, not a supported command/configuration API.
`launch_backend()` resolves the current executable through the OS and launches
it with only `--backend-process`. Paths must come from Backend-owned configuration, never command-line arguments. It reports executable creation, not ownership or readiness.
There is no automatic launch from the welcome TUI while configuration and IPC
are absent.

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

Configuration loading remains deferred: production `run_backend()` currently
returns exit code 3 without accessing user directories. The explicitly injected
`run_backend(paths)` internal overload exercises ownership/bootstrap in tests;
it acquires ownership and immediately returns exit code 3 (service unavailable); contention returns 4, other acquisition errors return 1.
It never starts Catalog lifecycle or claims Ready. There is no idle daemon or
placeholder IPC loop. The test-only child executable uses bounded file barriers
to exercise long-lived ownership, independent processes and forced termination;
these barriers are absent from production.

Phase 4.1b must replace the bounded bootstrap with connection/handshake handling,
retain ownership for the whole Backend lifetime, call Coordinator startup before
Ready, track clients/work, drain operations, explicitly finish the Session Marker
lifecycle, and only then release ownership. It must handle launch success followed
by a losing candidate, startup failure, and connect/shutdown races. Existing Marker
and Index algorithms are unchanged in 4.1a.
