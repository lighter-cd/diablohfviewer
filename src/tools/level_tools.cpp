// src/tools/level_tools.cpp
//
// level.inspect — inspect one level tileset (TIL/MIN/CEL/SOL/PAL) at the
// primitive level: a megatile's four micro pieces, every MIN block's
// TileType/frame, decoded 32x32 frame PNGs, and an assembled isometric image of
// the selected micro pieces. This is the backend for the browser level viewer
// (web/level.html) and is intentionally independent of scene.build.

#include "tools/level_tools.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "decoders/level.h"
#include "decoders/palette.h"
#include "decoders/png.h"
#include "engine/archive.h"
#include "tools/base64.h"

namespace {

std::string reqStr(const mcp::json& p, const char* key)
{
    auto it = p.find(key);
    if (it == p.end() || !it->is_string())
        throw std::invalid_argument(std::string("missing string param: ") + key);
    return it->get<std::string>();
}

bool extractAny(const std::vector<std::string>& archives, const std::string& name,
                std::vector<uint8_t>& out)
{
    for (const auto& a : archives) {
        engine::Archive arc;
        if (!arc.open(a))
            continue;
        if (arc.extract(name, out))
            return true;
    }
    return false;
}

std::vector<std::string> archiveStack(const mcp::json& params)
{
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

const char* typeName(decoders::TileType t)
{
    switch (t) {
    case decoders::TileType::Square: return "Square";
    case decoders::TileType::TransparentSquare: return "TransparentSquare";
    case decoders::TileType::LeftTriangle: return "LeftTriangle";
    case decoders::TileType::RightTriangle: return "RightTriangle";
    case decoders::TileType::LeftTrapezoid: return "LeftTrapezoid";
    case decoders::TileType::RightTrapezoid: return "RightTrapezoid";
    }
    return "Unknown";
}

struct CroppedImage {
    int width = 0;
    int height = 0;
    int left = 0; // offset in the source image
    int top = 0;
    std::vector<uint8_t> rgba;
};

// Crop to the bounding box of non-transparent pixels (plus a small margin) so
// individual 32x256 micro columns don't carry huge transparent areas.
CroppedImage cropOpaque(const std::vector<uint8_t>& rgba, int w, int h, int pad = 0)
{
    CroppedImage out;
    if (w <= 0 || h <= 0)
        return out;
    int minX = w, minY = h, maxX = -1, maxY = -1;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            if (rgba[(static_cast<size_t>(y) * w + x) * 4 + 3] == 0)
                continue;
            minX = std::min(minX, x);
            minY = std::min(minY, y);
            maxX = std::max(maxX, x);
            maxY = std::max(maxY, y);
        }
    }
    if (maxX < 0) {
        out.width = out.height = 1;
        out.rgba.assign(4, 0);
        return out;
    }
    minX = std::max(0, minX - pad);
    minY = std::max(0, minY - pad);
    maxX = std::min(w - 1, maxX + pad);
    maxY = std::min(h - 1, maxY + pad);
    out.left = minX;
    out.top = minY;
    out.width = maxX - minX + 1;
    out.height = maxY - minY + 1;
    out.rgba.resize(static_cast<size_t>(out.width) * out.height * 4);
    for (int y = 0; y < out.height; ++y) {
        const uint8_t* src = rgba.data() + (static_cast<size_t>(y + minY) * w + minX) * 4;
        uint8_t* dst = out.rgba.data() + static_cast<size_t>(y) * out.width * 4;
        std::copy(src, src + static_cast<size_t>(out.width) * 4, dst);
    }
    return out;
}

std::string pngBase64(const std::vector<uint8_t>& rgba, int w, int h)
{
    std::vector<uint8_t> png = decoders::encodePNG(static_cast<uint16_t>(w),
                                                   static_cast<uint16_t>(h), rgba);
    return png.empty() ? std::string() : mcp::base64::encode(png);
}

// DevilutionX uses the FIRST occurrence of a frame to decide its re-encode
// shape (dun_tile_data.cpp frameToTypeMap). Match that for level.inspect.
std::vector<decoders::TileType> buildFirstFrameTypes(
    const std::vector<decoders::MicroTileRef>& micros, int microLen)
{
    uint32_t maxFrame = 0;
    for (const auto& m : micros)
        for (int b = 0; b < microLen && b < 16; ++b)
            if (m.blocks[b].hasValue())
                maxFrame = std::max<uint32_t>(maxFrame, m.blocks[b].frame());
    std::vector<decoders::TileType> types(maxFrame, decoders::TileType::Square);
    std::vector<uint8_t> seen(maxFrame, 0);
    for (const auto& m : micros) {
        for (int b = 0; b < microLen && b < 16; ++b) {
            if (!m.blocks[b].hasValue())
                continue;
            const uint32_t f = m.blocks[b].frame();
            if (f == 0 || f > maxFrame)
                continue;
            if (!seen[f - 1]) {
                seen[f - 1] = 1;
                types[f - 1] = m.blocks[b].type();
            }
        }
    }
    return types;
}

