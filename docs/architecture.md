# Folio architecture

Status vocabulary throughout these documents: **Implemented** means present in the inspected repository; **Decided/Planned** means agreed direction with no claim of working code; **Open/Deferred** means a later design decision is required. Implementation evidence is linked below. This consolidation does not change any schema or behavior.

## Documentation map

- [Storage specification](storage-design.md): existing codecs, physical layout, atomic writes, Catalog, and Index contracts. Preserved during this consolidation; some future-facing statements require separately approved amendments.
- [Application](application-design.md): planned public boundary, processes, IPC, and editing.
- [Recovery](recovery-design.md): Session Marker, Journals, Trash, and pending drafts.
- [Vault](vault-design.md): privacy and encryption boundaries.
- [Git Sync](git-sync-design.md): branch/worktree direction.
- [Roadmap](roadmap.md): implementation order, gates, and dependencies.
- [Decisions](design-decisions.md): rationale, alternatives, and deferred details.

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

[Public headers](../include/folio/core/) define:

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

Core validation is deliberately limited: restore validates enum values and applicable calendar dates; Directory validates kind, Todo validates due dates/status/priority and subtask indices, and Log validates Log dates. Core does not verify filesystem membership, timezone databases, or persistence consistency. Empty Todo title/notes are allowed; localized placeholders belong to Frontends. Do not infer uniform validation on all constructors/setters from restore validation. See [domain implementations](../src/core/).

Logs have no user Directory. Memos/Todos support one level, optional membership on the Record, and no nested Directories. The Catalog derives reverse membership; Directory never duplicates it.

## Current component and build boundary — Implemented

[Root CMake](../CMakeLists.txt) builds one static `folio_core` (`Folio::Core`) containing domain implementations and private Storage. `folio` links it and FTXUI. `include/` is PUBLIC; `src/` PRIVATE. Tests explicitly receive `src/`; consumers do not. Dependencies are pinned submodules: FTXUI, GoogleTest/GoogleMock, stduuid, libsodium, toml++, and msgpack-cxx. No database or JSON serialization is used.

Namespaces are `folio::core`, `folio::core::storage`, `folio::core::storage::codec`, and Storage `internal` atomic helpers. The generic internal Result<T, E> is a variant, not a public Application contract.

| Component | Responsibility / evidence |
| --- | --- |
| [NormalRepository](../src/storage/normal_repository.hpp) | Authoritative Normal Entry operations; targeted identity verification, explicit UUID discovery, full scan |
| [MetadataCatalog](../src/storage/metadata_catalog.hpp) | Four typed maps, derived UUID membership sets, non-recursive listing and Record metadata search; no filesystem I/O |
| [CatalogCoordinator](../src/storage/catalog_coordinator.hpp) | Borrows owner-supplied Catalog; startup, full Refresh, Index shutdown persistence, stored synchronization callbacks and consistency status |
| [IndexCodec](../src/storage/index_codec.hpp) | Pure deterministic MessagePack encode/decode; temporary validated Catalog, no file I/O |
| [Filesystem](../src/storage/filesystem.hpp) | Binary reads, safe paths, filenames, atomic create/replace |

There is no BinaryIO, IndexStore, IndexBuilder, IndexRefresh, Application Service, daemon, or IPC implementation. The [executable](../src/tui/main.cpp) presents a welcome screen with q to exit; it does not initialize Storage or save an Index. The historical `binary_io_test.cpp` filename does not denote a production BinaryIO module.

## Catalog-centric flows — Implemented internal capabilities

```text
Future owner / current internal caller
    → NormalRepository: validate and persist authoritative Entry
    → typed MutationResultCallback: saved metadata + actual locator
    → MetadataCatalog: explicit insert/update + derived membership
    → unpersisted = true

Catalog query → memory only
CatalogCoordinator → IndexCodec → filesystem → current state_root/index/normal.msgpack
Explicit refresh_all → Repository.scan → typed metadata → Catalog.rebuild
```

