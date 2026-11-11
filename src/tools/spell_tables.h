// src/tools/spell_tables.h
//
// Header-only, dependency-free (no nlohmann/json) helpers that join the
// DevilutionX data tables into a "spell / monster attack -> visual stages"
// catalog. sprite_tools.cpp turns the returned structs into JSON for the
// `spell.list` MCP tool; tests/unit_tests.cpp unit-tests the pure functions here
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

#include "tools/spell_visual_model.h"

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

// Attacks that never go through GetMissileType(): their AI function calls
// AddMissile / StartRangedAttack itself, so the missile depends on the monster's
// intelligence (Counselor) or on the exact monster id (Familiar, Gloom), not on
// the AI. Transcribed from DevilutionX Source/monster.cpp:
//   CounselorAi(2728)  intel 0..3 -> Firebolt / ChargedBolt / LightningControl /
//                      Fireball; plus FlashBottom+FlashTop at close range.
//   BatAi(2436)        MT_FAMILIAR -> Lightning; MT_GLOOM -> Rhino charge.
//   MegaAi(2822)       InfernoControl (near-range breath).
//   HorkDemonAi(3013)  HorkSpawn (summon; only reachable through the unique
//                      monster row, whose ai overrides MT_HORKDMN's SkeletonMelee).
// Each form becomes its own catalog entry so every missile keeps its own
// projectile stage; 'condition' records why/when the game picks it.
struct AttackForm {
    std::string ai;
    std::string variant;                    // "" when the AI has a single form
    std::string missiles;                   // comma separated MissileIDs
    std::string condition;                  // Chinese trigger note
    int min_intelligence = -1;              // -1 = ignore
    int max_intelligence = -1;
    std::vector<std::string> only_monsters;  // monster ids (_monster_id); empty = any
};

inline const std::vector<AttackForm>& monsterAttackForms()
{
    static const std::vector<AttackForm> k = {
        {"Counselor", "Firebolt", "Firebolt", "智力 0 的顾问（Counselor）远程弹", 0, 0, {}},
        {"Counselor", "ChargedBolt", "ChargedBolt", "智力 1 的顾问（Magistrate）远程弹", 1, 1, {}},
        {"Counselor", "Lightning", "LightningControl", "智力 2 的顾问（Cabalist）远程弹", 2, 2, {}},
        {"Counselor", "Fireball", "Fireball", "智力 3 的顾问（Advocate / Arch-Litch Malignus）远程弹", 3, 3, {}},
        {"Counselor", "Flash", "FlashBottom,FlashTop", "近身（距离<2）时施放，双层闪光", -1, -1, {}},
        {"Bat", "Lightning", "Lightning", "仅 Familiar（MT_FAMILIAR）在撤退时放电", -1, -1, {"MT_FAMILIAR"}},
        {"Bat", "Charge", "Rhino", "仅 Gloom（MT_GLOOM）远距离冲撞（物理，非法术）", -1, -1, {"MT_GLOOM"}},
        {"Mega", "Inferno", "InfernoControl", "Slayer/Guardian/Vortex Lord/Balrog 近身喷吐", -1, -1, {}},
        {"HorkDemon", "HorkSpawn", "HorkSpawn", "召唤 Hork Spawn（落点需为空地；导弹本身无图形）", -1, -1, {}},
    };
    return k;
}

// True when this monster (id + intelligence) uses the given attack form.
inline bool attackFormMatches(const AttackForm& f, const std::string& monsterId, int intelligence)
{
    if (f.min_intelligence >= 0 && intelligence < f.min_intelligence) return false;
    if (f.max_intelligence >= 0 && intelligence > f.max_intelligence) return false;
    if (!f.only_monsters.empty() &&
        std::find(f.only_monsters.begin(), f.only_monsters.end(), monsterId) ==
            f.only_monsters.end())
        return false;
    return true;
}

// Unique monsters can override the base monster's AI (unique_monstdat.tsv); a
// few of those AIs just delegate to another AI function, so they must be folded
// onto the AI they actually run (Source/monster.cpp:2817 ZharAi -> CounselorAi,
// 2926 LazarusAi -> CounselorAi).
inline std::string normalizedMonsterAi(const std::string& ai)
{
    if (ai == "Zhar" || ai == "Lazarus") return "Counselor";
    return ai;
}

// Monster abilities the engine implements directly (no dedicated missile
// attack). Diablo 1 / Hellfire have no Diablo-2-style champion affixes: there is
// no Thunderstorm / Waller / Vortex / Shielding anywhere in Source/, so the only
// "affix-like" behaviours are the ones below (see docs/SPELLS.md).
struct AbilitySpec {
    std::string id;                          // stable id in the catalog
    std::string name;                        // engine / AI name
    std::string name_zh;                     // Chinese display name
    std::string description;                 // one-line Chinese effect
    std::string trigger;                     // when the engine fires it
    std::string element;                     // cast-action element
    std::string missiles;                    // optional MissileID chain
    std::string monster_ai;                  // select monsters by (normalized) AI
    std::vector<std::string> monster_ids;    // and/or by _monster_id
    std::string source_ref;                  // DevilutionX source location
};

