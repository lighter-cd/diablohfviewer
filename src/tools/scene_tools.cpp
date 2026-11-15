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
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "engine/archive.h"
#include "tools/base64.h"
#include "decoders/cel.h"
#include "decoders/clx.h"
#include "decoders/dungeon_gen.h"
#include "decoders/level.h"
#include "decoders/level_light.h"
#include "decoders/level_path.h"
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

// ---- Scene asset loading (shared by scene.build / scene.path) --------------

struct SceneAssets {
    decoders::SceneGrid grid;
    std::vector<decoders::MicroTileRef> micros;
    std::vector<uint8_t> solFlags;
    std::vector<uint8_t> palRgb;
    std::vector<uint8_t> palRgba;
    decoders::ReencodedLevelCel reencoded;
    std::vector<uint8_t> solid; // per-cell, from SOL (1 = blocks walking)
    std::vector<decoders::SpriteFrameRGBA> specialSprites; // dSpecial frames
    // Per-dLight-level copies for dungeon (ClxDrawLight); town stays unlit.
    std::vector<std::vector<decoders::SpriteFrameRGBA>> specialSpritesLit;
};

SceneAssets loadSceneAssets(const decoders::SceneSpec& spec,
                            const std::vector<std::string>& archives,
                            bool dirtBorder = false,
                            const decoders::DunFile* generatedDun = nullptr) {
    std::vector<uint8_t> celBuf, tilBuf, minBuf, solBuf, palBuf, dunBuf;
    std::vector<std::vector<uint8_t>> sectorBufs;
    auto need = [&](const std::string& n, std::vector<uint8_t>& b) {
        if (!extractAny(archives, n, b))
            throw std::runtime_error("extract failed (name not found?): " + n);
    };
    need(spec.cel, celBuf);
    need(spec.til, tilBuf);
    need(spec.min, minBuf);
    need(spec.sol, solBuf);
    need(spec.palette, palBuf);
    if (spec.layout == decoders::SceneLayout::Town) {
        for (const auto& s : spec.sectorDuns) {
            std::vector<uint8_t> b;
            need(s, b);
            sectorBufs.push_back(std::move(b));
        }
    } else if (generatedDun) {
        // Phase 5 (scene.random): the DUN grid was generated in memory from a
        // seed; no fixed preset .dun exists in the MPQ.
        (void)dunBuf;
    } else if (spec.randomLevel > 0) {
        throw std::runtime_error(
            "random scene '" + spec.id + "' requires scene.random generate");
    } else {
        need(spec.dun, dunBuf);
    }

    SceneAssets out;
    auto megatiles = decoders::loadTil(tilBuf);
    out.micros = decoders::loadMin(minBuf, spec.blocks, spec.microLen);
    out.solFlags = decoders::loadSol(solBuf);
    auto palRgb = decoders::loadPalette(palBuf);
    if (megatiles.empty()) throw std::runtime_error("TIL parse failed");
    if (out.micros.empty()) throw std::runtime_error("MIN parse failed");
    if (palRgb.empty()) throw std::runtime_error("palette parse failed");
    out.palRgb = palRgb;
    out.palRgba = decoders::paletteToRGBA(palRgb);

    if (spec.layout == decoders::SceneLayout::Town)
        out.grid = decoders::buildTownScene(spec, megatiles, sectorBufs);
    else if (generatedDun)
        out.grid = decoders::buildDungeonScene(spec, megatiles, *generatedDun, dirtBorder);
    else
        out.grid = decoders::buildDungeonScene(
            spec, megatiles, decoders::loadDun(dunBuf), dirtBorder);

    // dSpecial overlays come from a separate CEL (town: towns.cel tree
    // canopies; dungeon L1/L2: l1s.cel / l2s.cel arches and doors), not the
    // level CEL. Decode them into RGBA frames indexed by dSpecial id - 1.
    if (!spec.specialCel.empty()) {
        std::vector<uint8_t> specialBuf;
        if (extractAny(archives, spec.specialCel, specialBuf)) {
            const std::vector<uint8_t> clx = decoders::celToClx(
                specialBuf.data(), specialBuf.size(), spec.specialCelWidth);
            const std::vector<decoders::DecodedFrame> frames =
                decoders::decodeClx(clx, out.palRgb);
            out.specialSprites.reserve(frames.size());
            for (const auto& f : frames) {
                decoders::SpriteFrameRGBA s;
                s.width = f.width;
                s.height = f.height;
                s.rgba = f.rgba;
                out.specialSprites.push_back(std::move(s));
            }

            // DevilutionX scrollrt.cpp:912-935 draws a non-town dSpecial
            // overlay with ClxDrawLight(..., lightTableIndex): the sprite's
            // palette indices are remapped through LightTables[dLight]. Decode
            // one copy per light level so renderMap can pick the cell's shade.
            // Town overlays use the unlit ClxDraw, so they get no copies.
            if (spec.layout == decoders::SceneLayout::Dungeon && !clx.empty() &&
                out.palRgb.size() >= 256 * 3) {
                decoders::LightTableView lightTables;
                lightTables.build(static_cast<decoders::LightStyle>(
                    spec.lightStyle <= 2 ? spec.lightStyle : 0));
                out.specialSpritesLit.resize(decoders::kNumLightLevels);
                for (int level = 0; level < decoders::kNumLightLevels; ++level) {
                    std::vector<uint8_t> litRgb(256 * 3, 0);
                    for (int i = 0; i < 256; ++i) {
                        const uint8_t mapped = lightTables.tables[level][i];
                        for (int c = 0; c < 3; ++c)
                            litRgb[static_cast<size_t>(i) * 3 + c] =
                                out.palRgb[static_cast<size_t>(mapped) * 3 + c];
                    }
                    const auto litFrames = decoders::decodeClx(clx, litRgb);
                    auto& dst = out.specialSpritesLit[level];
                    dst.reserve(litFrames.size());
                    for (const auto& f : litFrames) {
                        decoders::SpriteFrameRGBA s;
                        s.width = f.width;
                        s.height = f.height;
                        s.rgba = f.rgba;
                        dst.push_back(std::move(s));
                    }
                }
            }
        }
    }

    auto frameTypes = decoders::buildFrameTypes(out.micros, spec.microLen);
    out.reencoded = decoders::reencodeLevelCel(celBuf, frameTypes);
    if (out.reencoded.numFrames == 0)
        throw std::runtime_error("level CEL parse failed");

    // Per-cell SOL flags, exactly like renderMap() resolves them: the cell's
    // 0-based micro index is the SOLData index; kNoMicro falls back to prefill.
    const size_t cells = static_cast<size_t>(out.grid.gw) * out.grid.gh;
    out.solid.assign(cells, 0);
    for (int j = 0; j < out.grid.gh; ++j) {
        for (int i = 0; i < out.grid.gw; ++i) {
            const size_t idx = static_cast<size_t>(j) * out.grid.gw + i;
            uint16_t mi = out.grid.micro[idx];
            if (mi == decoders::kNoMicro || mi >= out.micros.size())
                mi = spec.prefillMicro;
            if (mi == decoders::kNoMicro || mi >= out.micros.size())
                continue;
            if (!out.solFlags.empty() && mi < out.solFlags.size())
                out.solid[idx] = (out.solFlags[mi] & decoders::TileFlags::Solid) ? 1 : 0;
        }
    }
    return out;
}

// Parse optional static lights and build the per-cell light map. When lighting
// is enabled for a dungeon and no explicit light is given, a single central
// "torch" is added (auto_light) so the preview is not pitch black; this is a
// viewer convenience, not part of the map data.
std::vector<uint8_t> makeSceneLighting(const mcp::json& params,
                                       const decoders::SceneSpec& spec,
                                       int gw, int gh, uint8_t& ambientOut,
                                       std::vector<decoders::LightSource>& lightsOut,
                                       bool& autoLightOut) {
    ambientOut = static_cast<uint8_t>(
        std::clamp<int>(params.value("ambient", static_cast<int>(spec.ambientLight)), 0, 15));
    autoLightOut = params.value("auto_light", true);
    lightsOut.clear();
    if (params.contains("lights") && params["lights"].is_array()) {
        for (const auto& l : params["lights"]) {
            if (!l.is_object()) continue;
            decoders::LightSource src;
            src.x = l.value("x", 0);
            src.y = l.value("y", 0);
            src.radius = static_cast<uint8_t>(std::clamp<int>(l.value("radius", 15), 0, 15));
            src.offsetX = static_cast<int8_t>(l.value("offset_x", 0));
            src.offsetY = static_cast<int8_t>(l.value("offset_y", 0));
            lightsOut.push_back(src);
        }
    }
    if (params.value("lighting", false) || params.contains("lights") || params.contains("ambient")) {
        if (lightsOut.empty() && autoLightOut)
            lightsOut.push_back(decoders::LightSource{ gw / 2, gh / 2, 15, 0, 0 });
        const bool soft = spec.lightStyle == 2;
        return decoders::buildLightMap(gw, gh, ambientOut, lightsOut, soft).levels;
    }
    return {};
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
            const long baseX = (static_cast<long>(i) - j) * 32 - minX;
            const long baseY = (static_cast<long>(i) + j) * 16 - minY;
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
            {"ambient_light", s.ambientLight}, {"light_style", s.lightStyle},
            {"randomizable", s.randomLevel > 0}, {"random_level", s.randomLevel},
        });
    }
    mcp::ToolResult r;
    r.value = mcp::json{{"count", arr.size()}, {"scenes", arr}};
    return r;
}

