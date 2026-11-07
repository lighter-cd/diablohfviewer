// mpq-info/recover_phase0.cpp
//
// Phase 0 filename-recovery driver (the "coverage improvement route" from
// docs/PROJECT.md section 8 / 阶段 0).
//
// Diablo 1 / Hellfire MPQs ship WITHOUT a (listfile), so the block table only
// yields placeholder names ("FileNNNNNNN.xxx"). Real filenames are reconstructed
// by validating candidate names against the archive's one-way hash
// (StormLib's SFileOpenFileEx does this for us).
//
// The bulk of real names are not static literals — they are assembled at runtime
// in DevilutionX from splicing formulas. This program reads the *authoritative*
// data tables that feed those formulas and regenerates the candidate set:
//
//   plrgfx : Source/player.cpp:1506  plrgfx\{classPath}\{c}{a}{w}\{c}{a}{w}{type}.cl2
//   monsters: Source/monster.cpp:3221 monsters\{assetsSuffix}{L}.cl2   (L = "nwahds")
//   missiles: Source/tables/misdat.cpp:420  missiles\{name}.cl2
//
// It merges the generated candidates with the static harvest dictionary
// (mpq-info/dict/diablo1-harvest.txt), then for every known archive runs
// verify()+querySize() and writes:
//   - mpq-info/dict/recovered/<ARCHIVE>.txt   (name<TAB>size, sorted)
//   - mpq-info/dict/recovered/<ARCHIVE>.err   (open failure reason)
//   - mpq-info/dict/recovered.json            (aggregate, machine-readable)
//
// Build (from the project root):
//   g++ -I 3rdparty -I 3rdparty/StormLib/src -I src -std=c++17 \
//       mpq-info/recover_phase0.cpp src/engine/archive.cpp \
//       3rdparty/StormLib/build/libStormLib.a \
//       -lws2_32 -lwininet -static-libgcc -static-libstdc++ \
//       -o out/recover_phase0.exe
// Run (from the project root):
//   out/recover_phase0.exe [devilutionx_root] [mpq_root]
//
// Defaults: devilutionx_root = D:/work/xt/diablo/DevilutionX
//           mpq_root        = D:/work/xt/diablo/DevilutionX-Hell Fire

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <algorithm>

#include "engine/archive.h"
#include "nlohmann/json.hpp"

namespace fs = std::filesystem;
using json = nlohmann::json;

// ---------------------------------------------------------------------------
// Small TSV helpers
// ---------------------------------------------------------------------------

// Split a line on tabs (StormLib/DevilutionX TSVs are tab-separated).
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
static std::map<std::string, std::string> readKV(const fs::path& p) {
    std::map<std::string, std::string> kv;
    std::ifstream f(p);
    if (!f)
        return kv;
    std::string line;
    bool first = true;
    while (std::getline(f, line)) {
        if (first) { first = false; continue; } // header row
        if (line.empty())
            continue;
        auto parts = splitTab(line);
        if (parts.size() >= 2)
            kv[parts[0]] = parts[1];
    }
    return kv;
}

// Read one named column from a multi-column TSV (header names the columns).
// Returns the column's values across all data rows.
static std::vector<std::string> readColumnByName(const fs::path& p,
                                                 const std::string& col) {
    std::vector<std::string> values;
    std::ifstream f(p);
    if (!f)
        return values;
    std::string line;
    if (!std::getline(f, line))
        return values; // no header
    auto header = splitTab(line);
    int idx = -1;
    for (size_t i = 0; i < header.size(); i++) {
        if (header[i] == col) { idx = (int)i; break; }
    }
    if (idx < 0)
        return values;
    while (std::getline(f, line)) {
        if (line.empty())
            continue;
        auto parts = splitTab(line);
        if ((int)parts.size() > idx && !parts[idx].empty())
            values.push_back(parts[idx]);
    }
    return values;
}

