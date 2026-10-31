# Repository Guidelines

## Project Context & Structure

Folio is a personal TUI application written in C++20, built with CMake and FTXUI, with GoogleTest, GoogleMock, and CTest for testing. Log, Memo, Todo, and Directory are its four first-class Entry types. Core provides in-memory domain models and shared types, while the same `folio_backend` target contains a private Storage subsystem for codecs and Normal persistence. Subtasks belong to Todo. The TUI provides a welcome screen with `q` to exit.

- `lib/include/folio/core/`: public domain-model headers available to frontends.
- `lib/src/core/`: domain implementations and `lib/src/core/internal/`: private domain helpers.
- `lib/src/storage/`: private persistence implementation, including codecs, filesystem operations, and the Normal Repository. Storage depends on Core models; Core models do not depend on Storage.
- `app/src/main.cpp`: process-mode dispatch; `app/src/tui/`: terminal presentation.
- `lib/include/folio/application/` and `lib/src/application/`: shared Backend launch and connection functionality for all Frontends; CRUD/Query APIs remain deferred.
- `lib/src/application/internal/`: private OS process mechanisms, Backend ownership, and lifecycle implementation.
- `tests/core/` and `tests/storage/`: domain-model and persistence tests, registered through `tests/CMakeLists.txt`.
- `thirdparty/`: pinned dependency submodules.

## Architecture Rules

Read [architecture](docs/architecture.md), [design decisions](docs/design-decisions.md), and the [roadmap](docs/roadmap.md) before architectural work. Detailed planned boundaries live in [Application](docs/application-design.md), [Recovery](docs/recovery-design.md), [Vault](docs/vault-design.md), and [Git Sync](docs/git-sync-design.md). Distinguish Implemented, Decided/Planned, and Open/Deferred; a roadmap does not authorize implementing the next stage. Existing Storage contracts remain in [storage-design.md](docs/storage-design.md).

Preserve Catalog-centric ownership: the future Backend owns Catalog, Coordinator, and Repositories; Coordinator borrows Catalog. `unpersisted` means metadata is not saved to the Index, never that Entries are unsaved. Inconsistent is separate and blocks Index publication. Managed mutations use creation checks, known locators, and matching result callbacks; do not add implicit global scans. Shutdown saving is explicitly called by the lifecycle owner with error handling/retry, not a destructor or once-only guard.

Future direction is one configuration per OS user and one on-demand Backend with multiple Frontends using public Application APIs. Internal StoragePaths separates authoritative roots from explicitly supplied ApplicationPaths (config/data/state/cache/optional runtime). Bootstrap platform resolution and read-only config.toml loading are implemented; tests inject effective paths. Normal Index uses application.cache/index/normal.msgpack; former State indexes are ignored without migration or deletion. Durable Session Marker is implemented at application.state/session.marker. Startup/Refresh cannot establish Ready without it. The future owner must stop work and call finish_session() to save safely and durably clear the Marker; shutdown_save() alone leaves it intact. OS ownership and same-executable process launching are implemented in Phase 4.1a; IPC and Backend lifecycle integration are implemented in 4.1b; public Application operations remain deferred. Ownership must precede Catalog startup and outlive explicit session finalization. Never unlink lock targets or equate a spawned process with Ready. Recovery state/drafts must not live in Cache. Vault drafts require encrypt-only Frontend capability without decryption keys. Resolve deferred protocols and cryptography through design review.

An Entry is an independently identifiable persistent Folio object: Log, Memo, Todo, or Directory. Use Entry to describe all four conceptually; terminology alignment must preserve existing program behavior and does not require every API to support all four types. Distinguish a type only when its fields, membership rules, or persistence layout require it; never use “Entry” to implicitly exclude Directory. Record is shorthand for Log, Memo, and Todo only. Use it for genuinely narrower APIs; do not introduce separate behavior for Records and Directories when their behavior is shared. Directory membership is a relationship between Entries, not a domain hierarchy. Keep independent classes without a common Entry base class. Directory has no parent ID: only Memo/Todo single-level organization is supported. Persisted Folio Entries are authoritative; the Catalog and binary Index are rebuildable projections.

Keep dependency direction `folio -> folio_backend`, with TUI also using public Core domain APIs. The Application process foundation is private; do not expose Storage headers as public APIs. Core must remain independent of FTXUI and all presentation frameworks, with APIs reusable by a future CLI or alternative frontend. Keep business logic, storage, encryption, and synchronization out of UI components. Prefer clear module boundaries and straightforward solutions; do not create speculative modules, interfaces, placeholder classes, or unnecessary abstractions.

Keep the Core library boundary explicit. Public domain declarations belong under `lib/include/folio/core/`. Internal declarations belong alongside their implementations under `lib/src/`, including Storage headers under `lib/src/storage/`; do not expose implementation directories through `folio_backend`'s public include paths or include internal headers from public headers. The TUI links `folio_backend` for domain APIs and must not include or call Storage internals directly.

Use `folio::core` for domain models, `folio::core::storage` for persistence, `folio::core::storage::codec` for serialization, and `folio::core::storage::internal` for Atomic Write implementation/testing helpers. Keep one `folio_backend` library target: Storage is an internal subsystem in that target, not a separate library. Keep `lib/include/` PUBLIC and source implementation paths PRIVATE; expose implementation headers to tests only through explicit test-target include paths. Keep implementation-only dependencies PRIVATE where possible.