namespace {

bool fileExists(const std::string& p) {
    std::error_code ec;
    return std::filesystem::exists(p, ec);
}

bool readFile(const std::string& p, std::string& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return true;
}

// A stable key describing everything that affects the rendered output, so an
// identical request can be served from the on-disk cache without re-rendering.
std::string buildCacheKey(const std::string& id,
                          const std::vector<std::string>& archives,
                          const decoders::SceneSpec& spec,
                          const mcp::json& params, int maxW,
                          const std::string& outDir) {
    std::string a;
    for (size_t i = 0; i < archives.size(); ++i) { if (i) a += ";"; a += archives[i]; }
    // v8: random_l2/l3/l4 now use the Catacombs/Caves/Hell tilesets (distinct
    // floor/dirt megatiles, microLen/blocks, special table, palette), so stale
    // v7 Cathedral-tileset PNGs must be invalidated. v7: the tile atlas now
    // also packs the OPEN-door micros (L2 537/539 -> 12/16) so the front-end
    // can swap a closed door leaf for the open doorway locally (Phase 3 door
    // toggle) without a server round-trip. v6: layout now
    // also ships light-source definitions (default_ambient / lights /
    // soft_falloff) for the front-end Phase 2 real-time lighting, which changes
    // the layout schema — bump to invalidate stale pre-v6 caches that lacked
    // those fields. v5: catacombs doorways now render CLOSED door leaves
    // (AddL2Objs / SetDoorStateClosed), so their dPiece micro changed. Earlier:
    // v4 shaded dungeon dSpecial sprites by dLight (ClxDrawLight); v3 cropped
    // presets to the .dun bbox. Assembly inputs and switches are all in the key
    // so catalog changes / toggles invalidate cached PNGs automatically.
    return "v8|" + id + "|" + a + "|" +
           std::to_string(spec.floorMegaTile) + "|" + spec.specialCel + "|" +
           std::to_string(static_cast<int>(spec.specialTable)) + "|" +
           (params.value("dirt_border", false) ? "B1" : "B0") + "|" +
           (params.value("lighting", false) ? "L1" : "L0") + "|" +
           std::to_string(params.value("ambient", spec.ambientLight)) + "|" +
           params.value("lights", mcp::json::array()).dump() + "|" +
           (params.value("auto_light", true) ? "A1" : "A0") + "|" +
           std::to_string(maxW) + "|" + outDir;
}

// FNV-1a 64-bit, rendered as 16 hex chars. Used to namespace on-disk cache
// artifacts per parameter set, so lighting on/off (and any other param
// difference) get SEPARATE files that coexist instead of clobbering one shared
// filename. This is what lets the two cached variants persist independently.
uint64_t fnv1a64(const std::string& s) {
    uint64_t h = 1469598103934665603ULL;
    for (unsigned char c : s) { h ^= static_cast<uint64_t>(c); h *= 1099511628211ULL; }
    return h;
}
std::string hashHex(const std::string& s) {
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx",
                  static_cast<unsigned long long>(fnv1a64(s)));
    return std::string(buf);
}

} // namespace

