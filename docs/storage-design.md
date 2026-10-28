# Storage & Persistence Architecture v0.1

This document records the agreed direction for Folio persistence and the implemented milestones. Remaining design decisions are listed at the end.

## Principles and Boundaries

- Folio uses independent file-based persistence; there is no database.
- Entry files are authoritative. Indexes are derived, rebuildable data.
- Every persistent structured format has a version. Newer Folio versions may migrate older formats; older versions need not read newer formats.
- Storage, Vault, and future Git synchronization are separate concerns. Synchronization algorithms are outside this design phase.
- Startup configuration supplies `normal_root`, `vault_root`, `state_root`, and `cache_root` explicitly. No storage paths are hard-coded or defaulted by the filesystem layer. The state root is separate from the data roots.

## Normal Data Formats and Layout

Normal entries use `logs/`, `memos/`, and `todos/`, plus a logical Trash. Logs stay flat. Memo and Todo may be organized into one level of physical folders corresponding to their logical Directories. A Directory is grouping metadata, not an entry alongside Log, Memo, or Todo. Its stable UUID and UTF-8 logical display name are stored in `.folio-dir.toml`. Folio-managed physical Directory names are ASCII UUIDs (`<uuid>/`); renaming a Directory changes only its logical name and does not change its physical path.

Normal Logs and Memos are Markdown files with TOML front matter delimited by `+++`. Normal Todos and directory metadata are TOML. toml++ is pinned as a Git submodule under `thirdparty/` and is used by the in-memory TOML codec. Encoding and decoding produce/consume text and do not perform file I/O, encryption, or storage operations. Every structured document starts with `schema_version = 1`; unsupported versions are rejected.

### Current v1 Codec Scope

The codec covers common values (`Id`, `Tag`, `Privacy`, `TimestampContext`, `DateContext`, and `DueDate`), Todo and Directory TOML snapshots, and Log and Memo Markdown snapshots. It uses stable enum strings, UUID strings, UTC RFC 3339 timestamp strings, `YYYY-MM-DD` date strings, and omits absent optional fields. A due date is a `[due]` table whose `kind` is `Date` or `Datetime`; Todo subtasks preserve list order. Directory metadata contains `schema_version`, `id`, `kind` (`Memo` or `Todo`), and its UTF-8 logical `name`. `DirectoryKind` is defined once in the public Core Directory header and is preserved by the Directory object, Snapshot, codec, and catalog projection. New encodings include `kind`; legacy version-1 metadata without it is accepted only with an explicit trusted Repository module context. An explicit kind that disagrees with that context is rejected; decoding never guesses a default. IDs, logical names, and schema version remain unchanged. Encoders construct toml++ tables and arrays directly, without parsing fixed TOML templates at runtime.

### Todo TOML v1

Todo snapshots contain separate UTF-8 `title` and `notes` strings. Both are required string fields in TOML and may be empty; notes may contain multiline text. The schema remains version 1, with `title` and `notes` replacing the former `description` field; no legacy-description fallback or migration is provided. Serialization preserves both strings. Core does not supply placeholder titles or reject empty titles; localized defaults and input guidance belong to frontends. Editing either field updates the modification context.

Only the Todo title is projected into metadata for text queries and title sorting. Notes and subtask content remain exclusively in authoritative Entry data and are excluded from the catalog.

### Log and Memo Markdown v1

Both front-matter schemas require `schema_version = 1`, `id`, `privacy`, `title`, `tags` (an ordered string array, possibly empty), `created`, and `modified`. Log additionally requires `log_date`; Memo permits an optional `directory_id`. Time/date contexts preserve their value, timezone, and optional location using the common codecs. `content` occurs exclusively in the Markdown body and is forbidden in front matter. Unknown fields, including extra fields inside contexts, are rejected rather than discarded. Missing fields, invalid values, malformed TOML, and unsupported versions produce recoverable codec errors.