The owner must supply matching callbacks; standalone Repository use does not synchronize Catalog. Managed creation additionally supplies `creation_check()` to check all four Catalog identity maps before publication. It reserves no ID; single-writer execution is a future Backend obligation. Without that check, standalone creation retains global on-disk uniqueness discovery. Omitted Record locators intentionally request UUID discovery. Known locator operations never silently rediscover after StaleLocator. Directory load/rename use kind/UUID-derived paths directly.

Publication followed by failure is not rollback. Typed failure notifications distinguish BeforeAuthoritativeChange, AuthoritativeStateMayHaveChanged, and CatalogSynchronizationFailed. Coordinator callbacks mark Inconsistent for the latter two; original diagnostics survive. No automatic scan, rollback, or Index I/O follows. Optional/custom callbacks cannot certify consistency by themselves.

`unpersisted` means projected metadata differs from its last acknowledged Index snapshot, not that Entries are unsaved. Inserts, changed updates/relocations, removals, and successful full rebuilds set it. Identical updates and queries do not. `CatalogStatus::Inconsistent` independently blocks Index saving. Full Refresh succeeds atomically and restores Ready; failed Refresh preserves contents/status/persistence state.

Current startup_load attempts the Index once; missing/corrupt/unsupported/over-limit caches trigger reconstruction, while other I/O/path failures are reported. Valid Index startup performs no authoritative verification. Rebuild never immediately saves. shutdown_save acknowledges persistence only after successful atomic writing; repeated calls/retries are permitted. Once at orderly shutdown is caller policy, with explicit error handling, not destructor work or a once-only guard.

Current MessagePack schema is version 1. Its historical `entries` array contains Records, and `directories` contains Directory metadata; terminology alignment does not rename wire keys or require a schema bump. It stores primary typed metadata only, never reverse membership, Markdown bodies, Todo notes, or subtasks. Decoding validates a temporary Catalog and returns it marked persisted. Encoding is read-only and does not acknowledge a successful disk save. Detailed field representations and resource limits remain in [Storage's binary metadata codec section](storage-design.md#binary-metadata-codec).

Snapshot-to-metadata coverage guards in [metadata tests](../tests/storage/metadata_catalog_test.cpp) intentionally require explicit user approval before changing their field classifications. A requested model change alone is not permission to weaken the guards.

## Performance and future boundaries

**Implemented:** ordinary supplied-locator operations avoid global discovery; queries are memory-only; full scan detects malformed formal data and duplicate UUIDs across all four types. Tests cover these contracts in [Storage tests](../tests/storage/). Atomic writes can leave partial multi-file operations requiring reconciliation.

**Decided/Planned:** full scans are reserved for startup fallback, previous Session Marker recovery, and explicit Full Refresh. Explicit targeted discovery and standalone creation discovery remain deliberate API choices. Navigation may enumerate/count immediate formal objects without parsing them; equal counts are a practical indicator, never proof. No watchers, hashes, or periodic verification are required for uncommon external edits.

**Decided/Planned:** Application owns Catalog, Coordinator, and Repositories, presents public CLI/TUI APIs, and runs in one per-user Backend. Storage stays private. Normal Index moves from current State to Cache. Session Marker and locking are not implemented. Follow the [roadmap](roadmap.md), not speculative APIs.
## Effective storage paths — Implemented

Internal `StoragePaths` separates configurable `normal_root` and `vault_root` from an `ApplicationPaths application` value containing `config`, `data`, `state`, `cache`, and optional `runtime`. Internal callers supply these effective absolute paths; tests inject temporary locations. Platform defaults and configuration-file setup are deferred until application configuration is developed. No environment or home-directory lookup is performed.

`validate_storage_paths()` creates nothing, checks required absolute paths and existing directory types, and resolves existing filesystem aliases for overlap checks. Normal and Vault must be disjoint from each other and every application directory. Application categories may share a location or nest beneath a common parent; this permits native platform layouts without prescribing one. Path validation does not establish ownership, permissions, or a secure IPC location; future resolution/runtime setup must establish those properties. Cache and runtime must not be used to store durable recovery data.

The Index remains at `<application.state>/index/normal.msgpack` during this transition. Cache relocation, Session Marker, platform default resolution, and configuration persistence are not implemented.
