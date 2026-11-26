#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Ambroise Leclerc
"""Compares the build times of a CMake project without a cache, with ccache and with cppmcache.

The project is copied into the work folder, then built four times per launcher:

  1. full build, empty cache;
  2. full build from an empty build folder, populated cache;
  3. incremental build after a comment is appended to a file (--edit);
  4. incremental build after reverting to the original file.

Each scenario is measured only once: on a shared machine, differences of a few per cent are
not significant. The report is written in English, because it is published in the CI job
summary.

Example:
    tools/compare_caches.py --source . --work /tmp/bench --cppmcache build-gcc/cppmcache \\
        --edit src/cppmcache/Hash.cppm --cmake-arg=-DCMAKE_CXX_COMPILER=g++
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

# ccache counters that denote a compilation passed to the compiler without going through the cache.
CCACHE_UNCACHEABLE = (
    "autoconf_test", "bad_compiler_arguments", "bad_input_file", "bad_output_file", "called_for_preprocessing",
    "compiler_check_failed", "compiler_produced_empty_output", "compiler_produced_no_output",
    "compiler_produced_stdout", "could_not_find_compiler", "could_not_use_modules",
    "could_not_use_precompiled_header", "disabled", "error_hashing_extra_file", "internal_error",
    "modified_input_file", "multiple_source_files", "no_input_file", "output_to_stdout", "preprocessor_error",
    "unsupported_code_directive", "unsupported_compiler_option", "unsupported_environment_variable",
    "unsupported_source_encoding", "unsupported_source_language",
)

SCENARIOS = [
    ("cold", "Clean build, empty cache"),
    ("warm", "Clean build, populated cache"),
    ("edit", "Incremental build after a trailing comment in `{edit}`"),
    ("revert", "Incremental build after reverting that edit"),
]


def run(command: list[str], *, env: dict[str, str], cwd: Path | None = None, log: Path) -> None:
    """Runs a command; on failure, prints the end of its output and stops the script."""
    with log.open("ab") as output:
        output.write(f"\n$ {' '.join(command)}\n".encode())
        output.flush()
        status = subprocess.run(command, env=env, cwd=cwd, stdout=output, stderr=subprocess.STDOUT, check=False)
    if status.returncode != 0:
        tail = log.read_text(errors="replace").splitlines()[-40:]
        sys.exit(f"command failed ({status.returncode}): {' '.join(command)}\n" + "\n".join(tail))


def print_stats(tool: str, env: dict[str, str]) -> dict[str, int]:
    """Reads the "name<TAB>value" counters of ccache or cppmcache."""
    output = subprocess.run([tool, "--print-stats"], env=env, capture_output=True, text=True, check=True).stdout
    stats: dict[str, int] = {}
    for line in output.splitlines():
        name, _, value = line.partition("\t")
        if value.strip().lstrip("-").isdigit():
            stats[name] = int(value)
    return stats


class Launcher:
    """A launcher to compare: none, ccache or cppmcache."""

    def __init__(self, name: str, tool: str | None, work: Path) -> None:
        self.name = name
        self.tool = tool
        self.cache_dir = work / f"cache-{name}"
        self.env = dict(os.environ)
        # No user configuration may influence the measurement.
        for variable in list(self.env):
            if variable.startswith(("CCACHE_", "CPPMCACHE_")):
                del self.env[variable]
        if name == "ccache":
            self.env["CCACHE_DIR"] = str(self.cache_dir)
            self.env["CCACHE_CONFIGPATH"] = str(work / "ccache.conf")
        elif name == "cppmcache":
            self.env["CPPMCACHE_DIR"] = str(self.cache_dir)

    def reset_cache(self) -> None:
        shutil.rmtree(self.cache_dir, ignore_errors=True)

    def zero_stats(self) -> None:
        if self.tool:
            subprocess.run([self.tool, "-z"], env=self.env, capture_output=True, check=True)

    def stats(self) -> dict[str, int] | None:
        """Hits, misses and uncached compilations since the last reset to zero."""
        if not self.tool:
            return None
        raw = print_stats(self.tool, self.env)
        if self.name == "ccache":
            uncacheable = sum(raw.get(name, 0) for name in CCACHE_UNCACHEABLE)
            return {
                "hits": raw.get("direct_cache_hit", 0) + raw.get("preprocessed_cache_hit", 0),
                "misses": raw.get("cache_miss", 0),
                "uncached": uncacheable,
            }
        return {
            "hits": raw.get("hit", 0),
            "misses": raw.get("miss", 0),
            "uncached": raw.get("uncacheable", 0) + raw.get("bypass", 0),
        }


def measure(launcher: Launcher, source: Path, work: Path, args: argparse.Namespace) -> dict[str, dict]:
    build = work / f"build-{launcher.name}"
    log = work / f"{launcher.name}.log"
    log.unlink(missing_ok=True)
    edited = source / args.edit
    original = edited.read_bytes()

    def configure() -> None:
        shutil.rmtree(build, ignore_errors=True)
        command = ["cmake", "-S", str(source), "-B", str(build), "-G", "Ninja", f"-DCMAKE_BUILD_TYPE={args.build_type}"]
        if launcher.tool:
            command.append(f"-DCMAKE_CXX_COMPILER_LAUNCHER={launcher.tool}")
        run(command + args.cmake_arg, env=launcher.env, log=log)

    def timed_build() -> dict:
        launcher.zero_stats()
        command = ["cmake", "--build", str(build)]
        if args.jobs:
            command += ["--parallel", str(args.jobs)]
        started = time.monotonic()
        run(command, env=launcher.env, log=log)
        return {"seconds": round(time.monotonic() - started, 2), "stats": launcher.stats()}

    results: dict[str, dict] = {}
    launcher.reset_cache()
    configure()
    results["cold"] = timed_build()
    configure()
    results["warm"] = timed_build()
    try:
        edited.write_bytes(original + b"\n// cppmcache benchmark edit\n")
        results["edit"] = timed_build()
    finally:
        edited.write_bytes(original)
    results["revert"] = timed_build()
    return results


def cell(result: dict) -> str:
    text = f"{result['seconds']:.1f} s"
    stats = result["stats"]
    if stats is None:
        return text
    def count(number: int, singular: str, plural: str) -> str:
        return f"{number} {singular if number == 1 else plural}"

    parts = [count(stats["hits"], "hit", "hits"), count(stats["misses"], "miss", "misses")]
    if stats["uncached"]:
        parts.append(f"{stats['uncached']} not cached")
    return f"{text} ({', '.join(parts)})"


def markdown(report: dict) -> str:
    names = list(report["results"])
    titles = {"none": "No cache", "ccache": "ccache", "cppmcache": "cppmcache"}
    lines = [
        f"### Compiler cache comparison: {report['label']}",
        "",
        "| Scenario | " + " | ".join(titles[name] for name in names) + " |",
        "|---|" + "---:|" * len(names),
    ]
    for key, title in SCENARIOS:
        row = [cell(report["results"][name][key]) for name in names]
        lines.append(f"| {title.format(edit=report['edit'])} | " + " | ".join(row) + " |")
    lines += [
        "",
        "Wall-clock time of `cmake --build`, one run per cell, so differences of a few per cent are noise. "
        "“Not cached” counts compilations the tool passed straight to the compiler.",
        "",
    ]
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--source", type=Path, required=True, help="CMake project to build")
    parser.add_argument("--work", type=Path, required=True, help="work folder, emptied at the start")
    parser.add_argument("--cppmcache", required=True, help="cppmcache executable to measure")
    parser.add_argument("--ccache", default=shutil.which("ccache"), help="ccache executable (default: the one on the PATH)")
    parser.add_argument("--edit", required=True, help="source file to modify, relative to --source")
    parser.add_argument("--label", default="", help="report title (default: name of the source folder)")
    parser.add_argument("--build-type", default="Release")
    parser.add_argument("--jobs", type=int, default=0, help="number of parallel jobs (default: Ninja's)")
    parser.add_argument("--cmake-arg", action="append", default=[], help="additional argument for cmake, repeatable")
    parser.add_argument("--skip", action="append", default=[], choices=["none", "ccache"], help="launcher not to measure")
    parser.add_argument("--output-json", type=Path)
    parser.add_argument("--output-markdown", type=Path)
    args = parser.parse_args()

    cppmcache = str(Path(args.cppmcache).resolve())
    work = args.work.resolve()
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)
    (work / "ccache.conf").write_text("")

    # Copy of the sources: the script modifies a file, and all the launchers see the same paths.
    source = work / "source"
    shutil.copytree(args.source.resolve(), source, symlinks=True,
                    ignore=shutil.ignore_patterns("build*", ".git", "out", "__pycache__"))
    if not (source / args.edit).is_file():
        sys.exit(f"--edit: {args.edit} is not a file of the project")
    # The dependencies fetched by CPM are downloaded only once.
    os.environ.setdefault("CPM_SOURCE_CACHE", str(work / "cpm"))

    launchers = [Launcher("none", None, work)]
    if args.ccache:
        launchers.append(Launcher("ccache", args.ccache, work))
    else:
        print("ccache not found: it is left out of the comparison", file=sys.stderr)
    launchers.append(Launcher("cppmcache", cppmcache, work))
    launchers = [launcher for launcher in launchers if launcher.name not in args.skip]

    report = {"label": args.label or args.source.resolve().name, "edit": args.edit, "results": {}}
    for launcher in launchers:
        print(f"measuring {launcher.name}…", file=sys.stderr, flush=True)
        report["results"][launcher.name] = measure(launcher, source, work, args)

    text = markdown(report)
    print(text)
    if args.output_markdown:
        args.output_markdown.write_text(text)
    if args.output_json:
        args.output_json.write_text(json.dumps(report, indent=2) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
