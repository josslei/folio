# Repository Guidelines

## Project Context & Structure

Folio is a personal TUI application written in C++20, built with CMake and FTXUI, with GoogleTest, GoogleMock, and CTest for testing. Log, Memo, and Todo are its three primary Core structures. Core provides in-memory domain models and shared types, while the same `folio_core` target contains a private Storage subsystem for codecs and Normal persistence. Subtasks belong to Todo. The TUI provides a welcome screen with `q` to exit.

- `include/folio/core/`: public domain-model headers available to frontends.
- `src/core/`: domain implementations and `src/core/internal/`: private domain helpers.
- `src/storage/`: private persistence implementation, including codecs, filesystem operations, and the Normal Repository. Storage depends on Core models; Core models do not depend on Storage.
- `src/tui/`: terminal presentation and startup code for `folio`.
- `tests/core/` and `tests/storage/`: domain-model and persistence tests, registered through `tests/CMakeLists.txt`.
- `thirdparty/`: pinned dependency submodules.

## Architecture Rules

Keep dependency direction `folio (TUI) -> folio_core`. Core must remain independent of FTXUI and all presentation frameworks, with APIs reusable by a future CLI or alternative frontend. Keep business logic, storage, encryption, and synchronization out of UI components. Prefer clear module boundaries and straightforward solutions; do not create speculative modules, interfaces, placeholder classes, or unnecessary abstractions.

Keep the Core library boundary explicit. Public domain declarations belong under `include/folio/core/`. Internal declarations belong alongside their implementations under `src/`, including Storage headers under `src/storage/`; do not expose implementation directories through `folio_core`'s public include paths or include internal headers from public headers. The TUI links `folio_core` for domain APIs and must not include or call Storage internals directly.

Use `folio::core` for domain models, `folio::core::storage` for persistence, `folio::core::storage::codec` for serialization, and `folio::core::storage::internal` for Atomic Write implementation/testing helpers. Keep one `folio_core` library target: Storage is an internal subsystem in that target, not a separate library. Keep `include/` PUBLIC and source implementation paths PRIVATE; expose implementation headers to tests only through explicit test-target include paths. Keep implementation-only dependencies PRIVATE where possible.

Call `folio::core::ensure_sodium_initialized()` before using libsodium in Core code. `Id::generate()` already calls this shared one-time helper.

## Development Workflow

Inspect relevant code and conventions before editing. Keep changes focused on the request; avoid unrelated features and refactoring. For nontrivial work, briefly explain the approach before implementation. Resolve ordinary implementation decisions using existing conventions without requiring routine approval. Preserve backward compatibility where applicable unless a breaking change is requested. Report what changed, what was actually verified, and any verification limitations.

## Coding Standards

Use standard C++20 without compiler extensions. Follow existing two-space indentation, `snake_case` functions and files, `PascalCase` test suites, and `folio::core` namespaces. No formatter or linter is configured.

Prefer RAII, standard library facilities, explicit ownership and lifetimes, and composition over unnecessary inheritance. Avoid raw owning pointers. Keep public interfaces minimal and implementation details out of public headers where practical. Use English identifiers, comments, diagnostics, and error messages. Keep CMake target-based, with explicit dependency relationships and appropriate visibility.

## Testing Standards

Use GoogleTest for unit and integration tests, GoogleMock where mocking provides clear value, and CTest as the unified runner. Register tests using `gtest_discover_tests()` in `tests/CMakeLists.txt`; use descriptive suites and filenames such as `core_smoke_test.cpp`.

Add or update tests for meaningful behavior changes and regression tests for confirmed bug fixes where practical. Keep unit tests deterministic, isolated, and independent of terminals, networks, GitHub credentials, and real user environments. Use temporary directories and synthetic data for filesystem tests. Report results accurately, including tests that could not run. Cosmetic and documentation changes do not require new tests.

## Security & Data Safety

Never commit passwords, private keys, tokens, or real personal records. Use synthetic data in tests and examples; never log sensitive record contents or cryptographic secrets. Do not implement custom cryptographic algorithms; use established libraries when encryption is designed. Treat encryption, key handling, and privacy boundaries as security-critical. Avoid silent data loss and destructive overwrites; preserve user data during migrations and synchronization. GitHub Private repositories do not provide end-to-end encryption.

## Dependencies

FTXUI, GoogleTest, stduuid, and libsodium are Git submodules under `thirdparty/`. The project-owned libsodium CMake recipe is in `thirdparty/libsodium-cmake/`. Libsodium supplies UUIDv4 randomness now and is reserved for later cryptographic work; encryption is not implemented. Do not modify vendored sources during normal development or update submodules unless the task calls for it. Keep the recipe aligned with the pinned libsodium version. Justify new dependencies and keep test-only dependencies out of production targets.

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