// ---------------------------------------------------------------------------
// Candidate generation from DevilutionX splicing formulas
// ---------------------------------------------------------------------------

static void genPlayers(std::set<std::string>& out, const fs::path& classesRoot) {
    // classPath + classChar come from each classes/<c>/sprites.tsv.
    std::error_code ec;
    if (!fs::exists(classesRoot, ec))
        return;
    for (const auto& dirEntry : fs::directory_iterator(classesRoot, ec)) {
        if (!dirEntry.is_directory())
            continue;
        fs::path sp = dirEntry.path() / "sprites.tsv";
        auto kv = readKV(sp);
        auto itPath = kv.find("classPath");
        auto itChar = kv.find("classChar");
        if (itPath == kv.end() || itChar == kv.end())
            continue;
        const std::string& cpath = itPath->second;
        const std::string cchar = itChar->second; // single char, e.g. 'w'
        if (cchar.empty())
            continue;
        const char armour[] = {'l', 'm', 'h'};                 // light/medium/heavy
        const char wep[] = {'n', 'u', 's', 'd', 'b', 'a',
                            'm', 'h', 't'};                    // 9 weapon anims
        const char* types[] = {"as", "st", "aw", "wl", "at", "ht",
                               "lm", "fm", "qm", "dt", "bl"};   // 11 graphics
        std::string prefix(3, ' ');
        prefix[0] = cchar[0];
        for (char a : armour) {
            prefix[1] = a;
            for (char w : wep) {
                prefix[2] = w;
                for (const char* t : types) {
                    std::string name = "plrgfx\\" + cpath + "\\" + prefix + "\\" +
                                       prefix + t + ".cl2";
                    out.insert(name);
                    // Per-sprite TRN palette variant: GetPlayerGraphicTRN builds
                    // pszName + ".trn" for every player graphic (optional colour TRN).
                    out.insert("plrgfx\\" + cpath + "\\" + prefix + "\\" +
                               prefix + t + ".trn");
                }
            }
        }
    }
}

static void genMonsters(std::set<std::string>& out,
                        const std::vector<fs::path>& monFiles) {
    const char letters[] = {'n', 'w', 'a', 'h', 'd', 's'}; // Animletter = "nwahds"
    for (const auto& f : monFiles) {
        // assetsSuffix -> monsters\{suffix}{L}.cl2  (suffix may contain '\')
        auto suffixes = readColumnByName(f, "assetsSuffix");
        for (const auto& suf : suffixes) {
            for (char L : letters) {
                std::string name = "monsters\\" + suf + L + ".cl2";
                out.insert(name);
            }
        }
        // trnFile -> monsters\{trnFile}.trn
        auto trns = readColumnByName(f, "trnFile");
        for (const auto& trn : trns) {
            std::string name = "monsters\\" + trn + ".trn";
            out.insert(name);
        }
    }
}

static void genMissiles(std::set<std::string>& out,
                       const std::vector<fs::path>& misFiles) {
    for (const auto& f : misFiles) {
        auto names = readColumnByName(f, "name");
        for (const auto& n : names) {
            // Common case: sheet/sprites stored as .cl2
            out.insert("missiles\\" + n + ".cl2");
            // animFAmt==1 path uses the bare name (loaded directly as cl2)
            out.insert("missiles\\" + n);
        }
    }
}

