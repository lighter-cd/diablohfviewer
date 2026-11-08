// src/tools/map_tools.cpp
//
// map.build — decode a Diablo 1 / Hellfire level (TIL / MIN / DUN / SOL / level
// CEL) and render a complete isometric map (tiles) plus collision / lighting /
// transparency metadata. Writes the full map PNG and a downscaled preview PNG to
// out/maps/, a sidecar metadata JSON, and returns a summary + base64 preview.

#include "tools/registry.h"
#include "tools/base64.h"
#include "engine/archive.h"
#include "decoders/level.h"
#include "decoders/palette.h"
#include "decoders/png.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace {

// Pull a required string field from params; throw if missing / wrong type.
std::string reqStr(const mcp::json& p, const char* key) {
    auto it = p.find(key);
    if (it == p.end() || !it->is_string())
        throw std::invalid_argument(std::string("missing string param: ") + key);
    return it->get<std::string>();
}

// Open an archive, throwing a descriptive error on failure.
engine::Archive openOrThrow(const std::string& path) {
    engine::Archive a;
    if (!a.open(path))
        throw std::runtime_error("cannot open archive: " + path);
    return a;
}

// Try to extract `name` from any archive in `archives`; returns true on success.
bool extractFromArchives(const std::vector<std::string>& archives,
                         const std::string& name,
                         std::vector<uint8_t>& out) {
    for (const auto& a : archives) {
        engine::Archive arc;
        if (!arc.open(a))
            continue;
        if (arc.extract(name, out))
            return true;
    }
    return false;
}

// Downscale an RGBA image by integer `step` using nearest-neighbor, fitting it
// within `maxW` px wide. Returns the new rgba; sets outW/outH.
std::vector<uint8_t> downscale(const std::vector<uint8_t>& rgba, int w, int h,
                               int maxW, int& outW, int& outH) {
    if (w <= maxW || maxW <= 0) {
        outW = w;
        outH = h;
        return rgba;
    }
    const int step = (w + maxW - 1) / maxW; // >= 1
    outW = std::max(1, w / step);
    outH = std::max(1, h / step);
    std::vector<uint8_t> out(static_cast<size_t>(outW) * outH * 4);
    for (int y = 0; y < outH; ++y) {
        const int sy = y * step;
        for (int x = 0; x < outW; ++x) {
            const int sx = x * step;
            const uint8_t* p = rgba.data() + (static_cast<size_t>(sy) * w + sx) * 4;
            uint8_t* q = out.data() + (static_cast<size_t>(y) * outW + x) * 4;
            std::memcpy(q, p, 4);
        }
    }
    return out;
}

std::string sanitize(const std::string& name) {
    std::string s = name;
    for (char& c : s)
        if (c == '\\' || c == '/' || c == ':' || c == '.') c = '_';
    return s;
}

