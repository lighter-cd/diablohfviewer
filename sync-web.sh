#!/usr/bin/env bash
# sync-web.sh — 前端“构建/同步”步骤：把 web/ 复制到 build-mcp/web/（mcp-server 实际
# 提供页面的目录），并自动刷新 ?v=<token> 缓存版本号，破除浏览器缓存。
#
# 运行中的 mcp-server 从 build-mcp/web/ 读取页面；只改 web/ 而不执行本脚本，
# 运行时页面不会变化。每次“构建 / 前端更新”都应运行本脚本：
#     ./sync-web.sh
# 它与是否重新编译 C++ 无关：即便只改了前端，也要跑本脚本；C++ 未改动时无需停 exe、也
# 无需重新构建，静态文件落盘后浏览器刷新即可加载（token 已变，必破缓存）。
#
# 仅修改「复制后」的 build-mcp/web 内文件，源 web/ 保持不变（源里的 ?v 仅作占位，
# 浏览器永远从 build-mcp/web 加载）。
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
SRC="$ROOT/web"
DST="$ROOT/build-mcp/web"

# 1) 整体复制（build-mcp/ 整体被 .gitignore 忽略，属构建产物，可直接覆盖）。
rm -rf "$DST"
mkdir -p "$DST"
cp -r "$SRC/." "$DST/"

# 2) 生成新的缓存 token：秒级时间戳，保证每次构建唯一、可破除浏览器缓存。
TOKEN="$(date +%Y%m%d%H%M%S)"

# 3) 仅在 build 副本内刷新 ?v= token（index.html 引用的 css/js，以及 main.js 内
#    import 的 client/catalog/viewer；level.html/level.js 同法处理）。
for f in $(find "$DST" -type f \( -name '*.html' -o -name '*.js' \)); do
  sed -i -E "s/\?v=[A-Za-z0-9]+/?v=$TOKEN/g" "$f"
done

echo "synced web -> build-mcp/web  (cache token: $TOKEN)"