// Item drop / HUD sprites: Source/items.cpp:2428 -> items\{ItemDropNames[i]},
// loaded via LoadCel (CEL format). The names are a fixed game constant
// (Source/items.cpp:230, 44 entries), so they are embedded here rather than
// read from a TSV. Empirically verified: the stored extension is .cel.
static void genItems(std::set<std::string>& out) {
    static const char* kItemDropNames[] = {
        "armor2",   "axe",      "fbttle",   "bow",       "goldflip", "helmut",
        "mace",     "shield",   "swrdflip", "rock",      "cleaver",  "staff",
        "ring",     "crownf",   "larmor",   "wshield",   "scroll",   "fplatear",
        "fbook",    "food",     "fbttlebb", "fbttledy",  "fbttleor", "fbttlebr",
        "fbttlebl", "fbttleby", "fbttlewh", "fbttledb",  "fear",     "fbrain",
        "fmush",    "innsign",  "bldstn",   "fanvil",    "flazstaf", "bombs1",
        "halfps1",  "wholeps1", "runes1",   "teddys1",   "cows1",    "donkys1",
        "mooses1"};
    for (const char* n : kItemDropNames)
        out.insert("items\\" + std::string(n) + ".cel");
    // Static item HUD sprites (also CEL format): durability icons + automap.
    static const char* kItemExtras[] = {"duricons", "map\\mapztown"};
    for (const char* e : kItemExtras)
        out.insert("items\\" + std::string(e) + ".cel");
}

// Object (dungeon scenery / interactables) sprites: Source/objects.cpp:3720 ->
// objects\{ObjMasterLoadList[i]}, loaded via LoadCelWithStatus (CEL format).
// The names are the unique "file" column of objdat.tsv. Empirically verified:
// the stored extension is .cel.
static void genObjects(std::set<std::string>& out,
                       const std::vector<fs::path>& objFiles) {
    for (const auto& f : objFiles) {
        auto names = readColumnByName(f, "file");
        for (const auto& n : names)
            out.insert("objects\\" + n + ".cel");
    }
}

// Towner (NPC) sprites: Source/towners.cpp:130 ->
// LoadTownerAnimations(towner, entry.animPath, ...) where animPath is the full
// path (e.g. "towners\girl\girlw1") from the "animPath" column of towners.tsv.
// A few gendered/cow variants are hardcoded literals in towners.cpp. Loaded via
// LoadCel (CEL format). Empirically verified: the stored extension is .cel.
static void genTowners(std::set<std::string>& out,
                       const std::vector<fs::path>& twnFiles) {
    static const char* kExtras[] = {
        "towners\\farmer\\cfrmrn2",
        "towners\\farmer\\mfrmrn2",
        "towners\\girl\\girls1",
        "towners\\animals\\cow",
    };
    for (const auto& f : twnFiles) {
        auto paths = readColumnByName(f, "animPath");
        for (const auto& p : paths)
            out.insert(p + ".cel");
    }
    for (const char* e : kExtras)
        out.insert(std::string(e) + ".cel");
}

// TRN palette files (256-byte colour transformation tables). Two systematic
// families:
//  1. Font TRNs: Source/engine/render/text_render.cpp fixed list -> fonts\{name}.trn
//  2. Class / global TRNs:
//       - GetClassTRN: plrgfx\{sprites.tsv trn field}.trn  (e.g. plrgfx\warrior.trn)
//       - literals (lighting.cpp / trn.cpp): plrgfx\infra.trn, plrgfx\stone.trn,
//         gendata\pause.trn
// (Per-sprite player TRNs are emitted inside genPlayers as "{sprite}.trn".)
static void genFontTrn(std::set<std::string>& out) {
    static const char* kFontTrn[] = {
        "black",      "white",       "whitegold",  "red",       "blue",
        "orange",     "yellow",      "buttonface", "buttonpushed", "gamedialogwhite",
        "gamedialogyellow", "gamedialogred", "goldui", "grayui",
        "golduis",    "grayuis"};
    for (const char* n : kFontTrn)
        out.insert("fonts\\" + std::string(n) + ".trn");
}

static void genClassTrn(std::set<std::string>& out, const fs::path& classesRoot) {
    std::error_code ec;
    if (fs::exists(classesRoot, ec)) {
        for (const auto& dirEntry : fs::directory_iterator(classesRoot, ec)) {
            if (!dirEntry.is_directory())
                continue;
            auto kv = readKV(dirEntry.path() / "sprites.tsv");
            auto it = kv.find("trn");
            if (it != kv.end() && !it->second.empty())
                out.insert("plrgfx\\" + it->second + ".trn");
        }
    }
    out.insert("plrgfx\\infra.trn");
    out.insert("plrgfx\\stone.trn");
    out.insert("gendata\\pause.trn");
}

