#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""DiabloHFViewer release packager / publisher.

Builds a Windows release zip (exe + MinGW runtime + web frontend + optional
filename dictionary), writes a SHA-256 checksum, optionally tags the commit and
publishes the archive as a binary asset on BOTH remotes:

  * GitHub  (origin)  -> Releases, via the REST API
  * Gitee   (gitee)   -> 发行版 (Releases), via the v5 API

No game assets are ever packaged: the script refuses to include *.mpq archives
and only picks the files listed in ``build_file_list()``.

Usage (PowerShell):

    # 1) preview: package + checksum only, no git/network side effects
    python scripts/release.py --version v0.1.0 --dry-run

    # 2) anything that talks to the remotes needs tokens
    $env:GITHUB_TOKEN = "ghp_..."        # repo / Contents:write
    $env:GITEE_TOKEN  = "..."            # Gitee 私人令牌 (projects scope)
    python scripts/release.py --version v0.1.0

    # publish only one side, or reuse an already built zip
    python scripts/release.py --version v0.1.0 --skip-gitee
    python scripts/release.py --version v0.1.0 --no-package

Notes:
  * Both remote URLs are read from ``git remote get-url``; no repository name
    is hard-coded, so renaming/forking keeps working.
  * ``--dry-run`` never pushes tags and never calls the HTTP APIs.
  * Re-running for an existing tag is safe: the GitHub/Gitee release is reused
    and a same-named asset is replaced.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import urllib.error
import urllib.parse
import urllib.request
import zipfile
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SEP = "=" * 72

# DLLs that a MinGW build may import at runtime; the first match in the PE
# import table tells us what has to travel next to the exe.
KNOWN_RUNTIME_DLLS = [
    "libwinpthread-1.dll",
    "libgcc_s_seh-1.dll",
    "libgcc_s_dw2-1.dll",
    "libstdc++-6.dll",
    "libssp-0.dll",
    "zlib1.dll",
]

COMPLIANCE_FOOTER = (
    "\n---\n\n"
    "**本发布包不包含任何游戏资源。** 你必须自行持有《暗黑破坏神：地狱火》"
    "（Diablo: Hellfire）的官方正版文件（含 `DIABDAT.MPQ`）；本包不分发、"
    "不托管、不链接任何 MPQ / 精灵 / 调色板 / 音频。"
    "MIT 许可证只覆盖本项目代码（见 `LICENSE`）。\n"
)

START_BAT = (
    "@echo off\r\n"
    "rem DiabloHFViewer launcher - serves the viewer on http://localhost:18765/\r\n"
    "setlocal\r\n"
    "cd /d \"%~dp0\"\r\n"
    "start \"dhfviewer\" /min cmd /c \"timeout /t 2 >nul & explorer http://localhost:18765/\"\r\n"
    "mcp-server.exe --ws 18765\r\n"
    "if errorlevel 1 pause\r\n"
)

VERSION_FILE_TMPL = (
    "DiabloHFViewer {version}\r\n"
    "commit: {commit}\r\n"
    "built:  {built}\r\n"
    "port:   18765 (run start.bat)\r\n"
)


class ReleaseError(RuntimeError):
    pass


# --------------------------------------------------------------------------- #
# small helpers
# --------------------------------------------------------------------------- #
def log(msg: str) -> None:
    print(msg, flush=True)


def run(cmd: list[str], cwd: Path = ROOT, check: bool = True) -> str:
    proc = subprocess.run(
        cmd, cwd=str(cwd), capture_output=True, text=True, encoding="utf-8", errors="replace"
    )
    if check and proc.returncode != 0:
        raise ReleaseError(
            "command failed: %s\n%s\n%s" % (" ".join(cmd), proc.stdout.strip(), proc.stderr.strip())
        )
    return proc.stdout.strip()


