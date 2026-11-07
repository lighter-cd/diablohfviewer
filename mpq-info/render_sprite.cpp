// mpq-info/render_sprite.cpp
//
// Render one extracted Diablo 1 sprite (CL2 or CEL) to a PNG, using the
// src/decoders pipeline: CL2/CEL -> CLX -> RGBA frame(s) -> PNG.
//
// The CL2/CEL pixel layout does NOT store the frame width, so it must be known
// to decode the frames. The width comes from DevilutionX's animation tables:
//   - players : assets/txtdata/classes/<classPath>/sprites.tsv
//   - monsters: assets/txtdata/monsters/monstdat.tsv   (column "width")
//   - missiles: assets/txtdata/missiles/missile_sprites.tsv (column "width")
//   - objects : assets/txtdata/objects/objdat.tsv      (column "animWidth")
//   - towners : assets/txtdata/towners/towners.tsv     (column "animWidth")
//
// When <width> is given as the literal "auto" (or any non-numeric token),
// render_sprite inspects the sprite path, derives the sprite family, and looks
// the correct width up in those tables -- so a single command renders a frame
// without the caller memorizing per-sprite widths. Items (CEL) have no width
// table and still require a manual <width>.
//
// Palette is a 256*3 (or 256*4) .pal file.
//
// Build (mirrors recover_phase0.cpp / batch_extract.cpp):
//   g++ -I 3rdparty -I 3rdparty/StormLib/src -I src -std=c++17 \
//       mpq-info/render_sprite.cpp \
//       src/decoders/clx.cpp src/decoders/cl2.cpp src/decoders/cel.cpp \
//       src/decoders/pcx.cpp src/decoders/palette.cpp src/decoders/png.cpp \
//       -o out/render_sprite.exe
//
// Run:
//   out/render_sprite.exe <sprite> <palette.pal> <width|auto> \
//                         [out.png] [frameIndex] [devilutionx_root]
//
// width may be a number (manual) or the literal "auto" (look up by path).
// frameIndex defaults to 0; out.png defaults to out/render/sprite.png.
//
// Examples:
//   # warrior, light armour, unarmed, Stand (auto width = 96):
//   out/render_sprite.exe \
//     out/extracted/DIABDAT/plrgfx/warrior/wlu/wluas.cl2 \
//     out/extracted/devilutionx/ui_art/diablo.pal auto \
//     out/render/warrior_stand.png
//   # explicit override:
//   out/render_sprite.exe <sprite> <pal.pal> 128 out/render/a.png 0

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <string>
#include <vector>
#include <filesystem>

#include "decoders/cl2.h"
#include "decoders/cel.h"
#include "decoders/clx.h"
#include "decoders/palette.h"
#include "decoders/png.h"

namespace fs = std::filesystem;

