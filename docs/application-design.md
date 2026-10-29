# Public Application and process design

**Status: Decided/Planned**, except the explicitly identified current baseline. See [architecture](architecture.md), [decisions](design-decisions.md), and [roadmap](roadmap.md). No Public Application API, daemon, IPC, queue, Edit Lock registry, or push event system exists today.

## User configuration and executable

One OS user has one Folio data configuration with separately configurable Normal/Vault authoritative roots. OS accounts provide user separation. There is no Multi-Workspace Manager, named Workspace selection, registry, or management UUID.

The agreed direction is a single `folio` executable: default invocation launches TUI; command arguments request CLI operations such as `folio todo ...`. Exact grammar is Open/Deferred. Both modes use the same formal Public Application API. Current `folio` only shows a welcome screen.

The same executable preferably has an internal Backend mode. A Frontend launches another executable instance using posix_spawn or appropriate fork-and-exec on POSIX, CreateProcess on Windows. Never run the Backend inside a forked, initialized multithreaded Frontend without exec. Exact launcher arguments and handshake are deferred.

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

The future owner supplies known UUIDs/locators, Ready-state checks, creation checks and matching result callbacks, and serializes check → persistence → Catalog synchronization. It explicitly invokes shutdown_save during orderly shutdown and handles errors/retries; no destructor persistence or runtime once-only save guard.

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
