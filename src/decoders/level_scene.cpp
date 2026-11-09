// src/decoders/level_scene.cpp
//
// Scene assembly, ported from DevilutionX Source/levels/town.cpp (DrlgTPass3 /
// FillSector) and gendung.cpp (DRLG_LPass3). See level_scene.h for the model.

#include "decoders/level_scene.h"

#include <algorithm>
#include <cstddef>
#include <tuple>

namespace decoders {

namespace {

// Copy a megatile's four micro indices into a 2x2 block at (x, y). `mega` is
// the 1-based megatile id written to SceneGrid.megaId (0 = none / prefill).
void putMega(SceneGrid& g, int x, int y, const MegaTile& mt, uint16_t mega)
{
    const int w = g.gw, h = g.gh;
    const uint16_t v[4] = { mt.micro1, mt.micro2, mt.micro3, mt.micro4 };
    const int dx[4] = { 0, 1, 0, 1 };
    const int dy[4] = { 0, 0, 1, 1 };
    for (int k = 0; k < 4; ++k) {
        const int cx = x + dx[k], cy = y + dy[k];
        if (cx < 0 || cx >= w || cy < 0 || cy >= h)
            continue;
        const size_t idx = static_cast<size_t>(cy) * w + cx;
        g.micro[idx] = v[k];
        g.megaId[idx] = mega;
    }
}

void fillMicro(SceneGrid& g, uint16_t mi)
{
    std::fill(g.micro.begin(), g.micro.end(), mi);
}

} // namespace

SceneGrid buildTownScene(const SceneSpec& spec,
                         const std::vector<MegaTile>& megatiles,
                         const std::vector<std::vector<uint8_t>>& sectorDunBufs)
{
    SceneGrid g;
    g.gw = spec.gridW;
    g.gh = spec.gridH;
    const size_t cells = static_cast<size_t>(g.gw) * g.gh;
    g.micro.assign(cells, spec.prefillMicro);   // DrlgTPass3: prefill 426
    g.megaId.assign(cells, 0);
    g.special.assign(cells, 0);

    const size_t n = std::min(spec.sectorDuns.size(), sectorDunBufs.size());
    for (size_t s = 0; s < n; ++s) {
        const DunFile dun = loadDun(sectorDunBufs[s]);
        if (dun.width == 0 || dun.height == 0)
            continue;
        const int offX = (s < spec.sectorOffsets.size()) ? spec.sectorOffsets[s].first : 0;
        const int offY = (s < spec.sectorOffsets.size()) ? spec.sectorOffsets[s].second : 0;
        for (uint16_t j = 0; j < dun.height; ++j) {
            for (uint16_t i = 0; i < dun.width; ++i) {
                const uint16_t word = dun.tileLayer[static_cast<size_t>(j) * dun.width + i];
                const int x = offX + static_cast<int>(i) * 2;
                const int y = offY + static_cast<int>(j) * 2;
                if (word == 0) {
                    // FillSector: empty DUN cell -> micro 218 in all 4 corners.
                    const MegaTile empt{ spec.emptyMicro, spec.emptyMicro,
                                         spec.emptyMicro, spec.emptyMicro };
                    putMega(g, x, y, empt, 0);
                } else {
                    const size_t ti = static_cast<size_t>(word - 1);
                    if (ti >= megatiles.size())
                        continue;
                    putMega(g, x, y, megatiles[ti], word);
                }
            }
        }
    }

    // Deterministic Tristram post-sector patches (town.cpp:217-252). These
    // FillTile calls write dPiece directly (unlike dungeon[] edits), so they are
    // visible map content: warp stairs and the town fountain/well.
    if (spec.tristramPatches) {
        auto fillMega = [&](int x, int y, uint16_t id) {
            if (id == 0 || static_cast<size_t>(id - 1) >= megatiles.size())
                return;
            putMega(g, x, y, megatiles[static_cast<size_t>(id - 1)], id);
        };
        fillMega(48, 20, 320); // catacombs warp (closed state)
        fillMega(16, 68, 332); // caves warp
        fillMega(16, 70, 331); // caves warp
        fillMega(60, 70, 71);  // town fountain/well (clean state)
    }

    // DevilutionX DrlgTPass3 -> InitTownPieces: mark dSpecial cells after the
    // whole dPiece grid is assembled.
    for (size_t idx = 0; idx < cells; ++idx)
        g.special[idx] = townSpecialFrameId(g.micro[idx]);
    return g;
}

SceneGrid buildDungeonScene(const SceneSpec& spec,
                            const std::vector<MegaTile>& megatiles,
                            const DunFile& dun)
{
    SceneGrid g;
    // Size to the .dun preset plus a 16-megatile (32 micro) Dirt border, so
    // the scene is a complete room rather than a mostly-empty 112x112 plain.
    // sceneGridDims() mirrors this so scene.list reports the same values.
    if (dun.width && dun.height) {
        g.gw = static_cast<int>(dun.width) * 2 + 32;
        g.gh = static_cast<int>(dun.height) * 2 + 32;
    } else {
        const auto dims = sceneGridDims(spec);
        g.gw = dims.first;
        g.gh = dims.second;
    }
    const size_t cells = static_cast<size_t>(g.gw) * g.gh;
    g.megaId.assign(cells, 0);
    g.special.assign(cells, 0);

    // DRLG_LPass3: prefill every 2x2 block with the "Dirt" megatile's four
    // micros (megatiles[Dirt-1], Dirt=22 for L1), repeated across the grid.
    const size_t dirt = (spec.prefillMegaTile > 0)
                            ? static_cast<size_t>(spec.prefillMegaTile - 1) : 0;
    const MegaTile& dm = (dirt < megatiles.size())
                             ? megatiles[dirt] : MegaTile{};
    std::vector<uint16_t> four = { dm.micro1, dm.micro2, dm.micro3, dm.micro4 };
    for (int j = 0; j < g.gh; ++j) {
        for (int i = 0; i < g.gw; ++i) {
            g.micro.push_back(four[static_cast<size_t>(j % 2) * 2 + (i % 2)]);
        }
    }

    // Overlay the .dun tile layer at (16,16) (DRLG_LPass3 offset).
    if (dun.width && dun.height) {
        for (uint16_t j = 0; j < dun.height; ++j) {
            for (uint16_t i = 0; i < dun.width; ++i) {
                const uint16_t word = dun.tileLayer[static_cast<size_t>(j) * dun.width + i];
                if (word == 0)
                    continue;
                const size_t ti = static_cast<size_t>(word - 1);
                if (ti >= megatiles.size())
                    continue;
                putMega(g, 16 + static_cast<int>(i) * 2,
                        16 + static_cast<int>(j) * 2, megatiles[ti], word);
            }
        }
    }
    return g;
}

uint8_t townSpecialFrameId(uint16_t microId)
{
    // DevilutionX town.cpp::InitTownPieces (Tristram).
    switch (microId) {
    case 359: return 1;
    case 357: return 2;
    case 128: return 6;
    case 129: return 7;
    case 127: return 8;
    case 116: return 9;
    case 156: return 10;
    case 157: return 11;
    case 155: return 12;
    case 161: return 13;
    case 159: return 14;
    case 213: return 15;
    case 211: return 16;
    case 216: return 17;
    case 215: return 18;
    default: return 0;
    }
}

const std::vector<SceneSpec>& sceneCatalog()
{
    static const std::vector<SceneSpec> catalog = [] {
        std::vector<SceneSpec> v;

        // Tristram (the initial scene, DTYPE_TOWN). Retail DIABDAT.MPQ paths;
        // Hellfire nlevels variants are deferred.
        SceneSpec town;
        town.id = "tristram";
        town.title = "Tristram 城镇（初始场景）";
        town.cel = "levels/towndata/town.cel";
        town.til = "levels/towndata/town.til";
        town.min = "levels/towndata/town.min";
        town.sol = "levels/towndata/town.sol";
        town.palette = "levels/towndata/town.pal";
        town.specialCel = "levels/towndata/towns.cel";
        town.specialCelWidth = 64;
        town.sectorDuns = {
            "levels/towndata/sector1s.dun", "levels/towndata/sector2s.dun",
            "levels/towndata/sector3s.dun", "levels/towndata/sector4s.dun",
        };
        town.sectorOffsets = { { 46, 46 }, { 46, 0 }, { 0, 46 }, { 0, 0 } };
        town.layout = SceneLayout::Town;
        town.microLen = 16;
        town.blocks = 16;
        town.prefillMicro = 426;
        town.emptyMicro = 218;
        town.tristramPatches = true;
        // 96x96 = exact bbox covered by the four sectors (sector1s is 25x25
        // at (46,46) => 46+50=96). DevilutionX's dPiece is 112x112 with a
        // 16-cell prefill border; we crop to the meaningful scene to keep the
        // native PNG bounded (documented in docs/ARCHITECTURE.md).
        town.gridW = 96;
        town.gridH = 96;
        v.push_back(town);

        // Dungeon presets from the retail campaign. Prefill ids and micro/block
        // widths are taken from DevilutionX:
        //   L1 drlg_l1.cpp: DRLG_LPass3(Dirt-1) with Dirt=22, microLen 10
        //   L2 drlg_l2.cpp: DRLG_LPass3(12-1), microLen 10
        //   L3 drlg_l3.cpp: DRLG_LPass3(8-1),  microLen 10
        //   L4 drlg_l4.cpp: DRLG_LPass3(30-1), microLen 12 (blocks 16)
        struct DunScene {
            const char* id;
            const char* title;
            const char* dataDir;   // levels/lNdata
            const char* dun;
            int dunW;
            int dunH;
            const char* palette;
            uint8_t microLen;
            uint8_t blocks;
            uint16_t prefill;
            uint8_t ambient;
            uint8_t lightStyle;
        };
        const DunScene dunScenes[] = {
            { "l1_rnd2", "大教堂 L1 · rnd2 房间", "levels/l1data",
              "levels/l1data/rnd2.dun", 11, 11, "levels/l1data/l1_2.pal",
              10, 10, 22, 15, 0 },
            { "l1_skngdo", "大教堂 L1 · skngdo 大厅", "levels/l1data",
              "levels/l1data/skngdo.dun", 7, 7, "levels/l1data/l1_2.pal",
              10, 10, 22, 15, 0 },
            { "l2_blind", "地下墓穴 L2 · blind 房间", "levels/l2data",
              "levels/l2data/blind1.dun", 11, 11, "levels/l2data/l2_2.pal",
              10, 10, 12, 15, 0 },
            { "l3_anvil", "洞穴 L3 · anvil 房间", "levels/l3data",
              "levels/l3data/anvil.dun", 9, 9, "levels/l3data/l3pfoul.pal",
              10, 10, 8, 15, 0 },
            { "l4_diab1", "地狱 L4 · diab1 房间", "levels/l4data",
              "levels/l4data/diab1.dun", 6, 6, "levels/l4data/l4_1.pal",
              12, 16, 30, 15, 1 },
        };
        for (const DunScene& ds : dunScenes) {
            SceneSpec s;
            s.id = ds.id;
            s.title = ds.title;
            const std::string dir = ds.dataDir;
            const std::string stem = dir.substr(dir.find_last_of('/') + 1); // l1data
            const char level = stem[1];                                     // '1'..'4'
            s.cel = dir + "/l" + level + ".cel";
            s.til = dir + "/l" + level + ".til";
            s.min = dir + "/l" + level + ".min";
            s.sol = dir + "/l" + level + ".sol";
            s.palette = ds.palette;
            s.dun = ds.dun;
            s.layout = SceneLayout::Dungeon;
            s.microLen = ds.microLen;
            s.blocks = ds.blocks;
            s.prefillMegaTile = ds.prefill;
            s.ambientLight = ds.ambient;
            s.lightStyle = ds.lightStyle;
            s.dunW = ds.dunW;
            s.dunH = ds.dunH;
            s.gridW = ds.dunW * 2 + 32;
            s.gridH = ds.dunH * 2 + 32;
            v.push_back(s);
        }
        return v;
    }();
    return catalog;
}

const SceneSpec* findScene(const std::string& id)
{
    for (const auto& s : sceneCatalog())
        if (s.id == id)
            return &s;
    return nullptr;
}

std::pair<int, int> sceneGridDims(const SceneSpec& spec)
{
    if (spec.layout == SceneLayout::Dungeon && spec.dunW > 0 && spec.dunH > 0)
        return { spec.dunW * 2 + 32, spec.dunH * 2 + 32 };
    return { spec.gridW, spec.gridH };
}

} // namespace decoders