namespace {

bool readFile(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    std::streamoff sz = f.tellg();
    if (sz <= 0) return false;
    f.seekg(0, std::ios::beg);
    out.resize(static_cast<size_t>(sz));
    f.read(reinterpret_cast<char*>(out.data()), sz);
    return static_cast<bool>(f) || f.eof();
}

bool endsWith(const std::string& s, const std::string& suf) {
    return s.size() >= suf.size() &&
           s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
}

// --- TSV helpers (mirror recover_phase0.cpp) ---------------------------------

static std::vector<std::string> splitTab(const std::string& s) {
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

// Read a "Variable<TAB>Value" file (e.g. classes/<c>/sprites.tsv) into a map.
// The first line is a header (skipped).
static std::map<std::string, std::string> readKV(const fs::path& p) {
    std::map<std::string, std::string> kv;
    std::ifstream f(p);
    if (!f) return kv;
    std::string line;
    bool first = true;
    while (std::getline(f, line)) {
        if (first) { first = false; continue; }
        if (line.empty()) continue;
        auto parts = splitTab(line);
        if (parts.size() >= 2) kv[parts[0]] = parts[1];
    }
    return kv;
}

// Look up `valCol` for the row whose `keyCol` equals `keyVal` in a header TSV.
// Returns "" if not found.
static std::string lookupInTsv(const fs::path& p, const std::string& keyCol,
                               const std::string& keyVal,
                               const std::string& valCol) {
    std::ifstream f(p);
    if (!f) return "";
    std::string line;
    if (!std::getline(f, line)) return "";
    auto header = splitTab(line);
    int ki = -1, vi = -1;
    for (size_t i = 0; i < header.size(); i++) {
        if (header[i] == keyCol) ki = (int)i;
        else if (header[i] == valCol) vi = (int)i;
    }
    if (ki < 0 || vi < 0) return "";
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        auto parts = splitTab(line);
        if ((int)parts.size() > ki && parts[ki] == keyVal &&
            (int)parts.size() > vi)
            return parts[vi];
    }
    return "";
}

// Map a 2-char player sprite type code (e.g. "as") + weapon char to the
// sprites.tsv column name that holds its width.
// Source: DevilutionX Source/player.cpp:2098-2142 (type code) and
//         Source/player.cpp:1479-1502 (graphic -> column).
static bool playerTypeToColumn(const std::string& type, char weaponChar,
                               std::string& col) {
    if (type == "as" || type == "st") col = "stand";
    else if (type == "aw" || type == "wl") col = "walk";
    else if (type == "at") col = (weaponChar == 'b') ? "bow" : "attack";
    else if (type == "ht") col = "swHit";
    else if (type == "lm") col = "lightning";
    else if (type == "fm") col = "fire";
    else if (type == "qm") col = "magic";
    else if (type == "dt") col = "death";
    else if (type == "bl") col = "block";
    else return false;
    return true;
}

// Auto-determine the frame width for a sprite path, using DevilutionX's data
// tables. Returns the width (>0) on success, or <=0 on failure (with a
// human-readable reason in `dbg`).
static int autoWidth(const std::string& spritePath, const fs::path& devRoot,
                     std::string& dbg) {
    std::string p = spritePath;
    for (char& c : p)
        if (c == '\\') c = '/';
    auto splitSlash = [](const std::string& s) {
        std::vector<std::string> out;
        std::string cur;
        for (char c : s) {
            if (c == '/') { out.push_back(cur); cur.clear(); }
            else cur.push_back(c);
        }
        out.push_back(cur);
        return out;
    };
    std::vector<std::string> comps = splitSlash(p);

    auto findTok = [&](const std::string& tok) -> int {
        for (size_t i = 0; i < comps.size(); i++)
            if (comps[i] == tok) return (int)i;
        return -1;
    };
    // Rebuild the path component(s) after a token, joined with backslashes,
    // so sub-directory sprite names (e.g. monsters\animals\cow) survive.
    auto joinAfter = [&](int idx) -> std::string {
        std::string rel;
        for (int i = idx + 1; i < (int)comps.size(); i++) {
            if (!rel.empty()) rel += '\\';
            rel += comps[i];
        }
        return rel;
    };
    auto stripExt = [](std::string s) -> std::string {
        size_t dot = s.rfind('.');
        if (dot != std::string::npos) s = s.substr(0, dot);
        return s;
    };

    const fs::path classesRoot =
        devRoot / "assets" / "txtdata" / "classes";
    const std::vector<fs::path> monFiles = {
        devRoot / "assets" / "txtdata" / "monsters" / "monstdat.tsv",
        devRoot / "mods" / "hf" / "txtdata" / "monsters" / "monstdat.tsv",
    };
    const std::vector<fs::path> misFiles = {
        devRoot / "assets" / "txtdata" / "missiles" / "missile_sprites.tsv",
        devRoot / "mods" / "hf" / "txtdata" / "missiles" / "missile_sprites.tsv",
    };
    const std::vector<fs::path> objFiles = {
        devRoot / "assets" / "txtdata" / "objects" / "objdat.tsv",
    };
    const std::vector<fs::path> twnFiles = {
        devRoot / "assets" / "txtdata" / "towners" / "towners.tsv",
        devRoot / "mods" / "hf" / "txtdata" / "towners" / "towners.tsv",
    };

    int idx;
    if ((idx = findTok("plrgfx")) >= 0) {
        // plrgfx / <classPath> / <prefix=c+a+w> / <prefix><type>.cl2
        if ((size_t)idx + 3 >= comps.size()) {
            dbg = "plrgfx path too short";
            return -1;
        }
        std::string classPath = comps[idx + 1];
        std::string prefix = comps[idx + 2];
        std::string filename = stripExt(comps[idx + 3]);
        if (filename.size() < 2) { dbg = "plrgfx filename too short"; return -1; }
        std::string type = filename.substr(filename.size() - 2);
        char weaponChar = (prefix.size() >= 3) ? prefix[2] : '?';
        std::string col;
        if (!playerTypeToColumn(type, weaponChar, col)) {
            dbg = "unknown player type code '" + type + "'";
            return -1;
        }
        fs::path sp = classesRoot / classPath / "sprites.tsv";
        auto kv = readKV(sp);
        auto it = kv.find(col);
        if (it == kv.end()) {
            dbg = "no column '" + col + "' in " + sp.string();
            return -1;
        }
        dbg = "plrgfx/" + classPath + " type=" + type + " -> " + col + "=" +
              it->second;
        return atoi(it->second.c_str());
    }
    if ((idx = findTok("monsters")) >= 0) {
        std::string rel = stripExt(joinAfter(idx)); // assetsSuffix + L
        if (rel.size() < 2) { dbg = "monster name too short"; return -1; }
        std::string suffix = rel.substr(0, rel.size() - 1); // drop trailing L
        for (const auto& f : monFiles) {
            std::string w = lookupInTsv(f, "assetsSuffix", suffix, "width");
            if (!w.empty()) {
                dbg = "monster assetsSuffix=" + suffix + " width=" + w;
                return atoi(w.c_str());
            }
        }
        dbg = "monster assetsSuffix '" + suffix + "' not found in monstdat";
        return -1;
    }
    if ((idx = findTok("missiles")) >= 0) {
        std::string rel = stripExt(comps.back());
        for (const auto& f : misFiles) {
            std::string w = lookupInTsv(f, "name", rel, "width");
            if (!w.empty()) {
                dbg = "missile name=" + rel + " width=" + w;
                return atoi(w.c_str());
            }
        }
        dbg = "missile name '" + rel + "' not found in missile_sprites";
        return -1;
    }
    if ((idx = findTok("objects")) >= 0) {
        std::string rel = stripExt(comps.back());
        for (const auto& f : objFiles) {
            std::string w = lookupInTsv(f, "file", rel, "animWidth");
            if (!w.empty()) {
                dbg = "object file=" + rel + " animWidth=" + w;
                return atoi(w.c_str());
            }
        }
        dbg = "object '" + rel + "' not found in objdat";
        return -1;
    }
    if ((idx = findTok("towners")) >= 0) {
        std::string rel = stripExt("towners\\" + joinAfter(idx));
        for (const auto& f : twnFiles) {
            std::string w = lookupInTsv(f, "animPath", rel, "animWidth");
            if (!w.empty()) {
                dbg = "towner animPath=" + rel + " animWidth=" + w;
                return atoi(w.c_str());
            }
        }
        dbg = "towner '" + rel + "' not found in towners";
        return -1;
    }
    if ((idx = findTok("items")) >= 0) {
        dbg = "items (CEL) have no width table; pass a numeric <width>";
        return -1;
    }
    dbg = "unknown sprite family (expected plrgfx/monsters/missiles/"
          "objects/towners/items in the path)";
    return -1;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        fprintf(stderr,
                "usage: %s <sprite> <palette.pal> <width|auto> "
                "[out.png] [frameIndex] [devilutionx_root]\n",
                argv[0]);
        return 1;
    }
    const std::string spritePath = argv[1];
    const std::string palPath    = argv[2];
    const std::string widthArg   = argv[3];
    const std::string outPath    = (argc > 4) ? argv[4] : "out/render/sprite.png";
    const size_t frameIdx        = (argc > 5) ? static_cast<size_t>(atoi(argv[5])) : 0;
    const fs::path devRoot       = (argc > 6)
                                       ? fs::path(argv[6])
                                       : fs::path("D:/work/xt/diablo/DevilutionX");