The opening delimiter must be `+++` at byte zero, followed by LF or CRLF. The first subsequent line containing exactly `+++`, terminated by LF or CRLF, closes front matter. That entire delimiter line, including its line ending, is framing; the body begins immediately afterward. No blank separator line is inserted or removed. A closing delimiter without a line ending is invalid, including for empty content. LF and CRLF framing may be mixed; bare CR framing, a BOM, delimiter whitespace, and a preamble are not accepted.

Encoding emits LF framing and escapes metadata string newlines so that metadata cannot contain a standalone delimiter line. When writing front matter manually, a standalone `+++` line is reserved for framing, including within TOML multiline strings. All bytes after the closing line are opaque body content, including further `+++` lines, leading/trailing whitespace, mixed newline endings, and the absence of a final newline. Decoding never parses or normalizes Markdown. Snapshot serialization/restoration preserves identity and time contexts without generating IDs or consulting a clock.

New Normal entries receive filenames `YYYY-MM-DD_HHMMSS_<uuid>.<ext>`, based on UTC creation time and ASCII characters only. Filenames do not change when a title or Log date changes. Existing entry basenames may differ from this convention without affecting identity or validity, provided they retain the module's formal extension: `.md` for Logs and Memos, `.toml` for Todos. Ordering among entries created in the same second is unspecified.

### Normal repository

The Normal repository stores only `Normal` privacy snapshots. Logs live directly in `normal_root/logs/`. Memos and Todos live directly in their module root or in a single `<directory-uuid>/` child. Each child has `.folio-dir.toml` metadata whose ID must match the physical UUID directory name. The UUID stored in an entry is its identity; a path or filename is only a locator and cannot establish that two files are the same entry. A filename whose UUID or timestamp differs from the stored values is valid and is preserved during updates and moves. The official entry scan reads `.md` files for Logs and Memos and `.toml` files for Todos; `.folio-dir.toml` is read separately as Directory metadata. Other extensions are outside this scan. During discovery, two formal entry files with the same stored UUID are reported as an error even when discovery requested a different UUID. Atomic-write temporary files are outside the official entry scan, so their staged copy may contain the same UUID as the destination. Inspecting and resolving pending temporary files belongs to the planned recovery service; the entry scan does not parse, delete, or repair them. Recovery artifacts will likewise remain separate from formal entries when their format and location are defined. The repository still validates module placement, Directory references, and safe paths. Directory display names never determine physical paths. A future index is only a rebuildable lookup cache that maps stored UUIDs to actual paths; entry files remain authoritative.

Entry load and update methods accept an optional `relative_path`, allowing a future index to supply a cached location. With no path, the repository discovers the entry by its stored UUID using the full formal-file scan. With a path, it validates the relative path and module layout, reads that exact file, and checks its stored UUID against the requested ID or Snapshot ID without a full scan. A missing target or a different stored UUID returns `StaleLocator`; an unsafe path, malformed document, or other filesystem failure retains its own error. The repository does not automatically fall back from a supplied path to discovery. The caller can request discovery and refresh its cache after a stale result. Supplied-path operations validate their target but do not detect duplicates elsewhere; discovery and future index verification provide that global check.

An update atomically replaces the existing file found by its stored UUID and preserves its actual filename. A move serializes the entry with its new Directory ID and modification timestamp, atomically creates that payload at the destination using the same filename, then removes the source and syncs its parent on POSIX. If destination publication fails, the source remains; a post-publication failure may leave both paths, while a source removal sync failure leaves uncertain crash durability. Creation may first make the Normal root and module folder; a later failure can leave empty folders, and their directory entries are not separately synced. Directory creation may leave a UUID folder without valid metadata if its metadata write fails. These states require explicit reconciliation. **TODO:** journal and recover multi-file moves and Directory creation, including interrupted root/module/folder creation. The repository does not implement a transaction or silently resolve partial states.

## Directory Reconciliation

