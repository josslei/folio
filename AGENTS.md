# Repository Guidelines

## Project Context & Structure

Folio is a personal TUI application written in C++20, built with CMake and FTXUI, with GoogleTest, GoogleMock, and CTest for testing. Log, Memo, Todo, and Directory are its four first-class Entry types. Core provides in-memory domain models and shared types through `folio_shared`. Frontend-only code builds as `folio_frontend`; Backend-only code, including private Storage, builds as `folio_backend`. The single `folio` executable links both roles. Subtasks belong to Todo. The TUI provides a welcome screen with `q` to exit.

- `app/include/` and `app/src/`: Frontend-only interfaces and implementations, including launching/discovery, BackendConnectionHandle and TUI presentation.
- `app/src/main.cpp`: shared process dispatcher; the deliberate consumer of both role entry points.
- `server/include/backend.hpp`: narrow Backend execution entry point.
- `server/src/runtime/` and `server/src/ipc/`: private Backend lifecycle, ownership and connection scheduling.
- `server/src/`: Backend entry-point implementation and private operation handlers/executor.
- `server/include/storage/` and `server/src/storage/`: Backend-private persistence, Catalog, codecs and NormalRepository; never accessible to Frontend implementation.
- `lib/include/folio/` and `lib/src/`: shared Core models, Configuration, Logging, byte-oriented I/O, Application contracts and IPC facilities. Shared code depends on neither app nor server.
- `lib/include/folio/ipc/`: implementation-facing protocol/transport interfaces intentionally shared between modules; not the stable business API.
- `lib/src/core/internal/`, `lib/src/io/internal/` and `lib/src/logging/internal/`: colocated private helpers and test seams.
- `tests/core/` and `tests/storage/`: domain-model and persistence tests, registered through `tests/CMakeLists.txt`.
- `thirdparty/`: pinned dependency submodules.

## Architecture Rules

Read [architecture](docs/architecture.md), [design decisions](docs/design-decisions.md), and the [roadmap](docs/roadmap.md) before architectural work. Detailed planned boundaries live in [Application](docs/application-design.md), [Recovery](docs/recovery-design.md), [Vault](docs/vault-design.md), and [Git Sync](docs/git-sync-design.md). Distinguish Implemented, Decided/Planned, and Open/Deferred; a roadmap does not authorize implementing the next stage. Existing Storage contracts remain in [storage-design.md](docs/storage-design.md).

Preserve Catalog-centric ownership: the future Backend owns Catalog, Coordinator, and Repositories; Coordinator borrows Catalog. `unpersisted` means metadata is not saved to the Index, never that Entries are unsaved. Inconsistent is separate and blocks Index publication. Managed mutations use creation checks, known locators, and matching result callbacks; do not add implicit global scans. Shutdown saving is explicitly called by the lifecycle owner with error handling/retry, not a destructor or once-only guard.

Future direction is one configuration per OS user and one on-demand Backend with multiple Frontends using public Application APIs. Internal StoragePaths separates authoritative roots from explicitly supplied ApplicationPaths (config/data/state/cache/logs/optional runtime). Bootstrap platform resolution and folio_config.toml loading are implemented; tests inject effective paths. Phase 4.1c configuration combines defaults (<application.data>/normal and /vault) with optional absolute root overrides; missing files are atomically created from templates/folio_config.toml, malformed/unknown settings fail. See docs/configuration.md. Loading creates missing configuration only; configuration is fixed for a Backend session; Process Logging uses spdlog; initialize before TOML loading and flush after lifecycle cleanup. See docs/logging.md. Normal Index uses application.cache/index/normal.msgpack; former State indexes are ignored without migration or deletion. Durable Session Marker is implemented at application.state/session.marker. Startup/Refresh cannot establish Ready without it. The future owner must stop work and call finish_session() to save safely and durably clear the Marker; shutdown_save() alone leaves it intact. OS ownership and same-executable process launching are implemented in Phase 4.1a; IPC and Backend lifecycle integration are implemented in 4.1b; public Normal read operations are implemented in Phase 4.2a; Normal mutations and Entry Edit Locks are implemented in Phase 4.3a. See docs/application-access.md. Ownership must precede Catalog startup and outlive explicit session finalization. Never unlink lock targets or equate a spawned process with Ready. Recovery state/drafts must not live in Cache. Vault drafts require encrypt-only Frontend capability without decryption keys. Resolve deferred protocols and cryptography through design review.

