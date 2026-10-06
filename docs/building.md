---
title: Building from source
---

# Build and test ckmux

For everyday use, start with [release packages](installation.md). Source
builds support macOS, Linux, and native Windows.

## Requirements

| Requirement | Details |
|---|---|
| CMake | 3.28 or newer |
| C++ toolchain | C++20, including floating-point `std::to_chars` / `std::from_chars` |
| Git and network access | To obtain ckmux and the pinned ckVision release |
| Python 3 | Required for the test suite, which is enabled by default |

Use Apple Clang from the Xcode command-line tools on macOS, a modern GCC or
Clang toolchain on Linux, or Visual Studio's C++ desktop tools and Windows SDK
on Windows. Native Windows CI uses Visual Studio 2026. Ubuntu 24.04's standard
build tools meet the Linux baseline; Ubuntu 20.04's default toolchain does not.

With Apple's system C++ library, floating-point `std::from_chars` requires
macOS 26 or later. Setting an older deployment target does not make that API
available. Homebrew source builds using Apple Clang have the same requirement;
an alternative C++ standard library needs its own compatibility verification.

Typical prerequisites:

```sh
# macOS; install Homebrew separately if needed
xcode-select --install
brew install cmake python

# Ubuntu 24.04
sudo apt update
sudo apt install build-essential cmake git python3

# Fedora
sudo dnf install gcc-c++ cmake make git python3
```

Run only the block for your system. On Windows, install Git, CMake, Python 3,
and the **Desktop development with C++** workload, including tools for your
target architecture. Use a Developer PowerShell prompt.

## macOS and Linux

```sh
git clone https://github.com/cklukas/ckmux.git
cd ckmux
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DCKMUX_PREFER_CKVISION_SOURCE=OFF
cmake --build build --parallel 8
ctest --test-dir build --output-on-failure --parallel 1
./build/ckmux
```

Install under your account, or choose another prefix:

```sh
cmake --install build --prefix "$HOME/.local"
export PATH="$HOME/.local/bin:$PATH"
ckmux --version
```

The install includes the executable, Unix manual, and Markdown user guide.

## Windows

From a Developer PowerShell prompt in your source checkout, select x64:

```powershell
git clone https://github.com/cklukas/ckmux.git
Set-Location ckmux
cmake -S . -B build -G 'Visual Studio 18 2026' -A x64 `
  -DCKMUX_WINDOWS_ARCHITECTURE=x64 -DCKMUX_PREFER_CKVISION_SOURCE=OFF
cmake --build build --config Release --parallel 8
.\build\Release\ckmux.exe --version
.\build\Release\ckmux.exe
```

For ARM64 use `-A ARM64 -DCKMUX_WINDOWS_ARCHITECTURE=arm64` in a separate build
directory. Match the generator to the installed Visual Studio version.

```powershell
cmake --install build --config Release --prefix "$env:LOCALAPPDATA/ckmux-source"
& "$env:LOCALAPPDATA\ckmux-source\bin\ckmux.exe" --version
```

A source build without an app-local runtime uses inbox ConPTY with no Sixel
advertisement. The [Windows guide](windows.md#runtime-policy) explains the
qualified runtime and how to build the complete graphics-enabled ZIP/MSI.
Run native tests through the [isolated test host](windows.md#run-native-tests);
the real clipboard tests deliberately refuse an interactive window station.

## How ckVision is selected

The commands above set `CKMUX_PREFER_CKVISION_SOURCE=OFF`. CMake then uses an
installed ckVision package of the exact required version, or fetches the
pinned release automatically. You do not need to clone ckVision separately.

For co-development, a sibling `../ckvision` checkout is preferred by default.
Select another checkout with `CKMUX_CKVISION_SOURCE_DIR`, together with
`CKMUX_PREFER_CKVISION_SOURCE=ON`. A sibling checkout follows its source bytes;
it is not the same as a build against the released dependency.

## Build options and verification

| Option | Purpose |
|---|---|
| `CKMUX_BUILD_TESTING=OFF` | Build without the test suite or its Python requirement |
| `CKMUX_TEST_STRESS=ON` | Opt into local ten-instance and sustained-flood checks |
| `CKMUX_SANITIZE=address,undefined` | Instrument ckmux and its source-built ckVision dependency on supported Unix compilers |
| `CKMUX_BUILD_FUZZERS=ON` | Build libFuzzer targets with LLVM Clang |

Use a separate build directory for Debug/sanitizer configurations. For example,
on macOS or Linux with a source-built dependency:

```sh
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug \
  -DCKMUX_SANITIZE=address,undefined
cmake --build build-asan --parallel 8
ctest --test-dir build-asan --output-on-failure --parallel 1
```

Hosted CI runs functional suites serially with stress tests off. MSVC supports
`CKMUX_SANITIZE=address` only when the target toolchain supports it; see the
[Windows test policy](windows.md#run-native-tests). Do not combine an
instrumented ckmux with an uninstrumented installed ckVision library.

## Documentation

`docs/` is the user guide, `doc/` holds the installed-manual and key-page
templates, and `ckdocs.yml` defines site navigation. The key appendix is
generated from the command registry; update it with the
`ckmux_update_key_docs` build target when bindings change.

With [ckdocs](https://github.com/cklukas/ck-git-hosting) installed:

```sh
ckdocs check --root .
ckdocs serve --root . --out build/docs-preview
```

Keep usage pages about current behavior. Record user-visible release changes
in [CHANGES.md](https://github.com/cklukas/ckmux/blob/main/CHANGES.md).
Read the [contribution terms](https://github.com/cklukas/ckmux/blob/main/CONTRIBUTING.md)
before submitting a patch.
