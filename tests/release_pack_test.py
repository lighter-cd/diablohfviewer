#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Regression test for scripts/release.py (packaging only, no network).

Runs the packager in --dry-run mode into a temporary directory and checks the
release archive shape, checksum file and compliance rule (no game archives).

Usage:
    python tests/release_pack_test.py

Exit codes: 0 = PASS (or SKIP when build-mcp/mcp-server.exe is absent),
1 = FAIL.
"""

from __future__ import annotations

import hashlib
import shutil
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
EXE = ROOT / "build-mcp" / "mcp-server.exe"
VERSION = "v0.0.0-test"

failures: list[str] = []
checks = 0


def check(cond: bool, msg: str) -> None:
    global checks
    checks += 1
    if not cond:
        failures.append(msg)


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def main() -> int:
    if not EXE.is_file():
        print("SKIP: %s not found (build mcp-server first)" % EXE)
        return 0

    out_dir = Path(tempfile.mkdtemp(prefix="dhf_release_test_"))
    try:
        proc = subprocess.run(
            [
                sys.executable,
                str(ROOT / "scripts" / "release.py"),
                "--version", VERSION,
                "--dry-run",
                "--allow-dirty",
                "--out-dir", str(out_dir),
            ],
            cwd=str(ROOT),
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
        )
        check(proc.returncode == 0, "release.py exited %s: %s" % (proc.returncode, proc.stderr[-400:]))

        zip_path = out_dir / ("DiabloHFViewer-%s-win64.zip" % VERSION)
        sha_path = zip_path.with_suffix(".zip.sha256")
        check(zip_path.is_file(), "release zip not created")
        check(sha_path.is_file(), "checksum file not created")
        if not zip_path.is_file():
            return report()

        with zipfile.ZipFile(zip_path) as zf:
            names = zf.namelist()
            start_bat = zf.read("start.bat").decode("utf-8", "replace")
            version_txt = zf.read("VERSION.txt").decode("utf-8", "replace")

        required = [
            "mcp-server.exe",
            "web/index.html",
            "web/src/main.js",
            "web/styles.css",
            "LICENSE",
            "README.md",
            "start.bat",
            "VERSION.txt",
        ]
        for name in required:
            check(name in names, "zip is missing %s" % name)

        check("libwinpthread-1.dll" in names, "zip is missing the MinGW pthread DLL")

        # Every file of the tracked frontend must be inside the package.
        for src in sorted((ROOT / "web").rglob("*")):
            if src.is_file():
                arc = src.relative_to(ROOT).as_posix()
                check(arc in names, "frontend file not packaged: %s" % arc)

        # Compliance: never ship a game archive.
        bad = [n for n in names if n.lower().endswith((".mpq", ".mpq5"))]
        check(not bad, "game archive leaked into the release: %s" % bad)

        check("--ws 18765" in start_bat, "start.bat does not launch the WS bridge")
        check(VERSION in version_txt, "VERSION.txt has no version string")

        digest = sha256(zip_path)
        line = sha_path.read_text(encoding="ascii").split()
        check(line and line[0] == digest, "sha256 file does not match the archive")
        check(len(line) == 2 and line[1] == zip_path.name, "sha256 file names the wrong archive")

        # --no-dict must drop the optional filename dictionary.
        out2 = out_dir / "nodict"
        proc2 = subprocess.run(
            [
                sys.executable,
                str(ROOT / "scripts" / "release.py"),
                "--version", VERSION,
                "--dry-run",
                "--allow-dirty",
                "--no-dict",
                "--out-dir", str(out2),
            ],
            cwd=str(ROOT),
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
        )
        check(proc2.returncode == 0, "--no-dict run failed: %s" % proc2.stderr[-400:])
        z2 = out2 / ("DiabloHFViewer-%s-win64.zip" % VERSION)
        if z2.is_file():
            with zipfile.ZipFile(z2) as zf2:
                check(
                    "mpq-info/dict/recovered.json" not in zf2.namelist(),
                    "--no-dict still packaged recovered.json",
                )
        else:
            check(False, "--no-dict release zip not created")
    finally:
        shutil.rmtree(out_dir, ignore_errors=True)

    return report()


def report() -> int:
    if failures:
        print("FAIL: %d of %d checks failed" % (len(failures), checks))
        for f in failures:
            print("  - " + f)
        return 1
    print("PASS: %d checks" % checks)
    return 0


if __name__ == "__main__":
    sys.exit(main())
