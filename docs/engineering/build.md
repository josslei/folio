# Build, dependencies and run

**Status:** current CMake workflow. Root README offers a short quick start; this
page owns detailed engineering instructions. Require CMake 3.20+, Git and C/C++20
compilers. macOS uses Xcode Command Line Tools; Linux needs compiler/development
packages; Windows needs a Visual Studio C++ workload or an appropriate toolchain.
Platform implementation is not a claim of native runtime verification.

## Dependencies

FTXUI, GoogleTest, stduuid, libsodium, toml++, msgpack-cxx, CLI11, and spdlog are Git submodules under `thirdparty/`. The project-owned libsodium CMake recipe is in `thirdparty/libsodium-cmake/`. Libsodium supplies UUIDv4 randomness now and is reserved for later cryptographic work; encryption is not implemented. Do not modify vendored sources during normal development or update submodules unless the task calls for it. Keep the recipe aligned with the pinned libsodium version. Justify new dependencies and keep test-only dependencies out of production targets.

Initialize the pinned dependencies in an existing checkout:

```sh
git submodule update --init --recursive
```

Do not update vendored sources or submodule revisions unless the task authorizes
it. The libsodium recipe is project-owned; keep it aligned with the pinned source.
No system-wide libsodium installation is required. Test-only dependencies must not
become production requirements; justify new dependencies.

## Configure and compile

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

## Validation

The [testing guide](testing.md) owns focused/full-suite selection, serialized
sample generation and opt-in resource-limit instructions. The [harness inventory](harness.md)
distinguishes working facilities from unimplemented automation.
