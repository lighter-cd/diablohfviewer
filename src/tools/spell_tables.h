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

// Strip surrounding quotes and unescape gettext escape sequences ("\"", "\\",
// "\n", "\t"). Used when reading DevilutionX .po translation files.
inline std::string poUnquote(const std::string& s)
{
    std::string t = trim(s);
    if (t.size() >= 2 && t.front() == '"' && t.back() == '"')
        t = t.substr(1, t.size() - 2);
    std::string out;
    out.reserve(t.size());
    for (size_t i = 0; i < t.size(); ++i) {
        if (t[i] == '\\' && i + 1 < t.size()) {
            char n = t[i + 1];
            if (n == 'n') out += '\n';
            else if (n == 't') out += '\t';
            else if (n == '"') out += '"';
            else if (n == '\\') out += '\\';
            else { out += '\\'; out += n; }
            ++i;
        } else {
            out += t[i];
        }
    }
    // Drop Unicode format/zero-width characters the translation files sometimes
    // contain between characters (e.g. U+200B between 火 and 栓), which would
    // otherwise silently corrupt display names. Walk UTF-8 code points properly.
    std::string cleaned;
    cleaned.reserve(out.size());
    size_t i = 0;
    while (i < out.size()) {
        unsigned char c = static_cast<unsigned char>(out[i]);
        char32_t cp; int len;
        if (c < 0x80) { cp = c; len = 1; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; len = 2; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; len = 3; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; len = 4; }
        else { cleaned.push_back(out[i]); ++i; continue; }
        if (i + len > out.size()) { cleaned.push_back(out[i]); ++i; continue; }
        for (int k = 1; k < len; ++k)
            cp = (cp << 6) | (static_cast<unsigned char>(out[i + k]) & 0x3F);
        if (cp == 0x200B || cp == 0x200C || cp == 0x200D || cp == 0x2060 ||
            cp == 0xFEFF || cp == 0x00AD) { /* skip zero-width / format chars */ }
        else for (int k = 0; k < len; ++k) cleaned.push_back(out[i + k]);
        i += len;
    }
    return cleaned;
}