Directory is the logical grouping that Memo and Todo may reference. Normal storage may represent that grouping with one physical folder level; Log has no Directory association. External filesystem changes are validated and reconciled. Storage must not silently overwrite data when it finds unexpected or conflicting filesystem state.

## Privacy and Vault

Privacy levels are `Normal`, `Protected`, and `Hidden`. Normal entries are plaintext. Protected and Hidden entries use the same Vault encryption mechanism. While locked, Protected entries appear as classified placeholders; Hidden entries do not appear. Both display normally when unlocked.

Protected Logs expose their Log date publicly. Protected Memo and Todo entries expose existence only, not titles or other sensitive metadata. Public metadata must never disclose private titles, locations, timezones, tags, or logical directories. Privacy cannot be changed after persistent creation in the MVP.

Vault stores encrypted entries as independent binary files named `<uuid>.enc`, under flat `logs/`, `memos/`, `todos/`, and `trash/` directories. Logical directories are represented in encrypted metadata rather than physical Vault directories. Encrypted files use a versioned authenticated-encryption envelope containing magic, format version, algorithm and key identifiers, nonce, and authenticated ciphertext. The decrypted payload has its own schema version.

The planned cryptography is libsodium XChaCha20-Poly1305, with a random master key protected by a password-derived Argon2id key. Exact parameters and key lifecycle remain to be designed. Do not treat a GitHub Private repository as end-to-end encryption.

## Indexes and In-Memory Metadata

Entry files are the source of truth. The primary working index is a rebuildable, non-authoritative in-memory Metadata Catalog. Normal Storage and the future Vault share its logical metadata and query model; the catalog does not impose Normal filenames or physical Directory layouts. Its implementation headers remain private under `src/storage/` in `folio_core`, with namespace `folio::core::storage`. Domain Snapshots contain no index-specific fields.

Typed Log, Memo, Todo, and Directory metadata retain internal UUIDs and relative physical locators. Entry metadata includes its type, privacy, and complete created/modified TimestampContexts. Logs retain title, tags, and complete Log DateContext; Memos retain title, tags, and optional Directory ID; Todos retain title, status, priority, optional DueDate, and optional Directory ID. Directory metadata retains its UUID, UTF-8 logical name, module kind, and relative locator. Body and subtask content are excluded. Snapshot conversion copies metadata without I/O, generating IDs, or consulting a clock.

The catalog provides explicit insert, update, and remove operations with separate Entry and Directory identity registries. Duplicate insertion fails; a locator never establishes identity. Directory references must exist in the correct module. Queries return copies and never consult the filesystem. Module-root listing includes only root Entries, while a Directory listing includes only Entries with that Directory ID; child Directories are listed separately by module. Search spans Directories unless membership is explicitly constrained. Queries support indexed fields and deterministic sorting with UUID tie-breaking. Text matching folds ASCII case only; non-ASCII UTF-8 bytes are preserved and compared exactly without Unicode normalization or locale-dependent folding.

The internal interface is `src/storage/metadata_catalog.hpp`. `EntryMetadata` is a `std::variant<LogMetadata, MemoMetadata, TodoMetadata>`; `entry_type()` derives the EntryType from its alternative, preventing inconsistent type/payload pairs. `metadata_from()` overloads extract metadata from each Snapshot. `MetadataCatalog` offers `insert_entry`, `update_entry`, `remove_entry`, `find_entry`, the corresponding Directory operations, `list_root`, `list_directory`, `list_directories`, and `query`. Explicit Entry updates preserve the registered type but can replace stale projected fields, including creation context and privacy. Updating locator and Directory ID together is the catalog's relocation operation. Occupied locators are reported as metadata conflicts rather than establishing identity. Referenced Directories cannot be removed or moved to another module.

