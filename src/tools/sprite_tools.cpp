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
#include <fstream>

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

// Try each archive in turn until one yields the file. A palette (or the sprite
// itself) may live in a *different* MPQ than the one the sprite was listed from
// — e.g. a Hellfire palette applied to a DIABDAT sprite — so sprite.decode must
// be able to search an archive stack, not just the sprite's own archive.
std::vector<uint8_t> extractBufAny(const std::vector<engine::Archive>& archs,
                                   const std::string& name)
{
    for (auto& a : archs) {
        std::vector<uint8_t> buf;
        if (a.extract(name, buf))
            return buf;
    }
    throw std::runtime_error("extract failed (name not found?): " + name);
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

// Apply a 256-byte TRN remap table to a 768-byte (256*3) RGB palette: the result
// is a palette where entry i = original[trn[i]]. A pixel carrying palette index j
// then renders as original_palette[trn[j]] — exactly what DevilutionX ClxApplyTrans
// does when a sprite is tinted at draw time.
std::vector<uint8_t> applyTrn(const std::vector<uint8_t>& pal,
                              const std::vector<uint8_t>& trn)
{
    std::vector<uint8_t> out(256 * 3);
    for (int i = 0; i < 256; ++i) {
        int src = trn[i];
        if (src < 0) src = 0;
        if (src > 255) src = 255;
        size_t o = static_cast<size_t>(i) * 3;
        size_t s = static_cast<size_t>(src) * 3;
        out[o + 0] = pal[s + 0];
        out[o + 1] = pal[s + 1];
        out[o + 2] = pal[s + 2];
    }
    return out;
}

// Decide which TRN file (if any) to apply. Returns "" when apply_trn is off.
// When apply_trn is on but no explicit trn_name is given, the default is the
// per-class TRN DevilutionX uses: plrgfx\trn<class>.trn (the class's `trn`
// field = "trn" + class, e.g. trnwarrior). Note: for the shipped game those
// per-class TRN files are NOT present, so this resolves to a missing file and
// the decode falls back to palette-only — faithful to the game, where player
// tinting is a no-op for the base classes.
std::string resolveTrnName(const mcp::json& params, const std::string& spriteName);

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
std::vector<uint8_t> resolvePalette(const std::vector<engine::Archive>& archs,
                                    const mcp::json& params,
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
        auto pal = decoders::loadPalette(extractBufAny(archs, pn));
        if (pal.size() == 256 * 3)
            return pal;
        throw std::runtime_error("palette not valid: " + pn);
    }
    if (embeddedPal && embeddedPal->size() == 256 * 3)
        return *embeddedPal;

    // Try <base>.pal alongside the sprite (search the whole archive stack).
    std::string base = spriteName;
    auto pos = base.find_last_of("\\/");
    std::string dir  = (pos == std::string::npos) ? "" : base.substr(0, pos + 1);
    std::string file = (pos == std::string::npos) ? base : base.substr(pos + 1);
    auto dot = file.find_last_of('.');
    std::string stem = (dot == std::string::npos) ? file : file.substr(0, dot);
    std::string palName = dir + stem + ".pal";
    try {
        auto pal = decoders::loadPalette(extractBufAny(archs, palName));
        if (pal.size() == 256 * 3)
            return pal;
    } catch (const std::exception&) {
        // fall through to the "not found" error below
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

    // Build the archive stack: the sprite's own archive first, plus any extra
    // MPQs supplied via `archives` (e.g. the MPQ that holds the chosen palette).
    // This lets a palette in a different MPQ than the sprite still be resolved.
    std::vector<std::string> paths;
    paths.push_back(archive);
    if (params.contains("archives") && params["archives"].is_array()) {
        for (auto& ap : params["archives"]) {
            if (!ap.is_string())
                continue;
            std::string p = ap.get<std::string>();
            if (std::find(paths.begin(), paths.end(), p) == paths.end())
                paths.push_back(p);
        }
    }
    std::vector<engine::Archive> archs;
    for (auto& p : paths)
        archs.push_back(openOrThrow(p));

    auto buf = extractBufAny(archs, name);

    std::string lower = toLower(name);
    std::vector<uint8_t> clxBytes;
    std::string format;
    std::vector<decoders::DecodedFrame> frames;
    std::vector<uint8_t> embeddedPal;

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

    // Resolve the base palette first (CLX/CEL/CL2 path; PCX may keep embedded).
    std::vector<uint8_t> pal;
    bool havePal = false;
    if (format == "pcx") {
        decoders::DecodedFrame fr;
        if (decoders::decodePcx(buf, fr, &embeddedPal))
            frames.push_back(std::move(fr));
        try {
            pal = resolvePalette(archs, params, name, &embeddedPal);
            havePal = true;
        } catch (const std::exception&) {
            havePal = !embeddedPal.empty();
        }
    } else {
        pal = resolvePalette(archs, params, name, nullptr);
        havePal = true;
    }

    // Optional TRN remap (toggle via apply_trn). Remaps the palette so a pixel
    // with index j renders as palette[trn[j]] — DevilutionX ClxApplyTrans. The
    // default class TRN (plrgfx\trn<class>.trn) is not shipped for the base game
    // classes, so this is a no-op unless an explicit trn_name (e.g. the real
    // plrgfx\infra.trn / plrgfx\stone.trn) is supplied or a mod adds it.
    bool trnApplied = false;
    std::string trnNameResolved = resolveTrnName(params, name);
    if (!trnNameResolved.empty()) {
        try {
            auto trnBuf = extractBufAny(archs, trnNameResolved);
            if (trnBuf.size() == 256) {
                pal = applyTrn(pal, trnBuf);
                trnApplied = true;
            }
        } catch (const std::exception&) {
            // TRN file not present; keep palette-only.
        }
    }

    // Decode with the (possibly TRN-remapped) palette.
    if (format == "pcx") {
        if (!pal.empty()) {
            decoders::DecodedFrame fr;
            decoders::decodePcx(buf, fr, nullptr, &pal);
            frames.clear();
            frames.push_back(std::move(fr));
        }
    } else {
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
                         { "trn_applied", trnApplied },
                         { "trn_name", trnNameResolved },
                         { "frames", framesJson } };
    return r;
}

// Classify an in-archive name into family / class / type, and report whether it
// is something we enumerate: CL2 / CEL sprites, or .pal palette files. Used by
// sprite.list (the `kind` filter decides which of these to emit).
struct SpriteInfo {
    std::string family; // plrgfx / monsters / missiles / items / objects / towners / palette
    std::string cls;    // class path, assetsSuffix, missile/item/object name, etc.
    std::string type;   // 2-char player type code (as/at/...), or monster L letter
    bool        sprite = false;
};

static SpriteInfo classifySprite(const std::string& name, bool& isSpriteExt) {
    SpriteInfo r;
    isSpriteExt = false;
    std::string p = name;
    for (char& c : p)
        if (c == '\\') c = '/';
    size_t dot = p.rfind('.');
    if (dot == std::string::npos)
        return r;
    std::string ext = p.substr(dot + 1);
    if (ext != "cl2" && ext != "cel" && ext != "pal")
        return r; // only sprite / palette formats are enumerated
    isSpriteExt = true;
    if (ext == "pal") {            // palette file: tagged so the dropdown can list it
        r.family = "palette";
        r.sprite = true;
        return r;
    }

    std::vector<std::string> comps;
    std::string cur;
    for (char c : p) {
        if (c == '/') { comps.push_back(cur); cur.clear(); }
        else cur.push_back(c);
    }
    comps.push_back(cur);

    auto findTok = [&](const std::string& tok) -> int {
        for (size_t i = 0; i < comps.size(); i++)
            if (comps[i] == tok) return (int)i;
        return -1;
    };
    auto stripExt = [](std::string s) -> std::string {
        size_t d = s.rfind('.');
        if (d != std::string::npos) s = s.substr(0, d);
        return s;
    };
    auto joinAfter = [&](int idx) -> std::string {
        std::string rel;
        for (int i = idx + 1; i < (int)comps.size(); i++) {
            if (!rel.empty()) rel += '\\';
            rel += comps[i];
        }
        return rel;
    };

    int idx;
    if ((idx = findTok("plrgfx")) >= 0) {
        if ((size_t)idx + 3 >= comps.size()) return r;
        r.family = "plrgfx";
        r.cls = comps[idx + 1];
        std::string fn = stripExt(comps[idx + 3]);
        if (fn.size() >= 2) r.type = fn.substr(fn.size() - 2);
        r.sprite = true;
    } else if ((idx = findTok("monsters")) >= 0) {
        std::string rel = stripExt(joinAfter(idx));
        if (rel.size() < 2) return r;
        r.family = "monsters";
        r.cls = rel.substr(0, rel.size() - 1); // assetsSuffix (may include subdir)
        r.type = rel.substr(rel.size() - 1);    // L letter
        r.sprite = true;
    } else if ((idx = findTok("missiles")) >= 0) {
        r.family = "missiles";
        r.cls = stripExt(comps.back());
        r.sprite = true;
    } else if ((idx = findTok("items")) >= 0) {
        r.family = "items";
        r.cls = stripExt(comps.back());
        r.sprite = true;
    } else if ((idx = findTok("objects")) >= 0) {
        r.family = "objects";
        r.cls = stripExt(comps.back());
        r.sprite = true;
    } else if ((idx = findTok("towners")) >= 0) {
        r.family = "towners";
        r.cls = stripExt(joinAfter(idx));
        r.sprite = true;
    }
    return r;
}

std::string resolveTrnName(const mcp::json& params, const std::string& spriteName)
{
    if (!params.value("apply_trn", false))
        return "";
    if (params.contains("trn_name") && params["trn_name"].is_string())
        return params["trn_name"].get<std::string>();
    bool isSpriteExt = false;
    SpriteInfo si = classifySprite(spriteName, isSpriteExt);
    if (si.family == "plrgfx" && !si.cls.empty()) {
        std::string cls = si.cls;
        if (cls == "sorceror") cls = "sorcerer"; // data-dir spelling used by the trn field
        return "plrgfx\\trn" + cls + ".trn";
    }
    return "";
}

// Enumerate real sprite names (from recovered.json) grouped by family / class /
// type, with the auto-resolved frame width. Frontend uses this to build its
// catalog tree; it then calls sprite.decode(name, width:"auto") per leaf.
mcp::ToolResult toolSpriteList(const mcp::json& params)
{
    std::string dictPath = params.value("dict", "mpq-info/dict/recovered.json");
    std::filesystem::path dp = dictPath;
    if (!dp.is_absolute())
        dp = std::filesystem::current_path() / dp;
    std::ifstream f(dp);
    if (!f)
        throw std::runtime_error("cannot open dict (recovered.json): " + dp.string());
    mcp::json dict;
    f >> dict;

    std::filesystem::path devRoot =
        (params.contains("devilutionx_root") && params["devilutionx_root"].is_string())
            ? std::filesystem::path(params["devilutionx_root"].get<std::string>())
            : std::filesystem::path("D:/work/xt/diablo/DevilutionX");
    std::string famFilter =
        (params.contains("family") && params["family"].is_string())
            ? params["family"].get<std::string>() : "";
    std::string classFilter =
        (params.contains("class") && params["class"].is_string())
            ? params["class"].get<std::string>() : "";
    std::string kind =
        (params.contains("kind") && params["kind"].is_string())
            ? params["kind"].get<std::string>() : "sprite";
    // kind: "sprite" (default) | "palette" | "all"

    mcp::json sprites = mcp::json::array();
    if (dict.contains("archives") && dict["archives"].is_array()) {
        for (auto& arch : dict["archives"]) {
            std::string archName = arch.value("name", "");
            std::string archPath = arch.value("path", "");
            if (!arch.contains("recovered") || !arch["recovered"].is_array())
                continue;
            for (auto& e : arch["recovered"]) {
                if (!e.contains("name") || !e["name"].is_string())
                    continue;
                std::string name = e["name"].get<std::string>();
                bool isSpriteExt = false;
                SpriteInfo si = classifySprite(name, isSpriteExt);
                if (!isSpriteExt) // only enumerate CL2/CEL sprites and .pal palettes
                    continue;
                bool isPal = (si.family == "palette");
                if (kind == "sprite" && isPal) continue;
                if (kind == "palette" && !isPal) continue;
                if (!famFilter.empty() && si.family != famFilter)
                    continue;
                if (!classFilter.empty() && si.cls != classFilter)
                    continue;
                int w = 0;
                if (!isPal) {
                    std::string dbg;
                    int aw = decoders::sprite_width::autoWidth(name, devRoot, dbg);
                    if (aw > 0) w = aw;
                }
                sprites.push_back(mcp::json{
                    { "name", name },
                    { "archive", archName },
                    { "archive_path", archPath },
                    { "family", si.family },
                    { "class", si.cls },
                    { "type", si.type },
                    { "width", w } });
            }
        }
    }

    mcp::ToolResult r;
    r.value = mcp::json{ { "count", static_cast<int>(sprites.size()) },
                         { "sprites", sprites } };
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
        "frame only. The palette (or sprite) may live in a *different* MPQ than "
        "'archive'; pass 'archives' (array of extra MPQ paths) to search the whole "
        "stack for it.",
        json{ { "type", "object" },
              { "properties",
                json{ { "archive", json{ { "type", "string" } } },
                      { "archives", json{ { "type", "array" },
                                          { "items", json{ { "type", "string" } } },
                                          { "description", "extra MPQ paths to search for the sprite/palette (e.g. the MPQ holding the chosen palette); the sprite's 'archive' is always included" } } },
                      { "name",
                        json{ { "type", "string" },
                              { "description", "in-archive sprite name, e.g. data\\health.clx" } } },
                      { "palette_name", json{ { "type", "string" } } },
                      { "palette_b64", json{ { "type", "string" } } },
                      { "width", json{ { "type", "string" },
                                        { "description", "required for .cel/.cl2; an integer (e.g. 96) or the string \"auto\" to look up from DevilutionX data tables by sprite path" } } },
                      { "devilutionx_root", json{ { "type", "string" },
                                                   { "description", "DevilutionX source root for width:\"auto\" (default D:/work/xt/diablo/DevilutionX)" } } },
                      { "apply_trn", json{ { "type", "boolean" },
                                          { "description", "apply a TRN (256-byte palette remap) before decoding; default false = palette-only mode. When true, the class TRN plrgfx\\trn<class>.trn is used unless trn_name is given" } } },
                      { "trn_name", json{ { "type", "string" },
                                          { "description", "explicit TRN file in the MPQ to apply (e.g. plrgfx\\infra.trn for infravision tint, plrgfx\\stone.trn for petrify). Overrides the auto class TRN when apply_trn is true" } } },
                      { "frame", json{ { "type", "integer" } } } } },
              { "required", json::array({ "archive", "name" }) } },
        toolSpriteDecode });

    reg.registerTool(ToolDef{
        "sprite.list",
        "Enumerate real sprite names recovered from MPQs (no listfile; recovered via "
        "StormLib one-way-hash verify). Each entry is classified into family / class / "
        "type and carries the auto-resolved frame width (0 if no table). Filter with "
        "'family' / 'class'. Use kind='palette' to list the archive's .pal palettes "
        "(for the decode palette dropdown). The frontend builds its catalog tree from "
        "this and then calls sprite.decode(name, width:\"auto\") per leaf.",
        json{ { "type", "object" },
              { "properties",
                json{ { "dict",
                        json{ { "type", "string" },
                              { "description", "recovered.json path (default mpq-info/dict/recovered.json, relative to cwd)" } } },
                      { "devilutionx_root",
                        json{ { "type", "string" },
                              { "description", "DevilutionX source root for width auto-resolve (default D:/work/xt/diablo/DevilutionX)" } } },
                      { "family",
                        json{ { "type", "string" },
                              { "description", "filter by family: plrgfx / monsters / missiles / items / objects / towners (empty = all)" } } },
                      { "class",
                        json{ { "type", "string" },
                              { "description", "filter by class (classPath, assetsSuffix, or sprite name; empty = all)" } } },
                      { "kind",
                        json{ { "type", "string" },
                              { "description", "what to enumerate: 'sprite' (default, CL2/CEL) | 'palette' (.pal files) | 'all'" } } } } },
              { "required", json::array({}) } },
        toolSpriteList });
}

} // namespace tools
} // namespace mcp
