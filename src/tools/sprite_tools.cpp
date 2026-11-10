// src/tools/sprite_tools.cpp
//
// Implements palette.load and sprite.decode. Both pull files out of an MPQ through
// engine::Archive and feed them to the decoders in src/decoders/, returning RGBA
// the frontend can render directly.

#include "tools/registry.h"
#include "tools/base64.h"
#include "tools/spell_tables.h"
#include "engine/archive.h"

#include "decoders/palette.h"
#include "decoders/clx.h"
#include "decoders/cel.h"
#include "decoders/cl2.h"
#include "decoders/pcx.h"
#include "decoders/sprite_width.h"
#include "decoders/inv_widths.h"
#include "tools/ui_layout.h"

#include <filesystem>
#include <fstream>

#include <algorithm>
#include <map>
#include <set>
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

// For a monster sprite, return the direction count (totalFrames / framesPerDir
// from monstdat.tsv). Returns 0 when unknown (frontend falls back to 8).
int monsterDirectionCount(const std::string& spriteName,
                          const std::filesystem::path& devRoot);

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

    // Resolve width for CL2/CEL. Supports:
    //   - "widths": [..]   explicit per-frame widths (CEL only; overrides width)
    //   - "width":  N      a single integer width
    //   - "width":  "auto" look up from DevilutionX tables by sprite path; for
    //                      data\inv\objcurs*.cel this resolves to the built-in
    //                      per-frame width table (their frames vary in width).
    int spriteWidth = 0;
    bool useAutoWidth = false;
    std::vector<uint16_t> frameWidths;
    // devRoot is needed both for autoWidth and for monster direction count, so
    // declare it at function scope and (re)assign it here.
    std::filesystem::path devRoot =
        (params.contains("devilutionx_root") && params["devilutionx_root"].is_string())
            ? std::filesystem::path(params["devilutionx_root"].get<std::string>())
            : std::filesystem::path("D:/work/xt/diablo/DevilutionX");
    if (params.contains("widths")) {
        const auto& ws = params["widths"];
        if (!ws.is_array() || ws.empty())
            throw std::invalid_argument("'widths' must be a non-empty array of positive integers");
        for (const auto& w : ws) {
            if (!w.is_number_integer() || w.get<int>() <= 0 || w.get<int>() > 65535)
                throw std::invalid_argument("'widths' entries must be positive integers <= 65535");
            frameWidths.push_back(static_cast<uint16_t>(w.get<int>()));
        }
    }
    if (params.contains("width")) {
        const auto& w = params["width"];
        if (w.is_string())
            useAutoWidth = (w.get<std::string>() == "auto");
        else if (w.is_number_integer())
            spriteWidth = w.get<int>();
    }
    if (useAutoWidth && frameWidths.empty()) {
        // Inventory cursor CELs have a per-frame width table, not a single one.
        auto table = decoders::inv_widths::invWidthsForName(name);
        if (!table.empty()) {
            frameWidths = table.toVector();
        } else {
            std::string dbg;
            spriteWidth = decoders::sprite_width::autoWidth(name, devRoot, dbg);
            // Only CL2/CEL truly require a resolved width; PCX/CLX are
            // self-describing (PCX header, CLX frame table), so a failed lookup
            // is harmless there. The per-format checks below still enforce a
            // positive width for CL2/CEL.
        }
    }

    if (endsWith(lower, ".clx")) {
        format = "clx";
        clxBytes = buf;
    } else if (endsWith(lower, ".cl2")) {
        format = "cl2";
        if (!frameWidths.empty())
            throw std::runtime_error("per-frame 'widths' is only supported for .cel sprites");
        if (spriteWidth <= 0)
            throw std::runtime_error("cl2 requires a positive integer 'width' or width:\"auto\"");
        clxBytes = decoders::cl2ToClx(buf.data(), buf.size(), static_cast<uint16_t>(spriteWidth));
    } else if (endsWith(lower, ".cel")) {
        format = "cel";
        if (!frameWidths.empty()) {
            clxBytes = decoders::celToClx(buf.data(), buf.size(), frameWidths);
        } else {
            if (spriteWidth <= 0)
                throw std::runtime_error("cel requires a positive integer 'width' or width:\"auto\"");
            clxBytes = decoders::celToClx(buf.data(), buf.size(), static_cast<uint16_t>(spriteWidth));
        }
    } else if (endsWith(lower, ".pcx")) {
        format = "pcx";
        decoders::DecodedFrame fr;
        if (!decoders::decodePcx(buf, fr, &embeddedPal))
            throw std::runtime_error("unsupported or truncated PCX: " + name);
    } else {
        throw std::runtime_error(
            "unsupported sprite format (expected .clx/.cl2/.cel/.pcx): " + name);
    }

    // Resolve the base palette first (CLX/CEL/CL2 path; PCX may keep embedded).
    std::vector<uint8_t> pal;
    bool havePal = false;
    if (format == "pcx") {
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
        if (pal.empty() && !embeddedPal.empty())
            pal = embeddedPal;   // fall back to the PCX's own embedded palette
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

    // For monster sprites, report the direction count so the viewer can split the
    // flat frame list correctly. directionCount = totalFrames / framesPerDir,
    // where framesPerDir comes from monstdat.tsv frames[6] for the sprite's
    // animation letter. 0 means "unknown" (frontend falls back to 8).
    int directions = 0;
    {
        int fpd = monsterDirectionCount(name, devRoot); // frames-per-dir, 0 if not a monster
        if (fpd > 0 && !frames.empty())
            directions = static_cast<int>(std::lround(
                static_cast<double>(frames.size()) / static_cast<double>(fpd)));
    }

    mcp::ToolResult r;
    r.value = mcp::json{ { "name", name },
                         { "format", format },
                         { "width", totalW },
                         { "height", totalH },
                         { "frame_count", static_cast<int>(frames.size()) },
                         { "directions", directions },
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
    // plrgfx / monsters / missiles / items / objects / towners / inv / palette
    std::string family;
    std::string cls;    // class path, assetsSuffix, missile/item/object name, etc.
    std::string type;   // 2-char player type code (as/at/...), or monster L letter
    bool        sprite = false;
    int         fixedWidth = 0; // single known width for families without a TSV
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
    } else if (comps.size() >= 3 && comps[0] == "data" && comps[1] == "inv") {
        // data\inv\* : inventory cursor ICONS (objcurs*.cel, one width per
        // frame, resolved by decoders/inv_widths.h) and per-class inventory
        // panel backgrounds (inv*.cel, fixed width 320 = SidePanelSize.width,
        // DevilutionX control/control.hpp:38).
        r.family = "inv";
        const std::string stem = stripExt(comps.back());
        const bool isCursor = (stem.rfind("objcurs", 0) == 0);
        r.cls = isCursor ? "cursors" : "panels";
        r.type = "cel";
        r.fixedWidth = isCursor ? 0 : 320;
        r.sprite = true;
    }
    return r;
}

