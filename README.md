# cppmcache

[![CI](https://github.com/ambroise-leclerc/cppmcache/actions/workflows/ci.yml/badge.svg)](https://github.com/ambroise-leclerc/cppmcache/actions/workflows/ci.yml)
[![Licence: GPL-3.0-or-later](https://img.shields.io/badge/licence-GPL--3.0--or--later-blue)](LICENSE)

A compiler cache for C++ that understands named modules. It is used like ccache, as a prefix to the
compiler, and it also caches module interface units (the object **and** the BMI) and the
translation units that import modules.

> **Prototype.** GCC 16 and Clang (tested with 21 and 22) are supported on Linux, and MSVC on Windows.
> Any command the tool does not understand is passed to the compiler unchanged.

## Why

ccache cannot cache named-module compilations: imported BMIs are not part of its key, and a cache
hit cannot restore the BMI a module interface produces
([ccache#1252](https://github.com/ccache/ccache/issues/1252)). Used as a plain launcher on a
modules project it either passes the compilation straight through or, for a unit that imports a
module, may return an object compiled against an older BMI.

cppmcache is an experiment in what a module-aware key looks like. If it proves useful, it is
intended as the basis of a proposal to ccache.

## Usage

```sh
# In a CMake project (Ninja generator), with a cppmcache that is already built:
cmake -S . -B build -G Ninja -DCMAKE_CXX_COMPILER_LAUNCHER=/path/to/cppmcache

cppmcache -s             # statistics
cppmcache -z             # zero the counters
cppmcache -C             # clear the cache
cppmcache --print-stats  # counters as name<TAB>value
```

| Variable            | Effect                                                         |
|---------------------|----------------------------------------------------------------|
| `CPPMCACHE_DIR`     | cache directory (default: `~/.cache/cppmcache`)                |
| `CPPMCACHE_DISABLE` | run the compiler directly                                      |
| `CPPMCACHE_RECACHE` | ignore existing entries and replace them                       |
| `CPPMCACHE_LOG`     | file to which each call appends its outcome and the reason     |

### Building it

cppmcache is itself written with C++23 modules and `import std;`, so it needs a recent toolchain:
CMake 4.0 to 4.4, Ninja, and GCC 16.1, Clang 20 or later with libc++, or MSVC 19.40 or later.
There are no CMake presets; the build is configured like any other CMake project.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build
cmake --install build --prefix ~/.local
```

| Compiler         | Add to the configure command                                    |
|------------------|-----------------------------------------------------------------|
| GCC              | `-DCMAKE_CXX_COMPILER=g++`                                      |
| Clang and libc++ | `--toolchain cmake/toolchains/linux-clang-libcxx.cmake`, which picks the newest `/usr/lib/llvm-<N>` that ships libc++ modules, or `CPPMCACHE_LLVM_ROOT` |
| MSVC             | `-DCMAKE_CXX_COMPILER=cl`, from a Developer prompt              |

`-DCPPMCACHE_BUILD_TESTS=OFF` leaves the test suite, and its SpecLab download, out of the build.

### Importing it with CPM

A project can fetch cppmcache with [CPM.cmake](https://github.com/cpm-cmake/CPM.cmake), build it as
part of its own tree and compile chosen targets through it:

```cmake
CPMAddPackage("gh:ambroise-leclerc/cppmcache#<commit>")
cppmcache_enable(my_library my_executable)
```

`cppmcache_enable()` makes cppmcache the compiler launcher of each target and makes the target
depend on the tool, so it is built before the first cached compilation. An imported cppmcache
builds no tests, installs nothing and leaves the importing project's settings alone. Because it is
written with `import std;`, the importing project must open CMake's gate for it, by setting
`CMAKE_EXPERIMENTAL_CXX_IMPORT_STD` before its own `project()` call.

## How it works

The key is hybrid and has two levels.

1. **Manifest key**: the compiler, the normalised arguments (output paths removed), the working
   directory and the content of the source file. It leads to a manifest: the list of input sets
   already seen for that key, each one naming the headers read and the modules imported, with their
   digests.
2. **Action digest**, in the sense of the Remote Execution API: the command plus a Merkle tree of
   the inputs. An imported module appears in that tree under its **name** and the **digest of its
   BMI**, never under its path.

Local storage follows the REAPI model: a content-addressable store keyed by SHA-256 and an action
cache holding canonical protobuf `ActionResult` messages. A remote backend can be added later
without changing the keys.

**Early cutoff.** A consumer depends on the content of the BMIs it imports, not on the sources
upstream of them. When an interface unit is edited and its BMI does not change, everything
downstream is served from the cache.

**Interface digest.** GCC writes the build time into the `.gnu.c++.README` section of every BMI.
That section is informational, so it is left out of the digest of an imported BMI; otherwise two
identical compilations would never produce the same key.

The inputs of a compilation are discovered after the fact, from the dependency file (`-MD`) and
from the module mapping (`-fmodule-mapper=` for GCC, `-fmodule-file=name=path` for Clang). For MSVC
they come from the JSON report of `/sourceDependencies`, which cppmcache adds to the command, and
from the `/reference name=path` options. There is no preprocessor pass.

## Measurements

### MduX

[MduX](https://github.com/ambroise-leclerc/MduX), GCC 16.1, CMake 4.2, Ninja, 24 cores, Release
build: 343 translation units, 113 of them module interfaces. cppmcache is plugged in with
`-DENABLE_CACHE=OFF -DCMAKE_CXX_COMPILER_LAUNCHER=cppmcache`. ccache is absent from the table
because MduX's own launcher script keeps it away from module compilations, which here means all of
them.

| Scenario                                                       | No cache | cppmcache            |
|----------------------------------------------------------------|---------:|----------------------|
| Full build, empty cache                                        |   75.5 s | 75.5 s (343 misses)  |
| Full build from an empty build directory, populated cache      |   75.5 s | 15.2 s (343 hits)    |
| Comment edited in `mdux/core/Units.cppm` (122 units re-run)    |   34.4 s | 1.8 s (121 hits)     |
| Line added at the top of the same file                         |        – | 35.0 s (122 misses)  |
| File restored                                                  |        – | 2.0 s (122 hits)     |

All 950 MduX tests pass on the build served entirely from the cache.

### cppmcache building itself, against ccache

`tools/compare_caches.py` builds a project with no launcher, with ccache and with cppmcache, and
reports times and cache statistics. CI runs it on this repository for both compilers and publishes
the table in the job summary. On the same 24-core machine, with ccache 4.12.3:

| GCC 16.1                                    | No cache | ccache                 | cppmcache          |
|---------------------------------------------|---------:|------------------------|--------------------|
| Clean build, empty cache                    |   30.9 s | 33.7 s (30 not cached) | 35.9 s (30 misses) |
| Clean build, populated cache                |   31.4 s | 33.3 s (30 not cached) | 6.9 s (30 hits)    |
| Trailing comment added to `Hash.cppm`       |   23.8 s | 23.1 s (11 not cached) | 3.9 s (10 hits, 1 miss) |
| Edit reverted                               |   25.0 s | 22.7 s (11 not cached) | 0.4 s (11 hits)    |

| Clang 21, libc++                            | No cache | ccache                         | cppmcache          |
|---------------------------------------------|---------:|--------------------------------|--------------------|
| Clean build, empty cache                    |   19.8 s | 20.3 s (6 misses, 26 not cached) | 18.0 s (32 misses) |
| Clean build, populated cache                |   20.3 s | 17.3 s (6 hits, 26 not cached)   | 2.1 s (32 hits)    |
| Trailing comment added to `Hash.cppm`       |   14.4 s | 11.3 s (5 hits, 6 not cached)    | 12.7 s (11 misses) |
| Edit reverted                               |   14.0 s | 10.3 s (5 hits, 6 not cached)    | 0.3 s (11 hits)    |

| MSVC 19.52, GitHub runner, ccache 4.14.3    | No cache | ccache                 | cppmcache          |
|---------------------------------------------|---------:|------------------------|--------------------|
| Clean build, empty cache                    |   55.3 s | 56.7 s (34 misses)     | 58.3 s (34 misses) |
| Clean build, populated cache                |   55.1 s | 1.7 s (34 hits)        | 4.2 s (34 hits)    |
| Trailing comment added to `Hash.cppm`       |   24.8 s | build failed           | 23.3 s (4 hits, 8 misses) |
| Edit reverted                               |   24.8 s | build failed           | 0.6 s (12 hits)    |

The MSVC table comes from CI, on a shared four-core runner, so its times are not comparable with
the two tables above. With MSVC, ccache does cache module units, and the build then fails as soon
as an interface is edited: units served from the cache no longer match the modules they import.

Each cell is a single run. Two things are worth reading in the Clang table. ccache's five hits in
the third row are for units that were re-run because a BMI they import had changed: its key does
not include imported BMIs, so those objects come from the previous build. And cppmcache gets no
early cutoff there, because a Clang BMI changes whenever the text of its source does.

## Known limitations

- Absolute paths and the working directory are part of the key: nothing is shared between two
  checkouts of the same project.
- Early cutoff depends on the compiler producing an identical BMI. With GCC, shifting the lines of
  an interface changes its BMI; with Clang and MSVC, any edit to the interface source does.
- As with ccache's direct mode, a newly created header that shadows another one later in the
  include path is not detected.
- Not cached: coverage and profiling builds, `-gsplit-dwarf`, header units, `-MMD`, Clang's
  two-phase `--precompile`, units that use `__DATE__` or `__TIME__`, and linking.
- With MSVC, `/Zi` is not cached, because it writes debug information to a PDB shared between
  compilations. That is CMake's default for Debug builds; `/Z7`
  (`-DCMAKE_MSVC_DEBUG_INFORMATION_FORMAT=Embedded`) is cached. Precompiled headers and `/MP` are
  not cached either.
- The BMI of `std` is built by a CMake target that does not use the launcher, so it is rebuilt in
  every fresh build directory.
- No eviction: the size of the cache is not bounded.
- A restored dependency file is rewritten, not copied: the same dependencies, laid out differently.

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md). The project is written in British English throughout.

## Licence

cppmcache is free software, released under the GNU General Public Licence, version 3 or (at your
option) any later version, like ccache. See [LICENSE](LICENSE).

The test suite uses [SpecLab](https://github.com/ambroise-leclerc/SpecLab), fetched at configure
time by [CPM.cmake](https://github.com/cpm-cmake/CPM.cmake) (MIT, vendored in `cmake/CPM.cmake`).