// Internal workhorse shared by `scene.tiles` (fixed scenes) and
// `scene.random generate` (seed-generated dungeons). The extra parameters let
// the random path override the output `stem` (stable per-level name instead of
// a content hash), inject an in-memory generated DUN grid, and bypass the
// on-disk cache markers (a fixed stem + changing seed must never hit a stale
// `.tiles.result.json`). `skipCache=true` also suppresses writing those markers.
mcp::ToolResult sceneTilesImpl(const mcp::json& params,
                               const std::string& stemOverride,
                               const decoders::DunFile* generatedDun,
                               bool skipCache) {
    const std::string id = reqStr(params, "id");
    const decoders::SceneSpec* spec = decoders::findScene(id);
    if (!spec)
        throw std::runtime_error("unknown scene id: " + id);
    const std::vector<std::string> archives = archiveStack(params);
    const std::string outDir = params.value("out", std::string("out/scenes"));
    const bool dirtBorder = params.value("dirt_border", false);

    // Layout-affecting signature (reuses scene.build's key logic, tagged tiles).
    const std::string cacheKey =
        "tiles|" + buildCacheKey(id, archives, *spec, params, 0, outDir);
    const std::string tag = hashHex(cacheKey);
    const std::string base = stemOverride.empty() ? (sanitize(id) + "_" + tag)
                                                  : stemOverride;

    std::error_code ec;
    std::filesystem::create_directories(outDir, ec);
    const std::string tilesPngPath = outDir + "/" + base + "_tiles.png";
    const std::string specialPngPath = outDir + "/" + base + "_special.png";
    const std::string keyPath = outDir + "/" + base + ".tiles.key";
    const std::string resultPath = outDir + "/" + base + ".tiles.result.json";

    // Cache reuse: identical parameters -> return stored result without rebuilding.
    // Random maps bypass this: their stem is fixed but the seed changes per run.
    if (!skipCache && fileExists(keyPath) && fileExists(tilesPngPath) &&
        (spec->specialCel.empty() || fileExists(specialPngPath))) {
        std::string stored;
        if (readFile(keyPath, stored) && stored == cacheKey) {
            std::string resTxt;
            if (readFile(resultPath, resTxt)) {
                mcp::tools::reportProgress(100, "复用缓存");
                mcp::ToolResult r;
                r.value = mcp::json::parse(resTxt);
                return r;
            }
        }
    }

    mcp::tools::reportProgress(5, "解析场景规格 " + id);
    SceneAssets assets = loadSceneAssets(*spec, archives, dirtBorder, generatedDun);
    const decoders::SceneGrid& grid = assets.grid;
    const int gw = grid.gw, gh = grid.gh;
    const size_t cells = static_cast<size_t>(gw) * gh;
    const int microLen = spec->microLen;
    const int nRows = microLen / 2;
    const decoders::ReencodedLevelCel& cel = assets.reencoded;
    const std::vector<decoders::MicroTileRef>& micros = assets.micros;

    const auto drawable = [&](const decoders::LevelCelBlock& b) -> bool {
        if (!b.hasValue() || b.frame() == 0)
            return false;
        return (static_cast<uint32_t>(b.frame()) - 1) < cel.numFrames;
    };
    const auto resolveMi = [&](uint16_t mi) -> uint16_t {
        if (mi == decoders::kNoMicro || mi >= micros.size())
            mi = spec->prefillMicro;
        if (mi == decoders::kNoMicro || mi >= micros.size())
            return decoders::kNoMicro;
        return mi;
    };

    // ---- per-cell SOL flags (solid / blockLight / blockMissile) + special ----
    std::vector<uint8_t> solid(cells, 0), blockLight(cells, 0),
        blockMissile(cells, 0), transparent(cells, 0), special(cells, 0);
    for (int j = 0; j < gh; ++j) {
        for (int i = 0; i < gw; ++i) {
            const size_t idx = static_cast<size_t>(j) * gw + i;
            special[idx] = grid.special[idx];
            const uint16_t mi = resolveMi(grid.micro[idx]);
            if (mi != decoders::kNoMicro && !assets.solFlags.empty() &&
                mi < assets.solFlags.size()) {
                const uint8_t f = assets.solFlags[mi];
                solid[idx] = (f & decoders::TileFlags::Solid) ? 1 : 0;
                blockLight[idx] = (f & decoders::TileFlags::BlockLight) ? 1 : 0;
                blockMissile[idx] = (f & decoders::TileFlags::BlockMissile) ? 1 : 0;
            }
        }
    }

    // ---- optional per-cell dLight map (shipped precomputed; frontend can
    // also recompute with buildLightMap) ----
    uint8_t ambient = spec->ambientLight;
    std::vector<decoders::LightSource> lights;
    bool autoLight = false;
    auto lightVec = makeSceneLighting(params, *spec, gw, gh, ambient, lights, autoLight);
    const bool lightingEnabled = !lightVec.empty();

    // ---- 16 dLight shade tables (LightTables[level][paletteIndex]) ----
    decoders::LightTableView lightTables;
    lightTables.build(static_cast<decoders::LightStyle>(
        spec->lightStyle <= 2 ? spec->lightStyle : 0));

    // ---- collect micros referenced by the grid (+ prefill for floor fallback)
    std::set<uint16_t> used;
    for (size_t idx = 0; idx < cells; ++idx) {
        const uint16_t mi = resolveMi(grid.micro[idx]);
        if (mi != decoders::kNoMicro)
            used.insert(mi);
    }
    if (spec->prefillMicro != decoders::kNoMicro &&
        spec->prefillMicro < micros.size())
        used.insert(spec->prefillMicro);

    // Phase 3 (door local toggle): the front-end swaps a closed door leaf
    // (e.g. L2 537/539) for the OPEN doorway micro (12/16) with zero server
    // round-trip. Those open micros are NOT in `grid.micro` (the grid stores the
    // closed state), so they would be missing from the atlas and the door would
    // vanish when toggled. Pre-collect every door's open micro and pack it too.
    {
        std::set<uint16_t> doorOpenMicros;
        for (size_t idx = 0; idx < cells; ++idx) {
            const uint16_t mi = resolveMi(grid.micro[idx]);
            uint16_t open = 0;
            if (mi == 537) open = 12;       // OBJ_L2LDOOR (closed) -> open
            else if (mi == 539) open = 16;  // OBJ_L2RDOOR (closed) -> open
            if (open != 0 && open < micros.size())
                doorOpenMicros.insert(open);
        }
        for (uint16_t m : doorOpenMicros) used.insert(m);
    }

    // ---- pack the main index atlas (R = palette index, A = drawn / 0) ----
    // Per micro, the renderer needs:
    //   "mi:0:L" / "mi:1:R"  -> floor Pass1 (forced Left/Right triangle, with the
    //                           TransparentSquare exception from renderMap)
    //   "mi:b"               -> real-type decode of block b (wall Pass2; for
    //                           non-floor cells b=0..microLen-1, for floor b=2..)
    const int kAtlasCols = 64, kTile = 32;
    std::vector<std::pair<std::string, std::vector<uint8_t>>> tileFrames;
    for (uint16_t mi : used) {
        const decoders::MicroTileRef& m = micros[mi];
        auto pack = [&](const std::string& key, const decoders::LevelCelBlock& blk,
                        decoders::TileType forced) {
            if (!drawable(blk))
                return;
            std::vector<uint8_t> fbuf(static_cast<size_t>(kTile) * kTile * 4, 0);
            int fh = 0;
            decoders::decodeReencodedFrameAsIndex(cel, blk.frame() - 1, forced, fbuf, fh);
            tileFrames.emplace_back(key, std::move(fbuf));
        };
        // floor triangles (Pass1)
        decoders::TileType t0 = (m.blocks[0].type() == decoders::TileType::TransparentSquare)
                                    ? decoders::TileType::TransparentSquare
                                    : decoders::TileType::LeftTriangle;
        decoders::TileType t1 = (m.blocks[1].type() == decoders::TileType::TransparentSquare)
                                    ? decoders::TileType::TransparentSquare
                                    : decoders::TileType::RightTriangle;
        pack(std::to_string(mi) + ":0:L", m.blocks[0], t0);
        pack(std::to_string(mi) + ":1:R", m.blocks[1], t1);
        // real-type frames for the wall pass (b = 0 .. microLen-1)
        for (int b = 0; b < microLen && b < 16; ++b)
            pack(std::to_string(mi) + ":" + std::to_string(b), m.blocks[b], m.blocks[b].type());
    }

    const int atlasW = kAtlasCols * kTile;
    const int atlasRows = std::max(1, (static_cast<int>(tileFrames.size()) + kAtlasCols - 1) / kAtlasCols);
    const int atlasH = atlasRows * kTile;
    std::vector<uint8_t> tilesAtlas(static_cast<size_t>(atlasW) * atlasH * 4, 0);
    mcp::json framesJson = mcp::json::object();
    const size_t srcRowPitch = static_cast<size_t>(kTile) * 4;
    const size_t dstRowPitch = static_cast<size_t>(atlasW) * 4;
    for (size_t n = 0; n < tileFrames.size(); ++n) {
        const int ax = static_cast<int>(n % kAtlasCols) * kTile;
        const int ay = static_cast<int>(n / kAtlasCols) * kTile;
        const uint8_t* src = tileFrames[n].second.data();
        // Copy row-by-row: the frame is kTile*4 wide, but the atlas is atlasW*4
        // wide, so a flat memcpy would corrupt every tile by stuffing all frame
        // rows into the atlas's first row.
        for (int r = 0; r < kTile; ++r) {
            std::memcpy(tilesAtlas.data() + (static_cast<size_t>(ay + r) * atlasW + ax) * 4,
                        src + static_cast<size_t>(r) * srcRowPitch, srcRowPitch);
        }
        framesJson[tileFrames[n].first] = mcp::json::array({ax, ay});
    }
    mcp::tools::reportProgress(55, "打包主索引图集");

    // ---- special CEL -> index atlas (R = palette index, A = drawn / 0) -----
    mcp::json specialFramesJson = mcp::json::object();
    std::vector<uint8_t> specialAtlas;
    int specialAtlasW = 0, specialAtlasH = 0;
    std::string specialUrl;
    if (!spec->specialCel.empty()) {
        std::vector<uint8_t> specialBuf;
        if (extractAny(archives, spec->specialCel, specialBuf)) {
            const std::vector<uint8_t> clx = decoders::celToClx(
                specialBuf.data(), specialBuf.size(), spec->specialCelWidth);
            const auto frames = decoders::decodeClxIndex(clx); // index form
            const int shelfMax = 2048;
            int curX = 0, curY = 0, shelfH = 0;
            // Pass 1: lay out every frame into a rect on a shelf, recording the
            // final atlas dimensions. We must decide the atlas width/height up
            // front so the row stride NEVER changes mid-pack — growing the
            // buffer with a different stride would shift every already-placed
            // frame and silently corrupt the atlas.
            struct SRect { int x, y, w, h; };
            std::vector<SRect> srects;
            srects.reserve(frames.size());
            for (size_t fi = 0; fi < frames.size(); ++fi) {
                const int w = frames[fi].width, h = frames[fi].height;
                if (curX + w > shelfMax) { curX = 0; curY += shelfH; shelfH = 0; }
                srects.push_back({ curX, curY, w, h });
                curX += w;
                if (h > shelfH) shelfH = h;
            }
            specialAtlasW = curX > 0 ? curX : 0;
            specialAtlasH = curY + shelfH;
            std::vector<uint8_t> acc;
            if (specialAtlasW > 0 && specialAtlasH > 0) {
                acc.assign(static_cast<size_t>(specialAtlasW) * specialAtlasH * 4, 0);
                // Pass 2: blit each frame into its precomputed rect (fixed stride).
                for (size_t fi = 0; fi < frames.size(); ++fi) {
                    const SRect& r = srects[fi];
                    const uint8_t* src = frames[fi].rgba.data();
                    uint8_t* dst = acc.data() +
                        (static_cast<size_t>(r.y) * specialAtlasW + r.x) * 4;
                    for (int yy = 0; yy < r.h; ++yy) {
                        std::memcpy(dst + static_cast<size_t>(yy) * specialAtlasW * 4,
                                    src + static_cast<size_t>(yy) * r.w * 4,
                                    static_cast<size_t>(r.w) * 4);
                    }
                    specialFramesJson[std::to_string(fi + 1)] =
                        mcp::json::array({r.x, r.y, r.w, r.h});
                }
            }
            specialAtlas = std::move(acc);
            specialUrl = "/scenes/" + base + "_special.png";
        }
    }

    // ---- doors: closed-door micro -> open/closed pair (Phase 3 toggle) -----
    // Catacombs applyL2ClosedDoors maps left doorway 12/540 -> 537, right 16/541
    // -> 539. Emit the opposite mapping so the frontend can swap the cell micro
    // (and its dSpecial) between the closed leaf and the open doorway stub.
    mcp::json doorsJson = mcp::json::array();
    for (int j = 0; j < gh; ++j) {
        for (int i = 0; i < gw; ++i) {
            const size_t idx = static_cast<size_t>(j) * gw + i;
            const uint16_t mi = grid.micro[idx];
            uint16_t open = 0;
            if (mi == 537) open = 12;       // OBJ_L2LDOOR (left, closed)
            else if (mi == 539) open = 16;  // OBJ_L2RDOOR (right, closed)
            if (open == 0) continue;
            doorsJson.push_back(mcp::json{
                {"x", i}, {"y", j}, {"open", open}, {"closed", mi},
                {"open_special", 0}, {"closed_special", 0}});
        }
    }

    // ---- write atlas PNGs ----
    const std::vector<uint8_t> tilesPng =
        decoders::encodePNG(static_cast<uint16_t>(atlasW),
                            static_cast<uint16_t>(atlasH), tilesAtlas);
    if (tilesPng.empty())
        throw std::runtime_error("tile atlas PNG encode failed");
    auto writeBytes = [](const std::string& p, const std::vector<uint8_t>& b) {
        std::ofstream f(p, std::ios::binary);
        if (f) f.write(reinterpret_cast<const char*>(b.data()),
                       static_cast<std::streamsize>(b.size()));
    };
    writeBytes(tilesPngPath, tilesPng);
    std::vector<uint8_t> specialPng;
    if (!specialAtlas.empty() && specialAtlasW > 0 && specialAtlasH > 0) {
        specialPng = decoders::encodePNG(static_cast<uint16_t>(specialAtlasW),
                                         static_cast<uint16_t>(specialAtlasH),
                                         specialAtlas);
        if (specialPng.empty())
            throw std::runtime_error("special atlas PNG encode failed");
        writeBytes(specialPngPath, specialPng);
    }
    mcp::tools::reportProgress(80, "编码图集 PNG");

    // ---- assemble layout.json ----
    const decoders::SceneProjection proj = decoders::sceneProjection(gw, gh, microLen);
    const uint8_t bg[4] = {
        static_cast<uint8_t>((spec->background >> 16) & 0xFF),
        static_cast<uint8_t>((spec->background >> 8) & 0xFF),
        static_cast<uint8_t>(spec->background & 0xFF),
        static_cast<uint8_t>((spec->background >> 24) & 0xFF)};

    mcp::json microGridJson = mcp::json::array();
    for (size_t idx = 0; idx < cells; ++idx)
        microGridJson.push_back(grid.micro[idx]);

    auto flagArr = [](const std::vector<uint8_t>& v) {
        mcp::json a = mcp::json::array();
        for (uint8_t b : v) a.push_back(b);
        return a;
    };

    mcp::json paletteJson = mcp::json::array();
    for (uint8_t b : assets.palRgba) paletteJson.push_back(b);

    mcp::json lightTablesJson = mcp::json::array();
    for (int lvl = 0; lvl < decoders::kNumLightLevels; ++lvl) {
        mcp::json lvlArr = mcp::json::array();
        for (int p = 0; p < decoders::kLightTableSize; ++p)
            lvlArr.push_back(lightTables.tables[lvl][p]);
        lightTablesJson.push_back(lvlArr);
    }

    mcp::json layout = mcp::json{
        {"scene_id", id},
        {"micro_len", microLen},
        {"grid_width", gw},
        {"grid_height", gh},
        {"projection", mcp::json{{"minX", proj.minX}, {"minY", proj.minY},
                                 {"width", proj.width}, {"height", proj.height}}},
        {"micro_grid", microGridJson},
        {"atlas", mcp::json{{"url", "/scenes/" + base + "_tiles.png"},
                            {"tile", kTile}, {"cols", kAtlasCols},
                            {"frames", framesJson}}},
        {"palette", paletteJson},
        {"light_tables", lightTablesJson},
        {"light_style", spec->lightStyle},
        {"prefill_micro", spec->prefillMicro},
        {"solid", flagArr(solid)},
        {"block_light", flagArr(blockLight)},
        {"block_missile", flagArr(blockMissile)},
        {"transparent", flagArr(transparent)},
        {"special", flagArr(special)},
        {"doors", doorsJson},
        {"background", mcp::json::array({bg[0], bg[1], bg[2], bg[3]})},
    };
    if (!spec->specialCel.empty() && !specialFramesJson.empty()) {
        layout["special_atlas"] = mcp::json{
            {"url", specialUrl}, {"frames", specialFramesJson}};
    }
    if (lightingEnabled) {
        mcp::json lightArr = mcp::json::array();
        for (uint8_t v : lightVec) lightArr.push_back(v);
        layout["lighting"] = true;
        layout["ambient"] = ambient;
        layout["light"] = lightArr;
    } else {
        layout["lighting"] = false;
    }

    // Phase 2: always ship the light-source definitions so the front-end can
    // recompute the per-cell dLight map locally (real-time ambient slider +
    // draggable torch) with ZERO server round-trips, reproducing scene.build's
    // lit output exactly. `default_ambient` is the scene's ambient light level;
    // `lights` is the resolved source list (the auto central torch when lighting
    // was requested, else a fallback central torch so the front-end's default
    // lit state always matches scene.build's lit default); `soft_falloff` maps
    // to light_style == 2 (Nest/Crypt quadratic falloff).
    layout["default_ambient"] = ambient;
    layout["soft_falloff"] = (spec->lightStyle == 2);
    {
        mcp::json lightsJson = mcp::json::array();
        if (lights.empty()) {
            lightsJson.push_back(mcp::json{
                {"x", gw / 2}, {"y", gh / 2}, {"radius", 15},
                {"offset_x", 0}, {"offset_y", 0}});
        } else {
            for (const auto& s : lights) {
                lightsJson.push_back(mcp::json{
                    {"x", s.x}, {"y", s.y}, {"radius", s.radius},
                    {"offset_x", s.offsetX}, {"offset_y", s.offsetY}});
            }
        }
        layout["lights"] = lightsJson;
    }

    mcp::ToolResult r;
    r.value = mcp::json{
        {"scene_id", id}, {"title", spec->title},
        {"micro_len", microLen}, {"grid_width", gw}, {"grid_height", gh},
        {"tiles_url", "/scenes/" + base + "_tiles.png"},
        {"tiles_width", atlasW}, {"tiles_height", atlasH},
        {"special_url", specialUrl},
        {"layout", layout},
        {"frame_count", static_cast<int>(tileFrames.size())},
        {"has_lighting", lightingEnabled},
    };

    // ---- persist cache markers (fixed scenes only) ----
    if (!skipCache) {
        std::ofstream fk(keyPath); if (fk) fk << cacheKey;
        std::ofstream fr(resultPath); if (fr) fr << r.value.dump();
    }
    mcp::tools::reportProgress(100, "完成");
    return r;
}

