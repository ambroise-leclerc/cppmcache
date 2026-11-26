# Contributing to cppmcache

Thank you for considering a contribution. This guide is adapted from the one used by
[MduX](https://github.com/ambroise-leclerc/MduX).

## Licence of contributions

cppmcache is released under the GNU General Public Licence, version 3 or later
([LICENSE](LICENSE)). By submitting a contribution you agree that it is distributed under the same
terms. Every source file starts with:

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) <year> <your name>
```

Do not copy code from ccache or from any other project without saying so in the pull request,
even when the licence allows it.

## Authorship and attribution

Every commit must be attributable to a person who can be contacted about it.

- The author, the committer and every `Co-authored-by:` trailer must be a human account with a
  working contact address. GitHub's per-user `noreply` addresses are acceptable, because the person
  can be reached through GitHub.
- **No attribution to AI tools, anywhere**: not in commit messages, trailers, pull request
  descriptions, issue comments, code comments or release notes. That covers `Co-authored-by:` lines
  naming an assistant, "Generated with …" footers, session links and robot emoji.
- You may use any tool you like to write code, including AI assistants. You, not the tool, are the
  author: you are expected to understand the change, to have tested it, and to answer review
  comments about it.

CI enforces the mechanical part of this rule with `tools/check_attribution.py`, which reads the
commits and the description of each pull request. Run it before pushing:

```sh
tools/check_attribution.py origin/main..HEAD
```

## Language

Everything is written in **British English** (`behaviour`, `normalise`, `licence` as a noun): the
documentation, issues, pull requests, commit messages, source comments, test names and the tool's
own messages.

## Building and testing

You need CMake 4.0 to 4.4, Ninja, and GCC 16.1, Clang 20+ with libc++, or MSVC 19.40+. There are
no CMake presets: name the build directory and the compiler yourself.

```sh
cmake -S . -B build-gcc -G Ninja -DCMAKE_CXX_COMPILER=g++
cmake --build build-gcc && ctest --test-dir build-gcc

cmake -S . -B build-clang -G Ninja --toolchain cmake/toolchains/linux-clang-libcxx.cmake
cmake --build build-clang && ctest --test-dir build-clang
```

Few contributors have all three compilers at hand. CI builds and tests with each of them, so say in
the pull request which ones you ran yourself.

- **Unit tests** are [SpecLab](https://github.com/ambroise-leclerc/SpecLab) scenarios under
  `tests/spec/`, registered with `speclab::Register`. CTest lists one test per scenario.
- **Integration tests** (`tests/integration/Modules.cmake`) compile a small modules project through
  cppmcache with a real compiler. Each runs in two variants: a minimal command, and the form of
  command CMake writes. The second one exists because GCC changes the content of its dependency
  file when CMake's options are present; a change that passes only the first is not finished.
- `integration.remote.*` run several local caches against one server of Bazel's HTTP caching
  protocol. `tests/integration/CacheServer.py` provides that server; set `CPPMCACHE_TEST_REMOTE` to
  the URL of a real one, such as a bazel-remote, to run the same test against it.
- Code that includes a header of the standard library, or of a library that does, must not be in a
  translation unit that uses `import std;`. The networking code shows the pattern: one classic
  `.cpp` file behind a header that includes nothing (`RemoteTransport.hpp`).
- `integration.cpm-consumer` builds a small project that imports cppmcache with CPM. It guards the
  rule that cppmcache, as a subproject, sets nothing outside its own targets: keep new settings in
  `cppmcache_configure_target()` or behind `PROJECT_IS_TOP_LEVEL`.
- A change to how keys are computed should come with numbers. `tools/compare_caches.py` builds a
  project with no launcher, with ccache and with cppmcache:

  ```sh
  tools/compare_caches.py --source . --work /tmp/bench --cppmcache build-gcc/cppmcache \
      --edit src/cppmcache/Hash.cppm --cmake-arg=-DCMAKE_CXX_COMPILER=g++
  ```

## Coding style

We follow the [C++ Core Guidelines](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines).
The code is C++23, written as named modules with `import std;`.

### Naming

- **Classes and structs:** `UpperCamelCase` (`InputTree`, `ManifestEntry`).
- **Functions and methods:** `lowerCamelCase` (`parseCommand()`, `rootDigest()`).
- **Variables, including `const` and `constexpr` ones:** `lowerCamelCase` (`manifestKey`,
  `maxManifestEntries`). No Hungarian notation and no `k` prefix on constants: a constant is a
  variable and follows the same rule.
- **Private data members:** `lowerCamelCase` with a trailing underscore (`root_`).
- **Namespaces and module names:** lowercase (`cppmcache`, `cppmcache.store`).
- **Macros:** `ALL_CAPS_WITH_UNDERSCORES`. But do not use macros.

### Formatting

`.clang-format` is authoritative and CI checks it with clang-format 21:

```sh
clang-format -i $(git ls-files 'src/*.cppm' 'src/*.cpp' 'src/*.hpp' 'tests/*.cpp')
```

- Indent with 4 spaces, no tabs; lines up to 160 columns.
- Pointer and reference symbols go next to the type (`const std::string& name`).
- `.editorconfig` carries the same basics for editors, and `.gitattributes` keeps line endings as
  LF on every platform.

### File organisation

- Module interface units: `.cppm`. Other sources: `.cpp`.
- File names use `UpperCamelCase` (`Driver.cppm`, `StoreSpec.cpp`).
- Do not mix `import std;` with an `#include` of a standard library header in the same translation
  unit. A C header in the global module fragment is fine (see `Process.cppm`).
- Avoid friend-defaulted comparison operators in exported types: GCC 16.1 crashes in the importing
  unit. Use a member-defaulted or a hand-written operator.

### Documentation

We use Doxygen syntax.

- **File-level documentation:** open the block with `@file`, naming the file with its extension and
  no path, then `@brief`. No `@author` tags.

  ```cpp
  /**
   * @file Driver.cppm
   * @brief One sentence on what this file is.
   */
  ```

- **`///` line comments carry no `@brief`**: the comment is the brief.
- Comments say why, not what. A workaround for a compiler defect names the compiler and version.

## Pull requests

- One topic per pull request, with a title that says what changes for the user of the tool.
- Describe the change and its motivation, list the checks you ran with their results, and state
  what you did not verify. Fill in the pull request template.
- All CI jobs must pass before a merge.
- Add or update tests and documentation with the code. A new limitation belongs in the README.

## Releasing

A release is a version tag; the `Release` workflow does the rest.

1. Set the version in `project(cppmcache VERSION …)` in `CMakeLists.txt`, in a pull request. The
   same pull request runs the release workflow as a dry run: it builds the three packages and tests
   each one, and publishes nothing.
2. Once merged, tag that commit `vX.Y.Z` and push the tag. The tag must match the version of the
   project, or the workflow stops.
3. The workflow builds the binaries with their C++ runtime linked statically, runs the unpacked
   binary of each archive through the modules integration test, runs the Linux one on stock
   runners, then publishes the archives, `SHA256SUMS` and a build provenance attestation.

The action at the root of the repository (`action.yml`) installs those archives. CI exercises it
against the latest release.

## Reporting bugs

Open an issue with the template. The most useful attachment is the relevant lines of a
`CPPMCACHE_LOG` file: each call records whether it was a hit, a miss or passed through, and why.
