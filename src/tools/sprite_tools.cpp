// src/tools/sprite_tools.cpp
//
// Implements palette.load and sprite.decode. Both pull files out of an MPQ through
// engine::Archive and feed them to the decoders in src/decoders/, returning RGBA
// the frontend can render directly.

#include "tools/registry.h"
#include "tools/base64.h"
#include "engine/archive.h"

#include "decoders/palette.h"
#include "decoders/clx.h"
#include "decoders/cel.h"
#include "decoders/cl2.h"
#include "decoders/pcx.h"
#include "decoders/sprite_width.h"

#include <filesystem>

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

// ---- small shared helpers (mirror mpq_tools.cpp) ----------------------------

std::string reqStr(const mcp::json& p, const char* key)
{
    auto it = p.find(key);
    if (it == p.end() || !it->is_string())
        throw std::invalid_argument(std::string("missing string param: ") + key);
    return it->get<std::string>();
}

engine::Archive openOrThrow(const std::string& path)
{
    engine::Archive a;
    if (!a.open(path))
        throw std::runtime_error("cannot open archive: " + path);
    return a; // Archive is move-constructible
}

std::vector<uint8_t> extractBuf(engine::Archive& a, const std::string& name)
{
    std::vector<uint8_t> buf;
    if (!a.extract(name, buf))
        throw std::runtime_error("extract failed (name not found?): " + name);
    return buf;
}

std::string toLower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

bool endsWith(const std::string& s, const char* suf)
{
    size_t n = std::strlen(suf);
    return s.size() >= n && s.compare(s.size() - n, n, suf) == 0;
}

// Build a 768-byte RGB palette from raw bytes (768, 1024=RGBA, or >=768).
std::vector<uint8_t> paletteFromBytes(const std::vector<uint8_t>& raw)
{
    if (raw.size() == 256 * 3)
        return raw;
    if (raw.size() == 256 * 4) {
        std::vector<uint8_t> rgb;
        rgb.reserve(256 * 3);
        for (int i = 0; i < 256; ++i) {
            rgb.push_back(raw[static_cast<size_t>(i) * 4 + 0]);
            rgb.push_back(raw[static_cast<size_t>(i) * 4 + 1]);
            rgb.push_back(raw[static_cast<size_t>(i) * 4 + 2]);
        }
        return rgb;
    }
    return decoders::loadPalette(raw);
}

// Resolve a palette, trying (in priority order):
//   1. palette_b64        (base64 of 768 or 1024 bytes)
//   2. palette_name       (in-archive .pal)
//   3. embeddedPal        (e.g. a PCX-embedded palette)
//   4. <base>.pal         (same directory/stem as the sprite)
// Throws if nothing usable is found.
std::vector<uint8_t> resolvePalette(engine::Archive& a, const mcp::json& params,
                                    const std::string& spriteName,
                                    const std::vector<uint8_t>* embeddedPal)
{
    if (params.contains("palette_b64") && params["palette_b64"].is_string()) {
        auto raw = paletteFromBytes(mcp::base64::decode(params["palette_b64"].get<std::string>()));
        if (raw.size() == 256 * 3)
            return raw;
        throw std::runtime_error("palette_b64 is not a valid 256-colour palette");
    }
    if (params.contains("palette_name") && params["palette_name"].is_string()) {
        std::string pn = params["palette_name"].get<std::string>();
        auto pal = decoders::loadPalette(extractBuf(a, pn));
        if (pal.size() == 256 * 3)
            return pal;
        throw std::runtime_error("palette not valid: " + pn);
    }
    if (embeddedPal && embeddedPal->size() == 256 * 3)
        return *embeddedPal;

    // Try <base>.pal alongside the sprite.
    std::string base = spriteName;
    auto pos = base.find_last_of("\\/");
    std::string dir  = (pos == std::string::npos) ? "" : base.substr(0, pos + 1);
    std::string file = (pos == std::string::npos) ? base : base.substr(pos + 1);
    auto dot = file.find_last_of('.');
    std::string stem = (dot == std::string::npos) ? file : file.substr(0, dot);
    std::string palName = dir + stem + ".pal";
    if (a.verify(palName)) {
        auto pal = decoders::loadPalette(extractBuf(a, palName));
        if (pal.size() == 256 * 3)
            return pal;
    }
    throw std::runtime_error(
        "no palette supplied and none found (pass palette_name or palette_b64); tried " + palName);
}