inline const std::vector<AbilitySpec>& monsterAbilities()
{
    static const std::vector<AbilitySpec> k = {
        {"Blink", "Blink", "瞬移",
         "受到重击时瞬移到敌人身边，只改变位置、没有特效图像。",
         "IsHardHit 判定成立时（M_StartHit → Teleport）", "Magic", "", "", {"MT_BLINK"},
         "Source/monster.cpp:969 Teleport(), 3974-3976 M_StartHit"},
        {"AcidDeathPuddle", "Acid", "死亡酸池",
         "酸液系怪物死亡时在尸体处留下酸液池，持续造成伤害。",
         "酸液系怪物死亡时（AddMissile(AcidPuddle)）", "Acid", "AcidPuddle", "",
         {"MT_NACID", "MT_RACID", "MT_BACID", "MT_XACID", "MT_SPIDLORD"},
         "Source/monster.cpp:4039; Source/missiles.cpp AddAcidPuddle"},
        {"Hidden", "Sneak", "隐身",
         "潜行系怪物在远处保持隐身，靠近或交战时才显形。",
         "与玩家距离超过判定值时（SneakAi 设置 MFLAG_HIDDEN）", "Magic", "", "Sneak", {},
         "Source/monster.cpp:2558-2561 SneakAi"},
    };
    return k;
}

// Trap spell effects. Transcribed from DevilutionX Source/objects.cpp:
//   AddTrap(1230)      OBJ_TRAPL/OBJ_TRAPR wall traps: missile is rolled from
//                      {Arrow, Firebolt, LightningControl} with
//                      GenerateRnd((effectiveLevel / 3) + 1), so Arrow is always
//                      possible, Firebolt needs level >= 3, Lightning >= 6.
//   OperateChest(2043) trapped chests (OBJ_TCHEST1..3) cast Arrow / FireArrow /
//                      Nova (vanilla 0..2) and RingOfFire / StealPotions /
//                      StealMana (Hellfire 0..5).
//   UpdateFlameTrap(1590) Hellfire wall flame holes / flame levers: object
//                      animation + direct damage typed as FireWallControl.
struct TrapSpec {
    std::string id;
    std::string name;                        // MissileID / effect name
    std::string name_zh;
    std::string description;                 // one-line Chinese effect
    std::string trigger;                     // when it fires
    std::string element;                     // cast-action element
    std::string missiles;                    // optional MissileID chain
    std::vector<std::string> objects;        // OBJ_* ids that host it
    std::string source_ref;                  // DevilutionX source location
};

