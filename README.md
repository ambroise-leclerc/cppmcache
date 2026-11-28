# cppmcache

**A compiler cache that understands C++20 modules.**

[![CI](https://github.com/ambroise-leclerc/cppmcache/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/ambroise-leclerc/cppmcache/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/ambroise-leclerc/cppmcache?label=release)](https://github.com/ambroise-leclerc/cppmcache/releases)
[![Licence: GPL-3.0-or-later](https://img.shields.io/badge/licence-GPL--3.0--or--later-blue)](LICENSE)
![Status: prototype](https://img.shields.io/badge/status-prototype-orange)

[![Linux, GCC 16](https://img.shields.io/github/check-runs/ambroise-leclerc/cppmcache/main?nameFilter=Linux%2C%20GCC%2016.1&label=Linux%20%C2%B7%20GCC%2016)](https://github.com/ambroise-leclerc/cppmcache/actions/workflows/ci.yml?query=branch%3Amain)
[![Linux, Clang 21](https://img.shields.io/github/check-runs/ambroise-leclerc/cppmcache/main?nameFilter=Linux%2C%20Clang%2021%20with%20libc%2B%2B&label=Linux%20%C2%B7%20Clang%2021)](https://github.com/ambroise-leclerc/cppmcache/actions/workflows/ci.yml?query=branch%3Amain)
[![macOS, Clang 21](https://img.shields.io/github/check-runs/ambroise-leclerc/cppmcache/main?nameFilter=macOS%2C%20Clang%2021%20with%20libc%2B%2B&label=macOS%20%C2%B7%20Clang%2021)](https://github.com/ambroise-leclerc/cppmcache/actions/workflows/ci.yml?query=branch%3Amain)
[![Windows, MSVC](https://img.shields.io/github/check-runs/ambroise-leclerc/cppmcache/main?nameFilter=Windows%2C%20MSVC&label=Windows%20%C2%B7%20MSVC)](https://github.com/ambroise-leclerc/cppmcache/actions/workflows/ci.yml?query=branch%3Amain)

cppmcache is used like ccache, as a prefix to the compiler. Unlike ccache, it caches **module
interface units** (the object *and* the BMI) and **the units that import modules**, and it knows
when a change to a module does not reach its importers.

## Why

ccache does not cache named modules: imported BMIs are not part of its key, and a hit cannot
restore the BMI an interface produces ([ccache#1252](https://github.com/ccache/ccache/issues/1252)).
On a modules project it passes most compilations straight to the compiler, or, for a unit that
imports a module, may serve an object built against an older BMI.

## What you gain

Rebuilding from an empty build directory with a populated cache — a fresh CI job, a new checkout, a
branch switch:

| Project and compiler                         | No cache |                              ccache | cppmcache |    Gain |
|----------------------------------------------|---------:|------------------------------------:|----------:|--------:|
| [MduX](https://github.com/ambroise-leclerc/MduX), GCC 16.1, 343 units, 24 cores | 75.5 s | kept off every module unit | **15.2 s** | 5× |
| cppmcache itself, GCC 16.1                   |   46.8 s |            45.9 s (1 unit of 39 cached) | **2.8 s** | 17× |
| cppmcache itself, Clang 21 on Linux          |   21.4 s |          17.8 s (10 units of 39 cached) | **2.9 s** |  7× |
| cppmcache itself, Clang 21 on macOS          |   19.3 s |           18.8 s (2 units of 39 cached) | **3.9 s** |  5× |
| cppmcache itself, MSVC 19.52                 |   50.8 s | 1.6 s, then the build fails after an edit | **3.4 s** | 15× |

After a comment is edited in a core module interface of MduX, 122 units are re-run by the build
system: **34.4 s without a cache, 1.8 s with cppmcache** (121 hits), because the BMI did not change.

All 950 tests of MduX pass on a build served entirely from the cache. The four "cppmcache itself"
rows come from one CI run on GitHub's shared runners, one measurement per cell; CI republishes
them in the summary of every run. Method, full tables and the cases where cppmcache gains nothing
are in [docs/measurements.md](docs/measurements.md).

## Quick start

**In GitHub Actions**, with a released binary and the cache kept between runs:

```yaml
- uses: ambroise-leclerc/cppmcache@v0.1.2
- run: cmake -S . -B build -G Ninja -DCMAKE_CXX_COMPILER_LAUNCHER=cppmcache
- run: cmake --build build
- run: cppmcache -s
```

**Anywhere else**, download a binary from the
[releases](https://github.com/ambroise-leclerc/cppmcache/releases) (Linux x86_64, macOS arm64,
Windows x86_64) and name it as the compiler launcher:

```sh
cmake -S . -B build -G Ninja -DCMAKE_CXX_COMPILER_LAUNCHER=/path/to/cppmcache
```

**From source, inside your own build**, with [CPM.cmake](https://github.com/cpm-cmake/CPM.cmake):

```cmake
CPMAddPackage("gh:ambroise-leclerc/cppmcache#<commit>")
cppmcache_enable(my_library my_executable)
```

`cppmcache -s` shows the statistics, `-z` zeroes them and `-C` clears the cache.

## Supported compilers

| Compiler               | Platform       | Module commands cached                                         |
|------------------------|----------------|----------------------------------------------------------------|
| GCC 16                 | Linux          | `-fmodules`, `-fmodule-mapper=<file>`                          |
| Clang (tested: 21, 22) | Linux, macOS   | `-fmodule-output=`, `-fmodule-file=name=path` (one-phase)      |
| MSVC 19.40+            | Windows        | `/interface`, `/ifcOutput`, `/reference name=path`             |

These are the commands CMake writes with the Ninja generator. Anything cppmcache does not
understand is passed to the compiler unchanged, so it never breaks a build it cannot cache.

## Sharing the cache

| To share between…           | Set                                                              |
|-----------------------------|------------------------------------------------------------------|
| checkouts, or CI runners    | `CPPMCACHE_BASEDIR` to the root of the project: paths under it become relative |
| machines                    | `CPPMCACHE_REMOTE=http://host:port`, a [bazel-remote](https://github.com/buchgr/bazel-remote) or any server of Bazel's HTTP caching protocol |
| machines with different compiler dates | `CPPMCACHE_COMPILERCHECK=content`                     |

The remote cache has no TLS and no authentication yet: use it on a network you trust. Details are
in [docs/usage.md](docs/usage.md).

## Configuration

| Variable                      | Effect                                                          |
|-------------------------------|-----------------------------------------------------------------|
| `CPPMCACHE_DIR`               | cache directory (default: `~/.cache/cppmcache`; `~/Library/Caches/cppmcache` on macOS; `%LOCALAPPDATA%\cppmcache` on Windows) |
| `CPPMCACHE_BASEDIR`           | absolute directory under which paths are made relative          |
| `CPPMCACHE_COMPILERCHECK`     | `content`: identify the compiler by the digest of its executable |
| `CPPMCACHE_REMOTE`            | URL of a remote cache                                           |
| `CPPMCACHE_REMOTE_READ_ONLY`  | fetch from the remote cache but never publish to it             |
| `CPPMCACHE_REMOTE_TIMEOUT_MS` | time allowed for one remote operation (default: 10000)          |
| `CPPMCACHE_LOG`               | file to which each call appends its outcome and the reason      |
| `CPPMCACHE_DISABLE`           | run the compiler directly                                       |
| `CPPMCACHE_RECACHE`           | ignore existing entries and replace them                        |

## How it works, briefly

- **A key per module, not per path.** An imported module enters the key under its name and the
  digest of its BMI. The key is a Remote Execution API action digest; storage is a
  content-addressable store plus an action cache, locally and remotely.
- **Early cutoff.** A unit depends on the BMIs it imports, not on their sources. If an interface is
  edited and its BMI is unchanged, everything downstream is a hit.
- **Interface digest.** GCC writes the build time and directory into every BMI; cppmcache leaves
  that out of the digest, so identical interfaces compare equal.
- **No preprocessor pass.** Inputs are read from what the compiler reports: the dependency file and
  module mapping for GCC and Clang, `/sourceDependencies` for MSVC.
- **Header shadowing is detected.** A header created earlier in the search order than one a unit
  included changes what the compiler reads without changing any file it read, and so does a header
  that a `__has_include` test looked for and did not find. cppmcache checks the places where such a
  file would be, and recompiles.

The full description is in [docs/design.md](docs/design.md).

## Limits to know about

- It is a prototype: the size of the local cache is not bounded, and the format may change.
- Early cutoff needs the compiler to produce an identical BMI. With GCC, shifting the lines of an
  interface changes it; with Clang and MSVC, any edit to the interface source does.
- Not cached: coverage and profiling builds, header units, precompiled headers, MSVC's `/Zi`
  (`/Z7` is), Clang's two-phase `--precompile`, and linking.

The complete list is in [docs/usage.md](docs/usage.md#known-limitations).

## Building from source

cppmcache is itself written with C++23 modules and `import std;`. It needs CMake 4.0 to 4.4, Ninja,
and GCC 16.1, Clang 20+ with libc++, or MSVC 19.40+.

```sh
cmake -S . -B build -G Ninja -DCMAKE_CXX_COMPILER=g++
cmake --build build && ctest --test-dir build
```

Other compilers and options are in [docs/usage.md](docs/usage.md#building-it).

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md). The project is written in British English throughout. If
cppmcache proves useful, it is intended as the basis of a proposal to ccache.

## Licence

GNU General Public Licence, version 3 or (at your option) any later version, like ccache. See
[LICENSE](LICENSE).

The remote cache client is built on [stdnet](https://github.com/ambroise-leclerc/stdnet) and
[stdexec](https://github.com/NVIDIA/stdexec) (Apache 2.0 with LLVM exceptions). The tests use
[SpecLab](https://github.com/ambroise-leclerc/SpecLab). All are fetched at configure time by
[CPM.cmake](https://github.com/cpm-cmake/CPM.cmake) (MIT, vendored in `cmake/CPM.cmake`).
