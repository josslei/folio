# Storage & Persistence Architecture v0.1

This document records the agreed direction for Folio persistence. It is a design specification, not an implementation. Current Core/API differences and decisions still needed before implementation are listed at the end.

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

The codec covers common values (`Id`, `Tag`, `Privacy`, `TimestampContext`, `DateContext`, and `DueDate`), Todo and Directory TOML snapshots, and Log and Memo Markdown snapshots. It uses stable enum strings, UUID strings, UTC RFC 3339 timestamp strings, `YYYY-MM-DD` date strings, and omits absent optional fields. A due date is a `[due]` table whose `kind` is `Date` or `Datetime`; Todo subtasks preserve list order. Directory metadata contains `schema_version`, `id`, and its UTF-8 logical `name`. Encoders construct toml++ tables and arrays directly, without parsing fixed TOML templates at runtime.

### Log and Memo Markdown v1

Both front-matter schemas require `schema_version = 1`, `id`, `privacy`, `title`, `tags` (an ordered string array, possibly empty), `created`, and `modified`. Log additionally requires `log_date`; Memo permits an optional `directory_id`. Time/date contexts preserve their value, timezone, and optional location using the common codecs. `content` occurs exclusively in the Markdown body and is forbidden in front matter. Unknown fields, including extra fields inside contexts, are rejected rather than discarded. Missing fields, invalid values, malformed TOML, and unsupported versions produce recoverable codec errors.

The opening delimiter must be `+++` at byte zero, followed by LF or CRLF. The first subsequent line containing exactly `+++`, terminated by LF or CRLF, closes front matter. That entire delimiter line, including its line ending, is framing; the body begins immediately afterward. No blank separator line is inserted or removed. A closing delimiter without a line ending is invalid, including for empty content. LF and CRLF framing may be mixed; bare CR framing, a BOM, delimiter whitespace, and a preamble are not accepted.

Encoding emits LF framing and escapes metadata string newlines so that metadata cannot contain a standalone delimiter line. When writing front matter manually, a standalone `+++` line is reserved for framing, including within TOML multiline strings. All bytes after the closing line are opaque body content, including further `+++` lines, leading/trailing whitespace, mixed newline endings, and the absence of a final newline. Decoding never parses or normalizes Markdown. Snapshot serialization/restoration preserves identity and time contexts without generating IDs or consulting a clock.

Normal filenames are `YYYY-MM-DD_HHMMSS_<uuid>.<ext>`, based on UTC creation time and ASCII characters only. Filenames do not change when a title or Log date changes. Ordering among entries created in the same second is unspecified.

## Directory Reconciliation

Directory is the logical grouping that Memo and Todo may reference. Normal storage may represent that grouping with one physical folder level; Log has no Directory association. External filesystem changes are validated and reconciled. Storage must not silently overwrite data when it finds unexpected or conflicting filesystem state.

## Privacy and Vault

Privacy levels are `Normal`, `Protected`, and `Hidden`. Normal entries are plaintext. Protected and Hidden entries use the same Vault encryption mechanism. While locked, Protected entries appear as classified placeholders; Hidden entries do not appear. Both display normally when unlocked.

Protected Logs expose their Log date publicly. Protected Memo and Todo entries expose existence only, not titles or other sensitive metadata. Public metadata must never disclose private titles, locations, timezones, tags, or logical directories. Privacy cannot be changed after persistent creation in the MVP.

Vault stores encrypted entries as independent binary files named `<uuid>.enc`, under flat `logs/`, `memos/`, `todos/`, and `trash/` directories. Logical directories are represented in encrypted metadata rather than physical Vault directories. Encrypted files use a versioned authenticated-encryption envelope containing magic, format version, algorithm and key identifiers, nonce, and authenticated ciphertext. The decrypted payload has its own schema version.

The planned cryptography is libsodium XChaCha20-Poly1305, with a random master key protected by a password-derived Argon2id key. Exact parameters and key lifecycle remain to be designed. Do not treat a GitHub Private repository as end-to-end encryption.

## Indexes

Normal storage has a complete plaintext metadata index. Vault has a complete encrypted private metadata index and a minimal plaintext public index. Full indexes contain the metadata required for listing, filtering, and search, excluding body content and subtask text. The public index contains entry UUID, type, safe relative locator, visibility/privacy, and a Log date only for Protected Logs.

Indexes are versioned and may expand in later formats. They are rebuildable from entry files, which always remain authoritative. Detect inconsistencies and enter an explicit recovery state; never silently trust an inconsistent index. Support user-triggered verification and checks triggered by conditions, without requiring periodic full scans.

## Write Safety, Transactions, and Trash

Use atomic replacement for individual-file writes, with appropriate durability measures. Expand a batch operation into its complete plan before executing it. Normal and Vault each have a recovery journal; corresponding journals share a transaction ID. Both journals must be durably prepared before any data modification. Record each completed sub-operation and recover interrupted or partially completed transactions.

Recovery journals live under the configurable persistent state root and are distinct from any future Audit Log. Trash is one logical feature with separate Normal and Vault physical storage; Vault Trash remains encrypted.

## Core Model Implications

### Primary Entry Models

Log, Memo, and Todo are Folio's primary entry models. Each represents user-authored content or a task and owns its own entry data. Privacy levels are `Normal`, `Protected`, and `Hidden`; ordinary post-creation privacy mutation is not exposed. Tag rename and deletion update references in Log and Memo.

### Organization and Embedded Data

Directory is organizational metadata referenced optionally by Memo and Todo. It is not a primary entry model and has no Log association. Filesystem layout and `.folio-dir.toml` handling belong to storage code. Subtasks are part of Todo and are not independently indexed or persisted.

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
- Phase 3.1 provides path validation, Normal entry filename generation, and single-file atomic writing in a separate Storage library. It does not load configuration, create Vault storage, or provide repositories, indexes, journals, migrations, or filesystem reconciliation. `Subtask` is currently embedded in `Todo`, consistent with the design.
- `Tag` is a value type, but there is no shared tag rename/delete operation that updates Log and Memo references.
- `DateContext` includes a calendar date, timezone, and optional location; `TimestampContext` includes an absolute timestamp, timezone, and optional location. Privacy rules for these fields in indexes and public metadata remain defined by the Vault/index design.

## Decisions Needed Before the First Storage Milestone

The architecture establishes boundaries and formats but does not yet define enough detail for interoperable persistence. Before implementing the first storage milestone, decide:

- The exact index structures remain to be defined; entry and Directory v1 codec fields are documented above.
- How unknown fields are preserved or handled during future migration; the current Log/Memo v1 decoder rejects them.
- The exact Normal and Vault directory tree, including safe-relative-locator rules beyond the ASCII UUID Directory naming and filename patterns.
- How invalid or duplicate Directory metadata is reconciled during filesystem scanning.
- The transaction journal record format, sub-operation states, recovery decision rules, and platform-specific durability guarantees for atomic replacement.
- What conditions trigger consistency checks and how callers observe/resolve the explicit recovery state.
- The complete public-index visibility vocabulary and rules for each privacy level, including Hidden entries and locked/unlocked views.
- The first migration policy and supported source schema versions.

The exact cryptographic parameters, master-key lifecycle, Vault history handling, and cross-device merge remain deferred designs; they are not assumptions for this storage milestone.