Call `folio::core::ensure_sodium_initialized()` before using libsodium in Core code. `Id::generate()` already calls this shared one-time helper.

## Development Workflow

Inspect relevant code and conventions before editing. Keep changes focused on the request; avoid unrelated features and refactoring. For nontrivial work, briefly explain the approach before implementation. Resolve ordinary implementation decisions using existing conventions without requiring routine approval. Preserve backward compatibility where applicable unless a breaking change is requested. Report what changed, what was actually verified, and any verification limitations.

## Coding Standards

Use standard C++20 without compiler extensions. Follow existing two-space indentation, `snake_case` functions and files, `PascalCase` test suites, and `folio::core` namespaces. No formatter or linter is configured.

Prefer RAII, standard library facilities, explicit ownership and lifetimes, and composition over unnecessary inheritance. Avoid raw owning pointers. Keep public interfaces minimal and implementation details out of public headers where practical. Use English identifiers, comments, diagnostics, and error messages. Keep CMake target-based, with explicit dependency relationships and appropriate visibility.

## Testing Standards

Use GoogleTest for unit and integration tests, GoogleMock where mocking provides clear value, and CTest as the unified runner. Register tests using `gtest_discover_tests()` in `tests/CMakeLists.txt`; use descriptive suites and filenames such as `core_smoke_test.cpp`.

Keep resource-heavy boundary tests under `tests/resource_limits/`, behind the default-off `FOLIO_BUILD_RESOURCE_LIMIT_TESTS` option and the CTest `resource_limits` label. Do not allocate or repeatedly read large boundary fixtures in routine unit/integration tests. Keep small malformed-input and bounds checks in the normal suite; resource-limit tests verify correctness rather than benchmark performance.

Add or update tests for meaningful behavior changes and regression tests for confirmed bug fixes where practical. Keep unit tests deterministic, isolated, and independent of terminals, networks, GitHub credentials, and real user environments. Use temporary directories and synthetic data for filesystem tests. Report results accurately, including tests that could not run. Cosmetic and documentation changes do not require new tests.

## Security & Data Safety

Never commit passwords, private keys, tokens, or real personal records. Use synthetic data in tests and examples; never log sensitive record contents or cryptographic secrets. Do not implement custom cryptographic algorithms; use established libraries when encryption is designed. Treat encryption, key handling, and privacy boundaries as security-critical. Avoid silent data loss and destructive overwrites; preserve user data during migrations and synchronization. GitHub Private repositories do not provide end-to-end encryption.

## Dependencies

FTXUI, GoogleTest, stduuid, libsodium, toml++, msgpack-cxx, and CLI11 are Git submodules under `thirdparty/`. The project-owned libsodium CMake recipe is in `thirdparty/libsodium-cmake/`. Libsodium supplies UUIDv4 randomness now and is reserved for later cryptographic work; encryption is not implemented. Do not modify vendored sources during normal development or update submodules unless the task calls for it. Keep the recipe aligned with the pinned libsodium version. Justify new dependencies and keep test-only dependencies out of production targets.

## Build, Run & Test

Require CMake 3.20+, Git, and C and C++20 compilers. Initialize dependencies with `git submodule update --init --recursive`.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build --config Debug --parallel
ctest --test-dir build -C Debug --output-on-failure

cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build build-release --config Release --parallel
```

Pass `--parallel` to let CMake use available CPU cores; use `--parallel N` to cap concurrent jobs.

Single-configuration generators use `CMAKE_BUILD_TYPE`. Visual Studio and Xcode ignore it: select builds with `--config Debug` or `--config Release`, and tests with `-C Debug` or `-C Release`.

Run `./build/folio` on Unix-like systems with a single-configuration build, `./build/Debug/folio` with Xcode, or `build\Debug\folio.exe` with Visual Studio on Windows. Adjust paths for the chosen generator and configuration; press `q` to exit.

## Git & Documentation

Use concise imperative commit subjects; do not create commits unless requested. Preserve existing user changes and do not perform destructive Git operations without explicit authorization. Exclude generated build output from commits. Update README or relevant documentation when build instructions, architecture, or visible behavior materially change. Keep extensive implementation documentation out of this file. Pull requests should summarize changes and actual verification, with terminal output or screenshots where useful for TUI changes.

Phase 4.1a.5 uses one static folio_backend target and the folio executable. The executable receives app/src and lib/include only; lib/src is private and explicitly available to internal tests. The public Backend dispatch header exposes no Storage types. Phase 4.1b is stashed separately pending review; preserve it when restoring after the structural commit.

Public Application headers under lib/include/folio/application have corresponding implementations at lib/src/application/<name>.cpp. Header-only declarations/templates need no empty source file.

Private declarations in modules with public APIs belong in module-local internal/ directories (for example application/internal and core/internal). Storage is entirely private and lives directly under lib/src/storage/ without an additional internal/ directory. Sources implementing public headers retain matching module-relative paths.

Frontend-independent Backend launching, discovery, connection and handshake logic belongs in the Application library, not app/. Use BackendConnectionHandle for the shared connection abstraction. Frontends use launch_backend(); generic launch_process() and current_executable() remain private OS helpers.