`CatalogQuery` combines optional type, privacy, Directory membership, text/tag substring, created/modified timestamp ranges, Log-date range, and Todo status, priority, due-presence, due-kind, and due-range constraints. Constraints combine with AND. An absent membership constraint searches every Directory; an engaged empty optional selects roots, and an ID selects that Directory. Ranges are inclusive. Todo-specific constraints exclude other Entry types, and Log-date bounds exclude non-Logs. Due bounds match only the same Date/Datetime alternative; calendar dates and absolute timestamps are not implicitly converted using a timezone. Sorting supports UUID, type, created/modified time, title, Log date, due date, status, priority, and privacy. String sorting uses exact byte order; absent/inapplicable values remain last in both directions, and UUID ties remain ascending. Ascending due sorting puts calendar dates before datetimes. Root/Directory listings sort by UUID.

The implementation keeps Logs, Memos, and Todos in separate typed STL maps keyed by UUID strings, with a fourth map for Directory metadata. Entry UUID uniqueness and locator-conflict validation span all three Entry collections; splitting by type does not create independent identity namespaces. Private reverse membership maps contain only Entry UUIDs, grouped by Directory UUID, with separate Memo and Todo root sets. Primary Entry metadata remains the source of membership truth; these secondary structures can be rebuilt by inserting Directory metadata and then primary Entry records into a fresh catalog. There is no unchecked import path. Insert, explicit update (including relocation), and removal maintain both structures. Directory objects never own Entries or membership lists. Ordinary mutations require an existing Directory of the correct kind, and non-empty Directories cannot be removed or changed to another kind. A query with `type` supplied inspects only that type; with `type` absent it searches all Entry collections and merges the results before a single global sort. Callers select module search by supplying `type` and global search by omitting it; the catalog has no implicit current-module or UI state. `EntryMetadata` remains the shared variant for insertion/update inputs, UUID lookups, and mixed query/listing results.

UUID lookup uses ordered maps with O(log n) lookup per typed registry. Directory listings resolve only their k indexed members, with O(k log n) metadata lookup and deterministic UUID ordering, rather than scanning every Entry. Root membership sets keep Memo and Todo roots separate. Filtering remains linear in the selected collection sizes, with O(k log k) comparisons to sort k matches, in addition to text comparisons and metadata copying. Locator-conflict validation may still scan metadata; no query reads files.

Successful mutations mark the catalog dirty until an explicit acknowledgement of a persisted snapshot clears that flag. Failed validation and read-only queries leave the flag unchanged. No persistence, scanning, repository callbacks, concurrency, or cache scheduler is provided by the catalog itself. **TODO:** integrate catalog updates after successful repository operations; Entry writes must remain successful even if a subsequent metadata update fails, with enough context to refresh safely.

### Binary metadata codec

The private `src/storage/index_codec.hpp` interface exposes `index_codec::encode_index(const MetadataCatalog&)` and `index_codec::decode_index(std::string_view)`. Results are variants containing bytes/a fresh MetadataCatalog or an `IndexCodecError` with a code, logical field path, and diagnostic. Encoding reads a stable catalog snapshot through existing copy-returning query/list APIs; no new mutable export interface is needed. The codec performs no filesystem I/O and does not change Entry formats.

The codec uses the existing official msgpack-cxx submodule, pinned at `e1b8bc889d0c16714fc9236af4b954cbfa182be4` from `msgpack/msgpack-c`. Its CMake target is linked privately to `folio_core`, with Boost, dependency tests, examples, and documentation disabled. MessagePack types do not appear in public Core headers or the codec interface.

The version-1 wire representation is a single MessagePack map, with string keys and these fields:

