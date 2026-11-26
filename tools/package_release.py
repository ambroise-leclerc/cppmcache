#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Ambroise Leclerc
"""Packages a cppmcache binary for a release and tests the package.

The archive is named cppmcache-<version>-<platform> and holds bin/cppmcache, the licence and the
README. It is then unpacked elsewhere, and the unpacked binary compiles the modules project of the
integration tests through the given compiler: what is published is what was tested.

    tools/package_release.py --binary build/cppmcache --platform linux-x86_64 --compiler g++ --family gcc

When GITHUB_REF_NAME names a version tag, it must match the version of the project.
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def project_version() -> str:
    match = re.search(r"project\(cppmcache VERSION ([0-9]+\.[0-9]+\.[0-9]+)", (ROOT / "CMakeLists.txt").read_text())
    if not match:
        sys.exit("cannot read the version from CMakeLists.txt")
    return match.group(1)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--binary", type=Path, required=True, help="the cppmcache executable to package")
    parser.add_argument("--platform", required=True, help="platform name in the archive name, such as linux-x86_64")
    parser.add_argument("--compiler", required=True, help="compiler the packaged binary is tested with")
    parser.add_argument("--family", required=True, choices=["gcc", "clang", "msvc"], help="family of that compiler")
    parser.add_argument("--output", type=Path, default=ROOT / "dist", help="directory that receives the archive")
    args = parser.parse_args()

    version = project_version()
    tag = os.environ.get("GITHUB_REF_NAME", "")
    if os.environ.get("GITHUB_REF_TYPE") == "tag" and tag != f"v{version}":
        sys.exit(f"tag {tag} does not match the version of the project, {version}: update project(... VERSION ...) first")

    binary = args.binary.resolve()
    reported = subprocess.run([str(binary), "--version"], capture_output=True, text=True, check=True).stdout.strip()
    if reported != f"cppmcache version {version}":
        sys.exit(f"the binary reports '{reported}', expected version {version}")

    windows = args.platform.startswith("windows")
    name = f"cppmcache-{version}-{args.platform}"
    args.output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as staging_root:
        staging = Path(staging_root) / name
        (staging / "bin").mkdir(parents=True)
        shutil.copy2(binary, staging / "bin" / binary.name)
        for document in ("LICENSE", "README.md"):
            shutil.copy2(ROOT / document, staging / document)
        files = sorted(path for path in staging.rglob("*") if path.is_file())
        if windows:
            archive = args.output / f"{name}.zip"
            with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as package:
                for path in files:
                    package.write(path, path.relative_to(staging_root).as_posix())
        else:
            archive = args.output / f"{name}.tar.gz"
            with tarfile.open(archive, "w:gz") as package:
                for path in files:
                    package.add(path, path.relative_to(staging_root).as_posix())
    print(f"packaged {archive} ({archive.stat().st_size} bytes)")

    # The test runs the binary as a user gets it: unpacked from the archive, away from the build tree.
    with tempfile.TemporaryDirectory() as unpack_root:
        shutil.unpack_archive(archive, unpack_root)
        unpacked = Path(unpack_root) / name / "bin" / binary.name
        if not windows:
            unpacked.chmod(0o755)
        compiler = shutil.which(args.compiler) or args.compiler
        for variant in ("plain", "cmake"):
            command = [
                "cmake", f"-DCPPMCACHE={unpacked.as_posix()}", f"-DCOMPILER={Path(compiler).as_posix()}", f"-DFAMILY={args.family}",
                f"-DVARIANT={variant}", f"-DWORK_DIR={(Path(unpack_root) / ('work-' + variant)).as_posix()}",
                "-P", str(ROOT / "tests" / "integration" / "Modules.cmake"),
            ]
            subprocess.run(command, check=True)
    print(f"{name}: the packaged binary passed the modules integration test")
    return 0


if __name__ == "__main__":
    sys.exit(main())