// Load spell display names from DevilutionX gettext translations. Spell names
// live under `msgctxt "spell"` in Translations/<lang>.po (zh_TW preferred, then
// zh_CN). A few entries are .po bugs that reuse a format string as the name
// (e.g. Fireball -> "火球傷害: {:d}"); we skip any translation containing a
// "{:" placeholder so the English name survives as fallback.
inline std::map<std::string, std::string> loadSpellZhNames(
    const std::filesystem::path& devRoot)
{
    std::map<std::string, std::string> out;
    static const char* const kLocales[] = { "zh_TW.po", "zh_CN.po" };
    for (const char* fn : kLocales) {
        std::ifstream f(devRoot / "Translations" / fn);
        if (!f) continue;
        std::string line;
        std::string ctx, idBuf, strBuf;
        bool inMsgid = false;
        std::string* cur = nullptr;
        auto flush = [&]() {
            if (inMsgid && ctx == "spell" && !idBuf.empty() &&
                strBuf.find("{:") == std::string::npos) {
                if (out.find(idBuf) == out.end()) out[idBuf] = strBuf;
            }
            inMsgid = false;
            cur = nullptr;
            idBuf.clear();
            strBuf.clear();
        };
        while (std::getline(f, line)) {
            std::string tl = trim(line);
            if (tl.empty() || tl[0] == '#' || tl[0] == '\0') { flush(); ctx.clear(); continue; }
            if (tl.rfind("msgctxt \"", 0) == 0) { flush(); ctx = poUnquote(tl.substr(8)); continue; }
            if (tl.rfind("msgid ", 0) == 0 || tl.rfind("msgid\"", 0) == 0) {
                flush(); inMsgid = true; idBuf = poUnquote(tl.substr(6)); cur = &idBuf; continue;
            }
            if (tl.rfind("msgid_p", 0) == 0) { flush(); ctx.clear(); continue; } // skip plural
            if (tl.rfind("msgstr ", 0) == 0 || tl.rfind("msgstr\"", 0) == 0) {
                strBuf = poUnquote(tl.substr(7)); cur = &strBuf; flush(); continue;
            }
            if (cur) *cur += poUnquote(tl); // continuation quoted line
        }
    }
    return out;
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

inline std::string elementZh(const std::string& element)
{
    if (element == "Fire") return "火";
    if (element == "Lightning") return "雷";
    if (element == "Acid") return "酸";
    if (element == "Physical") return "物理";
    return "魔法";
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
    std::string name_zh;     // Chinese display name (from zh_TW/zh_CN .po; falls back to `name`)
    std::string description;  // synthesized Chinese description of the effect chain
    std::string source;      // player | monster
    std::string element;
    std::string cast_action;
    std::string monster_ai;
    std::vector<std::string> missiles;
    std::string mana_cost;
    std::string min_intelligence;
    std::string level;            // sBookLvl: spell tier / "法术级别"（法术书要求的角色等级）
    std::string staff_level;      // sStaffLvl
    std::string mana_multiplier;  // sManaAdj: 每升 1 级训练减少的法力
    std::string min_mana;         // sMinMana: 法力消耗下限
    std::string staff_min;        // sStaffMin
    std::string staff_max;        // sStaffMax
    std::vector<std::pair<int,int>> mana_by_level; // (level, base mana cost) for levels 1..bookLevel
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

// Build a Chinese description from the resolved effect chain and (player) stats.
// Graphic ids and MissileIDs are kept in English (no Chinese asset names exist in
// the data), but the structure and element words are Chinese.
inline std::string synthDescription(const Entry& e)
{
    std::string proj, imp, deb;
    for (const auto& s : e.stages) {
        for (const auto& l : s.layers) {
            if (l.role == "projectile" && l.kind == "sprite") {
                if (!proj.empty()) proj += "、";
                proj += l.graphic_id;
            } else if (l.role == "impact" && l.kind == "sprite") {
                if (!imp.empty()) imp += "、";
                imp += l.graphic_id;
            } else if (l.role == "debuff") {
                if (!deb.empty()) deb += "、";
                deb += (l.kind == "trn") ? (l.trn_preset + " TRN") : l.graphic_id;
            }
        }
    }
    if (e.source == "monster") {
        return e.monster_ai + " 远程攻击：发射 " +
               (e.missiles.empty() ? "?" : e.missiles[0]) + " 导弹。";
    }
    std::string d = elementZh(e.element) + "系法术。";
    if (!e.mana_cost.empty()) d += "法力消耗 " + e.mana_cost + "，";
    if (!e.min_intelligence.empty()) d += "最低智力 " + e.min_intelligence + "。";
    if (!e.level.empty()) {
        d += "法术等级 " + e.level;
        if (!e.mana_multiplier.empty() && e.mana_multiplier != "0")
            d += "（每升 1 级法力 -" + e.mana_multiplier + "，最低 " + e.min_mana + "）";
        d += "。";
    }
    d += "投射物：" + (proj.empty() ? "无" : proj) +
         "；命中：" + (imp.empty() ? "无" : imp) +
         "；状态：" + (deb.empty() ? "无" : deb) + "。";
    return d;
}

inline Entry deriveEntry(const Row& spell,
                         const std::map<std::string, Row>& misById,
                         const std::map<std::string, SpriteInfo>& sprById,
                         const std::string& source,
                         const std::map<std::string, std::string>* zhNames = nullptr)
{
    Entry e;
    e.source = source;
    e.id = trim(val(spell, "id"));
    e.name = trim(val(spell, "name"));
    if (e.id.empty()) e.id = e.name;
    if (e.name.empty()) e.name = e.id;
    e.element = firstElement(val(spell, "flags"));
    e.cast_action = elementToAction(e.element);
    e.mana_cost = trim(val(spell, "manaCost"));
    e.min_intelligence = trim(val(spell, "minIntelligence"));
    e.level = trim(val(spell, "bookLevel"));
    e.staff_level = trim(val(spell, "staffLevel"));
    e.mana_multiplier = trim(val(spell, "manaMultiplier"));
    e.min_mana = trim(val(spell, "minMana"));
    e.staff_min = trim(val(spell, "staffMin"));
    e.staff_max = trim(val(spell, "staffMax"));

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

    if (zhNames) {
        auto it = zhNames->find(e.name);
        if (it == zhNames->end()) it = zhNames->find(e.id);
        if (it != zhNames->end()) e.name_zh = it->second;
    }
    if (e.name_zh.empty()) e.name_zh = e.name;

    // Per-trained-level mana cost (base, before class multiplier). Mirrors
    // Source/spells.cpp::GetManaAmount: cost = max(manaCost - (lvl-1)*manaAdj, 0),
    // floored at minMana. Firebolt halves the adjustment (game special-case).
    if (source == "player") {
        const int base = std::atoi(e.mana_cost.c_str());
        const int adj = std::atoi(e.mana_multiplier.c_str());
        const int minM = std::atoi(e.min_mana.c_str());
        int maxLvl = std::atoi(e.level.c_str());
        if (maxLvl < 1) maxLvl = 1;
        const int halve = (e.id == "Firebolt" || e.name == "Firebolt") ? 2 : 1;
        for (int lvl = 1; lvl <= maxLvl; ++lvl) {
            int cost = base - (lvl - 1) * adj / halve;
            if (cost < 0) cost = 0;
            if (minM > 0 && cost < minM) cost = minM;
            e.mana_by_level.emplace_back(lvl, cost);
        }
    }

    e.description = synthDescription(e);
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

    auto zh = loadSpellZhNames(devRoot);

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
        c.spells.push_back(deriveEntry(r, misById, sprById, "player", &zh));
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
        Entry e = deriveEntry(pseudo, misById, sprById, "monster", &zh);
        e.monster_ai = kv.first;
        e.monsters = kv.second;
        e.default_ai = def;
        c.monster_attacks.push_back(std::move(e));
    }

    c.ok = true;
    return c;
}

} // namespace spell_tables