int monsterDirectionCount(const std::string& spriteName,
                         const std::filesystem::path& devRoot)
{
    bool isSpriteExt = false;
    SpriteInfo si = classifySprite(spriteName, isSpriteExt);
    if (si.family != "monsters" || si.cls.empty())
        return 0;
    std::string dbg;
    return decoders::sprite_width::monsterFramesPerDir(spriteName, devRoot, dbg);
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

// ---- Monster / NPC catalog synthesis ---------------------------------------
// Monster and NPC (towner) sprites have no MPQ listfile, so their real names
// cannot be recovered by hash verification like the rest of the catalog. Instead
// we derive them from the DevilutionX data tables (monstdat.tsv / towners.tsv),
// which the game itself uses to locate the files. Each synthesized entry mirrors
// the shape of a recovered.json sprite entry so the frontend reuses its existing
// tree / decode pipeline.
//
// Monster path formula (DevilutionX monster.cpp + Animletter="nwahds"):
//   monsters\{assetsSuffix}{L}.cl2   L in n/w/a/h/d/s  (Stand/Walk/Attack/
//   GotHit/Death/Special). frames[6] in monstdat.tsv lists frames-per-direction
//   per MonsterGraphic index, in the same order.
// Towner path formula (towners.cpp): towners\{animPath}.cel (animPath already
// includes the towners\ prefix). The cow is a hardcoded special case.

namespace {

std::vector<std::string> splitTsvRow(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == '\t' || c == '\r' || c == '\n') {
            out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    out.push_back(cur);
    return out;
}

int tsvCol(const std::vector<std::string>& header, const std::string& name) {
    for (size_t i = 0; i < header.size(); i++)
        if (header[i] == name)
            return (int)i;
    return -1;
}

// Build the monster catalog from monstdat.tsv (base + Hellfire mod).
mcp::json synthesizeMonsters(const std::filesystem::path& devRoot) {
    static const char letters[6] = {'n', 'w', 'a', 'h', 'd', 's'};
    static const char* animNames[6] = {"Stand", "Walk", "Attack", "GotHit",
                                       "Death", "Special"};
    mcp::json out = mcp::json::array();
    std::set<std::string> seen;
    const std::vector<std::filesystem::path> files = {
        devRoot / "assets" / "txtdata" / "monsters" / "monstdat.tsv",
        devRoot / "mods" / "hf" / "txtdata" / "monsters" / "monstdat.tsv"};
    for (const auto& f : files) {
        std::ifstream in(f);
        if (!in)
            continue;
        std::string line;
        if (!std::getline(in, line))
            continue;
        auto header = splitTsvRow(line);
        int ciSuffix = tsvCol(header, "assetsSuffix");
        int ciFrames = tsvCol(header, "frames[6]");
        if (ciSuffix < 0 || ciFrames < 0)
            continue;
        while (std::getline(in, line)) {
            if (line.empty())
                continue;
            auto parts = splitTsvRow(line);
            if ((int)parts.size() <= ciFrames)
                continue;
            std::string suffix = parts[ciSuffix];
            if (suffix.empty())
                continue;
            // monstdat.tsv keeps 4 unused beta monsters (Wyrm / Cave Slug /
            // Devil Wyrm / Devourer) that all share the `worm\worm` suffix. No
            // MPQ ships these CL2s, so they would appear in the catalog but
            // always fail to decode. Do NOT filter on availability=Never here:
            // 15 other Never suffixes (golem, unrav, diablo, ...) DO have real
            // CL2 files and must stay in the catalog.
            static const std::set<std::string> kNoSpriteSuffixes = {"worm\\worm"};
            if (kNoSpriteSuffixes.count(suffix))
                continue;
            // frames[6] is comma-separated: Stand,Walk,Attack,GotHit,Death,Special.
            std::vector<std::string> fr;
            std::string cur;
            for (char c : parts[ciFrames]) {
                if (c == ',') {
                    fr.push_back(cur);
                    cur.clear();
                } else {
                    cur.push_back(c);
                }
            }
            fr.push_back(cur);
            for (int i = 0; i < 6 && i < (int)fr.size(); i++) {
                int fpd = std::atoi(fr[i].c_str());
                if (fpd <= 0)
                    continue;
                char L = letters[i];
                std::string name = "monsters\\" + suffix + std::string(1, L) + ".cl2";
                if (!seen.insert(name).second)
                    continue; // already emitted (e.g. base + hf duplicate)
                std::string dbg;
                int w = decoders::sprite_width::autoWidth(name, devRoot, dbg);
                out.push_back(mcp::json{
                    {"name", name},
                    {"family", "monsters"},
                    {"class", suffix},
                    {"type", std::string(1, L)},
                    {"anim", animNames[i]},
                    {"frames_per_dir", fpd},
                    {"width", w > 0 ? w : 0}});
            }
        }
    }
    return out;
}

// Build the NPC (towner) catalog from towners.tsv, plus the hardcoded cow.
mcp::json synthesizeTowners(const std::filesystem::path& devRoot) {
    mcp::json out = mcp::json::array();
    std::set<std::string> seen;
    // TOWN_COW rows in towners.tsv have an empty animPath but a valid animWidth
    // (128). Capture it for the hardcoded cow entry below.
    int cowWidth = 0;
    const std::vector<std::filesystem::path> files = {
        devRoot / "assets" / "txtdata" / "towners" / "towners.tsv",
        devRoot / "mods" / "hf" / "txtdata" / "towners" / "towners.tsv"};
    for (const auto& f : files) {
        std::ifstream in(f);
        if (!in)
            continue;
        std::string line;
        if (!std::getline(in, line))
            continue;
        auto header = splitTsvRow(line);
        int ciPath = tsvCol(header, "animPath");
        int ciW = tsvCol(header, "animWidth");
        int ciName = tsvCol(header, "name");
        int ciType = tsvCol(header, "type");
        if (ciPath < 0)
            continue;
        while (std::getline(in, line)) {
            if (line.empty())
                continue;
            auto parts = splitTsvRow(line);
            if ((int)parts.size() <= ciPath)
                continue;
            std::string path = parts[ciPath];
            int w = (ciW >= 0 && (int)parts.size() > ciW)
                        ? std::atoi(parts[ciW].c_str())
                        : 0;
            if (path.empty()) {
                // TOWN_COW has no animPath (its CEL is hardcoded in towners.cpp)
                // but does carry animWidth — remember it for the cow entry.
                if (ciType >= 0 && (int)parts.size() > ciType &&
                    parts[ciType] == "TOWN_COW" && w > 0)
                    cowWidth = w;
                continue;
            }
            std::string name = path + ".cel";
            if (!seen.insert(name).second)
                continue;
            std::string disp = (ciName >= 0 && (int)parts.size() > ciName)
                                  ? parts[ciName]
                                  : path;
            out.push_back(mcp::json{
                {"name", name},
                {"family", "towners"},
                {"class", disp},
                {"type", ""},
                {"anim", "Idle"},
                {"frames_per_dir", 1},
                {"width", w}});
        }
    }
    // The cow is hardcoded in DevilutionX towners.cpp (towners/animals/cow.cel);
    // its towners.tsv rows have an empty animPath but animWidth=128, which we
    // captured above. Carry the real width so the viewer/decoder can use it.
    {
        std::string name = "towners\\animals\\cow.cel";
        if (seen.insert(name).second)
            out.push_back(mcp::json{{"name", name},
                                    {"family", "towners"},
                                    {"class", "cow"},
                                    {"type", ""},
                                    {"anim", "Idle"},
                                    {"frames_per_dir", 1},
                                    {"width", cowWidth > 0 ? cowWidth : 128}});
    }
    return out;
}

} // namespace

// Enumerate real sprite names (from recovered.json) grouped by family / class /
// type, with the auto-resolved frame width. Frontend uses this to build its
// catalog tree; it then calls sprite.decode(name, width:"auto") per leaf.
//
// When family is "monsters" or "towners", the names are synthesized from the
// DevilutionX data tables instead (those sprites have no MPQ listfile).
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
    // kind: "sprite" (default) | "palette" | "ui" | "all"
    //   - "ui": ONLY the UI layout's referenced art (ui_art\*.pcx, data\*.clx/.cel,
    //     ctrlpan\spelicon.cel, ...) from the static DiabloUI screen table
    //     (ui.layout). Lets the Catalog browser show the same art the GUI layout
    //     viewer composites, with frame info for animation playback. "all" stays
    //     sprite+palette only so existing callers keep their counts.

    // Map archive name -> full path (for resolving UI art archive hints).
    std::map<std::string, std::string> archNameToPath;
    std::string firstArchPath;
    if (dict.contains("archives") && dict["archives"].is_array()) {
        for (auto& arch : dict["archives"]) {
            std::string n = arch.value("name", "");
            std::string p = arch.value("path", "");
            if (p.empty()) continue;
            if (firstArchPath.empty()) firstArchPath = p;
            if (!n.empty() && !archNameToPath.count(n)) archNameToPath[n] = p;
        }
    }
    auto resolveArchPath = [&](const std::string& hint) -> std::string {
        if (!hint.empty()) {
            auto it = archNameToPath.find(hint);
            if (it != archNameToPath.end()) return it->second;
        }
        return firstArchPath;
    };

    mcp::json sprites = mcp::json::array();
    if (famFilter == "monsters" || famFilter == "towners") {
        // Monsters / towners have no MPQ listfile, so synthesize their catalog
        // from the DevilutionX data tables instead of recovered.json.
        mcp::json synth = (famFilter == "monsters") ? synthesizeMonsters(devRoot)
                                                     : synthesizeTowners(devRoot);
        for (auto& s : synth) {
            if (!classFilter.empty() && s.value("class", "") != classFilter)
                continue;
            sprites.push_back(s);
        }
    } else if (kind != "ui" && dict.contains("archives") && dict["archives"].is_array()) {
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
                    if (si.family == "inv") {
                        // Panels are single-frame width 320; cursor CELs are
                        // per-frame (no single width) so width 0 makes the
                        // frontend send width:"auto", which resolves the
                        // built-in table in sprite.decode.
                        w = si.fixedWidth;
                    } else {
                        std::string dbg;
                        int aw = decoders::sprite_width::autoWidth(name, devRoot, dbg);
                        if (aw > 0) w = aw;
                    }
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

    // Enumerate the UI layout's referenced art (ui_art\*.pcx, data\*.clx/.cel,
    // ctrlpan\spelicon.cel, ...) so the Catalog browser can show the same art the
    // GUI layout viewer composites. Deduplicated across all screens by name.
    if (kind == "ui") {
        std::set<std::string> seen;
        for (const auto& s : ui_layout::screens()) {
            for (const auto& art : ui_layout::uniqueArtRefs(s)) {
                if (seen.count(art.name)) continue;
                seen.insert(art.name);
                std::string nm = art.name;
                for (char& c : nm) if (c == '\\') c = '/';
                std::string cls = "ui";
                size_t sl = nm.find('/');
                if (sl != std::string::npos) cls = nm.substr(0, sl);
                sprites.push_back(mcp::json{
                    { "name", art.name },
                    { "archive", art.archive_hint },
                    { "archive_path", resolveArchPath(art.archive_hint) },
                    { "family", "ui" },
                    { "class", cls },
                    { "type", art.format },
                    { "palette", art.palette },
                    { "width", art.width },
                    { "frames", art.frames },
                    { "transparent_index", art.transparent_index } });
            }
        }
    }

    // Full MPQ stack from recovered.json. Some archives (notably devilutionx.mpq)
    // contain NO CL2/CEL sprites but DO hold the converted .clx UI art and
    // ui_art\diablo.pal. ui.build / scene.build search the `archives` stack, so
    // the frontend must receive EVERY archive path -- deriving the stack only
    // from sprite entries silently drops those archives and makes every .clx
    // background / dialog / button report `missing` or `decode_error`.
    mcp::json archives = mcp::json::array();
    if (dict.contains("archives") && dict["archives"].is_array()) {
        std::vector<std::string> seen;
        for (const auto& arch : dict["archives"]) {
            std::string archPath = arch.value("path", "");
            if (archPath.empty()) continue;
            if (std::find(seen.begin(), seen.end(), archPath) != seen.end()) continue;
            seen.push_back(archPath);
            archives.push_back(archPath);
        }
    }

    mcp::ToolResult r;
    r.value = mcp::json{ { "count", static_cast<int>(sprites.size()) },
                         { "archives", archives },
                         { "sprites", sprites } };
    return r;
}

// Join the DevilutionX spell / missile / monster tables into a stage-ordered
// catalog of visual effects (see tools/spell_tables.h for the pure logic).
// Numbered missiles\{name}{N}.cl2 effects are NOT in recovered.json, so this is
// the only way the frontend can discover fireball / fire wall / holy bolt etc.
mcp::ToolResult toolSpellList(const mcp::json& params)
{
    std::filesystem::path devRoot =
        (params.contains("devilutionx_root") && params["devilutionx_root"].is_string())
            ? std::filesystem::path(params["devilutionx_root"].get<std::string>())
            : std::filesystem::path("D:/work/xt/diablo/DevilutionX");
    const std::string sourceFilter =
        (params.contains("source") && params["source"].is_string())
            ? params["source"].get<std::string>() : "";
    const std::string idFilter =
        (params.contains("id") && params["id"].is_string())
            ? params["id"].get<std::string>() : "";

    spell_tables::Catalog cat = spell_tables::buildCatalog(devRoot);
    if (!cat.ok)
        throw std::runtime_error("spell.list: " + cat.error);

    auto layerJson = [](const spell_tables::Layer& l) -> mcp::json {
        mcp::json j = { { "kind", l.kind }, { "role", l.role },
                        { "anchor", l.anchor }, { "confidence", l.confidence } };
        if (l.kind == "cast_action") {
            j["action"] = l.action;
            j["element"] = l.element;
        } else if (l.kind == "sprite") {
            j["graphic_id"] = l.graphic_id;
            j["files"] = l.files;
            j["width"] = l.width;
            j["frames"] = l.frames;
            j["num_files"] = l.num_files;
            j["element"] = l.element;
        } else if (l.kind == "trn") {
            j["trn"] = mcp::json{ { "name", l.trn_name },
                                  { "preset", l.trn_preset },
                                  { "apply_to", l.anchor } };
        }
        return j;
    };

    auto entryJson = [&](const spell_tables::Entry& e) -> mcp::json {
        mcp::json stages = mcp::json::array();
        for (const auto& s : e.stages) {
            mcp::json layers = mcp::json::array();
            for (const auto& l : s.layers)
                layers.push_back(layerJson(l));
            stages.push_back(mcp::json{ { "id", s.id }, { "label", s.label },
                                        { "layers", layers } });
        }
        mcp::json manaCurve = mcp::json::array();
        for (const auto& kv : e.mana_by_level)
            manaCurve.push_back(mcp::json{ { "level", kv.first }, { "mana", kv.second } });
        mcp::json visualByLevel = mcp::json::array();
        for (const auto& kv : e.visual_by_level) {
            mcp::json pj = mcp::json::object();
            for (const auto& pv : kv.second)
                pj[pv.first] = pv.second;
            visualByLevel.push_back(mcp::json{ { "level", kv.first }, { "params", pj } });
        }
        mcp::json j = { { "id", e.id }, { "name", e.name },
                        { "name_zh", e.name_zh }, { "description", e.description },
                        { "source", e.source },
                        { "element", e.element }, { "cast_action", e.cast_action },
                        { "level", e.level }, { "staff_level", e.staff_level },
                        { "mana_cost", e.mana_cost }, { "mana_multiplier", e.mana_multiplier },
                        { "min_mana", e.min_mana },
                        { "staff_min", e.staff_min }, { "staff_max", e.staff_max },
                        { "mana_by_level", manaCurve },
                        { "visual_by_level", visualByLevel },
                        { "missiles", e.missiles }, { "stages", stages } };
        if (!e.monster_ai.empty()) {
            j["monster_ai"] = e.monster_ai;
            j["monsters"] = e.monsters;
            j["default_ai"] = e.default_ai;
            if (!e.variant.empty()) j["variant"] = e.variant;
            if (!e.condition.empty()) j["condition"] = e.condition;
        }
        if (e.is_ability) {
            j["ability"] = true;
            j["monsters"] = e.monsters;
            j["trigger"] = e.trigger;
            j["source_ref"] = e.source_ref;
        }
        if (e.is_trap) {
            j["trap"] = true;
            j["objects"] = e.objects;
            j["trigger"] = e.trigger;
            j["source_ref"] = e.source_ref;
        }
        return j;
    };

    auto want = [&](const spell_tables::Entry& e) -> bool {
        return idFilter.empty() || e.id == idFilter || e.name == idFilter;
    };

    mcp::json spells = mcp::json::array();
    mcp::json attacks = mcp::json::array();
    mcp::json abilities = mcp::json::array();
    mcp::json traps = mcp::json::array();
    if (sourceFilter.empty() || sourceFilter == "player")
        for (const auto& e : cat.spells)
            if (want(e)) spells.push_back(entryJson(e));
    if (sourceFilter.empty() || sourceFilter == "monster")
        for (const auto& e : cat.monster_attacks)
            if (want(e)) attacks.push_back(entryJson(e));
    if (sourceFilter.empty() || sourceFilter == "ability")
        for (const auto& e : cat.abilities)
            if (want(e)) abilities.push_back(entryJson(e));
    if (sourceFilter.empty() || sourceFilter == "trap")
        for (const auto& e : cat.traps)
            if (want(e)) traps.push_back(entryJson(e));

    mcp::json impacts = mcp::json::array();
    for (const auto& si : cat.impact_catalog)
        impacts.push_back(mcp::json{ { "id", si.id }, { "files", si.files },
                                     { "width", si.width }, { "frames", si.frames },
                                     { "num_files", si.num_files },
                                     { "confidence", "catalog" } });

    mcp::ToolResult r;
    r.value = mcp::json{
        { "count", spells.size() + attacks.size() + abilities.size() + traps.size() },
        { "spell_count", spells.size() },
        { "monster_attack_count", attacks.size() },
        { "ability_count", abilities.size() },
        { "trap_count", traps.size() },
        { "cast_action_map", mcp::json{ { "Fire", "fm" }, { "Lightning", "lm" },
                                        { "Magic", "qm" }, { "Physical", "qm" },
                                        { "Acid", "qm" } } },
        { "impact_catalog", impacts },
        { "spells", spells },
        { "monster_attacks", attacks },
        { "abilities", abilities },
        { "traps", traps } };
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
                      { "width", json{ { "type", json::array({"integer", "string"}) },
                                        { "description", "required for .cel/.cl2; an integer (e.g. 96) or the string \"auto\" to look up from DevilutionX data tables by sprite path" } } },
                      { "widths", json{ { "type", "array" },
                                         { "items", json{ { "type", "integer" } } },
                                         { "description", "optional per-frame widths for .cel sprites whose frames differ in width (e.g. data\\inv\\objcurs.cel, 179 frames). Overrides 'width'. With width:\"auto\" the built-in table for data\\inv\\objcurs*.cel is used automatically" } } },
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
        "(for the decode palette dropdown). Use kind='ui' to list the UI layout's "
        "referenced art (ui_art\\*.pcx, data\\*.clx/.cel, ctrlpan\\spelicon.cel, ...) "
        "from the static DiabloUI screen table, so the Catalog browser can show the "
        "same art the GUI layout viewer composites (with frame info for playback). "
        "kind='all' returns sprites + palettes together (no UI art). The frontend builds "
        "its catalog tree from this and then calls sprite.decode(name, width:\"auto\") "
        "per leaf. The 'inv' family holds data\\inv\\* inventory cursor icons "
        "(objcurs*.cel: frames have different widths, resolved automatically by "
        "sprite.decode from a built-in table) and inventory panel backgrounds "
        "(inv*.cel: single width 320). Also returns 'archives': EVERY MPQ path from recovered.json "
        "(including archives that hold no CL2/CEL sprites, e.g. devilutionx.mpq with "
        "its .clx UI art and ui_art\\diablo.pal) -- pass this full stack as 'archives' "
        "to ui.build / scene.build so cross-archive art resolves.",
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
                              { "description", "filter by family: plrgfx / monsters / missiles / items / objects / towners / inv / ui / palette (empty = all)" } } },
                      { "class",
                        json{ { "type", "string" },
                              { "description", "filter by class (classPath, assetsSuffix, or sprite name; empty = all)" } } },
                      { "kind",
                        json{ { "type", "string" },
                              { "description", "what to enumerate: 'sprite' (default, CL2/CEL) | 'palette' (.pal files) | 'ui' (UI layout art) | 'all' (sprite + palette)" } } } } },
              { "required", json::array({}) } },
        toolSpriteList });

    reg.registerTool(ToolDef{
        "spell.list",
        "Synthesize a stage-ordered catalog of spell / monster-attack visual effects "
        "from the DevilutionX data tables (spelldat/misdat/missile_sprites/monstdat). "
        "Each entry has ordered stages cast -> projectile -> impact -> debuff. Sprite "
        "layers carry the numbered missiles\\{name}{N}.cl2 file list plus an EXPLICIT "
        "width (width:\"auto\" does not work for numbered files). debuff layers may be "
        "TRN palette remaps (apply_trn on the target) or overlay sprites. "
        "Every entry also carries a Chinese display name (name_zh, resolved from "
        "DevilutionX Translations/zh_TW.po, falling back to the English name) and a "
        "synthesized Chinese description of the effect chain. Player entries additionally "
        "carry spell-level parameters from spelldat.tsv: level (sBookLvl, the spell tier), "
        "staff_level, mana_cost, mana_multiplier (mana saved per trained level), min_mana, "
        "staff_min/staff_max, and mana_by_level (an array of {level, mana} for levels "
        "1..bookLevel, computed from spelldat + the GetManaAmount formula; base mana before "
        "the class multiplier). "
        "visual_by_level is an array of {level, params} for levels 1..bookLevel (player) or "
        "1..16 (monster, where level = monster level). Each params map holds the spell's "
        "level-dependent VISUAL parameters transcribed from DevilutionX's handlers: "
        "duration_ticks / duration_sec (≈, 60 ticks/sec), speed (missile velocity), "
        "range_tiles / radius_tiles (crawl/search radius), count (projectile count), and "
        "spell-specific keys (e.g. segments, wall_length_tiles). Keys differ per spell "
        "because each spell hardcodes its own formula. A _note key marks values that depend "
        "on caster character level or dungeon level (approximated at refChar=50, refDungeon=0). "
        "Spells with no level-scaled visual params emit an empty visual_by_level. "
        "Monster attacks also cover AIs that pick the missile inside their AI function "
        "instead of GetMissileType() (Counselor -> Firebolt/ChargedBolt/LightningControl/" 
        "Fireball by intelligence plus a Flash form; Familiar -> Lightning; Gloom -> Rhino " 
        "charge; Mega -> Inferno). Those entries repeat the same " 
        "monster_ai once per form and add variant (the form name) and condition (a Chinese " 
        "note saying when the game uses it). Ids are 'AI' or 'AI.variant'. " 
        "abilities[] lists engine-level monster abilities that are not ranged attacks "
        "(Blink teleport on a hard hit, acid death puddle, sneak invisibility), each with "
        "monsters, trigger and source_ref (a DevilutionX source location). Diablo 1 / "
        "Hellfire have no Diablo-2 champion affixes (Thunderstorm / Waller / Vortex / "
        "Shielding do not exist in the engine). "
        "traps[] lists trap spell effects: wall traps (Arrow always, Firebolt at level >=3, "
        "LightningControl at level >=6), trapped chests (Arrow/FireArrow/Nova, plus Hellfire's "
        "RingOfFire/StealPotions/StealMana), and Hellfire flame traps. Each carries objects "
        "(OBJ_*), trigger and source_ref. "
        "impact_catalog lists all boom/splat effects (confidence='catalog') for manual "
        "selection. Filter with source='player'|'monster'|'ability'|'trap' and/or id.",
        json{ { "type", "object" },
              { "properties",
                json{ { "devilutionx_root",
                        json{ { "type", "string" },
                              { "description", "DevilutionX source root (default D:/work/xt/diablo/DevilutionX)" } } },
                      { "source",
                        json{ { "type", "string" },
                              { "description", "'player' | 'monster' | 'ability' | 'trap' | empty (all)" } } },
                      { "id",
                        json{ { "type", "string" },
                              { "description", "filter by spell/AI id or display name" } } } } },
              { "required", json::array({}) } },
        toolSpellList });
}

} // namespace tools
} // namespace mcp