inline const std::vector<TrapSpec>& trapSpecs()
{
    static const std::vector<TrapSpec> k = {
        {"Trap.Arrow", "Arrow", "箭矢陷阱",
         "墙上的机关陷阱向触发者射出一支箭。",
         "门/箱子/拉杆被触发且附近有玩家；任意有效层都会出现", "Physical", "Arrow",
         {"OBJ_TRAPL", "OBJ_TRAPR"}, "Source/objects.cpp:1230 AddTrap, 4196 OperateTrap"},
        {"Trap.Firebolt", "Firebolt", "火焰弹陷阱",
         "墙陷阱改为发射火焰弹（复用火球图形）。",
         "有效层 ≥3（GenerateRnd((effLevel/3)+1) 可摇到 1）", "Fire", "Firebolt",
         {"OBJ_TRAPL", "OBJ_TRAPR"}, "Source/objects.cpp:1230 AddTrap"},
        {"Trap.Lightning", "LightningControl", "闪电陷阱",
         "墙陷阱改为发射爬行闪电。",
         "有效层 ≥6（GenerateRnd((effLevel/3)+1) 可摇到 2）", "Lightning", "LightningControl",
         {"OBJ_TRAPL", "OBJ_TRAPR"}, "Source/objects.cpp:1230 AddTrap"},
        {"TrapChest.Arrow", "Arrow", "宝箱箭矢",
         "打开带陷阱的箱子时向玩家射出箭矢。",
         "宝箱陷阱类型 0（原版与地狱火都有）", "Physical", "Arrow",
         {"OBJ_TCHEST1", "OBJ_TCHEST2", "OBJ_TCHEST3"}, "Source/objects.cpp:2043 OperateChest"},
        {"TrapChest.FireArrow", "FireArrow", "宝箱火焰箭",
         "宝箱陷阱类型 1：射出带火焰的箭。",
         "宝箱陷阱类型 1（原版与地狱火都有）", "Fire", "FireArrow",
         {"OBJ_TCHEST1", "OBJ_TCHEST2", "OBJ_TCHEST3"}, "Source/objects.cpp:2043 OperateChest"},
        {"TrapChest.Nova", "Nova", "宝箱新星",
         "宝箱陷阱类型 2：以玩家为中心爆出环形闪电。",
         "宝箱陷阱类型 2（原版与地狱火都有）", "Lightning", "Nova",
         {"OBJ_TCHEST1", "OBJ_TCHEST2", "OBJ_TCHEST3"}, "Source/objects.cpp:2043 OperateChest"},
        {"TrapChest.RingOfFire", "RingOfFire", "宝箱火焰之环",
         "宝箱陷阱类型 3：在玩家周围生成一圈火焰墙。",
         "宝箱陷阱类型 3（仅地狱火：GenerateRnd(6)）", "Fire", "RingOfFire",
         {"OBJ_TCHEST1", "OBJ_TCHEST2", "OBJ_TCHEST3"}, "Source/objects.cpp:2043 OperateChest"},
        {"TrapChest.StealPotions", "StealPotions", "宝箱偷药水",
         "宝箱陷阱类型 4：偷走玩家一瓶治疗药水（无图形）。",
         "宝箱陷阱类型 4（仅地狱火）", "Physical", "StealPotions",
         {"OBJ_TCHEST1", "OBJ_TCHEST2", "OBJ_TCHEST3"}, "Source/objects.cpp:2043 OperateChest"},
        {"TrapChest.StealMana", "StealMana", "宝箱吸法力",
         "宝箱陷阱类型 5：吸走玩家法力（无图形）。",
         "宝箱陷阱类型 5（仅地狱火）", "Physical", "StealMana",
         {"OBJ_TCHEST1", "OBJ_TCHEST2", "OBJ_TCHEST3"}, "Source/objects.cpp:2043 OperateChest"},
        {"Trap.Flame", "FireWallControl", "墙上喷火口 / 火焰拉杆",
         "地狱火墙上的喷火口沿一条直线喷火；伤害类型按 FireWallControl 记账，"
         "画面是对象自身的火焰动画（无 missile 图形）。",
         "踩到喷火口的感应线（OBJ_FLAMEHOLE）或拉动火焰拉杆（OBJ_FLAMELVR）",
         "Fire", "", {"OBJ_FLAMEHOLE", "OBJ_FLAMELVR"}, "Source/objects.cpp:1590 UpdateFlameTrap, 2163 OperateTrapLever"},
    };
    return k;
}

// True when this monster (normalized AI + id) has the given ability.
inline bool abilityMatches(const AbilitySpec& a, const std::string& ai, const std::string& monsterId)
{
    if (!a.monster_ai.empty() && ai == a.monster_ai) return true;
    return std::find(a.monster_ids.begin(), a.monster_ids.end(), monsterId) !=
           a.monster_ids.end();
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
    std::string variant;          // monster: attack form of a multi-form AI (e.g. "Fireball" / "Flash")
    std::string condition;        // monster: when that form is used (Chinese note)
    bool is_ability = false;      // true for engine-level monster abilities
    bool is_trap = false;         // true for trap spell effects
    std::string trigger;          // ability/trap: when the engine fires it
    std::string source_ref;       // ability/trap: DevilutionX source location
    std::vector<std::string> objects;  // trap: OBJ_* hosting objects
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
    spell_visual::ByLevel visual_by_level;          // (level, {param_key -> value_string}) for levels 1..max
    std::vector<std::string> monsters;
    bool default_ai = false;
    std::vector<Stage> stages;
};

