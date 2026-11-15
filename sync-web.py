#!/usr/bin/env python3
"""sync-web.py — 前端“构建/同步”步骤（Windows / 无 bash 环境的等价脚本）。

作用与 `sync-web.sh` 完全相同：
  1. 把仓库里的 `web/` 整体复制到 `build-mcp/web/`（mcp-server 实际提供页面的目录）；
  2. 只在复制后的副本里，把所有 `?v=<token>` 刷新为新的时间戳 token（破除浏览器缓存）。

为什么需要这个脚本：Windows 上常常没有 bash（`bash ./sync-web.sh` 会报
`execvpe(/bin/bash) failed`）。**不要**用 PowerShell 的 `Get-Content -Raw` +
`[IO.File]::WriteAllText` 手工替代：PowerShell 5.1 会用系统 ANSI 代码页（中文 Windows
是 GBK）读取无 BOM 的 UTF-8 文件，再以 UTF-8 写出，中文会变成 `mpq閰嶇疆` 这类乱码，
甚至让 JS 出现 `Uncaught SyntaxError: Invalid or unexpected token`（整个模块不执行，
按钮全部失灵）。本脚本全程按**字节**处理，不涉及任何解码，绝不会改动中文内容。

用法（仓库根目录）：
    python sync-web.py
"""

import os
import re
import shutil
import time

ROOT = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(ROOT, "web")
DST = os.path.join(ROOT, "build-mcp", "web")
TOKEN_RE = re.compile(rb"\?v=[A-Za-z0-9]+")


def main() -> int:
    if not os.path.isdir(SRC):
        print(f"error: missing web source dir: {SRC}")
        return 1
    if os.path.isdir(DST):
        shutil.rmtree(DST)
    shutil.copytree(SRC, DST)

    token = time.strftime("%Y%m%d%H%M%S")
    token_b = ("?v=" + token).encode("ascii")
    touched = 0
    for dirpath, _, files in os.walk(DST):
        for name in files:
            if not (name.endswith(".html") or name.endswith(".js")):
                continue
            path = os.path.join(dirpath, name)
            with open(path, "rb") as f:
                data = f.read()
            new = TOKEN_RE.sub(token_b, data)
            if new != data:
                with open(path, "wb") as f:
                    f.write(new)
                touched += 1
    print(f"synced web -> build-mcp/web  (cache token: {token}, {touched} files updated)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