mcp::ToolResult toolMapBuild(const mcp::json& params) {
    // Collect the archive search stack.
    std::vector<std::string> archives;
    if (params.contains("archives") && params["archives"].is_array()) {
        for (const auto& a : params["archives"])
            if (a.is_string()) archives.push_back(a.get<std::string>());
    }
    // The explicit `archive` is always searched first.
    if (params.contains("archive") && params["archive"].is_string())
        archives.insert(archives.begin(), params["archive"].get<std::string>());
    if (archives.empty())
        throw std::invalid_argument("provide `archive` (and/or `archives`)");

    const std::string celName = reqStr(params, "cel");
    const std::string tilName = reqStr(params, "til");
    const std::string minName = reqStr(params, "min");
    const std::string solName = reqStr(params, "sol");
    const std::string dunName = reqStr(params, "dun");
    const std::string palName = params.value("palette", std::string("gendata/cut2.pal"));
    const int microLen = params.value("micro_len", 10);
    const int blocks = params.value("blocks", 10);
    const std::string outDir = params.value("out", std::string("out/maps"));

    // Extract + parse all level assets.
    std::vector<uint8_t> celBuf, tilBuf, minBuf, solBuf, dunBuf, palBuf;
    if (!extractFromArchives(archives, celName, celBuf))
        throw std::runtime_error("extract failed (name not found?): " + celName);
    if (!extractFromArchives(archives, tilName, tilBuf))
        throw std::runtime_error("extract failed (name not found?): " + tilName);
    if (!extractFromArchives(archives, minName, minBuf))
        throw std::runtime_error("extract failed (name not found?): " + minName);
    if (!extractFromArchives(archives, solName, solBuf))
        throw std::runtime_error("extract failed (name not found?): " + solName);
    if (!extractFromArchives(archives, dunName, dunBuf))
        throw std::runtime_error("extract failed (name not found?): " + dunName);
    if (!extractFromArchives(archives, palName, palBuf))
        throw std::runtime_error("extract failed (name not found?): " + palName);

    auto megatiles = decoders::loadTil(tilBuf);
    auto micros = decoders::loadMin(minBuf, static_cast<uint8_t>(blocks), static_cast<uint8_t>(microLen));
    auto dun = decoders::loadDun(dunBuf);
    auto solFlags = decoders::loadSol(solBuf);
    auto palRgb = decoders::loadPalette(palBuf);
    if (palRgb.empty())
        throw std::runtime_error("palette parse failed (need >= 256 triplets)");
    std::vector<uint8_t> palRgba = decoders::paletteToRGBA(palRgb);

    if (megatiles.empty())
        throw std::runtime_error("TIL parse failed");
    if (micros.empty())
        throw std::runtime_error("MIN parse failed");
    if (dun.width == 0 || dun.height == 0)
        throw std::runtime_error("DUN parse failed");

    // Build the micro grid (1-based micro indices, 0 = empty) + keep tileLayer.
    int gw = 0, gh = 0;
    std::vector<uint16_t> microGrid;
    decoders::buildMicroGrid(dun, megatiles, microGrid, gw, gh);

    // Build per-frame TileType table, re-encode the level CEL, render the map.
    std::vector<decoders::TileType> frameTypes =
        decoders::buildFrameTypes(micros, microLen);
    decoders::ReencodedLevelCel reencoded = decoders::reencodeLevelCel(celBuf, frameTypes);
    if (reencoded.numFrames == 0)
        throw std::runtime_error("level CEL parse failed");

    decoders::MapRenderResult map = decoders::renderMap(
        reencoded, micros, microGrid, gw, gh, solFlags,
        dun.transparency, dun.tileLayer, static_cast<int>(dun.width),
        static_cast<int>(dun.height), palRgba.data(), microLen);

    if (map.width == 0 || map.height == 0)
        throw std::runtime_error("map render produced empty output");

    // Ensure output directory exists.
    std::error_code ec;
    std::filesystem::create_directories(outDir, ec);

    const std::string base = sanitize(dunName);
    const std::string pngPath = outDir + "/" + base + ".png";
    const std::string previewPath = outDir + "/" + base + "_preview.png";
    const std::string metaPath = outDir + "/" + base + ".json";

    if (!decoders::writePNG(pngPath, static_cast<uint16_t>(map.width),
                            static_cast<uint16_t>(map.height), map.rgba))
        throw std::runtime_error("failed to write PNG: " + pngPath);

    // Downscaled preview PNG + base64.
    int pw = 0, ph = 0;
    std::vector<uint8_t> preview = downscale(map.rgba, map.width, map.height, 400, pw, ph);
    std::vector<uint8_t> previewPngBytes;
    // Encode the preview to bytes: writePNG writes to a file; reuse a temp path.
    if (!decoders::writePNG(previewPath, static_cast<uint16_t>(pw),
                            static_cast<uint16_t>(ph), preview))
        throw std::runtime_error("failed to write preview PNG: " + previewPath);
    {
        std::ifstream f(previewPath, std::ios::binary);
        previewPngBytes.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }
    const std::string previewB64 = mcp::base64::encode(previewPngBytes);

    // Sidecar metadata JSON (full per-cell grids).
    size_t cellCount = static_cast<size_t>(gw) * gh;
    mcp::json solidArr = mcp::json::array(), transArr = mcp::json::array();
    mcp::json blArr = mcp::json::array(), bmArr = mcp::json::array();
    for (size_t i = 0; i < cellCount; ++i) {
        solidArr.push_back(map.solid[i]);
        transArr.push_back(map.transparent[i]);
        blArr.push_back(map.blockLight[i]);
        bmArr.push_back(map.blockMissile[i]);
    }
    mcp::json meta = mcp::json{
        {"grid_width",  gw},
        {"grid_height", gh},
        {"micro_len",   microLen},
        {"frame_count", reencoded.numFrames},
        {"solid",       solidArr},
        {"transparent", transArr},
        {"block_light", blArr},
        {"block_missile", bmArr},
    };
    {
        std::ofstream f(metaPath);
        f << meta.dump();
    }

    // Summary counts.
    size_t solidCount = 0, transCount = 0;
    for (size_t i = 0; i < cellCount; ++i) {
        if (map.solid[i]) ++solidCount;
        if (map.transparent[i]) ++transCount;
    }

    mcp::ToolResult r;
    r.value = mcp::json{
        {"image_path",        pngPath},
        {"preview_base64",     previewB64},
        {"preview_path",       previewPath},
        {"meta_path",          metaPath},
        {"width",              map.width},
        {"height",             map.height},
        {"grid_width",         gw},
        {"grid_height",        gh},
        {"micro_len",          microLen},
        {"frame_count",        reencoded.numFrames},
        {"micro_count",        micros.size()},
        {"solid_count",        solidCount},
        {"transparent_count",   transCount},
    };
    return r;
}

} // namespace

