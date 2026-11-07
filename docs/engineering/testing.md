# Testing and verification

**Status:** current test workflow. [Build prerequisites](build.md) and the actual
[CTest registration](../../tests/CMakeLists.txt) are the implementation entry points.

## Test design and isolation

Use GoogleTest for unit and integration tests, GoogleMock where mocking provides clear value, and CTest as the unified runner. Register tests using `gtest_discover_tests()` in `tests/CMakeLists.txt`; use descriptive suites and filenames such as `core_smoke_test.cpp`.

Keep resource-heavy boundary tests under `tests/resource_limits/`, behind the default-off `FOLIO_BUILD_RESOURCE_LIMIT_TESTS` option and the CTest `resource_limits` label. Do not allocate or repeatedly read large boundary fixtures in routine unit/integration tests. Keep small malformed-input and bounds checks in the normal suite; resource-limit tests verify correctness rather than benchmark performance.

Add or update tests for meaningful behavior changes and regression tests for confirmed bug fixes where practical. Keep unit tests deterministic, isolated, and independent of terminals, networks, GitHub credentials, and real user environments. Use temporary directories and synthetic data for filesystem tests. Report results accurately, including tests that could not run. Cosmetic and documentation changes do not require new tests.

Use appropriate existing fault-injection seams for filesystem/lifecycle failures.
Exercise observable contracts and actual failure boundaries; do not weaken tests
or assert speculative future behavior to make a refactor pass. Keep heavy boundary
allocation in the opt-in suite. Source comments and historical reports are not a
substitute for executing the required checks.

## Commands and concurrency

Testing is enabled by default through CTest. Configure, build, and run:

```sh
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build --config Debug --parallel
ctest --test-dir build -C Debug --parallel --output-on-failure
```

Resource-heavy boundary tests live separately under `tests/resource_limits/` and
are excluded from the default build and CTest suite. Enable and run them explicitly:

```sh
cmake -S . -B build-resource-limits -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON -DFOLIO_BUILD_RESOURCE_LIMIT_TESTS=ON
cmake --build build-resource-limits --config Debug --target folio_resource_limit_tests --parallel
ctest --test-dir build-resource-limits -C Debug -L resource_limits --parallel 1 --output-on-failure
```

These check correctness at large input/resource boundaries, rather than benchmark
execution speed. Small malformed-input and bounds checks remain in the regular suite.
When the option is enabled, unfiltered CTest runs also include the resource-limit tests.

To produce a Release build without tests:

```sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build build-release --config Release --parallel
```

`--parallel` lets CMake use available CPU cores. To limit the number of concurrent build jobs, pass a number, such as `--parallel 4`.

Run CTest with `--parallel` to select concurrency automatically (requires CTest 3.29+). Older CTest versions require `--parallel N`. If concurrent tests consume too much memory, especially resource-limit tests, use an explicit lower count.

Use `-G "Visual Studio 17 2022"` or `-G Xcode` when selecting those generators. For single-configuration generators, set `CMAKE_BUILD_TYPE` to `Debug` or `Release`. The GoogleTest runtime is configured to match the MSVC runtime on Windows.

### Serialized samples

Generate serialized examples of all supported objects for visual review:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build --config Debug --parallel
ctest --test-dir build -C Debug -R CoreCodecSamplesTest --parallel --output-on-failure
```

Inspect `build/serialization-samples/Debug/`: `log.md`, `memo.md`, `todo-date.toml`, `todo-datetime.toml`, `todo-minimal.toml`, `directory.toml`, and `common-values.toml`. The test regenerates and replaces these files using the codecs; they contain synthetic data and are generated output rather than fixtures or manual templates. `Protected` and `Hidden` are metadata values in these plaintext samples; no encryption is performed.

For Release, configure and build with tests enabled in Release and use `-C Release`; output goes to `<build-directory>/serialization-samples/Release/`.

## Selecting validation

Use parallel incremental builds and focused tests during iteration. Run the full
relevant suite when required by task scope, a safety-sensitive change, or an explicit
instruction. Documentation-only edits require link/consistency review rather than
compilation. Resource-limit tests run separately with --parallel 1. If enabled in
a build used for normal tests, exclude their label from the parallel normal run:

```sh
ctest --test-dir build -C Debug -LE resource_limits --parallel --output-on-failure
```

Report what actually ran, failures and limitations, including build configuration
and OS. Native process/IPC/security/shutdown verification is platform-specific;
macOS results or cross-compilation do not prove Linux/Windows runtime behavior.
See [platform policy](../development/status.md#platform-release-verification-policy).
Historical counts belong to scoped [reports](../development/status.md#verification-evidence),
not permanently maintained guarantees in contracts.