// ---- tools ----------------------------------------------------------------

mcp::ToolResult toolPaletteLoad(const mcp::json& params)
{
    auto a = openOrThrow(reqStr(params, "archive"));
    std::string name = reqStr(params, "name");
    auto rgb = decoders::loadPalette(extractBuf(a, name));
    if (rgb.size() != 256 * 3)
        throw std::runtime_error("invalid palette (need 256*3 bytes): " + name);

    auto rgba = decoders::paletteToRGBA(rgb);
    mcp::json colors = mcp::json::array();
    for (int i = 0; i < 256; ++i)
        colors.push_back(mcp::json::array({ rgb[static_cast<size_t>(i) * 3],
                                             rgb[static_cast<size_t>(i) * 3 + 1],
                                             rgb[static_cast<size_t>(i) * 3 + 2] }));

    mcp::ToolResult r;
    r.value = mcp::json{ { "name", name },
                         { "count", 256 },
                         { "colors", colors },
                         { "rgba_base64", mcp::base64::encode(rgba) } };
    return r;
}

mcp::ToolResult toolSpriteDecode(const mcp::json& params)
{
    std::string archive = reqStr(params, "archive");
    std::string name    = reqStr(params, "name");
    auto a = openOrThrow(archive);
    auto buf = extractBuf(a, name);

    std::string lower = toLower(name);
    std::vector<uint8_t> clxBytes;
    std::string format;
    std::vector<decoders::DecodedFrame> frames;
    std::vector<uint8_t> embeddedPal;
    bool havePal = false;

    // Resolve width for CL2/CEL. Supports an explicit integer or the string
    // "auto" (look up from DevilutionX data tables by the sprite path).
    int spriteWidth = 0;
    bool useAutoWidth = false;
    if (params.contains("width")) {
        const auto& w = params["width"];
        if (w.is_string())
            useAutoWidth = (w.get<std::string>() == "auto");
        else if (w.is_number_integer())
            spriteWidth = w.get<int>();
    }
    if (useAutoWidth) {
        std::filesystem::path devRoot =
            (params.contains("devilutionx_root") && params["devilutionx_root"].is_string())
                ? std::filesystem::path(params["devilutionx_root"].get<std::string>())
                : std::filesystem::path("D:/work/xt/diablo/DevilutionX");
        std::string dbg;
        spriteWidth = decoders::sprite_width::autoWidth(name, devRoot, dbg);
        if (spriteWidth <= 0)
            throw std::runtime_error("auto width failed: " + dbg);
    }

    if (endsWith(lower, ".clx")) {
        format = "clx";
        clxBytes = buf;
    } else if (endsWith(lower, ".cl2")) {
        format = "cl2";
        if (spriteWidth <= 0)
            throw std::runtime_error("cl2 requires a positive integer 'width' or width:\"auto\"");
        clxBytes = decoders::cl2ToClx(buf.data(), buf.size(), static_cast<uint16_t>(spriteWidth));
    } else if (endsWith(lower, ".cel")) {
        format = "cel";
        if (spriteWidth <= 0)
            throw std::runtime_error("cel requires a positive integer 'width' or width:\"auto\"");
        clxBytes = decoders::celToClx(buf.data(), buf.size(), static_cast<uint16_t>(spriteWidth));
    } else if (endsWith(lower, ".pcx")) {
        format = "pcx";
        decoders::DecodedFrame fr;
        if (!decoders::decodePcx(buf, fr, &embeddedPal))
            throw std::runtime_error("unsupported or truncated PCX: " + name);
        frames.push_back(std::move(fr));
    } else {
        throw std::runtime_error(
            "unsupported sprite format (expected .clx/.cl2/.cel/.pcx): " + name);
    }

    if (format == "pcx") {
        // Prefer an explicitly supplied palette; otherwise keep the embedded one.
        try {
            std::vector<uint8_t> pal = resolvePalette(a, params, name, &embeddedPal);
            decoders::DecodedFrame fr;
            decoders::decodePcx(buf, fr, nullptr, &pal);
            frames.clear();
            frames.push_back(std::move(fr));
            havePal = true;
        } catch (const std::exception&) {
            havePal = !embeddedPal.empty();
        }
    } else {
        std::vector<uint8_t> pal = resolvePalette(a, params, name, nullptr);
        havePal = true;
        frames = decoders::decodeClx(clxBytes, pal);
    }

    mcp::json framesJson = mcp::json::array();
    int totalW = 0, totalH = 0;
    for (size_t i = 0; i < frames.size(); ++i) {
        auto& f = frames[i];
        framesJson.push_back(mcp::json{
            { "index", static_cast<int>(i) },
            { "width", f.width },
            { "height", f.height },
            { "rgba_base64", mcp::base64::encode(f.rgba) } });
        if (static_cast<int>(f.width) > totalW) totalW = static_cast<int>(f.width);
        totalH += static_cast<int>(f.height);
    }

    // Optional single-frame selection.
    if (params.contains("frame") && params["frame"].is_number_integer()) {
        int idx = params["frame"].get<int>();
        if (idx >= 0 && idx < static_cast<int>(framesJson.size())) {
            mcp::json only = mcp::json::array();
            only.push_back(framesJson[idx]);
            framesJson = std::move(only);
        }
    }

    mcp::ToolResult r;
    r.value = mcp::json{ { "name", name },
                         { "format", format },
                         { "width", totalW },
                         { "height", totalH },
                         { "frame_count", static_cast<int>(frames.size()) },
                         { "palette_resolved", havePal },
                         { "frames", framesJson } };
    return r;
}

} // namespace