def git(*args: str) -> str:
    return run(["git", *args])


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def parse_repo(url: str) -> tuple[str, str, str]:
    """Return (host, owner, repo) from any git remote URL shape."""
    u = url.strip()
    if u.startswith("git@"):
        host, _, path = u[4:].partition(":")
    else:
        parsed = urllib.parse.urlparse(u)
        host = parsed.hostname or ""
        path = parsed.path.lstrip("/")
    path = path.strip("/")
    if path.endswith(".git"):
        path = path[:-4]
    parts = [p for p in path.split("/") if p]
    if len(parts) < 2:
        raise ReleaseError("cannot parse remote URL: %s" % url)
    return host, parts[0], "/".join(parts[1:])


def remote_repo(name: str) -> tuple[str, str, str] | None:
    proc = subprocess.run(
        ["git", "remote", "get-url", name],
        cwd=str(ROOT),
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    if proc.returncode != 0:
        return None
    return parse_repo(proc.stdout.strip())


def http_json(url: str, token: str | None, method: str = "GET", payload=None,
              headers: dict | None = None):
    data = None
    hdrs = {"User-Agent": "dhf-release-script", "Accept": "application/json"}
    if payload is not None:
        data = json.dumps(payload).encode("utf-8")
        hdrs["Content-Type"] = "application/json"
    if token:
        hdrs["Authorization"] = "token %s" % token
    if headers:
        hdrs.update(headers)
    req = urllib.request.Request(url, data=data, headers=hdrs, method=method)
    try:
        with urllib.request.urlopen(req, timeout=120) as resp:
            body = resp.read().decode("utf-8", "replace")
            return resp.status, (json.loads(body) if body.strip() else None)
    except urllib.error.HTTPError as e:
        body = e.read().decode("utf-8", "replace")
        return e.code, {"error": body}


# --------------------------------------------------------------------------- #
# package assembly
# --------------------------------------------------------------------------- #
def pe_imports(exe: Path) -> list[str]:
    """Return the DLL names from the PE import directory (works for x86/x64)."""
    import struct

    data = exe.read_bytes()
    if data[:2] != b"MZ":
        raise ReleaseError("%s is not a PE image" % exe)
    e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
    if data[e_lfanew:e_lfanew + 4] != b"PE\0\0":
        raise ReleaseError("%s has no PE signature" % exe)
    coff = e_lfanew + 4
    num_sections = struct.unpack_from("<H", data, coff + 2)[0]
    opt_size = struct.unpack_from("<H", data, coff + 16)[0]
    opt = coff + 20
    magic = struct.unpack_from("<H", data, opt)[0]
    if magic == 0x20B:       # PE32+
        image_base_off, data_dir_off = opt + 24, opt + 112
    elif magic == 0x10B:     # PE32
        image_base_off, data_dir_off = opt + 28, opt + 96
    else:
        raise ReleaseError("unsupported PE optional-header magic 0x%x" % magic)
    import_rva, import_size = struct.unpack_from("<II", data, data_dir_off + 8)

    sections = []
    sec = opt + opt_size
    for i in range(num_sections):
        off = sec + i * 40
        vsize, vaddr, raw_size, raw_ptr = struct.unpack_from("<IIII", data, off + 8)
        sections.append((vaddr, max(vsize, raw_size), raw_ptr))

    def rva_to_off(rva: int) -> int:
        for vaddr, size, raw_ptr in sections:
            if vaddr <= rva < vaddr + size:
                return raw_ptr + (rva - vaddr)
        raise ReleaseError("RVA 0x%x outside all sections" % rva)

    names: list[str] = []
    if not import_rva:
        return names
    pos = rva_to_off(import_rva)
    while True:
        entry = data[pos:pos + 20]
        if len(entry) < 20 or entry == b"\0" * 20:
            break
        name_rva = struct.unpack_from("<I", entry, 12)[0]
        if not name_rva:
            break
        off = rva_to_off(name_rva)
        end = data.find(b"\0", off)
        names.append(data[off:end].decode("ascii", "replace"))
        pos += 20
    return names


def find_runtime_dlls(exe: Path, dll_dir: Path | None) -> list[Path]:
    imported = {n.lower() for n in pe_imports(exe)}
    needed = [name for name in KNOWN_RUNTIME_DLLS if name.lower() in imported]

    search: list[Path] = []
    if dll_dir:
        search.append(dll_dir)
    env_bin = os.environ.get("MINGW_BIN")
    if env_bin:
        search.append(Path(env_bin))
    gcc = shutil.which("gcc")
    if gcc:
        search.append(Path(gcc).resolve().parent)
    search.append(Path(r"D:/develop/mingw64/bin"))
    for entry in os.environ.get("PATH", "").split(os.pathsep):
        if entry:
            search.append(Path(entry))

    found: list[Path] = []
    for name in needed:
        hit = next((d / name for d in search if (d / name).is_file()), None)
        if hit is None:
            raise ReleaseError(
                "mcp-server.exe imports %s but it was not found; pass --dll-dir "
                "pointing at your MinGW bin/ directory" % name
            )
        found.append(hit)
    return found


def build_file_list(args) -> list[tuple[Path, str]]:
    """Return (source path, archive name) pairs; validates compliance rules."""
    exe = Path(args.exe) if args.exe else ROOT / "build-mcp" / "mcp-server.exe"
    if not exe.is_file():
        raise ReleaseError(
            "missing %s - build first: cmake --build build-mcp --target mcp-server" % exe
        )
    entries: list[tuple[Path, str]] = [(exe, "mcp-server.exe")]

    for dll in find_runtime_dlls(exe, Path(args.dll_dir) if args.dll_dir else None):
        entries.append((dll, dll.name))

    web = ROOT / "web"
    if not web.is_dir():
        raise ReleaseError("missing web/ frontend directory")
    for p in sorted(web.rglob("*")):
        if p.is_file():
            entries.append((p, p.relative_to(ROOT).as_posix()))

    dict_path = ROOT / "mpq-info" / "dict" / "recovered.json"
    if not args.no_dict:
        if dict_path.is_file():
            entries.append((dict_path, "mpq-info/dict/recovered.json"))
        else:
            log("[warn] mpq-info/dict/recovered.json not found: sprite.list will ask "
                "for a dict; pass --no-dict to silence")

    for extra in ("LICENSE", "README.md"):
        p = ROOT / extra
        if p.is_file():
            entries.append((p, extra))

    for src, arc in entries:
        if src.suffix.lower() in (".mpq", ".mpq5"):
            raise ReleaseError("refusing to package game archive: %s" % src)
    return entries


def create_package(args) -> tuple[Path, Path, str]:
    version = args.version
    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    zip_path = out_dir / ("DiabloHFViewer-%s-win64.zip" % version)

    entries = build_file_list(args)
    commit = git("rev-parse", "--short", "HEAD")
    built = datetime.now(timezone.utc).strftime("%Y-%m-%d %H:%M UTC")

    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as zf:
        for src, arc in entries:
            zf.write(src, arc)
        zf.writestr("VERSION.txt", VERSION_FILE_TMPL.format(version=version, commit=commit, built=built))
        zf.writestr("start.bat", START_BAT)

    digest = sha256_file(zip_path)
    sha_path = zip_path.with_suffix(".zip.sha256")
    sha_path.write_text(
        "%s  %s\n" % (digest, zip_path.name), encoding="ascii", newline="\n"
    )
    return zip_path, sha_path, digest


# --------------------------------------------------------------------------- #
# git tag
# --------------------------------------------------------------------------- #
def ensure_tag(version: str, push: bool, create: bool = True) -> None:
    if not re.fullmatch(r"v\d+\.\d+\.\d+([-.][0-9A-Za-z.]+)?", version):
        raise ReleaseError("version must look like v1.2.3 (got %r)" % version)

    existing = run(["git", "tag", "-l", version])
    if existing:
        log("[git] tag %s already exists locally, reusing" % version)
    elif create:
        git("tag", "-a", version, "-m", "DiabloHFViewer %s" % version)
        log("[git] created annotated tag %s" % version)
    else:
        log("[git] --dry-run: would create annotated tag %s (not created)" % version)
        return

    if not push:
        log("[git] --dry-run / --no-push: tags not pushed")
        return
    for remote in ("origin", "gitee"):
        if remote_repo(remote) is None:
            log("[git] remote %r missing, skipped" % remote)
            continue
        git("push", remote, "refs/tags/%s" % version)
        log("[git] pushed tag %s -> %s" % (version, remote))


# --------------------------------------------------------------------------- #
# release notes
# --------------------------------------------------------------------------- #
def default_notes(version: str) -> str:
    prev = run(["git", "describe", "--tags", "--abbrev=0", "HEAD^"], check=False)
    rng = "%s..HEAD" % prev if prev else "HEAD"
    log_text = run(
        ["git", "log", "--no-merges", "--pretty=format:  - %s", "--max-count=40", rng],
        check=False,
    )
    return (
        "## DiabloHFViewer %s\n\n"
        "Windows x64 portable build of the Diablo 1 / Hellfire MPQ research viewer.\n\n"
        "### 使用 / Usage\n\n"
        "1. 解压到任意目录；\n"
        "2. 双击 `start.bat`（等价于 `mcp-server.exe --ws 18765`）；\n"
        "3. 浏览器会自动打开 <http://localhost:18765/>，首次使用点右上角「mpq配置」"
        "选择你自己的正版 MPQ 目录。\n\n"
        "### 本版改动 / Changes\n\n%s\n" % (version, log_text or "  - (no commits)")
    ) + COMPLIANCE_FOOTER


# --------------------------------------------------------------------------- #
# GitHub
# --------------------------------------------------------------------------- #
def github_publish(version: str, notes: str, assets: list[Path], token: str, dry: bool) -> str | None:
    remote = remote_repo("origin")
    if remote is None:
        log("[github] remote 'origin' missing, skipped")
        return None
    _, owner, repo = remote
    api = "https://api.github.com/repos/%s/%s" % (owner, repo)
    if dry:
        log("[github] would publish release %s with %s" % (version, [a.name for a in assets]))
        return None

    status, body = http_json("%s/releases/tags/%s" % (api, version), token)
    if status == 200 and isinstance(body, dict):
        rel = body
        log("[github] reusing existing release id=%s" % rel["id"])
    else:
        status, rel = http_json(
            "%s/releases" % api, token, method="POST",
            payload={
                "tag_name": version,
                "name": "DiabloHFViewer %s" % version,
                "body": notes,
                "draft": False,
                "prerelease": "-" in version,
            },
        )
        if status not in (200, 201):
            raise ReleaseError("GitHub release create failed (%s): %s" % (status, rel))
        log("[github] created release id=%s" % rel["id"])

    release_id = rel["id"]
    for path in assets:
        upload = "https://uploads.github.com/repos/%s/%s/releases/%s/assets?name=%s" % (
            owner, repo, release_id, urllib.parse.quote(path.name),
        )
        data = path.read_bytes()
        req = urllib.request.Request(
            upload,
            data=data,
            method="POST",
            headers={
                "Authorization": "token %s" % token,
                "Content-Type": "application/octet-stream",
                "User-Agent": "dhf-release-script",
            },
        )
        try:
            with urllib.request.urlopen(req, timeout=600) as resp:
                log("[github] uploaded %s (%s bytes)" % (path.name, len(data)))
        except urllib.error.HTTPError as e:
            msg = e.read().decode("utf-8", "replace")
            if e.code == 422 and "already_exists" in msg:
                log("[github] asset %s already exists, deleting and retrying" % path.name)
                _, assets_list = http_json("%s/releases/%s/assets" % (api, release_id), token)
                for a in assets_list or []:
                    if a.get("name") == path.name:
                        http_json(
                            "%s/releases/assets/%s" % (api, a["id"]), token, method="DELETE"
                        )
                with urllib.request.urlopen(req, timeout=600) as resp:
                    log("[github] uploaded %s (replaced)" % path.name)
            else:
                raise ReleaseError("GitHub asset upload failed (%s): %s" % (e.code, msg))

    return "https://github.com/%s/%s/releases/tag/%s" % (owner, repo, version)


# --------------------------------------------------------------------------- #
# Gitee
# --------------------------------------------------------------------------- #
def gitee_multipart(url: str, field: str, path: Path, token: str) -> tuple[int, dict]:
    boundary = "----dhfrelease%s" % os.urandom(8).hex()
    head = (
        "--%s\r\nContent-Disposition: form-data; name=\"access_token\"\r\n\r\n%s\r\n"
        "--%s\r\nContent-Disposition: form-data; name=\"file\"; filename=\"%s\"\r\n"
        "Content-Type: application/octet-stream\r\n\r\n" % (boundary, token, boundary, path.name)
    ).encode("utf-8")
    tail = ("\r\n--%s--\r\n" % boundary).encode("utf-8")
    data = head + path.read_bytes() + tail
    req = urllib.request.Request(
        url,
        data=data,
        method="POST",
        headers={
            "Content-Type": "multipart/form-data; boundary=%s" % boundary,
            "User-Agent": "dhf-release-script",
        },
    )
    try:
        with urllib.request.urlopen(req, timeout=600) as resp:
            return resp.status, json.loads(resp.read().decode("utf-8", "replace") or "{}")
    except urllib.error.HTTPError as e:
        return e.code, {"error": e.read().decode("utf-8", "replace")}


def gitee_publish(version: str, notes: str, assets: list[Path], token: str, dry: bool) -> str | None:
    remote = remote_repo("gitee")
    if remote is None:
        log("[gitee] remote 'gitee' missing, skipped")
        return None
    _, owner, repo = remote
    api = "https://gitee.com/api/v5/repos/%s/%s" % (owner, repo)
    if dry:
        log("[gitee] would publish release %s with %s" % (version, [a.name for a in assets]))
        return None

    status, rel = http_json(
        "%s/releases?access_token=%s" % (api, urllib.parse.quote(token)),
        None,
        method="POST",
        payload={
            "tag_name": version,
            "target_commitish": "master",
            "name": "DiabloHFViewer %s" % version,
            "body": notes,
            "prerelease": "-" in version,
        },
    )
    if status not in (200, 201):
        # Gitee returns 400 when a release for the tag already exists.
        status2, existing = http_json("%s/releases/tags/%s" % (api, version), None)
        if status2 == 200 and isinstance(existing, dict):
            rel = existing
            log("[gitee] reusing existing release id=%s" % rel.get("id"))
        else:
            raise ReleaseError("Gitee release create failed (%s): %s" % (status, rel))
    else:
        log("[gitee] created release id=%s" % rel.get("id"))

    release_id = rel.get("id")
    if not release_id:
        raise ReleaseError("Gitee response has no release id: %s" % rel)

    for path in assets:
        status, resp = gitee_multipart(
            "%s/releases/%s/attach_files" % (api, release_id), "file", path, token
        )
        if status not in (200, 201):
            raise ReleaseError("Gitee attach failed (%s): %s" % (status, resp))
        log("[gitee] uploaded %s" % path.name)

    return "https://gitee.com/%s/%s/releases/tag/%s" % (owner, repo, version)


# --------------------------------------------------------------------------- #
def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Package and publish a DiabloHFViewer release.")
    ap.add_argument("--version", required=True, help="version tag, e.g. v0.1.0")
    ap.add_argument("--notes-file", help="release notes markdown (default: auto from git log)")
    ap.add_argument("--out-dir", default=str(ROOT / "dist"), help="artifact output dir (default: dist/)")
    ap.add_argument("--exe", help="path to mcp-server.exe (default: build-mcp/mcp-server.exe)")
    ap.add_argument("--dll-dir", help="MinGW bin/ directory holding runtime DLLs")
    ap.add_argument("--no-dict", action="store_true", help="do not package mpq-info/dict/recovered.json")
    ap.add_argument("--no-package", action="store_true", help="reuse the existing zip in --out-dir")
    ap.add_argument("--no-push", action="store_true", help="do not push tags")
    ap.add_argument("--skip-github", action="store_true")
    ap.add_argument("--skip-gitee", action="store_true")
    ap.add_argument("--dry-run", action="store_true", help="package + checksum only, no git/network")
    ap.add_argument("--allow-dirty", action="store_true", help="allow packing a dirty work tree")
    args = ap.parse_args(argv)

    log(SEP)
    log("DiabloHFViewer release  version=%s" % args.version)
    log(SEP)

    dirty = run(["git", "status", "--porcelain"], check=False)
    if dirty and not args.allow_dirty:
        raise ReleaseError(
            "work tree is dirty; commit first or pass --allow-dirty:\n%s" % dirty
        )

    zip_path = Path(args.out_dir) / ("DiabloHFViewer-%s-win64.zip" % args.version)
    sha_path = zip_path.with_suffix(".zip.sha256")
    if args.no_package:
        if not zip_path.is_file():
            raise ReleaseError("--no-package given but %s is missing" % zip_path)
        digest = sha256_file(zip_path)
        if not sha_path.is_file():
            sha_path.write_text("%s  %s\n" % (digest, zip_path.name), encoding="ascii", newline="\n")
        log("[pkg] reusing %s" % zip_path)
    else:
        zip_path, sha_path, digest = create_package(args)
        log("[pkg] %s (%.1f KiB)" % (zip_path.name, zip_path.stat().st_size / 1024))
        log("[pkg] sha256 %s" % digest)

    notes = (
        Path(args.notes_file).read_text(encoding="utf-8")
        if args.notes_file else default_notes(args.version)
    )

    ensure_tag(args.version, push=not (args.dry_run or args.no_push), create=not args.dry_run)

    assets = [zip_path, sha_path]
    if not args.dry_run and not args.skip_github:
        token = os.environ.get("GITHUB_TOKEN", "").strip()
        if not token:
            raise ReleaseError("GITHUB_TOKEN is not set (or use --skip-github)")
    else:
        token = ""
    gh_url = None if args.skip_github or args.dry_run else github_publish(
        args.version, notes, assets, token, False
    )
    if args.dry_run and not args.skip_github:
        github_publish(args.version, notes, assets, "", True)

    if not args.dry_run and not args.skip_gitee:
        gitee_token = os.environ.get("GITEE_TOKEN", "").strip()
        if not gitee_token:
            raise ReleaseError("GITEE_TOKEN is not set (or use --skip-gitee)")
    else:
        gitee_token = ""
    gitee_url = None if args.skip_gitee or args.dry_run else gitee_publish(
        args.version, notes, assets, gitee_token, False
    )
    if args.dry_run and not args.skip_gitee:
        gitee_publish(args.version, notes, assets, "", True)

    log(SEP)
    if args.dry_run:
        log("dry-run complete - nothing pushed, no release created")
    else:
        if gh_url:
            log("GitHub release: %s" % gh_url)
        if gitee_url:
            log("Gitee  release: %s" % gitee_url)
    log("artifact: %s" % zip_path)
    log(SEP)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except ReleaseError as exc:
        print("\n[error] %s" % exc, file=sys.stderr)
        sys.exit(1)
