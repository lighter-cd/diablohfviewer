#!/usr/bin/env bash
# Build the StormLib static library (MinGW-w64) used by the MCP server backend.
#
# The MCP server (src/) links 3rdparty/StormLib/build/libStormLib.a. This script
# produces exactly that. It does NOT build any CLI — the old standalone mpqextract
# utility has been retired (its features live in the mpq.* MCP tools).
#
# Prerequisites:
#   - MinGW-w64 g++ on PATH (e.g. D:/develop/mingw64/bin)  -> tested with 16.2.0
#   - StormLib vendored as the 3rdparty/StormLib git submodule
#
# Usage:
#   bash build-stormlib.sh
#
# Result:
#   3rdparty/StormLib/build/libStormLib.a

set -euo pipefail
cd "$(dirname "$0")"   # project root (this script lives at the repo root)

MINGW_BIN="${MINGW_BIN:-/d/develop/mingw64/bin}"
export PATH="$MINGW_BIN:$PATH"

SL=3rdparty/StormLib
BUILD="$SL/build"

echo ">> Building StormLib static lib"
mkdir -p "$BUILD"
cmake -G "MinGW Makefiles" \
  -DCMAKE_BUILD_TYPE=Release \
  -DSTORM_USE_BUNDLED_LIBRARIES=ON \
  -DSTORM_BUILD_TESTS=OFF \
  -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ \
  -S "$SL" -B "$BUILD"
cmake --build "$BUILD" --target storm -j4

echo ">> Done: $(ls -la "$BUILD/libStormLib.a" | awk '{print $5, $NF}')"