mcp::json blockJson(const decoders::LevelCelBlock& blk, int index)
{
    const decoders::TileType t = blk.type();
    return mcp::json{
        {"mt", index},
        {"data", blk.data},
        {"has_value", blk.hasValue() && blk.frame() != 0},
        {"type", static_cast<int>(t)},
        {"type_name", typeName(t)},
        {"frame", blk.hasValue() ? blk.frame() : 0},
    };
}

mcp::ToolResult toolLevelInspect(const mcp::json& params)
{
    const std::vector<std::string> archives = archiveStack(params);
    const std::string celName = reqStr(params, "cel");
    const std::string tilName = reqStr(params, "til");
    const std::string minName = reqStr(params, "min");
    const std::string palName = reqStr(params, "palette");
    const std::string solName = params.value("sol", std::string());
    const int blocks = params.value("blocks", 16);
    const int microLen = params.value("micro_len", blocks);

    std::vector<uint8_t> celBuf, tilBuf, minBuf, palBuf, solBuf;
    auto need = [&](const std::string& n, std::vector<uint8_t>& b) {
        if (!extractAny(archives, n, b))
            throw std::runtime_error("extract failed (name not found?): " + n);
    };
    need(celName, celBuf);
    need(tilName, tilBuf);
    need(minName, minBuf);
    need(palName, palBuf);
    if (!solName.empty())
        (void)extractAny(archives, solName, solBuf); // optional

    auto megatiles = decoders::loadTil(tilBuf);
    auto micros = decoders::loadMin(minBuf, static_cast<uint8_t>(blocks),
                                    static_cast<uint8_t>(microLen));
    auto palRgb = decoders::loadPalette(palBuf);
    if (megatiles.empty()) throw std::runtime_error("TIL parse failed");
    if (micros.empty()) throw std::runtime_error("MIN parse failed");
    if (palRgb.empty()) throw std::runtime_error("palette parse failed");
    const std::vector<uint8_t> palRgba = decoders::paletteToRGBA(palRgb);
    const std::vector<uint8_t> solFlags = decoders::loadSol(solBuf);

    auto frameTypes = buildFirstFrameTypes(micros, microLen);
    decoders::ReencodedLevelCel re =
        decoders::reencodeLevelCel(celBuf, frameTypes);
    if (re.numFrames == 0) throw std::runtime_error("level CEL parse failed");

    // Select micro pieces: explicit `micros[]`, or a 1-based `megatile`.
    std::vector<uint16_t> selectedMicros;
    int gridW = 2, gridH = 1;
    int megatileId = params.value("megatile", 0);
    if (megatileId > 0) {
        const size_t ti = static_cast<size_t>(megatileId - 1);
        if (ti >= megatiles.size())
            throw std::invalid_argument("megatile out of range (1-based)");
        const decoders::MegaTile& mt = megatiles[ti];
        selectedMicros = { mt.micro1, mt.micro2, mt.micro3, mt.micro4 };
        gridW = 2;
        gridH = 2;
    } else if (params.contains("micros") && params["micros"].is_array()) {
        for (const auto& m : params["micros"]) {
            if (m.is_number_integer())
                selectedMicros.push_back(static_cast<uint16_t>(m.get<int>()));
        }
        gridW = params.value("grid_width", 2);
        gridH = (static_cast<int>(selectedMicros.size()) + gridW - 1) / gridW;
    } else {
        throw std::invalid_argument("provide `megatile` (1-based) or `micros` array");
    }

    // Collect referenced (frame, type) pairs from the selected micros.
    std::map<uint32_t, std::set<int>> frameTypesUsed;
    for (uint16_t mi : selectedMicros) {
        if (mi >= micros.size())
            continue;
        for (int b = 0; b < microLen && b < 16; ++b) {
            const decoders::LevelCelBlock& blk = micros[mi].blocks[b];
            if (!blk.hasValue() || blk.frame() == 0)
                continue;
            frameTypesUsed[blk.frame()].insert(static_cast<int>(blk.type()));
        }
    }
    // Optional explicit `frames[]` (1-based), useful when inspecting unused frames.
    if (params.contains("frames") && params["frames"].is_array()) {
        for (const auto& f : params["frames"]) {
            if (!f.is_number_integer())
                continue;
            const uint32_t frame = static_cast<uint32_t>(f.get<int>());
            if (frame >= 1 && frame <= re.numFrames && frameTypesUsed.find(frame) == frameTypesUsed.end())
                frameTypesUsed[frame] = { static_cast<int>(re.frameTypes[frame - 1]) };
        }
    }

    mcp::json frameArr = mcp::json::array();
    for (const auto& [frame, tset] : frameTypesUsed) {
        for (int ti : tset) {
            const decoders::TileType type = static_cast<decoders::TileType>(ti);
            std::vector<uint8_t> rgba;
            int h = 0;
            decoders::decodeReencodedFrameAs(re, frame - 1, type, palRgba.data(), rgba, h);
            const CroppedImage crop = cropOpaque(rgba, 32, h, 0);
            frameArr.push_back(mcp::json{
                {"frame", frame},
                {"type", ti},
                {"type_name", typeName(type)},
                {"width", crop.width},
                {"height", crop.height},
                {"png_base64", pngBase64(crop.rgba, crop.width, crop.height)},
            });
        }
    }

    // Per-micro detailed block list + individual render (cropped).
    mcp::json microArr = mcp::json::array();
    for (uint16_t mi : selectedMicros) {
        mcp::json blocksArr = mcp::json::array();
        if (mi < micros.size()) {
            for (int b = 0; b < microLen && b < 16; ++b)
                blocksArr.push_back(blockJson(micros[mi].blocks[b], b));
        }
        std::vector<uint8_t> rgba;
        int mw = 0, mh = 0;
        if (mi < micros.size())
            decoders::renderMicroTile(re, micros[mi], palRgba.data(), microLen, rgba, mw, mh);
        const CroppedImage crop = cropOpaque(rgba, mw, mh, 0);
        microArr.push_back(mcp::json{
            {"id", mi},
            {"blocks", blocksArr},
            {"width", crop.width},
            {"height", crop.height},
            {"png_base64", pngBase64(crop.rgba, crop.width, crop.height)},
        });
    }

    // Assemble the selected micro pieces into an isometric preview on a
    // transparent background (same projection as scene.build/renderMap).
    std::vector<uint16_t> microGrid(static_cast<size_t>(gridW) * gridH, decoders::kNoMicro);
    for (size_t i = 0; i < selectedMicros.size(); ++i)
        microGrid[i] = selectedMicros[i];
    decoders::MapRenderOptions opts;
    opts.prefillMicro = decoders::kNoMicro;
    opts.background[0] = opts.background[1] = opts.background[2] = opts.background[3] = 0;
    decoders::MapRenderResult map = decoders::renderMap(
        re, micros, microGrid, gridW, gridH, solFlags,
        std::vector<int8_t>{}, palRgba.data(), microLen, opts);
    const CroppedImage assembly = cropOpaque(map.rgba, map.width, map.height, 2);

    mcp::ToolResult r;
    r.value = mcp::json{
        {"cel", celName}, {"til", tilName}, {"min", minName},
        {"palette", palName}, {"sol", solName},
        {"blocks", blocks}, {"micro_len", microLen},
        {"megatile", megatileId},
        {"megatile_count", megatiles.size()},
        {"micro_count", micros.size()},
        {"grid_width", gridW}, {"grid_height", gridH},
        {"micros", microArr},
        {"frames", frameArr},
        {"assembly", mcp::json{
             {"width", assembly.width}, {"height", assembly.height},
             {"native_width", map.width}, {"native_height", map.height},
             {"left", assembly.left}, {"top", assembly.top},
             {"png_base64", pngBase64(assembly.rgba, assembly.width, assembly.height)},
         }},
    };
    return r;
}

} // namespace