| Record | Required fields | Optional fields (omitted when absent) |
| --- | --- | --- |
| Top level | `directories` (array), `entries` (array), `schema_version` (integer 1) | None |
| Every Entry | `created`, `id`, `modified`, `privacy`, `relative_path`, `title`, `type` | Type-specific below |
| Log | Common fields, `log_date`, `tags` | None |
| Memo | Common fields, `tags` | `directory_id` |
| Todo | Common fields, `priority`, `status` | `directory_id`, `due_date` |
| Directory | `id`, `kind`, `name`, `relative_path` | None |
| TimestampContext | `nanoseconds`, `seconds`, `timezone` | `location` |
| DateContext | `date`, `timezone` | `location` |
| DueDate | `kind`, `value` | None |

UUIDs are lowercase canonical RFC-variant UUIDv4 strings. `type` is `Log`, `Memo`, or `Todo`; Directory `kind` is `Memo` or `Todo`; `privacy` is `Normal`, `Protected`, or `Hidden`; `status` is `Pending` or `Completed`; and `priority` is `None`, `Low`, `Medium`, or `High`. Tags are ordered UTF-8 string arrays. Titles, Directory names, timezones, and locations retain their exact UTF-8 bytes, including an explicitly present empty location. Todo notes/subtasks and Log/Memo body content are never encoded.

Timestamp `seconds` is a signed 64-bit integer counting whole Unix seconds rounded toward negative infinity; `nanoseconds` is an integer in `[0, 999999999]`. For example, half a second before the epoch is seconds -1 and nanoseconds 500000000. Decoding rejects overflow or precision that the platform's `system_clock` cannot represent. Calendar dates are exact `YYYY-MM-DD` strings with valid dates and years 0000–9999, matching the existing text codec range. DueDate uses `kind = Date` with a DateContext `value`, or `kind = Datetime` with a TimestampContext `value`. No implicit timezone conversion occurs. Relative locators use UTF-8 `/` separators; absolute paths, drive prefixes, backslashes, NUL, empty components, and `.`/`..` components are rejected. Locators do not establish identity or imply filesystem access.

Encoding sorts Entries and Directories independently by canonical UUID and writes map fields in fixed lexical key order. Tag order remains significant. Equivalent primary metadata produces identical bytes regardless of insertion or query sorting order. Derived membership lists are absent. Optional values use absent keys rather than `nil`; a present optional field must have its declared type. Unknown fields may be ignored only within resource limits and when they do not conflict with a known representation. All maps, including unknown nested maps, require unique UTF-8 string keys; known fields belonging to a different record alternative are rejected.

Safety limits are 64 MiB of encoded input/output, 100,000 combined Entry and Directory records, 100,000 elements per array (including tags), 64 pairs per map, nesting depth 16, and 8,000,000 total MessagePack objects. Individual string/binary/extension payloads cannot exceed the overall byte limit. These are cache resource bounds, not new Entry text restrictions; long titles remain supported within the aggregate byte budget. A bounded structural preflight checks lengths, depth, and total objects before msgpack-cxx allocates its decoded object tree. The decoder also rejects truncation, invalid markers, unexpected trailing bytes, unsupported versions, invalid UTF-8 in metadata, wrong/missing field types, invalid enums/dates/UUIDs, duplicate identities, and catalog integrity failures. MessagePack and allocation exceptions become explicit codec errors. Existing catalog locator-conflict validation scans primary records, so rebuilding a large catalog can take quadratic time; optimizing catalog import is deferred rather than introducing an unchecked import path.

Decoding never receives a mutable live catalog. It restores Directory records before Entry records into a temporary catalog using ordinary validated insertions; wire ordering does not determine whether a Directory reference resolves. Missing or wrong-kind references, duplicate identities, and occupied locators fail rather than being repaired. The existing catalog has no unchecked import or independent reference-verification API. Reverse Directory membership and root membership sets are reconstructed from primary records and are not serialized. Only a completely validated catalog is returned, marked clean. Encoding and failed decoding leave existing catalog contents and dirty state unchanged. Callers must check the result before replacing their current catalog; encoding alone does not acknowledge persistence or clear dirty state.

### Persistent cache policy and future rebuilding

