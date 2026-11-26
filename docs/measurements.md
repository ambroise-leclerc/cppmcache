# Measurements

Every table says where and how it was measured. Each cell is a single run, so differences of a few
per cent are noise: read the hit counts first.

## MduX

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

## cppmcache building itself, against ccache

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

## The four platforms, from one CI run

Commit `82eeca7` on `main`, GitHub's shared runners, 39 translation units. This is the run the
README quotes. "Not cached" counts compilations the tool passed straight to the compiler.

| GCC 16.1, ccache 4.11.2                     | No cache | ccache                         | cppmcache          |
|---------------------------------------------|---------:|--------------------------------|--------------------|
| Clean build, empty cache                    |   46.6 s | 47.2 s (1 miss, 38 not cached) | 46.9 s (39 misses) |
| Clean build, populated cache                |   46.8 s | 45.9 s (1 hit, 38 not cached)  | 2.8 s (39 hits)    |
| Trailing comment added to `Hash.cppm`       |   25.4 s | 25.4 s (14 not cached)         | 2.8 s (13 hits, 1 miss) |
| Edit reverted                               |   25.2 s | 25.5 s (14 not cached)         | 0.2 s (14 hits)    |

| Clang 21 on Linux, ccache 4.9.1             | No cache | ccache                           | cppmcache          |
|---------------------------------------------|---------:|----------------------------------|--------------------|
| Clean build, empty cache                    |   21.3 s | 22.0 s (10 misses, 29 not cached) | 22.2 s (39 misses) |
| Clean build, populated cache                |   21.4 s | 17.8 s (10 hits, 29 not cached)  | 2.9 s (39 hits)    |
| Trailing comment added to `Hash.cppm`       |   11.8 s | 9.2 s (7 hits, 7 not cached)     | 12.0 s (14 misses) |
| Edit reverted                               |   11.5 s | 9.1 s (7 hits, 7 not cached)     | 0.2 s (14 hits)    |

| Clang 21 on macOS, ccache 4.14.3            | No cache | ccache                                  | cppmcache          |
|---------------------------------------------|---------:|-----------------------------------------|--------------------|
| Clean build, empty cache                    |   19.1 s | 19.7 s (10 misses, 29 not cached)       | 20.5 s (39 misses) |
| Clean build, populated cache                |   19.3 s | 18.8 s (2 hits, 8 misses, 29 not cached) | 3.9 s (39 hits)    |
| Trailing comment added to `Hash.cppm`       |    9.6 s | 9.4 s (7 misses, 7 not cached)          | 9.6 s (14 misses)  |
| Edit reverted                               |    9.2 s | 9.1 s (2 hits, 5 misses, 7 not cached)  | 0.4 s (14 hits)    |

| MSVC 19.52, ccache 4.14.3                   | No cache | ccache             | cppmcache               |
|---------------------------------------------|---------:|--------------------|-------------------------|
| Clean build, empty cache                    |   51.2 s | 52.6 s (39 misses) | 63.6 s (39 misses)      |
| Clean build, populated cache                |   50.8 s | 1.6 s (39 hits)    | 3.4 s (39 hits)         |
| Trailing comment added to `Hash.cppm`       |   21.7 s | build failed       | 16.0 s (5 hits, 9 misses) |
| Edit reverted                               |   21.6 s | build failed       | 0.5 s (14 hits)         |

Where cppmcache gains nothing: the first build of anything (a miss costs a full compilation, and in
the MSVC run above the cold build with cppmcache was the slowest of the three), and, with Clang and
MSVC, the units downstream of an interface whose source changed in any way.
