# Folio architecture

Status vocabulary throughout these documents: **Implemented** means present in the inspected repository; **Decided/Planned** means agreed direction with no claim of working code; **Open/Deferred** means a later design decision is required. Implementation evidence is linked below. This consolidation does not change any schema or behavior.

## Documentation map

- [Storage specification](storage-design.md): existing codecs, physical layout, atomic writes, Catalog, and Index contracts. Preserved during this consolidation; some future-facing statements require separately approved amendments.
- [Application](application-design.md): public boundary, processes, IPC, and editing.
  Start with the [Backend responsibility overview](application-design.md#backend-responsibility-overview) for its module hierarchy.
- [Configuration](configuration.md): implemented defaults, user overrides, and startup snapshots.
- [File I/O](io.md): internal byte-oriented filesystem operations and atomic publication.
- [Recovery](recovery-design.md): Session Marker, Journals, Trash, and pending drafts.
- [Vault](vault-design.md): privacy and encryption boundaries.
- [Git Sync](git-sync-design.md): branch/worktree direction.
- [Roadmap](roadmap.md): implementation order, gates, and dependencies.
- [Decisions](design-decisions.md): rationale, alternatives, and deferred details.

## Application Operation taxonomy — Decided/Planned

Application Operation is the umbrella for Access Operations (Browse, Metadata
Query, Directory Listing, Record Loading) and Mutation Operations (Create,
Update, Move, and future Delete/Trash). These classifications require no C++
base classes. Normal Access Operations are implemented in Phase 4.2a; Normal Mutation execution and Edit Locks are implemented in Phase 4.3a;
Delete/Trash, Vault, Push and Recovery remain deferred.

Access Operations do not modify authoritative business data and can generally be
issued afresh after connection loss. Mutation Operations may already have changed
data, so they retain one logical `MutationId` across submission and future Recovery.
The strong `MutationId` type is implemented as a public Application contract.
This identity differs from a possible future
`RequestId` for an IPC exchange. Sequential synchronous IPC currently needs no
Request ID. Mutation IDs guarantee neither replay safety nor execution history.
See [mutation contracts and deferred guarantees](application-mutations.md).

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

[Public headers](../lib/include/folio/core/) define:

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

Core validation is deliberately limited: restore validates enum values and applicable calendar dates; Directory validates kind, Todo validates due dates/status/priority and subtask indices, and Log validates Log dates. Core does not verify filesystem membership, timezone databases, or persistence consistency. Empty Todo title/notes are allowed; localized placeholders belong to Frontends. Do not infer uniform validation on all constructors/setters from restore validation. See [domain implementations](../lib/src/core/).

Logs have no user Directory. Memos/Todos support one level, optional membership on the Record, and no nested Directories. The Catalog derives reverse membership; Directory never duplicates it.

## Current component and build boundary — Implemented

[Root CMake](../CMakeLists.txt) builds three static libraries: `folio_shared`
(`lib/`), `folio_frontend` (`app/`) and `folio_backend` (`server/`). Both roles
depend on shared code; neither role depends on the other. The single `folio`
executable is their shared dispatcher. Only lib/include propagates shared headers; app/include and server/include remain
role-private, with explicit dispatcher access; only internal tests receive private source paths.
FTXUI belongs to the Frontend and CLI11 to the dispatcher. Dependencies remain
pinned submodules; no database or JSON serialization is introduced.

Private process code uses `folio::application::internal`. Domain/Storage namespaces are `folio::core`, `folio::core::storage`, `folio::core::storage::codec`, and Storage `internal` atomic helpers. The generic internal Result<T, E> is a variant, not a public Application contract.

| Component | Responsibility / evidence |
| --- | --- |
| [NormalRepository](../server/include/storage/normal_repository.hpp) | Authoritative Normal Entry operations; targeted identity verification, explicit UUID discovery, full scan |
| [MetadataCatalog](../server/include/storage/metadata_catalog.hpp) | Four typed maps, derived UUID membership sets, non-recursive listing and Record metadata search; no filesystem I/O |
| [CatalogCoordinator](../server/include/storage/catalog_coordinator.hpp) | Borrows owner-supplied Catalog; startup, full Refresh, Index shutdown persistence, stored synchronization callbacks and consistency status |
| [IndexCodec](../server/include/storage/index_codec.hpp) | Pure deterministic MessagePack encode/decode; temporary validated Catalog, no file I/O |
| [I/O](../lib/include/folio/io/filesystem.hpp) | Generic byte reads, atomic create/replace, and durability operations |
| [Storage common types](../server/include/storage/common.hpp) | Storage-specific errors, mixed Repository results, and effective-path validation |

There is no separate BinaryIO, IndexStore or IndexBuilder. Frontend launching
lives under `app/src/`; Backend ownership/lifecycle under
`server/src/runtime/`. The [dispatcher](../app/src/main.cpp) selects the role;
only the Backend initializes Catalog lifecycle and finalizes its Index/Marker.
The historical `binary_io_test.cpp` filename does not denote a production module.

## Catalog-centric flows — Implemented internal capabilities

```text
Future owner / current internal caller
    → NormalRepository: validate and persist authoritative Entry
    → typed MutationResultCallback: saved metadata + actual locator
    → MetadataCatalog: explicit insert/update + derived membership
    → unpersisted = true

Catalog query → memory only
CatalogCoordinator → IndexCodec / I/O → application.cache/index/normal.msgpack
Explicit refresh_all → Repository.scan → typed metadata → Catalog.rebuild
```

The owner must supply matching callbacks; standalone Repository use does not synchronize Catalog. Managed creation additionally supplies `creation_check()` to check all four Catalog identity maps before publication. It reserves no ID; single-writer execution is a future Backend obligation. Without that check, standalone creation retains global on-disk uniqueness discovery. Omitted Record locators intentionally request UUID discovery. Known locator operations never silently rediscover after StaleLocator. Directory load/rename use kind/UUID-derived paths directly.

Publication followed by failure is not rollback. Typed failure notifications distinguish BeforeAuthoritativeChange, AuthoritativeStateMayHaveChanged, and CatalogSynchronizationFailed. Coordinator callbacks mark Inconsistent for the latter two; original diagnostics survive. No automatic scan, rollback, or Index I/O follows. Optional/custom callbacks cannot certify consistency by themselves.

`unpersisted` means projected metadata differs from its last acknowledged Index snapshot, not that Entries are unsaved. Inserts, changed updates/relocations, removals, and successful full rebuilds set it. Identical updates and queries do not. `CatalogStatus::Inconsistent` independently blocks Index saving. Full Refresh succeeds atomically and restores Ready; failed Refresh preserves contents/status/persistence state.

Current startup_load checks the durable State Session Marker first. A previous Marker bypasses Index loading and forces authoritative rebuilding. Without a Marker it attempts the Index once; missing/corrupt/unsupported/over-limit caches trigger reconstruction, while other I/O/path failures are reported. Valid Index startup performs no authoritative verification. Rebuild never immediately saves. shutdown_save acknowledges persistence only after successful atomic writing; repeated calls/retries are permitted. Once at orderly shutdown is caller policy, with explicit error handling, not destructor work or a once-only guard.

Current MessagePack schema is version 1. Its historical `entries` array contains Records, and `directories` contains Directory metadata; terminology alignment does not rename wire keys or require a schema bump. It stores primary typed metadata only, never reverse membership, Markdown bodies, Todo notes, or subtasks. Decoding validates a temporary Catalog and returns it marked persisted. Encoding is read-only and does not acknowledge a successful disk save. Detailed field representations and resource limits remain in [Storage's binary metadata codec section](storage-design.md#binary-metadata-codec).

Snapshot-to-metadata coverage guards in [metadata tests](../tests/storage/metadata_catalog_test.cpp) intentionally require explicit user approval before changing their field classifications. A requested model change alone is not permission to weaken the guards.

## Performance and future boundaries

**Implemented:** ordinary supplied-locator operations avoid global discovery; queries are memory-only; full scan detects malformed formal data and duplicate UUIDs across all four types. Tests cover these contracts in [Storage tests](../tests/storage/). Atomic writes can leave partial multi-file operations requiring reconciliation.

**Decided/Planned:** full scans are reserved for startup fallback, previous Session Marker recovery, and explicit Full Refresh. Explicit targeted discovery and standalone creation discovery remain deliberate API choices. Navigation may enumerate/count immediate formal objects without parsing them; equal counts are a practical indicator, never proof. No watchers, hashes, or periodic verification are required for uncommon external edits.

**Decided/Planned:** Application owns Catalog, Coordinator, and Repositories, presents public CLI/TUI APIs, and runs in one per-user Backend. Storage stays private. Normal Index Cache relocation is implemented. Durable Session Marker and private process ownership are implemented; Backend connections/lifecycle integration are not. Follow the [roadmap](roadmap.md), not speculative APIs.
## Effective storage paths — Implemented

Internal `StoragePaths` separates configurable `normal_root` and `vault_root` from an `ApplicationPaths application` value containing `config`, `data`, `state`, `cache`, and optional `runtime`. Internal callers supply these effective absolute paths; tests inject temporary locations. Application bootstrap resolves platform defaults and loads validated optional configuration overrides. See [Configuration](configuration.md) for the implemented Phase 4.1c contract; tests inject temporary paths.

`validate_storage_paths()` creates nothing, checks required absolute paths and existing directory types, and resolves existing filesystem aliases for overlap checks. Normal and Vault must be disjoint from each other and application support directories. Data may be their strict ancestor, allowing the default authoritative roots beneath Data. Application categories may share a location or nest beneath a common parent; this permits native platform layouts without prescribing one. Path validation does not establish ownership, permissions, or a secure IPC location; future resolution/runtime setup must establish those properties. Cache and runtime must not be used to store durable recovery data.

The Normal Index uses `<application.cache>/index/normal.msgpack`. Former State indexes are ignored, without reading, migrating, or deleting them. Missing Cache triggers authoritative rebuilding; saving occurs only on an explicit shutdown save when required, never immediately after rebuilding. Cache parents are created only during save. Durable Session Marker is implemented in Phase 3.4.3 and Backend lifecycle in 4.1b. Platform resolution and optional read-only configuration are implemented through 4.1c; no automatic configuration writer exists.

## Session lifecycle — Implemented internal support

The centralized Marker location is `<application.state>/session.marker`, a regular
file whose presence alone is significant; no payload schema or PID check exists.
It must be outside Cache and Runtime (shared durable application parents remain
valid). State and Marker symlinks/unsafe file types are rejected. Startup does
not delete or replace a previous Marker. A validated candidate Catalog is adopted
only after Marker establishment succeeds. `InitializationOutcome` distinguishes
previous-session recovery from a rejected Cache through
`previous_session_incomplete` and `index_error` respectively.

The future Backend must acquire an exclusive OS lock **before** `startup_load()`,
accept work only after success, stop admissions and finish active work, then call
`finish_session()` with explicit error handling/retry before releasing the lock.
The private OS ownership utility is implemented in Phase 4.1a; its integration with Catalog startup/finalization and Backend connections remains Phase 4.1b.

`finish_session()` calls retryable `shutdown_save()` when necessary, checks that
a regular persisted Index exists, synchronizes the Index and its parent links,
and only then removes the Marker and synchronizes State. An unchanged Index is
not rewritten. Missing Cache during an otherwise clean session prevents successful
finalization: retain the Marker and use explicit Refresh/retry or report failure.
`shutdown_save()` alone leaves the Marker in place; it is a lower-level save
checkpoint, not a clean-session certificate. No destructor saves or removes it.

Successful finalization sets `CatalogStatus::Stopped`; no more authoritative
operations are allowed. Failed saving or Inconsistent/Unavailable Catalog state
cannot authorize cleanup. If unlink succeeds but its directory sync fails, the
Index is already safe; return the original error, disable managed work, and allow
a cleanup retry (which syncs State even if the Marker is now absent). Alternatively,
a successful explicit Refresh reestablishes the Marker before restoring Ready.
A failed Refresh preserves the previous Catalog and state. Runtime Refresh and
CRUD do not rewrite the Marker or access the Index.

### Durability scope

POSIX helpers use no-follow path checks, file fsync and parent-directory fsync.
New directory links, including Cache/State ancestors, are synced before lifecycle
success; existing links are synced again on retry. Index publication reuses
the existing atomic primitives. Presence-only Marker creation uses exclusive
empty-file creation and file/parent sync, avoiding staged payloads or orphan
temporary files; sync failure retains the published Marker conservatively. These are filesystem-contract guarantees, not universal
power-loss guarantees: macOS uses fsync, not F_FULLFSYNC, and device caches and
filesystem support still matter ([Apple fsync documentation](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man2/fsync.2.html)).

Windows uses Unicode paths, file flushing and the existing atomic publication
primitive. File contents use [FlushFileBuffers](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-flushfilebuffers);
publication uses [MoveFileExW](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-movefileexw)
with WRITE_THROUGH. Directory creation and DeleteFileW cleanup have no explicit
parent-directory flush in this implementation, so Windows power-loss ordering is
weaker than the POSIX contract. Process-abandonment recovery remains supported.
Windows/Linux execution and actual power-loss tests require platform validation.
The Marker does not repair partially completed authoritative multi-file operations;
scan conflicts remain errors for later Recovery design.

## Runtime-role module structure — Implemented

```text
app/                         Frontend-only code
  include/                   BackendConnectionHandle, launcher and TUI entry
  src/                       Frontend implementations
  src/main.cpp               Shared dispatcher (explicit ownership exception)
server/                      Backend-only code
  include/                   Backend headers: entry point, runtime, IPC, Storage
  src/                       Entry point, handlers and request executor
  src/runtime/               Lifecycle, state machine and ownership implementations
  src/ipc/                   Backend connection management
  src/storage/               Storage implementations (headers in include/      storage)
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
preserves the pending runtime implementation without expanding its behavior.

## Backend runtime scheduling — Implemented

The existing BackendStateMachine owns lifecycle. Bounded IPC workers own individual
connections and OS waits; one Backend-owned queue and executor own all operation
execution. Access and Mutation Operations share admission and scheduling.
Accepted is session-only responsibility, followed by a final result/error.
Disconnection does not retract accepted work. Stopping/Finish drain and join
runtime resources before explicit Catalog finalization under RepositoryOwnership.
Public methods remain synchronous; UI/IPC Worker integration is deferred to 4.4b.
See [IPC runtime contracts and TODOs](ipc-protocol.md).

## Normal Mutations — Phase 4.3a implemented

BackendConnectionHandle submits Normal Create/Update/Move, Directory Create/Rename
and immediate Todo status changes through the existing shared MessagePack protocol,
admission and sequential executor. Backend-owned Entry Edit Locks are connection-
scoped, including ownership retained by already Accepted work after disconnection.
Full Editable Fields exclude identity, creation, privacy and membership; Backend
loads current authoritative state before Update. Move manages membership/location;
Frontend Modification Context is shared by both and is not a version token.
See [mutation contracts, evidence and deferred guarantees](application-mutations.md).