namespace mcp {
namespace tools {

void registerMapTools(ToolRegistry& reg) {
    reg.registerTool(ToolDef{
        "map.build",
        "Decode a Diablo 1 / Hellfire level (TIL/MIN/DUN/SOL/level CEL) and render "
        "a complete isometric map (tiles) plus collision / lighting / transparency "
        "metadata. Writes the map PNG + a downscaled preview PNG + a sidecar JSON of "
        "per-cell flags to out/maps/, and returns a summary with a base64 preview. "
        "Lighting is rendered FullyLit (per-pixel lightmaps are a runtime effect and "
        "are deferred).",
        json{{"type", "object"},
             {"properties", json{
                 {"archive",    json{{"type","string"},
                                     {"description","primary MPQ path"}}},
                 {"archives",   json{
                                     {"type", "array"},
                                     {"items", json{{"type", "string"}}},
                                     {"description", "additional MPQ search paths"}}},
                 {"cel",        json{{"type","string"},{"description","level CEL in-archive name, e.g. levels/l1data/l1.cel"}}},
                 {"til",        json{{"type","string"},{"description","TIL in-archive name"}}},
                 {"min",        json{{"type","string"},{"description","MIN in-archive name"}}},
                 {"sol",        json{{"type","string"},{"description","SOL in-archive name"}}},
                 {"dun",        json{{"type","string"},{"description","DUN in-archive name"}}},
                 {"palette",    json{{"type","string"},{"description","palette (default gendata/cut2.pal)"}}},
                 {"micro_len",  json{{"type","integer"},{"description","10 normal, 12 Hell"}}},
                 {"blocks",     json{{"type","integer"},{"description","MIN stride: 10 normal, 16 Hell"}}},
                 {"out",        json{{"type","string"},{"description","output dir (default out/maps)"}}},
             }},
             {"required", json::array({"cel", "til", "min", "sol", "dun"})}},
        toolMapBuild});
}

} // namespace tools
} // namespace mcp