    int width = atoi(widthArg.c_str());
    bool autoW = (widthArg != "auto") ? (width <= 0) : true;
    if (autoW) {
        std::string dbg;
        width = autoWidth(spritePath, devRoot, dbg);
        printf("[auto-width] %s\n", dbg.c_str());
        if (width <= 0) {
            fprintf(stderr,
                    "cannot auto-determine width: %s\n"
                    "(pass a numeric <width> manually, or check devilutionx_root)\n",
                    dbg.c_str());
            return 1;
        }
    }

    if (width <= 0 || width > 65535) {
        fprintf(stderr, "invalid width %d\n", width);
        return 1;
    }

    std::vector<uint8_t> sprite, palBytes;
    if (!readFile(spritePath, sprite)) {
        fprintf(stderr, "cannot read sprite %s\n", spritePath.c_str());
        return 1;
    }
    if (!readFile(palPath, palBytes)) {
        fprintf(stderr, "cannot read palette %s\n", palPath.c_str());
        return 1;
    }

    std::vector<uint8_t> pal = decoders::loadPalette(palBytes);
    if (pal.size() < 256 * 3) {
        fprintf(stderr, "palette too small (%zu bytes)\n", palBytes.size());
        return 1;
    }

    // CL2/CEL -> CLX.
    std::vector<uint8_t> clx;
    const bool isCel = endsWith(spritePath, ".cel") || endsWith(spritePath, ".CEL");
    if (isCel)
        clx = decoders::celToClx(sprite.data(), sprite.size(),
                                 static_cast<uint16_t>(width));
    else
        clx = decoders::cl2ToClx(sprite.data(), sprite.size(),
                                 static_cast<uint16_t>(width));

    if (clx.empty()) {
        fprintf(stderr, "conversion to CLX failed (wrong width?)\n");
        return 1;
    }

    std::vector<decoders::DecodedFrame> frames = decoders::decodeClx(clx, pal);
    if (frames.empty()) {
        fprintf(stderr, "no frames decoded\n");
        return 1;
    }
    if (frameIdx >= frames.size()) {
        fprintf(stderr, "frame %zu out of range (have %zu frames)\n",
                frameIdx, frames.size());
        return 1;
    }

    const decoders::DecodedFrame& fr = frames[frameIdx];
    printf("decoded %zu frame(s); rendering frame %zu: %ux%u (width hint %d)\n",
           frames.size(), frameIdx, fr.width, fr.height, width);

    if (!decoders::writePNG(outPath, fr.width, fr.height, fr.rgba)) {
        fprintf(stderr, "writePNG failed: %s\n", outPath.c_str());
        return 1;
    }
    printf("wrote %s\n", outPath.c_str());
    return 0;
}
