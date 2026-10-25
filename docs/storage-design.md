# Storage & Persistence Architecture v0.1

This document records the agreed direction for Folio persistence. It is a design specification, not an implementation. Current Core/API differences and decisions still needed before implementation are listed at the end.

## Principles and Boundaries

- Folio uses independent file-based persistence; there is no database.
- Entry files are authoritative. Indexes are derived, rebuildable data.
- Every persistent structured format has a version. Newer Folio versions may migrate older formats; older versions need not read newer formats.
- Storage, Vault, and future Git synchronization are separate concerns. Synchronization algorithms are outside this design phase.
- Persistent roots, `state_root`, and `cache_root` are configurable, with platform-appropriate defaults. The state root is separate from Git-tracked data roots.

## Normal Data Formats and Layout

Normal entries use `logs/`, `memos/`, and `todos/`, plus a logical Trash. Logs stay flat. Memo and Todo may use one level of physical user directories. Directory metadata is TOML. A Directory has a stable UUID; its display name is taken from the filesystem. Its metadata file is `.folio-dir.toml`.

Normal Logs and Memos are Markdown files with TOML front matter delimited by `+++`. Normal Todos and directory metadata are TOML. Use toml++; if adopted, pin it as a Git submodule under `thirdparty/`. Every structured file starts with `schema_version = 1`.

Normal filenames are `YYYY-MM-DD_HHMMSS_<uuid>.<ext>`, based on UTC creation time and ASCII characters only. Filenames do not change when a title or Log date changes. Ordering among entries created in the same second is unspecified.

## Directory Reconciliation

Memo and Todo support one physical directory level; Log does not. External filesystem changes are validated and reconciled. Storage must not silently overwrite data when it finds unexpected or conflicting filesystem state.

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

- `Directory` is a lightweight Core concept associated with Memo and Todo; filesystem layout and `.folio-dir.toml` handling belong to future storage code.
- Change `Privacy` to `Normal`, `Protected`, and `Hidden`.
- Add static restore factories that reconstruct stored IDs, creation and modification metadata, and all other persisted fields.
- Do not expose ordinary post-creation privacy mutation.
- Tag rename and deletion update references in Log and Memo.
- Subtasks remain embedded in their parent Todo and are not independently indexed or persisted.

These are intended Core adjustments, not claims about the current API. Storage, Vault, and sync behavior must not be placed in TUI components.

## Git Boundaries

Use one Git repository with separate Normal and Vault branches checked out through Git worktrees. Vault history compaction and cross-device semantic merge belong to a later Sync subsystem; this phase does not specify or implement those algorithms.

## Current Core Discrepancies

The public headers under `include/folio/core/` currently describe in-memory models and differ from this design in these ways:

- `types.hpp` defines `Privacy { Normal, Private }`; it has no `Protected` or `Hidden` values.
- `Memo` and `Todo` now store optional Directory IDs; `Log` has no Directory field. Directory remains an in-memory logical model and has no filesystem access or `.folio-dir.toml` behavior.
- Time data is represented by `TimestampContext` and `DateContext`; the design has not yet specified their serialized field names or exact TOML representation.
- `Log`, `Memo`, and `Todo` expose privacy setters, and there are no static restore factories. Their constructors create new IDs and initialize creation/modification metadata rather than restoring stored state.
- No storage, Vault, index, journal, migration, or filesystem reconciliation API is currently present. `Subtask` is currently embedded in `Todo`, consistent with the design.
- `Tag` is a value type, but there is no shared tag rename/delete operation that updates Log and Memo references.
- `DateContext` includes a calendar date, timezone, and optional location; `TimestampContext` includes an absolute timestamp, timezone, and optional location. The API therefore carries the relevant context, but the design still needs to define storage and privacy rules for each field.

## Decisions Needed Before the First Storage Milestone

The architecture establishes boundaries and formats but does not yet define enough detail for interoperable persistence. Before implementing the first storage milestone, decide:

- The exact serialized fields and types for Log, Memo, Todo, Directory metadata, and each index, including date/time encoding and timezone representation.
- The Markdown front-matter/body boundary, TOML escaping/encoding rules, and how unknown fields are preserved or handled during migration.
- The exact Normal and Vault directory tree, including physical naming and safe-relative-locator rules beyond the filename pattern and flat Vault top-level directories.
- How a stable Directory UUID is associated with a physical Normal directory when its filesystem name changes, and how invalid or duplicate metadata is reconciled.
- The transaction journal record format, sub-operation states, recovery decision rules, and platform-specific durability guarantees for atomic replacement.
- What conditions trigger consistency checks and how callers observe/resolve the explicit recovery state.
- The complete public-index visibility vocabulary and rules for each privacy level, including Hidden entries and locked/unlocked views.
- The first migration policy and supported source schema versions.

The exact cryptographic parameters, master-key lifecycle, Vault history handling, and cross-device merge remain deferred designs; they are not assumptions for this storage milestone.