// Public scene.tiles: fixed scene, hash-derived stem, normal cache reuse.
mcp::ToolResult toolSceneTiles(const mcp::json& params) {
    return sceneTilesImpl(params, "", nullptr, false);
}

mcp::ToolResult toolSceneBuild(const mcp::json& params) {
    const std::string id = reqStr(params, "id");
    const decoders::SceneSpec* spec = decoders::findScene(id);
    if (!spec)
        throw std::runtime_error("unknown scene id: " + id);
    const std::vector<std::string> archives = archiveStack(params);
    const std::string outDir = params.value("out", std::string("out/scenes"));
    const int maxW = params.value("preview_max_width", 1000);

    // A stable signature of everything affecting the output. Two scenes that
    // differ ONLY in lighting produce DIFFERENT keys, so each lighting state gets
    // its OWN on-disk cache (tristram_<hash>.png / .key / .result.json ...). They
    // are NOT generated together — only the requested variant is built; the other
    // persists once created, so toggling lighting back reuses it instead of
    // re-rendering. (The <hash> suffix namespaces the artifacts per parameter
    // set; without it both variants shared one filename and clobbered each other,
    // so only the last-built variant ever survived.)
    const std::string cacheKey = buildCacheKey(id, archives, *spec, params, maxW, outDir);
    const std::string tag = hashHex(cacheKey);

    // Resolve output paths up front so the cache check below and the (later)
    // write step share the same names. The <tag> suffix namespaces the artifacts
    // per parameter set, letting lit/unlit (and any other variant) coexist.
    const std::string base = sanitize(id) + "_" + tag;
    std::error_code ec;
    std::filesystem::create_directories(outDir, ec);
    const std::string pngPath = outDir + "/" + base + ".png";
    const std::string previewPath = outDir + "/" + base + "_preview.png";
    const std::string overlayPath = outDir + "/" + base + "_overlay.png";
    const std::string nativeOverlayPath = outDir + "/" + base + "_overlay_native.png";
    const std::string metaPath = outDir + "/" + base + ".json";
    const std::string keyPath = outDir + "/" + base + ".key";
    const std::string resultPath = outDir + "/" + base + ".result.json";

    // Cache reuse: if a prior build used identical parameters, return the stored
    // result WITHOUT re-rendering (the PNGs on disk are still served via HTTP).
    if (fileExists(keyPath) && fileExists(pngPath) && fileExists(previewPath) &&
        fileExists(nativeOverlayPath)) {
        std::string stored;
        if (readFile(keyPath, stored) && stored == cacheKey) {
            std::string resTxt;
            if (readFile(resultPath, resTxt)) {
                mcp::tools::reportProgress(100, "复用缓存");
                mcp::ToolResult r;
                r.value = mcp::json::parse(resTxt);
                return r;
            }
        }
    }

    mcp::tools::reportProgress(2, "解析场景规格 " + id);
    const bool dirtBorder = params.value("dirt_border", false);
    SceneAssets assets = loadSceneAssets(*spec, archives, dirtBorder);
    mcp::tools::reportProgress(35, "提取 MPQ 资源 / 组装网格");
    const decoders::SceneGrid& grid = assets.grid;
    decoders::MapRenderOptions opts;
    opts.prefillMicro = spec->prefillMicro;
    const uint32_t bgc = spec->background;
    opts.background[0] = static_cast<uint8_t>((bgc >> 16) & 0xFF);
    opts.background[1] = static_cast<uint8_t>((bgc >> 8) & 0xFF);
    opts.background[2] = static_cast<uint8_t>(bgc & 0xFF);
    opts.background[3] = static_cast<uint8_t>((bgc >> 24) & 0xFF);

    uint8_t ambient = spec->ambientLight;
    std::vector<decoders::LightSource> lights;
    bool autoLight = false;
    opts.lightLevels = makeSceneLighting(params, *spec, grid.gw, grid.gh,
                                         ambient, lights, autoLight);
    opts.lightStyle = spec->lightStyle;
    const bool lightingEnabled = !opts.lightLevels.empty();
    if (lightingEnabled)
        mcp::tools::reportProgress(50, "计算光照贴图");
    opts.special = grid.special;
    opts.specialSprites = std::move(assets.specialSprites);
    opts.specialSpritesLit = std::move(assets.specialSpritesLit);

    decoders::MapRenderResult map = decoders::renderMap(
        assets.reencoded, assets.micros, grid.micro, grid.gw, grid.gh,
        assets.solFlags, std::vector<int8_t>{}, assets.palRgba.data(),
        spec->microLen, opts);
    if (map.width == 0 || map.height == 0)
        throw std::runtime_error("scene render produced empty output");
    mcp::tools::reportProgress(72, "渲染等距地图");

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
    // Native-size overlay (step=1) so it aligns pixel-perfectly with the full
    // native base image when the web viewer loads both by URL.
    std::vector<uint8_t> nativeOverlay =
        buildOverlay(map, spec->microLen, 1, map.width, map.height);

    const std::vector<uint8_t> nativePng =
        decoders::encodePNG(static_cast<uint16_t>(map.width),
                            static_cast<uint16_t>(map.height), map.rgba);
    const std::vector<uint8_t> previewPng =
        decoders::encodePNG(static_cast<uint16_t>(pw), static_cast<uint16_t>(ph), preview);
    const std::vector<uint8_t> overlayPng =
        decoders::encodePNG(static_cast<uint16_t>(pw), static_cast<uint16_t>(ph), overlay);
    const std::vector<uint8_t> nativeOverlayPng =
        decoders::encodePNG(static_cast<uint16_t>(map.width),
                            static_cast<uint16_t>(map.height), nativeOverlay);
    if (nativePng.empty() || previewPng.empty() || overlayPng.empty()
        || nativeOverlayPng.empty())
        throw std::runtime_error("PNG encode failed");
    mcp::tools::reportProgress(88, "编码 PNG / 生成叠加层");

    // Write artifacts to disk (paths/base were resolved earlier, before the cache
    // check).
    auto writeBytes = [](const std::string& p, const std::vector<uint8_t>& b) {
        std::ofstream f(p, std::ios::binary);
        if (f) f.write(reinterpret_cast<const char*>(b.data()),
                       static_cast<std::streamsize>(b.size()));
    };
    writeBytes(pngPath, nativePng);
    writeBytes(previewPath, previewPng);
    writeBytes(overlayPath, overlayPng);
    writeBytes(nativeOverlayPath, nativeOverlayPng);
    mcp::tools::reportProgress(98, "写入磁盘");

    // Sidecar metadata (per-cell flag grids).
    size_t solidCount = 0, transCount = 0, lightLitCount = 0, specialCount = 0;
    for (size_t i = 0; i < map.solid.size(); ++i) {
        if (map.solid[i]) ++solidCount;
        if (map.transparent[i]) ++transCount;
        if (lightingEnabled && i < map.light.size() && map.light[i] == 0) ++lightLitCount;
        if (i < grid.special.size() && grid.special[i] != 0) ++specialCount;
    }
    mcp::json solidArr = mcp::json::array(), transArr = mcp::json::array();
    mcp::json blArr = mcp::json::array(), bmArr = mcp::json::array();
    mcp::json walkArr = mcp::json::array(), lightArr = mcp::json::array();
    for (size_t i = 0; i < map.solid.size(); ++i) {
        solidArr.push_back(map.solid[i]);
        transArr.push_back(map.transparent[i]);
        blArr.push_back(map.blockLight[i]);
        bmArr.push_back(map.blockMissile[i]);
        walkArr.push_back(map.solid[i] ? 0 : 1);
        if (lightingEnabled && i < map.light.size())
            lightArr.push_back(map.light[i]);
    }
    mcp::json meta = mcp::json{
        {"scene_id", id}, {"grid_width", map.gridW}, {"grid_height", map.gridH},
        {"micro_len", spec->microLen}, {"frame_count", assets.reencoded.numFrames},
        {"native_width", map.width}, {"native_height", map.height},
        {"transparent_pixel_count", transparentCount},
        {"solid", solidArr}, {"transparent", transArr},
        {"block_light", blArr}, {"block_missile", bmArr},
        {"walkable", walkArr},
    };
    meta["special_count"] = specialCount;
    const size_t specialLitLevels = opts.specialSpritesLit.size();
    meta["special_lit_levels"] = static_cast<int>(specialLitLevels);
    meta["special"] = mcp::json::array();
    for (const uint8_t id : grid.special)
        meta["special"].push_back(id);
    if (lightingEnabled) {
        meta["light"] = lightArr;
        meta["ambient"] = ambient;
        meta["light_style"] = spec->lightStyle;
    }
    {
        std::ofstream f(metaPath);
        if (f) f << meta.dump();
    }

    const int walkableCount =
        decoders::countWalkableCells(assets.solid, map.gridW, map.gridH);
    mcp::tools::reportProgress(100, "完成");

    mcp::ToolResult r;
    r.value = mcp::json{
        {"scene_id", id}, {"title", spec->title},
        {"dirt_border", dirtBorder},
        {"image_path", pngPath}, {"preview_path", previewPath},
        {"overlay_path", overlayPath}, {"meta_path", metaPath},
        // HTTP endpoints served by the mcp-server on the same port as the WS
        // bridge: /scenes/<id>.png (native), /scenes/<id>_preview.png
        // (downscaled), /scenes/<id>_overlay_native.png (native overlay).
        {"image_url", "/scenes/" + base + ".png"},
        {"preview_url", "/scenes/" + base + "_preview.png"},
        {"overlay_url", "/scenes/" + base + "_overlay_native.png"},
        {"image_base64", mcp::base64::encode(previewPng)},
        {"overlay_base64", mcp::base64::encode(overlayPng)},
        {"width", map.width}, {"height", map.height},
        {"preview_width", pw}, {"preview_height", ph}, {"scale", step},
        {"grid_width", map.gridW}, {"grid_height", map.gridH},
        {"micro_len", spec->microLen}, {"frame_count", assets.reencoded.numFrames},
        {"solid_count", solidCount}, {"walkable_count", walkableCount},
        {"transparent_count", transCount},
        {"transparent_pixel_count", transparentCount},
        {"special_count", specialCount},
        {"lighting", lightingEnabled}, {"ambient", ambient},
        {"light_lit_count", lightLitCount},
        {"meta", meta},
    };

    // Persist cache markers so an identical future request can be served from
    // disk without re-rendering: the key (parameter signature) and the full
    // result JSON.
    {
        std::ofstream fk(keyPath); if (fk) fk << cacheKey;
        std::ofstream fr(resultPath); if (fr) fr << r.value.dump();
    }
    return r;
}