Persistent indexes are low-priority, rebuildable caches, rather than authoritative databases. The Normal cache belongs under configurable `state_root`, outside `normal_root`, and must not be committed or pushed to Git. The persistence lifecycle is strict: load the persisted index once at application startup, use the in-memory catalog for the entire session, and save the complete index once during orderly shutdown if dirty. There are no index disk reads or writes during ordinary application operations. Explicit cache load/save primitives are available independently; lifecycle orchestration, authoritative file scanning, and repository integration remain future work. Missing, unsupported, or corrupted caches must never modify authoritative Entries. Ordinary listing, filtering, and metadata search use the loaded catalog without parsing Entry files again.

The Normal index is deliberately local-only: placing it under `state_root` rather than `normal_root` keeps this rebuildable cache out of the data tree synchronized to the Git remote. Its metadata is complete plaintext. Entry locators inside the index remain relative to the associated repository's data root, not to the index file's location.

### Normal index cache store

The internal `src/storage/index_store.hpp` component, `NormalIndexStore`, stores the complete Normal index at `<state_root>/index/normal.msgpack`. The relative location is centralized in the store. Startup configuration supplies `StoragePaths`; the store validates root separation and never uses `normal_root`, `vault_root`, or `cache_root` as the cache destination. Entry locators inside the decoded metadata remain relative to the repository data root. The constructor only stores configuration; filesystem access occurs through explicit load/save operations.

`load()` opens and reads the complete bounded binary payload, then calls IndexCodec. It returns a newly reconstructed clean MetadataCatalog on success, or an explicit error distinguishing a missing cache, corrupt data, unsupported schema, and filesystem/I/O failures. Resource-limit and invalid-path errors remain explicit. A missing or invalid cache is never returned as a valid empty catalog. Loading does not create directories, retry reads, scan Entries, repair files, rebuild metadata, or mutate a live catalog.

`save(const MetadataCatalog&)` returns `Saved`, `SkippedClean`, or an error. A clean catalog is skipped before any filesystem I/O. For a dirty catalog, encoding completes before parent creation; the store then creates missing state/index directories and invokes exactly one existing atomic create or replace operation for the full snapshot. An absent destination uses create-only semantics; an existing regular destination uses replacement. There is no retry that switches creation to replacement after a race. Symbolic-link redirects and non-regular cache destinations are rejected. Callers must exclude external filesystem mutation during an operation, matching the atomic writer contract.

Save never mutates the supplied catalog or acknowledges persistence. The lifecycle owner may call `mark_clean()` only after `Saved`; encoding success alone is insufficient. All save failures leave dirty state unchanged and propagate diagnostic context, including the atomic writer's publication flag. A failure after publication can leave the new index visible without confirmed crash durability; the store does not roll it back. Parent creation can leave empty directories on later failure, and newly created ancestors do not gain stronger crash-durability guarantees than the existing directory-creation/atomic-write primitives. **TODO:** address parent-directory creation durability and partial states with the future transaction/recovery design; do not add a journal here.

The intended calling policy belongs to a future lifecycle owner:

```text
startup           -> one index load
runtime           -> memory-only catalog
orderly shutdown  -> one full index save if dirty
```

The store does not enforce invocation counts or hold session state. It implements no timers, background persistence, debouncing, incremental writes, automatic reload, or TUI/Repository integration. Unexpected termination can leave a stale index; later verification and rebuilding must reconcile it against authoritative Entry files. A cache-save failure must never invalidate a successfully saved Entry.

### Vault index synchronization policy

Vault has a different synchronization policy. Its planned complete encrypted private index and minimal plaintext public index are both intended to synchronize with Vault data to the Git remote. Synchronizing the private index avoids decrypting and parsing every Vault Entry merely to reconstruct metadata on another device; after unlocking, Folio can decrypt the private index instead. The public index must be available before Vault decryption so Folio can expose the permitted metadata while locked. It contains Entry UUID, type, safe relative locator, visibility/privacy, and a Log date only for Protected Logs; it must not disclose private titles, tags, timezones, locations, or logical Directories. Both Vault indexes remain auxiliary projections of authoritative Entry data. Their exact physical paths, encryption implementation, synchronization consistency handling, and rebuilding remain future work; the shared catalog does not enforce a locked-Vault visibility policy.