struct Catalog {
    std::vector<Entry> spells;
    std::vector<Entry> monster_attacks;
    std::vector<Entry> abilities;
    std::vector<Entry> traps;
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
        std::string d = e.monster_ai;
        if (!e.variant.empty()) d += " / " + e.variant;
        d += " 远程攻击：发射 ";
        if (e.missiles.empty()) {
            d += "?";
        } else {
            for (size_t i = 0; i < e.missiles.size(); ++i) {
                if (i > 0) d += "、";
                d += e.missiles[i];
            }
        }
        d += " 导弹。";
        if (!e.condition.empty()) d += e.condition + "。";
        return d;
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
                         const std::map<std::string, std::string>* zhNames = nullptr,
                         const std::string& variant = "",
                         const std::string& condition = "")
{
    Entry e;
    e.source = source;
    e.variant = variant;
    e.condition = condition;
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

    e.visual_by_level = spell_visual::computeVisualByLevel(e.name, source, e.level, e.missiles);

    // deriveEntry is also called with synthetic rows for monster AIs, where the
    // row name IS the AI id; keep monster_ai filled so the description can lead
    // with it (buildCatalog overwrites it for GetMissileType entries anyway).
    if (source == "monster" && e.monster_ai.empty()) e.monster_ai = e.name;

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

    // Unique monsters override the base monster's AI / intelligence
    // (unique_monstdat.tsv). Load them as extra monster rows so ranged attacks
    // and abilities are attributed to the unique's name and its own AI.
    Table uniques = mergeByKey(
        parseTsvTable(devRoot / "assets" / "txtdata" / "monsters" / "unique_monstdat.tsv"),
        parseTsvTable(devRoot / "mods" / "hf" / "txtdata" / "monsters" / "unique_monstdat.tsv"),
        "type");
    for (const auto& u : uniques) {
        const auto type = trim(val(u, "type"));
        const auto nm = trim(val(u, "name"));
        if (type.empty() || nm.empty()) continue;
        Row r;
        r["_monster_id"] = type;
        r["name"] = nm;
        r["ai"] = trim(val(u, "ai"));
        r["intelligence"] = trim(val(u, "intelligence"));
        mon.push_back(std::move(r));
    }

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
        auto ai = normalizedMonsterAi(trim(val(r, "ai")));
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

    // Attacks cast directly inside their AI function (Counselor / Bat / Mega /
    // HorkDemon), which bypass GetMissileType(). One entry per attack form; the
    // monster list is narrowed by intelligence or monster id exactly as the AI
    // code picks the missile.
    for (const auto& f : monsterAttackForms()) {
        std::vector<std::string> names;
        for (const auto& r : mon) {
            if (normalizedMonsterAi(trim(val(r, "ai"))) != f.ai) continue;
            if (!attackFormMatches(f, trim(val(r, "_monster_id")),
                                   std::atoi(val(r, "intelligence").c_str())))
                continue;
            auto nm = trim(val(r, "name"));
            if (nm.empty()) continue;
            if (std::find(names.begin(), names.end(), nm) == names.end())
                names.push_back(nm);
        }
        if (names.empty()) continue; // AI absent from the loaded tables

        Row pseudo;
        pseudo["id"] = f.variant.empty() ? f.ai : (f.ai + "." + f.variant);
        pseudo["name"] = f.ai;
        pseudo["missiles"] = f.missiles;
        auto first = splitComma(f.missiles);
        auto mi = first.empty() ? misById.end() : misById.find(trim(first[0]));
        pseudo["flags"] = (mi == misById.end()) ? "" : val(mi->second, "flags");

        Entry e = deriveEntry(pseudo, misById, sprById, "monster", &zh, f.variant, f.condition);
        e.monster_ai = f.ai;
        e.monsters = names;
        e.default_ai = false;
        c.monster_attacks.push_back(std::move(e));
    }

    // Engine-level monster abilities (teleport on hard hit, death acid puddle,
    // sneak invisibility). They are not ranged attacks, so they carry their own
    // array; the sprite chain (if any) is still resolved through misdat.
    for (const auto& spec : monsterAbilities()) {
        std::vector<std::string> names;
        for (const auto& r : mon) {
            const auto ai = normalizedMonsterAi(trim(val(r, "ai")));
            if (!abilityMatches(spec, ai, trim(val(r, "_monster_id")))) continue;
            auto nm = trim(val(r, "name"));
            if (nm.empty()) continue;
            if (std::find(names.begin(), names.end(), nm) == names.end())
                names.push_back(nm);
        }

        Row pseudo;
        pseudo["id"] = spec.id;
        pseudo["name"] = spec.name;
        pseudo["flags"] = spec.element;
        pseudo["missiles"] = spec.missiles;
        Entry e = deriveEntry(pseudo, misById, sprById, "monster", nullptr);
        e.is_ability = true;
        e.monster_ai.clear();
        e.name_zh = spec.name_zh;
        e.description = spec.name_zh + "（" + spec.name + "）：" + spec.description +
                        " 触发：" + spec.trigger + "。";
        e.trigger = spec.trigger;
        e.source_ref = spec.source_ref;
        e.monsters = names;
        c.abilities.push_back(std::move(e));
    }

    // Trap spell effects (wall traps / trapped chests / Hellfire flame traps).
    for (const auto& spec : trapSpecs()) {
        Row pseudo;
        pseudo["id"] = spec.id;
        pseudo["name"] = spec.name;
        pseudo["flags"] = spec.element;
        pseudo["missiles"] = spec.missiles;
        Entry e = deriveEntry(pseudo, misById, sprById, "monster", nullptr);
        e.is_trap = true;
        e.monster_ai.clear();
        e.name_zh = spec.name_zh;
        e.description = spec.name_zh + "（" + spec.name + "）：" + spec.description +
                        " 触发：" + spec.trigger + "。";
        e.trigger = spec.trigger;
        e.source_ref = spec.source_ref;
        e.objects = spec.objects;
        c.traps.push_back(std::move(e));
    }

    c.ok = true;
    return c;
}

} // namespace spell_tables