mcp::ToolResult toolScenePath(const mcp::json& params) {
    const std::string id = reqStr(params, "id");
    const decoders::SceneSpec* spec = decoders::findScene(id);
    if (!spec)
        throw std::runtime_error("unknown scene id: " + id);
    const std::vector<std::string> archives = archiveStack(params);

    auto readPoint = [&](const char* key) -> decoders::PathPoint {
        if (!params.contains(key) || !params[key].is_object())
            throw std::invalid_argument(std::string("missing point object param: ") + key);
        const mcp::json& p = params[key];
        if (!p.contains("x") || !p.contains("y") || !p["x"].is_number_integer()
            || !p["y"].is_number_integer())
            throw std::invalid_argument(std::string("point `") + key + "` needs integer x/y");
        return decoders::PathPoint{ p["x"].get<int>(), p["y"].get<int>() };
    };
    const decoders::PathPoint from = readPoint("from");
    const decoders::PathPoint to = readPoint("to");
    const int maxSteps = std::clamp<int>(params.value("max_steps", 1024), 1, 100000);

    // Match scene.build: with dirt_border:true the grid gains a 32-micro
    // border and the DUN offset shifts by 16, so from/to stay aligned.
    const bool dirtBorder = params.value("dirt_border", false);
    SceneAssets assets = loadSceneAssets(*spec, archives, dirtBorder);
    const int gw = assets.grid.gw;
    const int gh = assets.grid.gh;
    const decoders::PathResult found = decoders::findPath(
        assets.solid, gw, gh, from.x, from.y, to.x, to.y, maxSteps);

    const decoders::SceneProjection proj =
        decoders::sceneProjection(gw, gh, spec->microLen);
    const int maxW = params.value("preview_max_width", 1000);
    const int scale = (proj.width > maxW && maxW > 0)
                          ? static_cast<int>((proj.width + maxW - 1) / maxW) : 1;
    const int Hm = (spec->microLen / 2) * 32;

    // Project a cell to the centre of its 64x32 floor diamond, matching
    // renderMap()/buildOverlay() exactly.
    auto project = [&](const decoders::PathPoint& p) {
        const long sx = (static_cast<long>(p.x) - p.y) * 32 - proj.minX + 32;
        const long colTop = (static_cast<long>(p.x) + p.y) * 16 - proj.minY - Hm + 1;
        const long sy = colTop + (Hm - 32) + 16;
        return std::pair<int, int>{ static_cast<int>(sx), static_cast<int>(sy) };
    };

    mcp::json pathArr = mcp::json::array();
    for (const decoders::PathPoint& p : found.path) {
        const auto [sx, sy] = project(p);
        pathArr.push_back(mcp::json{
            {"x", p.x}, {"y", p.y},
            {"screen_x", sx}, {"screen_y", sy},
            {"preview_x", sx / scale}, {"preview_y", sy / scale},
        });
    }

    mcp::ToolResult r;
    r.value = mcp::json{
        {"scene_id", id}, {"title", spec->title},
        {"dirt_border", dirtBorder},
        {"found", found.found}, {"cost", found.cost},
        {"steps", found.path.empty() ? 0 : static_cast<int>(found.path.size()) - 1},
        {"grid_width", gw}, {"grid_height", gh},
        {"micro_len", spec->microLen},
        {"from", mcp::json{{"x", from.x}, {"y", from.y}}},
        {"to", mcp::json{{"x", to.x}, {"y", to.y}}},
        {"walkable_count", decoders::countWalkableCells(assets.solid, gw, gh)},
        {"scale", scale},
        {"native_width", proj.width}, {"native_height", proj.height},
        {"preview_width", static_cast<int>(proj.width / scale)},
        {"preview_height", static_cast<int>(proj.height / scale)},
        {"path", pathArr},
    };
    return r;
}