### Future rebuilding and verification

Future rebuilding will scan recognized formal Entry files and read Directory metadata separately, identify Entries by stored UUID regardless of filename, and report duplicate identities or ambiguous structures without silently repairing them. Opening an Entry at a known locator remains a direct Repository operation; identity validation and `StaleLocator` handling follow the repository contract above. **TODO:** add explicit refresh/verification and lightweight immediate-file count checks on entering a physical Directory. Equal counts cannot detect content edits or one-for-one replacements; these checks must not promise full verification or constant-time enumeration.

## Write Safety, Transactions, and Trash

### Current single-file writes

Storage exposes separate `create_file_atomic` and `replace_file_atomic` operations. Both write opaque bytes, so Normal repositories can pass encoded plaintext and a future Vault repository can pass an already encrypted binary envelope through the same API. The atomic writer does not parse, encode, or encrypt the payload. Create never overwrites an existing path; replace requires an existing regular file. Callers must exclude external filesystem mutation during an operation. Folio writers acquire exclusive access to the parent directory (an advisory directory lock on POSIX, a named mutex keyed by directory identity on Windows); contention returns an error. Each operation owns at most one temporary pathname. A pending temporary file for the same destination returns `RecoveryRequired` before another is created and is left untouched, so retries do not accumulate multiple temporary files for an entry.

Temporary files are siblings named `.<destination-filename>.tmp-<random-uuid>`, for example `.2026-10-24_093015_47183823-2574-4bfd-b411-99ed177d3e43.md.tmp-550e8400-e29b-41d4-a716-446655440000`. The suffix uses a freshly generated UUIDv4. These names are staging artifacts and must not be treated as entry filenames.

On POSIX, the writer syncs the completed temporary file and then the parent directory before publication. Replace renames the temporary file over the destination and syncs the parent directory again. Rename consumes the temporary name; a later sync failure does not undo the rename. Create instead links the temporary file to the destination without overwriting, syncs the directory, removes the temporary link, and syncs the directory again. This creates only one temporary pathname.

A failed write, file sync, or publication attempt triggers removal of the operation's temporary file before returning. If the staging directory sync fails, the writer removes the temp and attempts to sync that removal; the destination is unchanged. If the publication directory sync fails after create, the writer removes the temporary link and attempts to sync that removal. A failed publication sync after replace leaves the new destination with no temporary name because rename consumed it. Cleanup can also fail, in which case a temporary file may remain and requires recovery. Crashes can leave temporary files as well.

`StorageError::replacement_completed` is true after either create or replace publishes, even if a later sync or cleanup fails. A publication sync failure reports uncertain crash durability, not a rollback. Callers must reconcile such failures before further writes; automated recovery and journals are not implemented. Failed cleanup sync after create means publication was synced but temporary-link removal was not confirmed durable. These checks do not guarantee recovery from arbitrary filesystem or hardware failures.

### Planned transactions

Use atomic replacement for individual-file writes, with appropriate durability measures. Expand a batch operation into its complete plan before executing it. Normal and Vault each have a recovery journal; corresponding journals share a transaction ID. Both journals must be durably prepared before any data modification. Record each completed sub-operation and recover interrupted or partially completed transactions.

Recovery journals live under the configurable persistent state root and are distinct from any future Audit Log. Trash is one logical feature with separate Normal and Vault physical storage; Vault Trash remains encrypted.

## Core Model Implications

### Primary Entry Models