// Merge the static harvest dictionary (one candidate per line) into `out`.
static void mergeHarvest(std::set<std::string>& out, const fs::path& harvest) {
    std::ifstream f(harvest);
    if (!f)
        return;
    std::string line;
    while (std::getline(f, line)) {
        while (!line.empty() &&
               (line.back() == '\n' || line.back() == '\r'))
            line.pop_back();
        if (!line.empty())
            out.insert(line);
    }
}

// ---------------------------------------------------------------------------
// Archive list + recovery
// ---------------------------------------------------------------------------

struct ArchiveSpec {
    std::string name; // e.g. "DIABDAT.MPQ"
    std::string path; // full filesystem path
};

static std::string nowISO() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);
    return std::string(buf);
}

int main(int argc, char** argv) {
    fs::path devRoot = (argc > 1) ? argv[1]
                                  : fs::path("D:/work/xt/diablo/DevilutionX");
    fs::path mpqRoot = (argc > 2)
                           ? argv[2]
                           : fs::path("D:/work/xt/diablo/DevilutionX-Hell Fire");

    fs::path projectRoot = fs::current_path();
    fs::path dictDir = projectRoot / "mpq-info" / "dict";
    fs::path recoveredDir = dictDir / "recovered";
    std::error_code ec;
    fs::create_directories(recoveredDir, ec);

    // --- Generate candidates from splicing formulas ---
    std::set<std::string> candidates;
    fs::path classesRoot = devRoot / "assets" / "txtdata" / "classes";
    genPlayers(candidates, classesRoot);

    std::vector<fs::path> monFiles = {
        devRoot / "assets" / "txtdata" / "monsters" / "monstdat.tsv",
        devRoot / "mods" / "hf" / "txtdata" / "monsters" / "monstdat.tsv",
    };
    genMonsters(candidates, monFiles);

    std::vector<fs::path> misFiles = {
        devRoot / "assets" / "txtdata" / "missiles" / "missile_sprites.tsv",
        devRoot / "mods" / "hf" / "txtdata" / "missiles" / "missile_sprites.tsv",
    };
    genMissiles(candidates, misFiles);

    // Item drop / HUD sprites (fixed game constant from items.cpp).
    genItems(candidates);

    // Object sprites (objdat.tsv "file" column).
    std::vector<fs::path> objFiles = {
        devRoot / "assets" / "txtdata" / "objects" / "objdat.tsv",
    };
    genObjects(candidates, objFiles);

    // Towner (NPC) sprites (towners.tsv "animPath" column + hardcoded variants).
    std::vector<fs::path> twnFiles = {
        devRoot / "assets" / "txtdata" / "towners" / "towners.tsv",
        devRoot / "mods" / "hf" / "txtdata" / "towners" / "towners.tsv",
    };
    genTowners(candidates, twnFiles);

    // TRN palette files: font TRNs (fixed list) + class/global TRNs.
    genFontTrn(candidates);
    genClassTrn(candidates, classesRoot);

    // --- Merge static harvest dictionary ---
    mergeHarvest(candidates, dictDir / "diablo1-harvest.txt");

    fprintf(stderr,
            "[recover_phase0] devRoot=%s\n"
            "[recover_phase0] mpqRoot=%s\n"
            "[recover_phase0] total candidate names = %zu\n",
            devRoot.string().c_str(), mpqRoot.string().c_str(),
            candidates.size());

    // --- Archive list ---
    const char* archiveNames[] = {
        "DIABDAT.MPQ", "hellfire.mpq", "hfmonk.mpq", "hfvoice.mpq",
        "hfopt1.mpq",  "hfopt2.mpq",   "devilutionx.mpq", "fonts.mpq",
        "spawn.mpq",   "hfmusic.mpq",  "es.mpq", "pl.mpq", "ru.mpq"};
    std::vector<ArchiveSpec> archives;
    for (const char* n : archiveNames)
        archives.push_back({n, (mpqRoot / n).string()});

    // --- Recover ---
    json jArchives = json::array();
    unsigned grandTotal = 0;

    for (const auto& spec : archives) {
        engine::Archive a;
        // Strip a trailing ".mpq"/".MPQ" so outputs match the prior
        // "<BASENAME>.txt" / "<BASENAME>.err" convention (e.g. "DIABDAT.txt").
        std::string safeName = spec.name;
        auto dot = safeName.rfind(".mpq");
        if (dot != std::string::npos && dot + 4 == safeName.size())
            safeName = safeName.substr(0, dot);
        dot = safeName.rfind(".MPQ");
        if (dot != std::string::npos && dot + 4 == safeName.size())
            safeName = safeName.substr(0, dot);
        for (char& c : safeName)
            if (c == '\\' || c == '/' || c == ':') c = '_';
        fs::path outTxt = recoveredDir / (safeName + ".txt");
        fs::path outErr = recoveredDir / (safeName + ".err");

        json jArch;
        jArch["name"] = spec.name;
        jArch["path"] = spec.path;

        if (!a.open(spec.path)) {
            std::string msg = "cannot open archive (missing or not a Diablo MPQ)";
            std::ofstream e(outErr);
            e << msg << "\n";
            jArch["totalFiles"] = 0;
            jArch["recovered"] = json::array();
            jArchives.push_back(jArch);
            fprintf(stderr, "[recover_phase0] %-16s SKIP (%s)\n",
                    spec.name.c_str(), msg.c_str());
            continue;
        }

        auto entries = a.list();
        uint64_t totalFiles = entries.size();
        jArch["totalFiles"] = totalFiles;

        std::vector<std::pair<std::string, uint64_t>> found;
        unsigned checked = 0;
        for (const auto& name : candidates) {
            checked++;
            uint64_t sz = 0;
            if (a.verify(name) && a.querySize(name, sz))
                found.emplace_back(name, sz);
        }

        // Sort by name for a stable, diff-friendly output.
        std::sort(found.begin(), found.end(),
                  [](const auto& x, const auto& y) { return x.first < y.first; });

        std::ofstream o(outTxt);
        json jRec = json::array();
        for (const auto& [n, sz] : found) {
            o << n << '\t' << sz << '\n';
            jRec.push_back({{"name", n}, {"size", sz}});
        }
        o.close();
        if (fs::exists(outErr, ec))
            fs::remove(outErr, ec);

        jArch["recovered"] = jRec;
        jArchives.push_back(jArch);
        grandTotal += (unsigned)found.size();

        fprintf(stderr,
                "[recover_phase0] %-16s total=%5llu  checked=%zu  found=%zu\n",
                spec.name.c_str(), (unsigned long long)totalFiles,
                candidates.size(), found.size());
    }

    // --- Write aggregate JSON ---
    json out;
    out["generated"] = nowISO();
    out["engine"] = "StormLib (src/engine, mpq-info/recover_phase0.cpp)";
    out["dictionary"] =
        "diablo1-harvest.txt + formula-generated candidates "
        "(plrgfx/monsters/missiles)";
    out["candidateCount"] = (unsigned)candidates.size();
    out["archives"] = jArchives;

    fs::path jsonPath = dictDir / "recovered.json";
    std::ofstream jf(jsonPath);
    jf << out.dump(2) << "\n";

    fprintf(stderr,
            "[recover_phase0] DONE. grandTotal recovered across archives = %u\n"
            "[recover_phase0] wrote %s\n",
            grandTotal, jsonPath.string().c_str());
    return 0;
}