namespace mcp {
namespace tools {

void registerLevelTools(ToolRegistry& reg)
{
    reg.registerTool(ToolDef{
        "level.inspect",
        "Inspect a Diablo 1/Hellfire level tileset at the primitive level: "
        "megatile -> micro pieces -> MIN blocks (TileType/frame) -> decoded CEL "
        "frames, plus an assembled isometric preview. Used to debug individual "
        "map objects such as the Tristram fountain (megatile 71 / pieces 257-260 "
        "/ town.cel frames 594-605).",
        json{{"type", "object"},
             {"properties", json{
                 {"archive", json{{"type", "string"}, {"description", "primary MPQ path"}}},
                 {"archives", json{{"type", "array"}, {"items", json{{"type", "string"}}},
                                    {"description", "additional MPQ search paths"}}},
                 {"cel", json{{"type", "string"}, {"description", "level CEL path"}}},
                 {"til", json{{"type", "string"}, {"description", "TIL path"}}},
                 {"min", json{{"type", "string"}, {"description", "MIN path"}}},
                 {"sol", json{{"type", "string"}, {"description", "optional SOL path"}}},
                 {"palette", json{{"type", "string"}, {"description", "palette path"}}},
                 {"blocks", json{{"type", "integer"}, {"description", "MIN stride: 16 town/L4, 10 L1-L3"}}},
                 {"micro_len", json{{"type", "integer"}, {"description", "drawn micro blocks: 16 town, 10 L1-L3, 12 L4"}}},
                 {"megatile", json{{"type", "integer"}, {"description", "1-based megatile id in TIL"}}},
                 {"micros", json{{"type", "array"}, {"items", json{{"type", "integer"}}},
                                  {"description", "0-based micro piece ids (alternative to megatile)"}}},
                 {"grid_width", json{{"type", "integer"}, {"description", "micro grid width for `micros` (default 2)"}}},
                 {"frames", json{{"type", "array"}, {"items", json{{"type", "integer"}}},
                                  {"description", "extra 1-based CEL frames to decode"}}},
             }},
             {"required", json::array({"archive", "cel", "til", "min", "palette", "megatile"})}},
        toolLevelInspect});
}

} // namespace tools
} // namespace mcp
