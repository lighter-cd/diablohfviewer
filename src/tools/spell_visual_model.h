// spell_visual_model.h — 法术/怪物攻击「级别 → 视觉参数」模型（header-only，纯函数，可单测）
//
// 背景：Diablo 1 的法术级别不改变美术资源（sprite 不变），但会改变视觉表现参数——
// 持续时间、范围/半径、弹射物数量、弹速，以及其它法术特有字段（墙段数、爬行半径等）。
// 这些参数**不在任何数据表里**，而是写死在 DevilutionX 的 C++ 处理函数
// （spells.cpp 的施放函数、missiles.cpp 的 Add<X> handler、以及墙体生长代码
// TryGrowWall/PlaceWall）中，且每个法术结构不同、公式各异。
//
// 因此这里**逐法术手工转录其真实公式**（标注对应的源码位置），建成注册表，供 spell_tables
// 在 deriveEntry 时调用，生成每个级别对应的视觉参数键值表。
//
// 参考假设（保证可复现，已在 _note 标注）：
//   - kRefCharLevel：含施法者角色等级(getCharacterLevel())的项，用满级 50 近似。
//   - kRefDungeonLevel：含地下城层(currlevel)的项，用 0 近似。
//   - kTicksPerSec：duration_ticks 换算 duration_sec 的近似帧率（Diablo ~60 ticks/sec）。
//   - 怪物攻击按"怪物等级" 1..kMonsterMaxLvl 建模，键前缀 "M:"。

#pragma once

#include <algorithm>
#include <cstdio>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace spell_visual {

using ParamMap = std::map<std::string, std::string>;
using ByLevel = std::vector<std::pair<int, ParamMap>>;

constexpr int kRefCharLevel = 50;     // 满级角色，用于含 getCharacterLevel() 的项
constexpr int kRefDungeonLevel = 0;   // 地下城层，用于含 currlevel 的项
constexpr int kTicksPerSec = 60;      // duration_ticks -> 秒 的近似换算
constexpr int kMonsterMaxLvl = 16;    // 怪物攻击按怪物等级 1..N 建模
constexpr int kMaxCrawlRadius = 18;   // Source/lighting.h MaxCrawlRadius

// 把动画 tick 数换算成近似秒数字符串
inline std::string fmtSec(int ticks)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f", static_cast<double>(ticks) / kTicksPerSec);
    return std::string(buf);
}

// ---- 逐法术公式（转录自 DevilutionX 源码，行号对应 Source/missiles.cpp） ----

// FireWall: AddFireWall(1954) duration = 10*(lvl+1)*16 (+currlevel 若玩家/中立)。
// 墙长由地形决定（Crawl 无半径上限），非等级公式，不建模。
static ParamMap fwFireWall(int lvl, int, int refDungeon)
{
    ParamMap m;
    int dur = 10 * (lvl + 1) * 16;
    if (refDungeon > 0) dur += refDungeon; // 对应 +currlevel 分支
    m["duration_ticks"] = std::to_string(dur);
    m["duration_sec"] = fmtSec(dur);
    m["_note"] = "墙长受地形阻挡决定，无等级公式；持续含地下城层项(≈)";
    return m;
}

// Chain Lightning(玩家) -> LightningControl：AddLightningControl(1992) duration=256、velocity=32；
// ProcessChainLightning(3587) 以半径 min(lvl+3, MaxCrawlRadius) 生成爬行闪电。
static ParamMap fwLightningControl(int lvl, int, int)
{
    ParamMap m;
    m["duration_ticks"] = "256";
    m["duration_sec"] = fmtSec(256);
    m["speed"] = "32";
    m["radius_tiles"] = std::to_string(std::min(lvl + 3, kMaxCrawlRadius));
    return m;
}

// FlameWave: AddFlameWave(2170) duration=255、velocity=16；
// ProcessFlameWaveControl(3884) 墙段数 = (lvl/2)+2（双向生长，墙长约 2*段+1 格）。
static ParamMap fwFlameWave(int lvl, int, int)
{
    ParamMap m;
    m["duration_ticks"] = "255";
    m["duration_sec"] = fmtSec(255);
    m["speed"] = "16";
    int seg = (lvl / 2) + 2;
    m["segments"] = std::to_string(seg);
    m["wall_length_tiles"] = std::to_string(2 * seg + 1);
    return m;
}

