// src/tools/spell_tables.h
//
// Header-only, dependency-free (no nlohmann/json) helpers that join the
// DevilutionX data tables into a "spell / monster attack -> visual stages"
// catalog. sprite_tools.cpp turns the returned structs into JSON for the
// `spell.list` MCP tool; out/unit_tests.cpp unit-tests the pure functions here
// without needing any MPQ or TSV files on disk.

#pragma once

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace spell_tables {

using Row = std::map<std::string, std::string>;
using Table = std::vector<Row>;

inline std::string trim(std::string s)
{
    const size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    const size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

inline const std::string& val(const Row& r, const std::string& k)
{
    static const std::string kEmpty;
    auto it = r.find(k);
    return it == r.end() ? kEmpty : it->second;
}

inline std::vector<std::string> splitLine(const std::string& s, char sep)
{
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == sep || c == '\r' || c == '\n') {
            out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    out.push_back(cur);
    return out;
}

inline std::vector<std::string> splitComma(const std::string& s)
{
    return splitLine(s, ',');
}

// Parse a header TSV from an in-memory string (used by unit tests).
inline Table parseTsvText(const std::string& text)
{
    Table out;
    auto lines = splitLine(text, '\n');
    if (lines.empty()) return out;
    auto header = splitLine(lines[0], '\t');
    for (auto& h : header) h = trim(h);
    for (size_t i = 1; i < lines.size(); ++i) {
        if (trim(lines[i]).empty()) continue;
        auto cells = splitLine(lines[i], '\t');
        Row r;
        for (size_t j = 0; j < header.size() && j < cells.size(); ++j)
            r[header[j]] = trim(cells[j]);
        out.push_back(std::move(r));
    }
    return out;
}

inline Table parseTsvTable(const std::filesystem::path& p)
{
    std::ifstream f(p);
    if (!f) return {};
    const std::string text((std::istreambuf_iterator<char>(f)),
                           std::istreambuf_iterator<char>());
    return parseTsvText(text);
}

// base rows win; extra rows with a new key are appended (Hellfire additions).
inline Table mergeByKey(Table base, const Table& extra, const std::string& key)
{
    std::set<std::string> seen;
    for (const auto& r : base) {
        auto k = trim(val(r, key));
        if (!k.empty()) seen.insert(k);
    }
    for (const auto& r : extra) {
        auto k = trim(val(r, key));
        if (k.empty() || seen.count(k)) continue;
        base.push_back(r);
        seen.insert(k);
    }
    return base;
}

inline std::string firstElement(const std::string& flags)
{
    if (flags.find("Fire") != std::string::npos) return "Fire";
    if (flags.find("Lightning") != std::string::npos) return "Lightning";
    if (flags.find("Acid") != std::string::npos) return "Acid";
    if (flags.find("Physical") != std::string::npos) return "Physical";
    return "Magic";
}

// Fire -> fm, Lightning -> lm, everything else -> qm (Magic action).
inline std::string elementToAction(const std::string& element)
{
    if (element == "Fire") return "fm";
    if (element == "Lightning") return "lm";
    return "qm";
}

struct SpriteInfo {
    std::string id;
    std::string base_name;
    int width = 0;
    int num_files = 0;   // missile_sprites.numFrames = number of numbered .cl2 files
    int frames = 0;      // frameLength[0] = frames inside each numbered file
    std::vector<std::string> files;
    bool valid() const { return width > 0 && !files.empty(); }
};

// numFrames>1 -> missiles\{name}{N}.cl2 (N=1..numFrames)
// numFrames==1 -> missiles\{name}.cl2 (no numeric suffix)
inline SpriteInfo spriteFilesFor(const Row& r)
{
    SpriteInfo s;
    s.id = trim(val(r, "id"));
    s.base_name = trim(val(r, "name"));
    s.width = std::atoi(val(r, "width").c_str());
    s.num_files = std::atoi(val(r, "numFrames").c_str());
    auto fl = splitComma(val(r, "frameLength"));
    s.frames = fl.empty() ? 0 : std::atoi(fl[0].c_str());
    if (s.base_name.empty() || s.base_name == "?" || s.width <= 0) return s;
    if (s.num_files > 1) {
        for (int i = 1; i <= s.num_files; ++i)
            s.files.push_back("missiles\\" + s.base_name + std::to_string(i) + ".cl2");
    } else {
        s.files.push_back("missiles\\" + s.base_name + ".cl2");
    }
    return s;
}

// Transcribed from DevilutionX Source/monster.cpp::GetMissileType().
inline const std::map<std::string, std::string>& aiMissileTable()
{
    static const std::map<std::string, std::string> k = {
        {"GoatRanged", "Arrow"}, {"Succubus", "BloodStar"},
        {"LazarusSuccubus", "BloodStar"}, {"Acid", "Acid"},
        {"AcidUnique", "Acid"}, {"FireBat", "Firebolt"},
        {"Torchant", "Fireball"}, {"Lich", "OrangeFlare"},
        {"ArchLich", "YellowFlare"}, {"Psychorb", "BlueFlare"},
        {"Necromorb", "RedFlare"}, {"Magma", "MagmaBall"},
        {"Storm", "ThinLightningControl"}, {"Diablo", "DiabloApocalypse"},
        {"BoneDemon", "BlueFlare2"},
    };
    return k;
}

inline std::string aiToMissile(const std::string& ai, bool& isDefault)
{
    auto it = aiMissileTable().find(ai);
    if (it != aiMissileTable().end()) {
        isDefault = false;
        return it->second;
    }
    // Includes SkeletonRanged, FireMan, HorkDemon, ... (no explicit case).
    isDefault = true;
    return "Arrow";
}

inline const std::set<std::string>& rangedAiSet()
{
    static const std::set<std::string> k = [] {
        std::set<std::string> s;
        for (const auto& kv : aiMissileTable()) s.insert(kv.first);
        s.insert("SkeletonRanged"); // falls through GetMissileType default -> Arrow
        return s;
    }();
    return k;
}

inline bool isImpactGraphic(const std::string& id)
{
    static const char* kws[] = {"Explosion", "Boom", "Splat", "Shatter", "Hit", "Puddle", "Beam"};
    for (auto k : kws)
        if (id.find(k) != std::string::npos) return true;
    return false;
}

// Impact/boom graphics have no data-table foreign key (they are spawned by
// AddMissile(...) inside DevilutionX's Process* functions). Auto-associate by
// naming convention (graphic + "Explosion") and cover the rest here.
inline std::string impactOverride(const std::string& missileOrGraphic)
{
    static const std::map<std::string, std::string> k = {
        {"Fireball", "MagmaBallExplosion"}, {"Firebolt", "MagmaBallExplosion"},
        {"Apocalypse", "ApocalypseBoom"}, {"DiabloApocalypse", "DiabloApocalypseBoom"},
        {"Acid", "AcidSplat"}, {"StoneCurse", "StoneCurseShatter"},
        {"Inferno", "BigExplosion"}, {"FlameWave", "BigExplosion"},
    };
    auto it = k.find(missileOrGraphic);
    return it == k.end() ? "" : it->second;
}

inline bool isStatusGraphic(const std::string& id)
{
    static const char* kws[] = {"Shield", "Curse", "Rune", "Portal", "Reflect",
                               "Healing", "Resurrect", "Infravision", "Phasing",
                               "Etherealize", "Repair", "Recharge", "Disarm",
                               "Identify", "Telekinesis", "Beam", "Golem"};
    for (auto k : kws)
        if (id.find(k) != std::string::npos) return true;
    return false;
}

struct Layer {
    std::string kind;        // cast_action | sprite | trn
    std::string role;        // caster | projectile | impact | debuff
    std::string anchor;      // player | monster | world
    std::string action;      // cast_action: fm / lm / qm
    std::string element;
    std::string graphic_id;
    std::vector<std::string> files;
    int width = 0;
    int frames = 0;
    int num_files = 0;
    std::string trn_name;
    std::string trn_preset;
    std::string confidence;  // auto | override | catalog
};

struct Stage {
    std::string id;          // cast | projectile | impact | debuff
    std::string label;
    std::vector<Layer> layers;
};

struct Entry {
    std::string id;
    std::string name;
    std::string source;      // player | monster
    std::string element;
    std::string cast_action;
    std::string monster_ai;
    std::vector<std::string> missiles;
    std::vector<std::string> monsters;
    bool default_ai = false;
    std::vector<Stage> stages;
};

struct Catalog {
    std::vector<Entry> spells;
    std::vector<Entry> monster_attacks;
    std::vector<SpriteInfo> impact_catalog;
    bool ok = false;
    std::string error;
};

inline Layer makeSpriteLayer(const SpriteInfo& si, const std::string& role,
                             const std::string& anchor, const std::string& confidence,
                             const std::string& element)
{
    Layer l;
    l.kind = "sprite";
    l.role = role;
    l.anchor = anchor;
    l.confidence = confidence;
    l.graphic_id = si.id;
    l.files = si.files;
    l.width = si.width;
    l.frames = si.frames;
    l.num_files = si.num_files;
    l.element = element;
    return l;
}

inline Layer makeTrnLayer(const std::string& name, const std::string& preset)
{
    Layer l;
    l.kind = "trn";
    l.role = "debuff";
    l.anchor = "monster";
    l.confidence = "override";
    l.trn_name = name;
    l.trn_preset = preset;
    return l;
}

inline Entry deriveEntry(const Row& spell,
                         const std::map<std::string, Row>& misById,
                         const std::map<std::string, SpriteInfo>& sprById,
                         const std::string& source)
{
    Entry e;
    e.source = source;
    e.id = trim(val(spell, "id"));
    e.name = trim(val(spell, "name"));
    if (e.id.empty()) e.id = e.name;
    if (e.name.empty()) e.name = e.id;
    e.element = firstElement(val(spell, "flags"));
    e.cast_action = elementToAction(e.element);

    for (const auto& t : splitComma(val(spell, "missiles"))) {
        auto s = trim(t);
        if (!s.empty()) e.missiles.push_back(s);
    }

    Stage cast{"cast", "施法", {}};
    Layer cl;
    cl.kind = "cast_action";
    cl.role = "caster";
    cl.anchor = (source == "monster") ? "monster" : "player";
    cl.action = e.cast_action;
    cl.element = e.element;
    cl.confidence = "auto";
    cast.layers.push_back(cl);
    e.stages.push_back(std::move(cast));

    Stage proj{"projectile", "投射", {}};
    Stage impact{"impact", "命中", {}};
    Stage debuff{"debuff", "状态", {}};

    for (const auto& mid : e.missiles) {
        auto mi = misById.find(mid);
        if (mi == misById.end()) continue;
        const Row& mr = mi->second;
        const std::string graphic = trim(val(mr, "graphic"));
        const std::string elem = firstElement(val(mr, "flags"));

        if (!graphic.empty()) {
            auto si = sprById.find(graphic);
            if (si != sprById.end() && si->second.valid()) {
                if (isStatusGraphic(graphic))
                    debuff.layers.push_back(
                        makeSpriteLayer(si->second, "debuff", "monster", "auto", elem));
                else
                    proj.layers.push_back(
                        makeSpriteLayer(si->second, "projectile", "world", "auto", elem));
            }
        }

        std::string ig = impactOverride(mid);
        if (ig.empty()) ig = impactOverride(graphic);
        bool overridden = !ig.empty();
        if (ig.empty() && !graphic.empty()) {
            std::string guess = graphic + "Explosion";
            if (sprById.count(guess)) ig = guess;
        }
        if (!ig.empty()) {
            auto si = sprById.find(ig);
            if (si != sprById.end() && si->second.valid())
                impact.layers.push_back(makeSpriteLayer(
                    si->second, "impact", "monster",
                    overridden ? "override" : "auto", elem));
        }
    }

    // Debuff TRN remaps (approximate tint on the monster layer; infra.trn /
    // stone.trn are player TRNs but apply_trn is palette-wide).
    if (e.id == "Infravision" || e.name == "Infravision")
        debuff.layers.push_back(makeTrnLayer("plrgfx\\infra.trn", "infra"));
    else if (e.id == "StoneCurse" || e.name == "Stone Curse")
        debuff.layers.push_back(makeTrnLayer("plrgfx\\stone.trn", "stone"));

    e.stages.push_back(std::move(proj));
    e.stages.push_back(std::move(impact));
    e.stages.push_back(std::move(debuff));
    return e;
}

inline Catalog buildCatalog(const std::filesystem::path& devRoot)
{
    Catalog c;
    auto load = [&](const char* rel) {
        Table base = parseTsvTable(devRoot / "assets" / "txtdata" / rel);
        Table hf = parseTsvTable(devRoot / "mods" / "hf" / "txtdata" / rel);
        return mergeByKey(std::move(base), hf, "id");
    };
    const Table spells = load("spells/spelldat.tsv");
    const Table mis = load("missiles/misdat.tsv");
    const Table spr = load("missiles/missile_sprites.tsv");
    Table mon = mergeByKey(
        parseTsvTable(devRoot / "assets" / "txtdata" / "monsters" / "monstdat.tsv"),
        parseTsvTable(devRoot / "mods" / "hf" / "txtdata" / "monsters" / "monstdat.tsv"),
        "_monster_id");

    if (spells.empty() || mis.empty() || spr.empty()) {
        c.error = "missing DevilutionX data tables (spells/missiles)";
        return c;
    }

    std::map<std::string, Row> misById;
    for (const auto& r : mis) {
        auto k = trim(val(r, "id"));
        if (!k.empty() && !misById.count(k)) misById[k] = r;
    }
    std::map<std::string, SpriteInfo> sprById;
    for (const auto& r : spr) {
        auto k = trim(val(r, "id"));
        if (!k.empty() && !sprById.count(k)) sprById[k] = spriteFilesFor(r);
    }

    for (const auto& r : spr) {
        if (isImpactGraphic(trim(val(r, "id")))) {
            auto si = spriteFilesFor(r);
            if (si.valid()) c.impact_catalog.push_back(std::move(si));
        }
    }

    for (const auto& r : spells) {
        if (trim(val(r, "name")).empty()) continue;
        c.spells.push_back(deriveEntry(r, misById, sprById, "player"));
    }

    std::map<std::string, std::vector<std::string>> byAi;
    for (const auto& r : mon) {
        auto ai = trim(val(r, "ai"));
        if (!rangedAiSet().count(ai)) continue;
        auto nm = trim(val(r, "name"));
        if (!nm.empty()) byAi[ai].push_back(nm);
    }
    for (const auto& kv : byAi) {
        bool def = false;
        Row pseudo;
        pseudo["id"] = kv.first;
        pseudo["name"] = kv.first;
        pseudo["flags"] = "";
        pseudo["missiles"] = aiToMissile(kv.first, def);
        Entry e = deriveEntry(pseudo, misById, sprById, "monster");
        e.monster_ai = kv.first;
        e.monsters = kv.second;
        e.default_ai = def;
        c.monster_attacks.push_back(std::move(e));
    }

    c.ok = true;
    return c;
}

} // namespace spell_tables
