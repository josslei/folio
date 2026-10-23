# Folio

Folio is a cross-platform personal logging application with a terminal user interface. Its core library is independent of the TUI so other frontends can reuse it.

## Technology

- C++20 and CMake 3.20 or newer
- FTXUI for the terminal interface
- GoogleTest and GoogleMock for tests
- Git submodules for dependencies

## Requirements

Install CMake, Git, and a C++20 compiler. On Windows, use a Visual Studio installation with the C++ workload; on macOS, install Xcode Command Line Tools; on Linux, install a C++ compiler and its standard library development package. The terminal must support interactive input for the application.

## Checkout

Clone the repository and initialize its submodules:

```sh
git clone --recurse-submodules <repository-url>
cd folio
```

For an existing checkout, run `git submodule update --init --recursive`.

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

Testing is enabled by default through CTest. Configure, build, and run:

```sh
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build --config Debug --parallel
ctest --test-dir build -C Debug --output-on-failure
```

To produce a Release build without tests:

```sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build build-release --config Release --parallel
```

`--parallel` lets CMake use available CPU cores. To limit the number of concurrent build jobs, pass a number, such as `--parallel 4`.

Use `-G "Visual Studio 17 2022"` or `-G Xcode` when selecting those generators. For single-configuration generators, set `CMAKE_BUILD_TYPE` to `Debug` or `Release`. The GoogleTest runtime is configured to match the MSVC runtime on Windows.