// ---- Phase 5: random dungeon maps (scene.random) ----------------------------
// Generates a full Cathedral level (drlg_l1, levels/l1data) from a seed via the
// native dhfc::generateCathedral, then feeds it through the SAME scene.tiles
// pipeline (sceneTilesImpl) so the front-end renders it exactly like a fixed
// scene. Results are persisted as flat files in out/scenes/ (served at
// /scenes/<flat>): one fixed "<id>_default" stem (overwritten on regenerate) +
// normalized-name snapshots created by "save", listed by "list".

static bool writeTextFile(const std::string& path, const std::string& text) {
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
    return true;
}

// "YYYYMMDD_HHMMSS" (for stems) and "YYYY-MM-DD HH:MM:SS" (for display), UTC.
static std::string utcStamp(bool display) {
    std::time_t t = std::time(nullptr);
    std::tm* g = std::gmtime(&t);
    char buf[32];
    if (!g) return std::string("19700101_000000");
    std::strftime(buf, sizeof(buf),
                  display ? "%Y-%m-%d %H:%M:%S" : "%Y%m%d_%H%M%S", g);
    return std::string(buf);
}

static uint32_t makeRandomSeed() {
    std::random_device rd;
    std::mt19937 gen(rd());
    uint32_t s = static_cast<uint32_t>(gen());
    // Mix in the clock so two generators in the same second still differ.
    s ^= static_cast<uint32_t>(std::chrono::steady_clock::now()
                                   .time_since_epoch().count());
    return s == 0 ? 0x9E3779B9u : s;
}

// List the persisted results for one random scene (small .meta.json files).
static mcp::json sceneRandomList(const std::string& id) {
    const std::string outDir = "out/scenes";
    const std::string prefix = sanitize(id) + "_";
    mcp::json results = mcp::json::array();
    std::error_code ec;
    if (std::filesystem::exists(outDir, ec)) {
        for (auto it = std::filesystem::directory_iterator(outDir);
             it != std::filesystem::directory_iterator(); ++it) {
            if (!it->is_regular_file()) continue;
            const std::string fn = it->path().filename().string();
            const std::string suffix = ".meta.json";
            if (fn.compare(0, prefix.size(), prefix) != 0 ||
                fn.size() <= suffix.size() ||
                fn.compare(fn.size() - suffix.size(), suffix.size(), suffix) != 0)
                continue;
            std::string txt;
            if (!readFile(it->path().string(), txt)) continue;
            try {
                mcp::json meta = mcp::json::parse(txt);
                if (!meta.contains("layout_url"))
                    meta["layout_url"] = "/scenes/" +
                        fn.substr(0, fn.size() - suffix.size()) + ".layout.json";
                results.push_back(std::move(meta));
            } catch (...) { /* ignore corrupt meta */ }
        }
    }
    // Default first, then newest (created string sorts chronologically).
    std::vector<mcp::json> v;
    for (auto& e : results) v.push_back(e);
    std::stable_sort(v.begin(), v.end(), [](const mcp::json& a, const mcp::json& b) {
        const bool ad = a.value("is_default", false);
        const bool bd = b.value("is_default", false);
        if (ad != bd) return ad > bd;
        return a.value("created", std::string()) > b.value("created", std::string());
    });
    mcp::json arr = mcp::json::array();
    mcp::json def = nullptr;
    for (auto& e : v) {
        if (e.value("is_default", false) && (def.is_null()))
            def = e.value("result_id", std::string());
        arr.push_back(e);
    }
    return mcp::json{{"results", arr}, {"default", def}, {"count", arr.size()}};
}

// Resolve quest set-piece DUN layers from the MPQ stack. Missing files disable
// just that quest (graceful) so every level remains generatable on any MPQ.
static void loadCathedralSetPieces(const std::vector<std::string>& archives,
                                   dhfc::CathedralOptions& opt,
                                   dhfc::DunTileLayer& butcher,
                                   dhfc::DunTileLayer& skng,
                                   dhfc::DunTileLayer& banner) {
    auto load = [&archives](const char* name, dhfc::DunTileLayer& out) -> bool {
        std::vector<uint8_t> buf;
        if (!extractAny(archives, name, buf)) return false;
        dhfc::DunTileLayer layer = dhfc::parseDunTileLayer(buf);
        if (!layer.valid()) return false;
        out = std::move(layer);
        return true;
    };
    if (opt.butcherQuest) {
        if (load("levels/l1data/rnd6.dun", butcher)) opt.butcherSetPiece = &butcher;
        else opt.butcherQuest = false;
    }
    if (opt.skeletonKingQuest) {
        if (load("levels/l1data/skngdo.dun", skng)) opt.skeletonKingSetPiece = &skng;
        else opt.skeletonKingQuest = false;
    }
    if (opt.bannerQuest) {
        if (load("levels/l1data/banner2.dun", banner)) opt.bannerSetPiece = &banner;
        else opt.bannerQuest = false;
    }
}