An Entry is an independently identifiable persistent Folio object: Log, Memo, Todo, or Directory. Use Entry to describe all four conceptually; terminology alignment must preserve existing program behavior and does not require every API to support all four types. Distinguish a type only when its fields, membership rules, or persistence layout require it; never use “Entry” to implicitly exclude Directory. Record is shorthand for Log, Memo, and Todo only. Use it for genuinely narrower APIs; do not introduce separate behavior for Records and Directories when their behavior is shared. Directory membership is a relationship between Entries, not a domain hierarchy. Keep independent classes without a common Entry base class. Directory has no parent ID: only Memo/Todo single-level organization is supported. Persisted Folio Entries are authoritative; the Catalog and binary Index are rebuildable projections.

Keep dependencies `folio -> folio_frontend / folio_backend -> folio_shared`. Frontend code must not depend on server interfaces; Backend code must not depend on app interfaces. The shared dispatcher is the deliberate exception. TUI uses shared public Core domain APIs. The Application process foundation is private; do not expose Storage headers as public APIs. Core must remain independent of FTXUI and all presentation frameworks, with APIs reusable by a future CLI or alternative frontend. Keep business logic, storage, encryption, and synchronization out of UI components. Prefer clear module boundaries and straightforward solutions; do not create speculative modules, interfaces, placeholder classes, or unnecessary abstractions.

Keep module boundaries explicit: each top-level module owns its `include/` declarations and `src/` implementations. Core declarations remain in `lib/include/folio/core/`; Storage declarations stay under `server/include/storage/` and implementations under `server/src/storage/`, both Backend-private. No production target receives another module's complete `src/` include path. Shared I/O and protocol interfaces used across module boundaries live in `lib/include/folio/`; private atomic/test hooks remain under module-local internal/ directories beneath `lib/src/`. These implementation-facing headers are not public business operations.

Use `folio::core` for domain models, `folio::core::storage` for persistence, `folio::core::storage::codec` for serialization, and `folio::core::storage::internal` for Storage implementation/testing helpers. Storage remains an internal subsystem of `folio_backend`, not a separate library. Each target exports only its own include directory and shared dependency requirements; source paths remain PRIVATE, except explicit test-only access. Keep implementation-only dependencies PRIVATE where possible.

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

FTXUI, GoogleTest, stduuid, libsodium, toml++, msgpack-cxx, CLI11, and spdlog are Git submodules under `thirdparty/`. The project-owned libsodium CMake recipe is in `thirdparty/libsodium-cmake/`. Libsodium supplies UUIDv4 randomness now and is reserved for later cryptographic work; encryption is not implemented. Do not modify vendored sources during normal development or update submodules unless the task calls for it. Keep the recipe aligned with the pinned libsodium version. Justify new dependencies and keep test-only dependencies out of production targets.

## Build, Run & Test

Require CMake 3.20+, Git, and C and C++20 compilers. Initialize dependencies with `git submodule update --init --recursive`.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build --config Debug --parallel
ctest --test-dir build -C Debug --parallel --output-on-failure

cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build build-release --config Release --parallel
```

Pass `--parallel` to let CMake use available CPU cores; use `--parallel N` to cap concurrent jobs.
Run CTest with `--parallel` for automatic concurrency (requires CTest 3.29+). Older CTest versions require `--parallel N`. Use an explicit lower count if memory pressure warrants it, especially for resource-limit tests.
Use parallel incremental builds for routine edits. Run focused tests during iteration and the full suite when required by the task or the change's scope; documentation-only edits need no compilation or test run. Run the opt-in resource-limit suite separately with `--parallel 1`.

Single-configuration generators use `CMAKE_BUILD_TYPE`. Visual Studio and Xcode ignore it: select builds with `--config Debug` or `--config Release`, and tests with `-C Debug` or `-C Release`.

Run `./build/folio` on Unix-like systems with a single-configuration build, `./build/Debug/folio` with Xcode, or `build\Debug\folio.exe` with Visual Studio on Windows. Adjust paths for the chosen generator and configuration; press `q` to exit.

## Git & Documentation

Use concise imperative commit subjects; do not create commits unless requested. Preserve existing user changes and do not perform destructive Git operations without explicit authorization. Exclude generated build output from commits. Update README or relevant documentation when build instructions, architecture, or visible behavior materially change. Keep extensive implementation documentation out of this file. Pull requests should summarize changes and actual verification, with terminal output or screenshots where useful for TUI changes.

Runtime-role ownership supersedes the earlier Phase 4.1a.5 single-library layout. Keep one executable and three role-aligned static libraries: shared, Frontend and Backend. Only the dispatcher links both role entry points; linking them into one binary does not authorize bypassing IPC. Preserve pending runtime work and existing Git staging during structural refactors.

Headers in each module's include/ tree have corresponding module-relative sources where implementations exist. include/ does not imply public visibility: only lib/include is shared, while app/include and server/include remain role-private. Implementation details intentionally colocated with sources belong under internal/. Header-only declarations/templates need no empty source file. Keep ordinary headers out of src/; internal/ is the explicit exception for colocated implementation details and test seams. Use subdirectories for real responsibilities (Core, IPC, Storage, runtime, TUI), not generic application/ wrappers inside app/ or server/. Shared Application contracts remain under lib/application; shared IPC interfaces/implementations live under lib/include/folio/ipc and lib/src/ipc.

Launching/discovery, handshake and BackendConnectionHandle are reusable across TUI/CLI/future GUI but are Frontend-only, so belong under app/. Frontends use launch_backend(); generic launch_process() and current_executable() remain private app OS helpers. The Backend run_backend() entry point belongs under server/. Shared Application contracts do not own either runtime role.

Shared configuration APIs belong in lib/include/folio/config/ with matching implementations in lib/src/config/. Public configuration headers must not depend on private Storage or IPC interfaces.

Low-level file operations are implemented in lib/src/io/, with cross-module filesystem/path interfaces in lib/include/folio/io/. Atomic creation and replacement remain distinct. Writes use byte spans with zero-copy string-view overloads; reads return owning opaque bytes. Config owns effective-path policy; Storage results preserve distinct Storage and I/O errors, including publication flags; shared declarations live in storage/common.hpp. See docs/io.md. Tests inject configuration inputs explicitly; do not restore global configuration overrides.

Process diagnostics use the single default spdlog logger (`folio/logging/logging.hpp`).
Use source-aware SPDLOG macros at handling boundaries, without changing error
propagation or lifecycle behavior. Never log raw Entry/IPC payloads, parser error
text, secrets, or arbitrary exception messages. Platform log paths require no
TOML loading. Tests inject log roots; no rotation/retention is implemented.

Shared locator-free metadata types live in `lib/include/folio/core/metadata.hpp`. Storage uses private `Located<Metadata>` composition for locator information; Application returns the Core metadata values. Do not duplicate metadata field definitions in Application or expose private locators.

Backend IPC workers own connections and OS readiness waits; one shared bounded
RequestScheduler/executor serializes all accepted Application Operations. Workers
must not access live Catalog/Repository state. Accepted is session-only admission,
not a final result or a durability promise. Disconnecting does not cancel accepted
work. Stop admission, drain accepted work and join workers before explicit Catalog
finalization; retain RepositoryOwnership until afterward. Public methods remain
blocking and must eventually run off the UI thread. See docs/ipc-protocol.md.

Mutation identity, editing data and result contracts live in
lib/include/folio/application/mutation.hpp.
Backend MutationOrchestrator handlers and the weak connection-owned edit-lock registry
run only on the sequential executor. Resolve current Catalog locators, verify
Ready and lock/membership preconditions before writes, and reconstruct Update
Snapshots from authoritative state plus Full Editable Fields. Move owns membership
and location; Frontend Modification Context is shared by Update/Move, never a
version token. Backend generates Entry UUIDs; Frontend generates MutationId.
Accepted actions retain their connection session token after disconnect, so lock
ownership expires only when the worker and accepted actions release it. Immediate
Rename/Move/status changes reject another owner's lock but need no temporary lock
on unlocked Entries. Preserve structured publication/durability evidence; never
infer NotApplied from lost confirmation. See docs/application-mutations.md.

Backend operation handling and scheduling declarations live under server/include/operations/
with matching server/src/operations/ implementations. IPC owns the Backend-private
connection session token under server/include/ipc/. Access operations and mutation
orchestration are peers; the executor callback routes between them.

AccessOrchestrator and MutationOrchestrator are concrete executor-only peers under
server/include/operations/ with matching sources; both borrow Storage dependencies.
AccessOrchestrator handles read-only operations without owning mutable editing state.
