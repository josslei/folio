# Internal File I/O — Implemented

`lib/src/io/` implements data-format-independent file operations in
`folio_shared`. Cross-module filesystem/path interfaces live under
`lib/include/folio/io/`, consumed by Config, Logging and Backend Storage without
exposing `lib/src/`. Atomic-write implementation seams and test hooks remain
private. I/O has no dependency on Core models, Config, Storage or Application.

## Files and interfaces

- `filesystem.hpp/.cpp`: bounded `read_file`, atomic creation/replacement, durable
  directory and empty-file creation, regular-file synchronization and removal.
- `path.hpp/.cpp`: generic NUL checking, native path-component comparison,
  containment and overlap predicates.
- `atomic_write.hpp`: internal atomic-write implementation declaration and sync
  injection seam used by existing fault tests.
- `filesystem_test_hooks.hpp`: scoped-by-caller lifecycle synchronization injection
  for deterministic durability tests; no production override is installed.

Write inputs use `std::span<const std::byte>`. Thin `std::string_view` overloads
use `std::as_bytes` with no copy, allocation, encoding or endian conversion.
Reads return an owning `std::string` containing opaque bytes, preserving existing
codec integration without another allocation or a separate text-reading path.
Embedded NUL and arbitrary byte values are preserved. Parsing, encoding and
cryptography belong to callers. Read callers supply the size cap and confinement
policy; the read primitive does not impose Repository locator rules.

## Atomic and durability contracts

`create_file_atomic` requires an absent target and must not overwrite one.
`replace_file_atomic` requires an existing regular target. Both retain the
existing staging, directory locking, reserved temporary-name convention, symlink
checks and publication/synchronization order. Staging uses a random UUIDv4-shaped
suffix, not a Folio Entry identity. No multi-file transaction is introduced.

`FileError` preserves InvalidPath, IoError, DurabilityFailure and RecoveryRequired
and the original system diagnostic. `replacement_completed` means publication or
removal occurred, not that crash durability was established. Failures after
publication do not roll back the published file. `ReadError` distinguishes I/O
failures and size-limit failures.

Durable lifecycle helpers preserve their existing POSIX parent-directory sync
ordering and Windows limitations. Windows file flushing does not establish the
same parent-directory metadata guarantees as POSIX. This refactor adds no stronger
power-loss claim, repair mechanism or Recovery workflow.

## Ownership and adapters

Config owns TOML/default interpretation and effective-root constraints. Storage
continues to own Entry filenames, locators, discovery, layout and mutation safety.
Generic filesystem operations are called directly through I/O; Storage provides
neither duplicate operation wrappers nor FileError-to-StorageError conversion.
`storage/common.hpp` owns shared Storage types and declares the path-validation
adapter. Config owns effective-path policy; Storage delegates to
`config::validate_effective_paths()` before enforcing Repository layout rules.
Normal Entry filename generation lives with NormalRepository.

Repository loads, discovery and scans use `StorageResult<T>` with distinct success,
`StorageError` and original `io::FileError` alternatives. Mutation failures retain
their stage and a variant of those two error types. Coordinator diagnostics retain
filesystem failures in `file_error` separately from `storage_error`. Publication
flags survive unchanged and still drive inconsistency notification.

Storage errors describe domain/persistence semantics: for example `InvalidLayout`,
`CatalogUnavailable`, `MetadataNotFound` and `PostPersistFailure`. Generic path,
I/O, durability and pending-file recovery errors belong to I/O. Atomic-write tests
inject failures directly into the I/O seam. Reads use `io::read_file`; Repository
reads perform their path checks first.

Dependency direction: Application → Config/Storage; Config → I/O;
Storage → Config/I/O. I/O has no dependency back on these modules. No generic
Infrastructure directory or filesystem interface hierarchy exists.

Phase 4.3a adds `FileError::durability_confirmed` for atomic destination publication
whose required synchronization completed before a later temporary-artifact cleanup
failure. `replacement_completed` remains publication only. Repository errors retain
per-step observations for single-file writes and compound Move; these facts do not
change the atomic publication algorithm or promise durability of unsynchronized
bootstrap ancestors. Application maps evidence rather than inferring outcomes from
error codes. See [mutation evidence](mutations.md).