Log, Memo, and Todo are Folio's primary entry models. Each represents user-authored content or a task and owns its own entry data. Privacy levels are `Normal`, `Protected`, and `Hidden`; ordinary post-creation privacy mutation is not exposed. Tag rename and deletion update references in Log and Memo.

### Organization and Embedded Data

Directory is organizational metadata with an explicit Memo or Todo kind, referenced optionally by Entries of that module. It is not a primary entry model and has no Log association. Filesystem layout and `.folio-dir.toml` handling belong to storage code. Subtasks are part of Todo and are not independently indexed or persisted.

### Snapshot, Restore, and Codec Support

Typed in-memory snapshots and static restore factories are available for Log, Memo, Todo, and Directory. Snapshots preserve IDs and stored state and do not contain schema versions. The in-memory v1 codecs map common value types plus Todo and Directory snapshots to TOML, and Log and Memo snapshots to Markdown with TOML front matter; they do not perform file I/O or encryption. Directory remains organizational metadata rather than a primary entry type. Storage, Vault, and sync behavior must not be placed in TUI components.

## Git Boundaries

Use one Git repository with separate Normal and Vault branches checked out through Git worktrees. Vault history compaction and cross-device semantic merge belong to a later Sync subsystem; this phase does not specify or implement those algorithms.

## Current Core Discrepancies

The public headers under `include/folio/core/` currently describe in-memory models and differ from this design in these ways:

- `types.hpp` now defines `Privacy { Normal, Protected, Hidden }`, matching the design.
- `Memo` and `Todo` now store optional Directory IDs; `Log` has no Directory field. Directory remains an in-memory logical model with no filesystem access or physical path mapping in Core; its v1 TOML metadata codec is in-memory only.
- Time data is represented by `TimestampContext` and `DateContext`; the in-memory codec serializes them as RFC 3339 UTC timestamp strings and `YYYY-MM-DD` date strings, with timezone and optional location stored separately.
- `Log`, `Memo`, `Todo`, and `Directory` expose typed in-memory snapshots, static restore factories, and v1 in-memory codecs. Privacy remains immutable after construction.
- The internal Storage subsystem in `folio_core` provides path validation, Normal entry filename generation, single-file atomic writing, and a Normal repository for plaintext entries and Directory metadata. Storage implementation headers live under `src/storage/` and are not part of the public Core API. It does not load configuration, create Vault storage, or provide persistent indexes, journals, migrations, or automated filesystem reconciliation. Its in-memory Metadata Catalog is a separate metadata projection with no repository integration yet. `Subtask` is currently embedded in `Todo`, consistent with the design.
- `Tag` is a value type, but there is no shared tag rename/delete operation that updates Log and Memo references.
- `DateContext` includes a calendar date, timezone, and optional location; `TimestampContext` includes an absolute timestamp, timezone, and optional location. Privacy rules for these fields in indexes and public metadata remain defined by the Vault/index design.

## Decisions Needed Before the First Storage Milestone

The architecture establishes boundaries and formats but does not yet define enough detail for interoperable persistence. Before implementing the first storage milestone, decide:

- The binary index encoding, cache lifecycle, and rebuild/integration interfaces remain to be implemented; the in-memory metadata model is described above.
- How unknown fields are preserved or handled during future migration; the current Log/Memo v1 decoder rejects them.
- The exact Normal and Vault directory tree, including safe-relative-locator rules beyond the ASCII UUID Directory naming and filename patterns.
- How invalid or duplicate Directory metadata is reconciled during filesystem scanning.
- The transaction journal record format, sub-operation states, recovery decision rules, and platform-specific durability guarantees for atomic replacement.
- What conditions trigger consistency checks and how callers observe/resolve the explicit recovery state.
- The complete public-index visibility vocabulary and rules for each privacy level, including Hidden entries and locked/unlocked views.
- The first migration policy and supported source schema versions.

The exact cryptographic parameters, master-key lifecycle, Vault history handling, and cross-device merge remain deferred designs; they are not assumptions for this storage milestone.
