# Folio

Folio is a cross-platform personal logging application with a terminal user interface. Its core library is independent of the TUI so other frontends can reuse it. Log, Memo, Todo, and Directory are four first-class Folio Entries with independent Core models. Directory organizes Memo or Todo Entries within one level; Subtask belongs to Todo, and Tag is a shared value type.

## Project layout

- `app/`: Frontend interfaces and implementation (TUI, Backend connection and launching).
- `server/`: Backend entry point, lifecycle, IPC serving, operation execution and private Storage.
- `lib/`: shared Core, Configuration, Logging, I/O, Application contracts and IPC facilities.
- `tests/`: isolated unit/integration tests, with opt-in resource-limit coverage.

Each code module owns its `include/` headers and `src/` implementations. Only lib/include is shared; app/include and server/include remain role-private. The shared dispatcher is
`app/src/main.cpp`. One executable links the `folio_frontend` and `folio_backend`
static libraries, which both depend on `folio_shared`; runtime Frontends still
communicate with the separate Backend process through IPC.

## Technology

- C++20 and CMake 3.20 or newer
- FTXUI for the terminal interface
- GoogleTest and GoogleMock for tests
- stduuid for UUIDv4 identifiers
- libsodium for UUIDv4 randomness and future cryptographic work
- Git submodules for dependencies

## Requirements

Install CMake, Git, and C and C++20 compilers. On Windows, use a Visual Studio installation with the C++ workload; on macOS, install Xcode Command Line Tools; on Linux, install C and C++ compilers and their development packages. The terminal must support interactive input for the application.

## Checkout

Clone the repository and initialize its submodules:

```sh
git clone --recurse-submodules <repository-url>
cd folio
```

For an existing checkout, run `git submodule update --init --recursive`.

The pinned libsodium source is built from `thirdparty/libsodium` using Folio's CMake recipe in `thirdparty/libsodium-cmake/`; no system-wide libsodium installation is required.

Core initializes libsodium on first use through `folio::core::ensure_sodium_initialized()`. `Id::generate()` calls this helper before requesting random bytes; other Core code can reuse it when needed.

## Configure and build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --config Debug --parallel
```

Multi-configuration generators such as Visual Studio and Xcode select the configuration at build time; `CMAKE_BUILD_TYPE` is ignored by those generators.

## Run

```sh
./build/folio
```

On a multi-configuration Windows generator, run `build\Debug\folio.exe` (or the selected configuration directory). Press `q` to exit.

## Tests

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build --config Debug --parallel
ctest --test-dir build -C Debug --parallel --output-on-failure
```

See the [testing guide](docs/engineering/testing.md) for focused tests, parallelism,
serialized samples and the separately run opt-in resource-limit suite. Detailed
[build and dependency instructions](docs/engineering/build.md) cover Release,
multi-configuration generators and platform paths.

## Architecture and development status

Core, Normal Storage, Backend/IPC, public Normal Access/Mutation operations and Entry Edit Locks are implemented. The executable currently shows a welcome screen; complete editing integration, persistent Recovery, Vault encryption and Git Sync remain deferred.

Start with the [documentation index](docs/README.md), [compact architectural baseline](docs/architecture/overview.md), [roadmap](docs/development/roadmap.md), and [decision register](docs/architecture/decisions.md). See [Storage contracts](docs/contracts/storage.md) for current persistence details, and [Application lifecycle](docs/contracts/backend-lifecycle.md) plus future [Recovery](docs/development/plans/recovery.md), [Vault](docs/development/plans/vault.md), and [Git Sync](docs/development/plans/git-sync.md) designs for future boundaries.
