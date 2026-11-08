// src/tools/scene_tools.cpp
//
// scene.list / scene.build — assemble and render complete Diablo 1 / Hellfire
// scenes (Tristram town, L1 presets) using the level_* decoder pipeline.
//
// Rendering is faithful to DevilutionX DrawGame (two-pass floors + walls,
// opaque background, prefilled dPiece). The native PNG is written to disk; a
// bounded, integer-downscaled preview + an aligned overlay are returned as
// base64 PNGs for the WebSocket-based web viewer.

#include "tools/scene_tools.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "engine/archive.h"
#include "tools/base64.h"
#include "decoders/level.h"
#include "decoders/level_scene.h"
#include "decoders/palette.h"
#include "decoders/png.h"

namespace {

std::string reqStr(const mcp::json& p, const char* key) {
    auto it = p.find(key);
    if (it == p.end() || !it->is_string())
        throw std::invalid_argument(std::string("missing string param: ") + key);
    return it->get<std::string>();
}

// Try to extract `name` from any archive in the stack. The Archive layer
// normalizes in-archive names ('/' -> '\\'), so callers may use either form.
bool extractAny(const std::vector<std::string>& archives,
                const std::string& name, std::vector<uint8_t>& out) {
    for (const auto& a : archives) {
        engine::Archive arc;
        if (!arc.open(a))
            continue;
        if (arc.extract(name, out))
            return true;
    }
    return false;
}

std::vector<std::string> archiveStack(const mcp::json& params) {
    std::vector<std::string> archives;
    if (params.contains("archive") && params["archive"].is_string())
        archives.push_back(params["archive"].get<std::string>());
    if (params.contains("archives") && params["archives"].is_array()) {
        for (const auto& a : params["archives"])
            if (a.is_string()) archives.push_back(a.get<std::string>());
    }
    if (archives.empty())
        throw std::invalid_argument("provide `archive` (and/or `archives`)");
    return archives;
}

std::string sanitize(const std::string& s) {
    std::string r = s;
    for (char& c : r)
        if (c == '\\' || c == '/' || c == ':' || c == '.') c = '_';
    return r;
}

// Nearest-neighbour integer downscale. `step` is chosen once and applied to
// BOTH the base image and the overlay so they stay pixel-aligned.
std::vector<uint8_t> downscale(const std::vector<uint8_t>& rgba, int w, int h,
                               int step, int& outW, int& outH) {
    outW = std::max(1, w / step);
    outH = std::max(1, h / step);
    std::vector<uint8_t> out(static_cast<size_t>(outW) * outH * 4);
    for (int y = 0; y < outH; ++y) {
        const int sy = y * step;
        for (int x = 0; x < outW; ++x) {
            const int sx = x * step;
            const uint8_t* p = rgba.data() + (static_cast<size_t>(sy) * w + sx) * 4;
            uint8_t* q = out.data() + (static_cast<size_t>(y) * outW + x) * 4;
            q[0] = p[0]; q[1] = p[1]; q[2] = p[2]; q[3] = p[3];
        }
    }
    return out;
}

// ---- Overlay (drawn directly at preview resolution) ------------------------

void putPixel(std::vector<uint8_t>& rgba, int w, int h, int x, int y,
              uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (x < 0 || x >= w || y < 0 || y >= h) return;
    uint8_t* q = rgba.data() + (static_cast<size_t>(y) * w + x) * 4;
    q[0] = r; q[1] = g; q[2] = b; q[3] = a;
}

void line(std::vector<uint8_t>& rgba, int w, int h,
          int x0, int y0, int x1, int y1,
          uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    int dx = std::abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -std::abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        putPixel(rgba, w, h, x0, y0, r, g, b, a);
        if (x0 == x1 && y0 == y1) break;
        const int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

// Fill an isometric diamond (top/right/bottom/left corners) with a tint.
void fillDiamond(std::vector<uint8_t>& rgba, int w, int h,
                 int cx, int cy, int halfW, int halfH,
                 uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    for (int y = -halfH; y <= halfH; ++y) {
        const int span = halfW * (halfH - std::abs(y)) / std::max(1, halfH);
        for (int x = -span; x <= span; ++x)
            putPixel(rgba, w, h, cx + x, cy + y, r, g, b, a);
    }
}

// Build the overlay at preview resolution using the SAME SceneProjection as
// renderMap (scaled by 1/step). Grid lines + SOL flag tint per cell.
std::vector<uint8_t> buildOverlay(const decoders::MapRenderResult& map,
                                  int microLen, int step, int outW, int outH) {
    std::vector<uint8_t> ov(static_cast<size_t>(outW) * outH * 4, 0);
    const decoders::SceneProjection proj =
        decoders::sceneProjection(map.gridW, map.gridH, microLen);
    const int Hm = (microLen / 2) * 32;
    const long minX = proj.minX, minY = proj.minY;

    for (int j = 0; j < map.gridH; ++j) {
        for (int i = 0; i < map.gridW; ++i) {
            const size_t idx = static_cast<size_t>(j) * map.gridW + i;
            const long baseX = (static_cast<long>(j) - i) * 32 - minX;
            const long baseY = -(static_cast<long>(j) + i) * 16 - minY;
            const long slot = baseY - Hm + 1 + (Hm - 32); // floor slot top
            // Diamond corners in native coords -> preview coords.
            const int topX = static_cast<int>((baseX + 32) / step);
            const int topY = static_cast<int>(slot / step);
            const int rightX = static_cast<int>((baseX + 64) / step);
            const int rightY = static_cast<int>((slot + 16) / step);
            const int botX = topX;
            const int botY = static_cast<int>((slot + 32) / step);
            const int leftX = static_cast<int>(baseX / step);
            const int leftY = rightY;
            const int cx = (leftX + rightX) / 2;
            const int cy = (topY + botY) / 2;

            // SOL tint: solid (red) > transparent flag (blue) > blockLight
            // (yellow). Kept translucent so the underlying tile art shows.
            const int halfW = std::max(1, (rightX - leftX) / 2);
            const int halfH = std::max(1, (botY - topY) / 2);
            if (map.solid[idx])
                fillDiamond(ov, outW, outH, cx, cy, halfW, halfH, 255, 60, 60, 55);
            else if (idx < map.transparent.size() && map.transparent[idx])
                fillDiamond(ov, outW, outH, cx, cy, halfW, halfH, 60, 160, 255, 55);
            else if (idx < map.blockLight.size() && map.blockLight[idx])
                fillDiamond(ov, outW, outH, cx, cy, halfW, halfH, 255, 200, 60, 45);

            // Major grid only (every 8th cell). At a 96x96 grid downscaled ~7x,
            // per-cell outlines collapse into an unreadable mesh; a coarse
            // reference grid stays legible while the tint carries per-cell data.
            if (i % 8 == 0 && j % 8 == 0) {
                const uint8_t gr = 255, gg = 255, gb = 255, ga = 140;
                line(ov, outW, outH, topX, topY, rightX, rightY, gr, gg, gb, ga);
                line(ov, outW, outH, rightX, rightY, botX, botY, gr, gg, gb, ga);
                line(ov, outW, outH, botX, botY, leftX, leftY, gr, gg, gb, ga);
                line(ov, outW, outH, leftX, leftY, topX, topY, gr, gg, gb, ga);
            }
        }
    }
    return ov;
}

mcp::ToolResult toolSceneList(const mcp::json&) {
    mcp::json arr = mcp::json::array();
    for (const auto& s : decoders::sceneCatalog()) {
        const auto [gw, gh] = decoders::sceneGridDims(s);
        arr.push_back(mcp::json{
            {"id", s.id}, {"title", s.title},
            {"layout", s.layout == decoders::SceneLayout::Town ? "town" : "dungeon"},
            {"grid_width", gw}, {"grid_height", gh},
            {"micro_len", s.microLen}, {"blocks", s.blocks},
        });
    }
    mcp::ToolResult r;
    r.value = mcp::json{{"count", arr.size()}, {"scenes", arr}};
    return r;
}

mcp::ToolResult toolSceneBuild(const mcp::json& params) {
    const std::string id = reqStr(params, "id");
    const decoders::SceneSpec* spec = decoders::findScene(id);
    if (!spec)
        throw std::runtime_error("unknown scene id: " + id);
    const std::vector<std::string> archives = archiveStack(params);
    const std::string outDir = params.value("out", std::string("out/scenes"));
    const int maxW = params.value("preview_max_width", 1000);

    // Extract assets.
    std::vector<uint8_t> celBuf, tilBuf, minBuf, solBuf, palBuf, dunBuf;
    std::vector<std::vector<uint8_t>> sectorBufs;
    auto need = [&](const std::string& n, std::vector<uint8_t>& b) {
        if (!extractAny(archives, n, b))
            throw std::runtime_error("extract failed (name not found?): " + n);
    };
    need(spec->cel, celBuf);
    need(spec->til, tilBuf);
    need(spec->min, minBuf);
    need(spec->sol, solBuf);
    need(spec->palette, palBuf);
    if (spec->layout == decoders::SceneLayout::Town) {
        for (const auto& s : spec->sectorDuns) {
            std::vector<uint8_t> b;
            need(s, b);
            sectorBufs.push_back(std::move(b));
        }
    } else {
        need(spec->dun, dunBuf);
    }

    auto megatiles = decoders::loadTil(tilBuf);
    auto micros = decoders::loadMin(minBuf, spec->blocks, spec->microLen);
    auto solFlags = decoders::loadSol(solBuf);
    auto palRgb = decoders::loadPalette(palBuf);
    if (megatiles.empty()) throw std::runtime_error("TIL parse failed");
    if (micros.empty()) throw std::runtime_error("MIN parse failed");
    if (palRgb.empty()) throw std::runtime_error("palette parse failed");
    std::vector<uint8_t> palRgba = decoders::paletteToRGBA(palRgb);

    // Assemble the full dPiece grid.
    decoders::SceneGrid grid;
    if (spec->layout == decoders::SceneLayout::Town) {
        grid = decoders::buildTownScene(*spec, megatiles, sectorBufs);
    } else {
        grid = decoders::buildDungeonScene(*spec, megatiles, decoders::loadDun(dunBuf));
    }

    // Re-encode the level CEL and render.
    auto frameTypes = decoders::buildFrameTypes(micros, spec->microLen);
    decoders::ReencodedLevelCel reencoded = decoders::reencodeLevelCel(celBuf, frameTypes);
    if (reencoded.numFrames == 0) throw std::runtime_error("level CEL parse failed");

    decoders::MapRenderOptions opts;
    opts.prefillMicro = spec->prefillMicro;
    const uint32_t bgc = spec->background;
    opts.background[0] = static_cast<uint8_t>((bgc >> 16) & 0xFF);
    opts.background[1] = static_cast<uint8_t>((bgc >> 8) & 0xFF);
    opts.background[2] = static_cast<uint8_t>(bgc & 0xFF);
    opts.background[3] = static_cast<uint8_t>((bgc >> 24) & 0xFF);

    decoders::MapRenderResult map = decoders::renderMap(
        reencoded, micros, grid.micro, grid.gw, grid.gh, solFlags,
        std::vector<int8_t>{}, palRgba.data(), spec->microLen, opts);
    if (map.width == 0 || map.height == 0)
        throw std::runtime_error("scene render produced empty output");

    // Verify no transparent pixels remain (the "black blocks" fix).
    size_t transparentCount = 0;
    for (size_t i = 3; i < map.rgba.size(); i += 4)
        if (map.rgba[i] == 0) ++transparentCount;

    // Integer downscale (same step for base + overlay).
    const int step = (map.width > maxW && maxW > 0)
                         ? (map.width + maxW - 1) / maxW : 1;
    int pw = 0, ph = 0;
    std::vector<uint8_t> preview = downscale(map.rgba, map.width, map.height, step, pw, ph);
    std::vector<uint8_t> overlay = buildOverlay(map, spec->microLen, step, pw, ph);

    const std::vector<uint8_t> nativePng =
        decoders::encodePNG(static_cast<uint16_t>(map.width),
                            static_cast<uint16_t>(map.height), map.rgba);
    const std::vector<uint8_t> previewPng =
        decoders::encodePNG(static_cast<uint16_t>(pw), static_cast<uint16_t>(ph), preview);
    const std::vector<uint8_t> overlayPng =
        decoders::encodePNG(static_cast<uint16_t>(pw), static_cast<uint16_t>(ph), overlay);
    if (nativePng.empty() || previewPng.empty() || overlayPng.empty())
        throw std::runtime_error("PNG encode failed");

    // Write artifacts to disk.
    std::error_code ec;
    std::filesystem::create_directories(outDir, ec);
    const std::string base = sanitize(id);
    const std::string pngPath = outDir + "/" + base + ".png";
    const std::string previewPath = outDir + "/" + base + "_preview.png";
    const std::string overlayPath = outDir + "/" + base + "_overlay.png";
    const std::string metaPath = outDir + "/" + base + ".json";
    auto writeBytes = [](const std::string& p, const std::vector<uint8_t>& b) {
        std::ofstream f(p, std::ios::binary);
        if (f) f.write(reinterpret_cast<const char*>(b.data()),
                       static_cast<std::streamsize>(b.size()));
    };
    writeBytes(pngPath, nativePng);
    writeBytes(previewPath, previewPng);
    writeBytes(overlayPath, overlayPng);

    // Sidecar metadata (per-cell flag grids).
    mcp::json solidArr = mcp::json::array(), transArr = mcp::json::array();
    mcp::json blArr = mcp::json::array(), bmArr = mcp::json::array();
    for (size_t i = 0; i < map.solid.size(); ++i) {
        solidArr.push_back(map.solid[i]);
        transArr.push_back(map.transparent[i]);
        blArr.push_back(map.blockLight[i]);
        bmArr.push_back(map.blockMissile[i]);
    }
    mcp::json meta = mcp::json{
        {"scene_id", id}, {"grid_width", map.gridW}, {"grid_height", map.gridH},
        {"micro_len", spec->microLen}, {"frame_count", reencoded.numFrames},
        {"native_width", map.width}, {"native_height", map.height},
        {"transparent_pixel_count", transparentCount},
        {"solid", solidArr}, {"transparent", transArr},
        {"block_light", blArr}, {"block_missile", bmArr},
    };
    {
        std::ofstream f(metaPath);
        if (f) f << meta.dump();
    }

    size_t solidCount = 0, transCount = 0;
    for (size_t i = 0; i < map.solid.size(); ++i) {
        if (map.solid[i]) ++solidCount;
        if (map.transparent[i]) ++transCount;
    }

    mcp::ToolResult r;
    r.value = mcp::json{
        {"scene_id", id}, {"title", spec->title},
        {"image_path", pngPath}, {"preview_path", previewPath},
        {"overlay_path", overlayPath}, {"meta_path", metaPath},
        {"image_base64", mcp::base64::encode(previewPng)},
        {"overlay_base64", mcp::base64::encode(overlayPng)},
        {"width", map.width}, {"height", map.height},
        {"preview_width", pw}, {"preview_height", ph}, {"scale", step},
        {"grid_width", map.gridW}, {"grid_height", map.gridH},
        {"micro_len", spec->microLen}, {"frame_count", reencoded.numFrames},
        {"solid_count", solidCount}, {"transparent_count", transCount},
        {"transparent_pixel_count", transparentCount},
        {"meta", meta},
    };
    return r;
}

} // namespace

namespace mcp {
namespace tools {

void registerSceneTools(ToolRegistry& reg) {
    reg.registerTool(ToolDef{
        "scene.list",
        "List available Diablo 1 / Hellfire scenes (Tristram town + L1 presets) "
        "that scene.build can render. Returns id/title/layout/grid dims; no MPQ needed.",
        json{{"type", "object"}, {"properties", json::object()}},
        toolSceneList});

    reg.registerTool(ToolDef{
        "scene.build",
        "Assemble and render a COMPLETE Diablo 1 / Hellfire scene (two-pass "
        "DevilutionX floor+wall rendering, opaque background, prefilled dPiece, "
        "no transparent holes). Writes a native PNG + per-cell SOL metadata JSON "
        "to disk and returns a bounded base64 PNG preview plus an aligned overlay "
        "PNG (grid lines + solid/transparent/blockLight tint).",
        json{{"type", "object"},
             {"properties", json{
                 {"id", json{{"type", "string"}, {"description", "scene id from scene.list, e.g. tristram"}}},
                 {"archive", json{{"type", "string"}, {"description", "primary MPQ path"}}},
                 {"archives", json{{"type", "array"}, {"items", json{{"type", "string"}}},
                                    {"description", "additional MPQ search paths"}}},
                 {"out", json{{"type", "string"}, {"description", "output dir (default out/scenes)"}}},
                 {"preview_max_width", json{{"type", "integer"},
                                             {"description", "max preview width, default 1000"}}},
             }},
             {"required", json::array({"id", "archive"})}},
        toolSceneBuild});
}

} // namespace tools
} // namespace mcp
