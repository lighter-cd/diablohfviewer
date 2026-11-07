// src/decoders/sprite_width.cpp
//
// Implementation of the auto frame-width lookup described in sprite_width.h.
// The TSV helpers are file-local; only playerTypeToColumn and autoWidth are
// exported.

#include "decoders/sprite_width.h"

#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace decoders {
namespace sprite_width {
namespace {

std::vector<std::string> splitTab(const std::string& s) {
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
std::map<std::string, std::string> readKV(const std::filesystem::path& p) {
    std::map<std::string, std::string> kv;
    std::ifstream f(p);
    if (!f)
        return kv;
    std::string line;
    bool first = true;
    while (std::getline(f, line)) {
        if (first) {
            first = false;
            continue;
        }
        if (line.empty())
            continue;
        auto parts = splitTab(line);
        if (parts.size() >= 2)
            kv[parts[0]] = parts[1];
    }
    return kv;
}

// Look up `valCol` for the row whose `keyCol` equals `keyVal` in a header TSV.
// Returns "" if not found.
std::string lookupInTsv(const std::filesystem::path& p, const std::string& keyCol,
                        const std::string& keyVal, const std::string& valCol) {
    std::ifstream f(p);
    if (!f)
        return "";
    std::string line;
    if (!std::getline(f, line))
        return "";
    auto header = splitTab(line);
    int ki = -1, vi = -1;
    for (size_t i = 0; i < header.size(); i++) {
        if (header[i] == keyCol)
            ki = (int)i;
        else if (header[i] == valCol)
            vi = (int)i;
    }
    if (ki < 0 || vi < 0)
        return "";
    while (std::getline(f, line)) {
        if (line.empty())
            continue;
        auto parts = splitTab(line);
        if ((int)parts.size() > ki && parts[ki] == keyVal && (int)parts.size() > vi)
            return parts[vi];
    }
    return "";
}

} // namespace

bool playerTypeToColumn(const std::string& type, char weaponChar, std::string& col) {
    if (type == "as" || type == "st")
        col = "stand";
    else if (type == "aw" || type == "wl")
        col = "walk";
    else if (type == "at")
        col = (weaponChar == 'b') ? "bow" : "attack";
    else if (type == "ht")
        col = "swHit";
    else if (type == "lm")
        col = "lightning";
    else if (type == "fm")
        col = "fire";
    else if (type == "qm")
        col = "magic";
    else if (type == "dt")
        col = "death";
    else if (type == "bl")
        col = "block";
    else
        return false;
    return true;
}

int autoWidth(const std::string& spritePath, const std::filesystem::path& devRoot,
              std::string& dbg) {
    std::string p = spritePath;
    for (char& c : p)
        if (c == '\\')
            c = '/';
    auto splitSlash = [](const std::string& s) {
        std::vector<std::string> out;
        std::string cur;
        for (char c : s) {
            if (c == '/') {
                out.push_back(cur);
                cur.clear();
            } else {
                cur.push_back(c);
            }
        }
        out.push_back(cur);
        return out;
    };
    std::vector<std::string> comps = splitSlash(p);

    auto findTok = [&](const std::string& tok) -> int {
        for (size_t i = 0; i < comps.size(); i++)
            if (comps[i] == tok)
                return (int)i;
        return -1;
    };
    // Rebuild the path component(s) after a token, joined with backslashes, so
    // sub-directory sprite names (e.g. monsters\animals\cow) survive.
    auto joinAfter = [&](int idx) -> std::string {
        std::string rel;
        for (int i = idx + 1; i < (int)comps.size(); i++) {
            if (!rel.empty())
                rel += '\\';
            rel += comps[i];
        }
        return rel;
    };
    auto stripExt = [](std::string s) -> std::string {
        size_t dot = s.rfind('.');
        if (dot != std::string::npos)
            s = s.substr(0, dot);
        return s;
    };

    const std::filesystem::path classesRoot =
        devRoot / "assets" / "txtdata" / "classes";
    const std::vector<std::filesystem::path> monFiles = {
        devRoot / "assets" / "txtdata" / "monsters" / "monstdat.tsv",
        devRoot / "mods" / "hf" / "txtdata" / "monsters" / "monstdat.tsv",
    };
    const std::vector<std::filesystem::path> misFiles = {
        devRoot / "assets" / "txtdata" / "missiles" / "missile_sprites.tsv",
        devRoot / "mods" / "hf" / "txtdata" / "missiles" / "missile_sprites.tsv",
    };
    const std::vector<std::filesystem::path> objFiles = {
        devRoot / "assets" / "txtdata" / "objects" / "objdat.tsv",
    };
    const std::vector<std::filesystem::path> twnFiles = {
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
        if (filename.size() < 2) {
            dbg = "plrgfx filename too short";
            return -1;
        }
        std::string type = filename.substr(filename.size() - 2);
        char weaponChar = (prefix.size() >= 3) ? prefix[2] : '?';
        std::string col;
        if (!playerTypeToColumn(type, weaponChar, col)) {
            dbg = "unknown player type code '" + type + "'";
            return -1;
        }
        // DevilutionX quirk: the in-MPQ sorcerer sprite folder is spelled
        // "sorceror" (with an 'o') but the class data directory is "sorcerer"
        // (with an 'e'). Map the MPQ path token to the real directory so the
        // width table can be located; other classes match their folder 1:1.
        std::string dirName = classPath;
        if (classPath == "sorceror")
            dirName = "sorcerer";
        std::filesystem::path sp = classesRoot / dirName / "sprites.tsv";
        auto kv = readKV(sp);
        auto it = kv.find(col);
        if (it == kv.end()) {
            dbg = "no column '" + col + "' in " + sp.string();
            return -1;
        }
        dbg = "plrgfx/" + classPath + " type=" + type + " -> " + col + "=" + it->second;
        return atoi(it->second.c_str());
    }
    if ((idx = findTok("monsters")) >= 0) {
        std::string rel = stripExt(joinAfter(idx)); // assetsSuffix + L
        if (rel.size() < 2) {
            dbg = "monster name too short";
            return -1;
        }
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
        dbg = "items (CEL) have no width table; pass a numeric width";
        return -1;
    }
    dbg = "unknown sprite family (expected plrgfx/monsters/missiles/"
          "objects/towners/items in the path)";
    return -1;
}

int monsterFramesPerDir(const std::string& spritePath,
                        const std::filesystem::path& devRoot, std::string& dbg) {
    std::string p = spritePath;
    for (char& c : p)
        if (c == '\\')
            c = '/';
    auto splitSlash = [](const std::string& s) {
        std::vector<std::string> out;
        std::string cur;
        for (char c : s) {
            if (c == '/') {
                out.push_back(cur);
                cur.clear();
            } else {
                cur.push_back(c);
            }
        }
        out.push_back(cur);
        return out;
    };
    std::vector<std::string> comps = splitSlash(p);
    int idx = -1;
    for (size_t i = 0; i < comps.size(); i++)
        if (comps[i] == "monsters") {
            idx = (int)i;
            break;
        }
    if (idx < 0) {
        dbg = "not a monsters/ sprite path";
        return 0;
    }
    // rel = assetsSuffix + trailing animation letter L (e.g. "zombie\\zombieN").
    std::string rel;
    for (int i = idx + 1; i < (int)comps.size(); i++) {
        if (!rel.empty())
            rel += '\\';
        rel += comps[i];
    }
    size_t dot = rel.rfind('.');
    if (dot != std::string::npos)
        rel = rel.substr(0, dot);
    if (rel.size() < 2) {
        dbg = "monster name too short";
        return 0;
    }
    std::string suffix = rel.substr(0, rel.size() - 1); // assetsSuffix
    char L = rel.back();                                 // animation letter
    // MonsterGraphic order: Stand, Walk, Attack, GotHit, Death, Special
    // Animletter "nwahds" maps them to letters.
    int animIndex = -1;
    switch (L) {
        case 'n': animIndex = 0; break;
        case 'w': animIndex = 1; break;
        case 'a': animIndex = 2; break;
        case 'h': animIndex = 3; break;
        case 'd': animIndex = 4; break;
        case 's': animIndex = 5; break;
        default: animIndex = -1; break;
    }
    if (animIndex < 0) {
        dbg = "unknown monster anim letter '" + std::string(1, L) + "'";
        return 0;
    }
    const std::vector<std::filesystem::path> monFiles = {
        devRoot / "assets" / "txtdata" / "monsters" / "monstdat.tsv",
        devRoot / "mods" / "hf" / "txtdata" / "monsters" / "monstdat.tsv",
    };
    for (const auto& f : monFiles) {
        std::string frames = lookupInTsv(f, "assetsSuffix", suffix, "frames[6]");
        if (frames.empty())
            continue;
        // frames[6] is comma-separated: stand,walk,attack,hit,death,special
        std::vector<std::string> parts;
        std::string cur;
        for (char c : frames) {
            if (c == ',') {
                parts.push_back(cur);
                cur.clear();
            } else {
                cur.push_back(c);
            }
        }
        parts.push_back(cur);
        if ((int)parts.size() > animIndex) {
            int fpd = atoi(parts[animIndex].c_str());
            dbg = "monster " + suffix + " anim '" + std::string(1, L) +
                  "' framesPerDir=" + std::to_string(fpd);
            return fpd;
        }
    }
    dbg = "monster assetsSuffix '" + suffix + "' not found in monstdat";
    return 0;
}

} // namespace sprite_width
} // namespace decoders