namespace mcp {
namespace tools {

void registerSpriteTools(ToolRegistry& reg)
{
    reg.registerTool(ToolDef{
        "palette.load",
        "Load a Diablo 1 .pal palette (256 colours, 8-bit RGB) from an MPQ and return "
        "it as a 256-entry [r,g,b] array plus an RGBA base64 blob.",
        json{ { "type", "object" },
              { "properties",
                json{ { "archive", json{ { "type", "string" } } },
                      { "name",
                        json{ { "type", "string" },
                              { "description", "in-archive palette name, e.g. gendata\\cut2.pal" } } } } },
              { "required", json::array({ "archive", "name" }) } },
        toolPaletteLoad });

    reg.registerTool(ToolDef{
        "sprite.decode",
        "Decode a sprite (.clx/.cl2/.cel/.pcx) from an MPQ into RGBA frames the "
        "frontend can render. CEL/CL2 need a 'width'; PCX/CLX read their own "
        "dimensions. The palette comes from palette_name, palette_b64, an embedded "
        "PCX palette, or <name>.pal alongside the sprite. Pass 'frame' to get one "
        "frame only.",
        json{ { "type", "object" },
              { "properties",
                json{ { "archive", json{ { "type", "string" } } },
                      { "name",
                        json{ { "type", "string" },
                              { "description", "in-archive sprite name, e.g. data\\health.clx" } } },
                      { "palette_name", json{ { "type", "string" } } },
                      { "palette_b64", json{ { "type", "string" } } },
                      { "width", json{ { "type", "string" },
                                        { "description", "required for .cel/.cl2; an integer (e.g. 96) or the string \"auto\" to look up from DevilutionX data tables by sprite path" } } },
                      { "devilutionx_root", json{ { "type", "string" },
                                                   { "description", "DevilutionX source root for width:\"auto\" (default D:/work/xt/diablo/DevilutionX)" } } },
                      { "frame", json{ { "type", "integer" } } } } },
              { "required", json::array({ "archive", "name" }) } },
        toolSpriteDecode });
}

} // namespace tools
} // namespace mcp