// ChargedBolt: spells.cpp(221) 施放循环 i=(lvl/2)+3 个弹；AddChargedBolt(2697) duration=256、velocity=8。
static ParamMap fwChargedBolt(int lvl, int, int)
{
    ParamMap m;
    m["duration_ticks"] = "256";
    m["duration_sec"] = fmtSec(256);
    m["speed"] = "8";
    m["count"] = std::to_string((lvl / 2) + 3);
    return m;
}

// HolyBolt: AddHolyBolt(2715) sp = 16 + min(lvl*2, 47)（非陷阱即玩家/怪物均生效）。
static ParamMap fwHolyBolt(int lvl, int, int)
{
    ParamMap m;
    m["duration_ticks"] = "256";
    m["duration_sec"] = fmtSec(256);
    m["speed"] = std::to_string(16 + std::min(lvl * 2, 47));
    return m;
}

// Guardian: AddGuardian(2181) dur = lvl + charLevel/2，clamp<=30，<<4，min 30。
static ParamMap fwGuardian(int lvl, int refChar, int)
{
    ParamMap m;
    int dur = lvl + refChar / 2;
    if (dur > 30) dur = 30;
    dur <<= 4;
    if (dur < 30) dur = 30;
    m["duration_ticks"] = std::to_string(dur);
    m["duration_sec"] = fmtSec(dur);
    m["_note"] = "持续含角色等级项(≈, refChar=" + std::to_string(refChar) + ")";
    return m;
}

// 怪物 Torchant -> Fireball：AddFireball(1978) 怪物施放时 sp = 16 + min(lvl*2, 34)。
static ParamMap fwMonFireball(int lvl, int, int)
{
    ParamMap m;
    m["duration_ticks"] = "256";
    m["duration_sec"] = fmtSec(256);
    m["speed"] = std::to_string(16 + std::min(lvl * 2, 34));
    return m;
}

// 怪物 Storm -> ThinLightningControl：同 LightningControl 爬行半径 min(lvl+3, Max)。
static ParamMap fwMonThinLightning(int lvl, int, int)
{
    ParamMap m;
    m["duration_ticks"] = "256";
    m["duration_sec"] = fmtSec(256);
    m["speed"] = "32";
    m["radius_tiles"] = std::to_string(std::min(lvl + 3, kMaxCrawlRadius));
    return m;
}

using VisualFn = ParamMap (*)(int level, int refChar, int refDungeon);

inline const std::map<std::string, VisualFn>& registry()
{
    static const std::map<std::string, VisualFn> r = {
        // 玩家法术（键 = spelldat 的 name，注意含空格）
        { "Fire Wall", fwFireWall },
        { "Chain Lightning", fwLightningControl },
        { "Lightning", fwLightningControl },
        { "Flame Wave", fwFlameWave },
        { "Charged Bolt", fwChargedBolt },
        { "Holy Bolt", fwHolyBolt },
        { "Guardian", fwGuardian },
        // 怪物攻击（键 = "M:" + MissileID）
        { "M:Fireball", fwMonFireball },
        { "M:ThinLightningControl", fwMonThinLightning },
    };
    return r;
}

// 计算某 Entry 的各级视觉参数。
//   key 解析：玩家用 spellName；怪物用 "M:" + missiles[0]。
//   玩家 level 区间 1..bookLevel；怪物 1..kMonsterMaxLvl。
//   未在注册表的法术返回空（前端显示「无级别视觉缩放」）。
inline ByLevel computeVisualByLevel(const std::string& spellName, const std::string& source,
                                    const std::string& bookLevel,
                                    const std::vector<std::string>& missiles)
{
    ByLevel result;
    const auto& reg = registry();
    const std::string key = (source == "monster" && !missiles.empty()) ? ("M:" + missiles[0]) : spellName;
    auto it = reg.find(key);
    if (it == reg.end())
        return result;

    int maxLvl;
    if (source == "monster")
        maxLvl = kMonsterMaxLvl;
    else {
        maxLvl = std::atoi(bookLevel.c_str());
        if (maxLvl < 1) maxLvl = 1;
    }
    for (int lvl = 1; lvl <= maxLvl; ++lvl)
        result.emplace_back(lvl, it->second(lvl, kRefCharLevel, kRefDungeonLevel));
    return result;
}

} // namespace spell_visual
