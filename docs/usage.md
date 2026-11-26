# Using cppmcache

The [README](../README.md) has the short version. This page has the details.

## In GitHub Actions

The repository is also an action that installs a released binary, checks it against the checksums
of the release, puts it on the `PATH` and keeps the cache directory between runs.

```yaml
- uses: ambroise-leclerc/cppmcache@v0.1.0
  with:
    cache-key: ${{ matrix.compiler }}   # jobs that must not share a cache get different values
- run: cmake -S . -B build -G Ninja -DCMAKE_CXX_COMPILER_LAUNCHER=cppmcache
- run: cmake --build build
- run: cppmcache -s
```

| Input            | Default                    | Meaning                                                        |
|------------------|----------------------------|----------------------------------------------------------------|
| `version`        | `latest`                   | version to install, such as `0.1.0`                            |
| `cache`          | `true`                     | restore and save the cache directory with `actions/cache`      |
| `cache-key`      | empty                      | text added to the key of the saved cache                       |
| `base-directory` | the workspace              | sets `CPPMCACHE_BASEDIR`; empty leaves relocation off          |
| `remote`         | empty                      | sets `CPPMCACHE_REMOTE`                                        |

It sets `CPPMCACHE_DIR`, `CPPMCACHE_BASEDIR` and `CPPMCACHE_COMPILERCHECK=content` for the following
steps, and its outputs are `version`, `path` and `cache-directory`.

- Binaries are published for Linux x86_64 (glibc 2.34 or later), macOS 15 on Apple Silicon and
  Windows x86_64. On another platform, build cppmcache inside your build with CPM, as below.
- Every run saves a new copy of the cache and restores the most recent one. cppmcache has no
  eviction yet, so that copy grows until GitHub's own limits remove old ones.
- Each release carries a `SHA256SUMS` file, which the action checks, and a build provenance
  attestation: `gh attestation verify <archive> --repo ambroise-leclerc/cppmcache`.

## Sharing a cache between locations

By default the working directory and every absolute path of a command are part of its key, so two
checkouts of a project share nothing. Set `CPPMCACHE_BASEDIR` to the root of the checkout, as with
ccache's `base_dir`:

```sh
export CPPMCACHE_BASEDIR=$PWD        # the directory that holds both the sources and the build tree
```

- Absolute paths under that directory — source files and the values of options such as `-I` — are
  rewritten relative to the working directory, in the key **and in the command the compiler is run
  with**, so its outputs do not record the location either. Diagnostics then show relative paths.
- Paths outside it, such as system headers, stay absolute: they must be the same wherever the cache
  is shared. Paths inside macro definitions are never rewritten.
- The working directory leaves the key, unless the command produces debug information without a
  `-fdebug-prefix-map`, because the object then records it.
- Two machines also need the same compiler identity. The default one includes the date of the
  executable; `CPPMCACHE_COMPILERCHECK=content` uses its content instead.

Measured on this repository, built by CMake from one directory and then from another with the same
cache: 37 misses, then 37 hits.

## Sharing a cache between machines

With `CPPMCACHE_REMOTE` set, cppmcache also uses a server of
[Bazel's HTTP caching protocol](https://bazel.build/remote/caching#http-caching): blobs under
`/cas/<sha256>`, action results under `/ac/<sha256>`. That is what
[bazel-remote](https://github.com/buchgr/bazel-remote) serves, and the layout ccache's own HTTP
storage calls `bazel`.

```sh
docker run -d -p 8080:8080 buchgr/bazel-remote-cache --max_size=10 --dir=/data
export CPPMCACHE_REMOTE=http://localhost:8080
```

- The local cache is always consulted first: a local hit costs no network request.
- A result missing locally is fetched from the server, checked against its digest, and kept in the
  local cache. A new result is published after it has been stored locally, unless the cache is
  read-only.
- The server never fails a build. If it cannot be reached, cppmcache counts a remote error, leaves
  it alone for the rest of that compilation, and carries on with the local cache.
- **There is no TLS and no authentication.** Only `http://` URLs are accepted: use it on a network
  you trust, or through a tunnel.

## Importing it with CPM

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

## Building it

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
| Clang on macOS   | `--toolchain cmake/toolchains/macos-llvm.cmake`, which uses Homebrew's `llvm@21` or `CPPMCACHE_LLVM_ROOT`. Apple's Clang cannot build cppmcache: it has no `std` module. |

`-DCPPMCACHE_BUILD_TESTS=OFF` leaves the test suite, and its SpecLab download, out of the build.
`-DCPPMCACHE_REMOTE=OFF` leaves out the remote cache client and the download of
[stdnet](https://github.com/ambroise-leclerc/stdnet), the networking library it is built on.

## Known limitations

- Without `CPPMCACHE_BASEDIR`, absolute paths and the working directory are part of the key:
  nothing is shared between two checkouts of the same project.
- With Clang and MSVC, a module is shared across locations only when its BMI comes from the cache.
  One that is recompiled elsewhere outside the launcher is a different file, and its importers miss.
  With GCC this does not matter: the interface digest ignores what a BMI says about its location.
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
- No eviction: the size of the local cache is not bounded.
- The remote cache has no TLS, no authentication and no compression, and it speaks HTTP, not the
  gRPC interface of the Remote Execution API. Uploads are synchronous: a miss waits for them.
- A restored dependency file is rewritten, not copied: the same dependencies, laid out differently.