mcp::ToolResult toolSceneRandom(const mcp::json& params) {
    const std::string action = reqStr(params, "action");
    const std::string id = reqStr(params, "id");
    const decoders::SceneSpec* spec = decoders::findScene(id);
    if (!spec)
        throw std::runtime_error("unknown scene id: " + id);
    if (spec->randomLevel <= 0)
        throw std::runtime_error("scene '" + id + "' is not randomizable");
    const std::string outDir = "out/scenes"; // serveHttp hardcodes this path
    std::error_code ec;
    std::filesystem::create_directories(outDir, ec);

    if (action == "list") {
        mcp::ToolResult r;
        r.value = sceneRandomList(id);
        return r;
    }

    if (action == "save") {
        const std::string defaultStem = sanitize(id) + "_default";
        const std::string from = params.value("from", defaultStem);
        const std::string fromLayout = outDir + "/" + from + ".layout.json";
        const std::string fromMeta = outDir + "/" + from + ".meta.json";
        std::string layoutTxt, metaTxt;
        if (!readFile(fromLayout, layoutTxt) || !readFile(fromMeta, metaTxt))
            throw std::runtime_error("nothing to save: generate a layout first");
        mcp::json result = mcp::json::parse(layoutTxt);
        mcp::json meta = mcp::json::parse(metaTxt);

        const uint32_t seed = static_cast<uint32_t>(meta.value("seed", 0ULL));
        std::string stem = sanitize(id) + "_" + utcStamp(false) + "_s" +
                           std::to_string(seed);
        // Avoid clobbering an identical second+seed snapshot.
        for (int n = 2; fileExists(outDir + "/" + stem + ".meta.json"); ++n)
            stem = sanitize(id) + "_" + utcStamp(false) + "_s" +
                   std::to_string(seed) + "_" + std::to_string(n);

        // Copy the atlas PNGs so a later regenerate (overwriting _default)
        // cannot corrupt this snapshot, then rewrite all atlas URLs.
        const std::string tilesSrc = outDir + "/" + from + "_tiles.png";
        const std::string tilesDst = outDir + "/" + stem + "_tiles.png";
        if (fileExists(tilesSrc))
            std::filesystem::copy_file(tilesSrc, tilesDst,
                std::filesystem::copy_options::overwrite_existing, ec);
        const std::string specSrc = outDir + "/" + from + "_special.png";
        const std::string specDst = outDir + "/" + stem + "_special.png";
        const bool hasSpecial = fileExists(specSrc);
        if (hasSpecial)
            std::filesystem::copy_file(specSrc, specDst,
                std::filesystem::copy_options::overwrite_existing, ec);

        const std::string newTilesUrl = "/scenes/" + stem + "_tiles.png";
        const std::string newSpecUrl = "/scenes/" + stem + "_special.png";
        result["tiles_url"] = newTilesUrl;
        result["layout"]["atlas"]["url"] = newTilesUrl;
        if (hasSpecial) {
            result["special_url"] = newSpecUrl;
            if (result["layout"].contains("special_atlas"))
                result["layout"]["special_atlas"]["url"] = newSpecUrl;
        }
        result["is_default"] = false;
        result["result_id"] = stem;
        result["created"] = utcStamp(true);
        result["layout_url"] = "/scenes/" + stem + ".layout.json";

        meta["is_default"] = false;
        meta["result_id"] = stem;
        meta["created"] = result["created"].get<std::string>();
        meta["layout_url"] = "/scenes/" + stem + ".layout.json";

        writeTextFile(outDir + "/" + stem + ".layout.json", result.dump());
        writeTextFile(outDir + "/" + stem + ".meta.json", meta.dump());
        mcp::ToolResult r;
        r.value = sceneRandomList(id);
        r.value["saved"] = stem;
        return r;
    }

    if (action != "generate")
        throw std::runtime_error("scene.random action must be generate|save|list");

    // ---- generate ----
    const std::vector<std::string> archives = archiveStack(params);
    uint32_t seed = 0;
    auto seedIt = params.find("seed");
    if (seedIt != params.end() && seedIt->is_number_integer())
        seed = static_cast<uint32_t>(seedIt->get<uint64_t>() & 0xFFFFFFFFull);
    else
        seed = makeRandomSeed();

    const std::string quests = params.value("quests", std::string("vanilla"));
    if (quests != "vanilla" && quests != "none" && quests != "all")
        throw std::runtime_error("quests must be one of: vanilla, none, all");
    const int level = spec->randomLevel;

    // Set-piece loader: graceful on missing file (generators tolerate null).
    const auto loadOpt = [&archives](const std::string& rel) -> std::optional<dhfc::DunTileLayer> {
        std::vector<uint8_t> buf;
        if (!extractAny(archives, rel, buf)) return std::nullopt;
        dhfc::DunTileLayer layer = dhfc::parseDunTileLayer(buf);
        if (!layer.valid()) return std::nullopt;
        return layer;
    };

    mcp::tools::reportProgress(1, "生成随机地牢 L" + std::to_string(level));
    dhfc::DungeonTileGrid grid;
    std::string dungeonType;
    if (level <= 4) {
        dungeonType = "Cathedral";
        dhfc::CathedralOptions opt;
        opt.level = level; opt.seed = seed;
        opt.originalCathedral = params.value("original_cathedral", true);
        opt.multiplayerQuests = false;
        if (quests == "vanilla")      dhfc::applyVanillaCathedralQuests(opt);
        else if (quests == "none")  { opt.butcherQuest = opt.poisonWaterQuest = false;
                                      opt.skeletonKingQuest = opt.bannerQuest = false; }
        else                        { opt.butcherQuest = opt.poisonWaterQuest = true;
                                      opt.skeletonKingQuest = opt.bannerQuest = true; }
        dhfc::DunTileLayer butcher, skng, banner;
        loadCathedralSetPieces(archives, opt, butcher, skng, banner);
        grid = dhfc::generateCathedral(opt);
    } else if (level <= 8) {
        dungeonType = "Catacombs";
        dhfc::CatacombsOptions opt;
        opt.level = level; opt.seed = seed;
        if (quests == "vanilla")      dhfc::applyVanillaCatacombsQuests(opt);
        else if (quests == "none")  { opt.bloodQuest = opt.schambQuest = opt.blindQuest = false; }
        else                        { opt.bloodQuest = opt.schambQuest = opt.blindQuest = true; }
        const std::string ofs = "levels/l2data/";
        auto blind = opt.blindQuest ? loadOpt(ofs + "blind1.dun") : std::nullopt;
        auto blood1 = opt.bloodQuest ? loadOpt(ofs + "blood1.dun") : std::nullopt;
        auto blood2 = opt.bloodQuest ? loadOpt(ofs + "blood2.dun") : std::nullopt;
        auto bonestr1 = opt.schambQuest ? loadOpt(ofs + "bonestr1.dun") : std::nullopt;
        auto bonestr2 = opt.schambQuest ? loadOpt(ofs + "bonestr2.dun") : std::nullopt;
        opt.blind1 = blind ? &*blind : nullptr;
        opt.blood1 = blood1 ? &*blood1 : nullptr;
        opt.blood2 = blood2 ? &*blood2 : nullptr;
        opt.bonestr1 = bonestr1 ? &*bonestr1 : nullptr;
        opt.bonestr2 = bonestr2 ? &*bonestr2 : nullptr;
        grid = dhfc::generateCatacombs(opt);
    } else if (level <= 12) {
        dungeonType = "Caves";
        dhfc::CavesOptions opt;
        opt.level = level; opt.seed = seed;
        if (quests == "vanilla")      dhfc::applyVanillaCavesQuests(opt);
        else if (quests == "none")    opt.anvilQuest = false;
        else                        { opt.anvilQuest = true; opt.anvilQuestLevel = 10; }
        auto anvil = opt.anvilQuest ? loadOpt("levels/l3data/anvil.dun") : std::nullopt;
        opt.anvil = anvil ? &*anvil : nullptr;
        grid = dhfc::generateCaves(opt);
    } else {
        dungeonType = "Hell";
        dhfc::HellOptions opt;
        opt.level = level; opt.seed = seed;
        if (quests == "vanilla")      dhfc::applyVanillaHellQuests(opt);
        else if (quests == "none")  { opt.warlordQuest = opt.betrayerQuest = opt.diabloQuest = false; }
        else                        { opt.warlordQuest = true; opt.betrayerQuest = true;
                                      opt.diabloQuest = (level == 16); }
        const std::string ofs = "levels/l4data/";
        auto warlord = opt.warlordQuest ? loadOpt(ofs + "warlord.dun") : std::nullopt;
        auto warlord2 = opt.warlordQuest ? loadOpt(ofs + "warlord2.dun") : std::nullopt;
        auto vile1 = opt.warlordQuest ? loadOpt(ofs + "vile1.dun") : std::nullopt;
        opt.warlord = warlord ? &*warlord : nullptr;
        opt.warlord2 = warlord2 ? &*warlord2 : nullptr;
        opt.vile1 = vile1 ? &*vile1 : nullptr;
        std::array<std::optional<dhfc::DunTileLayer>, 7> diab;
        if (opt.diabloQuest) {
            static const char* diabNames[7] = { "diab1.dun", "diab2a.dun", "diab2b.dun",
                                                "diab3a.dun", "diab3b.dun", "diab4a.dun", "diab4b.dun" };
            for (int i = 0; i < 7; ++i) {
                diab[i] = loadOpt(ofs + diabNames[i]);
                opt.diab[i] = diab[i] ? &*diab[i] : nullptr;
            }
        }
        grid = dhfc::generateHell(opt);
    }

    // DungeonTileGrid (40x40 uint8, 1-based megatile ids) -> decoders::DunFile.
    decoders::DunFile dun;
    dun.width = static_cast<uint16_t>(grid.Width);
    dun.height = static_cast<uint16_t>(grid.Height);
    dun.tileLayer.resize(static_cast<size_t>(grid.Width) * grid.Height);
    for (size_t i = 0; i < dun.tileLayer.size(); ++i)
        dun.tileLayer[i] = grid.tiles[i];

    // Run the SAME scene.tiles pipeline, with a stable stem + injected DUN,
    // bypassing the cache (fixed stem + changing seed must never hit stale data).
    mcp::json tileParams = mcp::json{
        {"id", id},
        {"out", outDir},
        {"lighting", false}, // front-end owns lighting
        {"dirt_border", params.value("dirt_border", false)},
    };
    if (params.contains("archive")) tileParams["archive"] = params["archive"];
    if (params.contains("archives")) tileParams["archives"] = params["archives"];
    const std::string stem = sanitize(id) + "_default";
    mcp::ToolResult tiles = sceneTilesImpl(tileParams, stem, &dun, true);

    // Merge metadata at the TOP LEVEL so the inline return and the persisted
    // .layout.json are the exact same object the front-end consumes.
    mcp::json result = tiles.value;
    const std::string created = utcStamp(true);
    result["random"] = true;
    result["random_level"] = spec->randomLevel;
    result["dungeon_type"] = dungeonType;
    result["seed"] = static_cast<uint64_t>(seed);
    result["quests"] = quests;
    result["created"] = created;
    result["result_id"] = stem;
    result["is_default"] = true;
    result["layout_url"] = "/scenes/" + stem + ".layout.json";

    mcp::json meta = mcp::json{
        {"id", id}, {"level", spec->randomLevel}, {"dungeon_type", dungeonType},
        {"seed", static_cast<uint64_t>(seed)},
        {"quests", quests}, {"created", created}, {"result_id", stem},
        {"is_default", true}, {"layout_url", "/scenes/" + stem + ".layout.json"},
    };
    writeTextFile(outDir + "/" + stem + ".layout.json", result.dump());
    writeTextFile(outDir + "/" + stem + ".meta.json", meta.dump());

    mcp::ToolResult r;
    r.value = std::move(result);
    return r;
}

} // namespace

namespace mcp {
namespace tools {

mcp::ToolResult toolSceneClearCache(const mcp::json& params) {
    const std::string outDir = params.value("out", std::string("out/scenes"));
    std::error_code ec;
    size_t n = 0;
    if (std::filesystem::exists(outDir, ec)) {
        for (auto it = std::filesystem::directory_iterator(outDir);
             it != std::filesystem::directory_iterator(); ++it) {
            if (it->is_regular_file()) {
                std::filesystem::remove(it->path(), ec);
                ++n;
            }
        }
    }
    mcp::ToolResult r;
    r.value = mcp::json{{"cleared", static_cast<long long>(n)}};
    return r;
}

void registerSceneTools(ToolRegistry& reg) {
    reg.registerTool(ToolDef{
        "scene.list",
        "List available Diablo 1 / Hellfire scenes (Tristram town + L1/L2/L3/L4 "
        "presets) that scene.build can render. Returns id/title/layout/grid dims/"
        "ambient light; no MPQ needed.",
        json{{"type", "object"}, {"properties", json::object()}},
        toolSceneList});

    reg.registerTool(ToolDef{
        "scene.build",
        "Assemble and render a COMPLETE Diablo 1 / Hellfire scene (two-pass "
        "DevilutionX floor+wall rendering, opaque background, prefilled dPiece, "
        "no transparent holes). Writes a native PNG + per-cell SOL/walkable "
        "metadata JSON to disk and returns a bounded base64 PNG preview plus an "
        "aligned overlay PNG (grid lines + solid/transparent/blockLight tint). "
        "Optional `lighting:true` applies DevilutionX dLight shade tables "
        "(ambient + `lights`, auto central torch by default). Dungeon presets "
        "are cropped to the .dun bbox by default; `dirt_border:true` restores "
        "the 32-micro Dirt border for a faithful full dPiece view.",
        json{{"type", "object"},
             {"properties", json{
                 {"id", json{{"type", "string"}, {"description", "scene id from scene.list, e.g. tristram"}}},
                 {"archive", json{{"type", "string"}, {"description", "primary MPQ path"}}},
                 {"archives", json{{"type", "array"}, {"items", json{{"type", "string"}}},
                                    {"description", "additional MPQ search paths"}}},
                 {"out", json{{"type", "string"}, {"description", "output dir (default out/scenes)"}}},
                 {"preview_max_width", json{{"type", "integer"},
                                             {"description", "max preview width, default 1000"}}},
                 {"lighting", json{{"type", "boolean"},
                                    {"description", "apply per-cell dLight shading (default false = FullyLit)"}}},
                 {"ambient", json{{"type", "integer"},
                                  {"description", "ambient light level 0..15 (default per scene: town 0, dungeon 15)"}}},
                 {"lights", json{{"type", "array"}, {"items", json{{"type", "object"}}},
                                  {"description", "static lights: {x,y,radius,offset_x,offset_y} in MICRO coords"}}},
                 {"auto_light", json{{"type", "boolean"},
                                     {"description", "when lighting and no lights: add a central radius-15 torch (default true)"}}},
                 {"dirt_border", json{{"type", "boolean"},
                                       {"description", "dungeon presets: draw DevilutionX's 32-micro Dirt border (faithful dPiece); default false = crop to the .dun bbox so the whole room is visible"}}},
             }},
             {"required", json::array({"id", "archive"})}},
        toolSceneBuild});

    reg.registerTool(ToolDef{
        "scene.tiles",
        "Assemble a Diablo 1 / Hellfire scene into a FRONT-END-RENDERABLE form: a "
        "palette-INDEX tile atlas PNG (R = palette index, A = drawn / 0), a special "
        "sprite index atlas, and a layout JSON (projection, micro_grid, atlas frame "
        "map, palette, 16 dLight shade tables, per-cell SOL/walkable/light flags, "
        "dSpecial overlay ids, and door open/closed pairs). The front-end "
        "SceneRenderer reassembles the isometric map and re-applies per-cell "
        "lighting locally, so opening doors or moving a torch needs no rebuild. "
        "Twin track of scene.build (native PNG) which is kept for export / "
        "thumbnails; this tool is the data source for the web viewer migration.",
        json{{"type", "object"},
             {"properties", json{
                 {"id", json{{"type", "string"}, {"description", "scene id from scene.list, e.g. l1_rnd2"}}},
                 {"archive", json{{"type", "string"}, {"description", "primary MPQ path"}}},
                 {"archives", json{{"type", "array"}, {"items", json{{"type", "string"}}},
                                    {"description", "additional MPQ search paths"}}},
                 {"out", json{{"type", "string"}, {"description", "output dir (default out/scenes)"}}},
                 {"lighting", json{{"type", "boolean"},
                                    {"description", "precompute per-cell dLight levels (default false = FullyLit)"}}},
                 {"ambient", json{{"type", "integer"},
                                  {"description", "ambient light level 0..15 (default per scene)"}}},
                 {"lights", json{{"type", "array"}, {"items", json{{"type", "object"}}},
                                  {"description", "static lights: {x,y,radius,offset_x,offset_y} in MICRO coords"}}},
                 {"auto_light", json{{"type", "boolean"},
                                     {"description", "when lighting and no lights: add a central torch (default true)"}}},
                 {"dirt_border", json{{"type", "boolean"},
                                       {"description", "dungeon presets: draw the 32-micro Dirt border (default false = cropped bbox)"}}},
             }},
             {"required", json::array({"id", "archive"})}},
        toolSceneTiles});

    reg.registerTool(ToolDef{
        "scene.random",
        "Random dungeon map rendering for 4 seed-generated levels, one per "
        "dungeon type: random_l1=L1 Cathedral (drlg_l1), random_l2=L5 Catacombs "
        "(drlg_l2), random_l3=L9 Caves (drlg_l3), random_l4=L13 Hell (drlg_l4). "
        "Listed by scene.list as randomizable:true. Each uses the matching native "
        "DevilutionX generator (dhfc::generateCathedral/Catacombs/Caves/Hell) and "
        "tileset, and emits the SAME front-end-renderable form as scene.tiles "
        "(index atlas PNG + layout JSON). action=generate builds a fresh level "
        "from a random (or explicit) seed, overwrites that level's default cache "
        "and returns the full scene.tiles result inline (with dungeon_type); "
        "action=save normalizes-naming a snapshot of the current result so it can "
        "be switched via the recent-results list; action=list enumerates default "
        "+ saved results. scene.clearCache deletes all default + saved results. "
        "Test script: tests/regress_scene_random.mjs.",
        json{{"type", "object"},
             {"properties", json{
                 {"action", json{{"type", "string"},
                                 {"description", "generate | save | list"}}},
                 {"id", json{{"type", "string"},
                             {"description", "random scene id: random_l1..random_l4 (Cathedral/Catacombs/Caves/Hell)"}}},
                 {"seed", json{{"type", "integer"},
                               {"description", "uint32 seed (generate); omit for a fresh random seed, echoed back"}}},
                 {"quests", json{{"type", "string"},
                                 {"description", "vanilla (default) | none | all (generate)"}}},
                 {"original_cathedral", json{{"type", "boolean"},
                                             {"description", "Diablo original Cathedral (default true); false=Hellfire (generate)"}}},
                 {"dirt_border", json{{"type", "boolean"},
                                      {"description", "draw the faithful 32-micro Dirt border (default false)"}}},
                 {"from", json{{"type", "string"},
                               {"description", "result_id/stem to snapshot (save); defaults to <id>_default"}}},
                 {"archive", json{{"type", "string"}, {"description", "primary MPQ path (generate)"}}},
                 {"archives", json{{"type", "array"}, {"items", json{{"type", "string"}}},
                                   {"description", "additional MPQ search paths (generate)"}}},
             }},
             {"required", json::array({"action", "id"})}},
        toolSceneRandom});

    reg.registerTool(ToolDef{
        "scene.clearCache",
        "Delete all cached scene artifacts under the scene output dir (default "
        "out/scenes): native/overlay/preview PNGs, metadata JSON, and the "
        "per-request cache markers (.key/.result.json). Lets the UI force a fresh "
        "scene.build on the next request. Pass `out` to target a custom dir.",
        json{{"type", "object"},
             {"properties", json{
                 {"out", json{{"type", "string"},
                             {"description", "scene output dir (default out/scenes)"}}}}}},
        toolSceneClearCache});

    reg.registerTool(ToolDef{
        "scene.path",
        "A* path finding over a decoded scene's SOL collision grid (DevilutionX "
        "debug/devices/paths.cpp costs: 100 axis-aligned, 101 diagonal). Returns "
        "the MICRO-tile path plus native/preview screen coordinates aligned with "
        "scene.build's projection. `from`/`to` are {x,y} micro cells.",
        json{{"type", "object"},
             {"properties", json{
                 {"id", json{{"type", "string"}, {"description", "scene id from scene.list"}}},
                 {"archive", json{{"type", "string"}, {"description", "primary MPQ path"}}},
                 {"archives", json{{"type", "array"}, {"items", json{{"type", "string"}}},
                                    {"description", "additional MPQ search paths"}}},
                 {"from", json{{"type", "object"},
                                {"properties", json{{"x", json{{"type", "integer"}}},
                                                    {"y", json{{"type", "integer"}}}}}}},
                 {"to", json{{"type", "object"},
                              {"properties", json{{"x", json{{"type", "integer"}}},
                                                  {"y", json{{"type", "integer"}}}}}}},
                 {"max_steps", json{{"type", "integer"},
                                     {"description", "search step budget, default 1024"}}},
                 {"preview_max_width", json{{"type", "integer"},
                                             {"description", "match scene.build preview scale, default 1000"}}},
                 {"dirt_border", json{{"type", "boolean"},
                                       {"description", "match scene.build: use the 32-micro Dirt border (default false = cropped bbox)"}}},
             }},
             {"required", json::array({"id", "archive", "from", "to"})}},
        toolScenePath});
}

} // namespace tools
} // namespace mcp
