// src/decoders/level_scene.cpp
//
// Scene assembly, ported from DevilutionX Source/levels/town.cpp (DrlgTPass3 /
// FillSector), gendung.cpp (DRLG_LPass3 / PlaceDunTiles / LoadDungeonBase) and
// drlg_l1.cpp / drlg_l2.cpp (InitDungeonPieces). See level_scene.h for the model.

#include "decoders/level_scene.h"

#include <algorithm>
#include <cstddef>
#include <initializer_list>
#include <string>
#include <tuple>

namespace decoders {

namespace {

bool microIn(uint16_t v, std::initializer_list<uint16_t> ids)
{
    for (uint16_t id : ids)
        if (v == id)
            return true;
    return false;
}

// DevilutionX drlg_l1.cpp::InitDungeonPieces / drlg_l2.cpp::InitDungeonPieces.
// Arch / door megatiles only carry a 32px stub in the level MIN; their 64px
// body is drawn from the special CEL indexed by dSpecial - 1
// (scrollrt.cpp:912-935). Without this pass the doors render as broken stubs.
uint8_t dungeonSpecialId(DungeonSpecialTable table, uint16_t micro)
{
    if (table == DungeonSpecialTable::Cathedral) {
        if (microIn(micro, { 11, 70, 320, 210, 340, 417 })) return 1;
        if (microIn(micro, { 10, 248, 324, 343, 330, 420 })) return 2;
        if (micro == 252) return 3;
        if (micro == 254) return 4;
        if (micro == 258) return 5;
        if (micro == 266) return 6;
    } else if (table == DungeonSpecialTable::Catacombs) {
        if (microIn(micro, { 540, 177, 550 })) return 5;
        if (microIn(micro, { 541, 552 })) return 6;
    }
    return 0;
}

void applyDungeonSpecial(SceneGrid& g, DungeonSpecialTable table)
{
    if (table == DungeonSpecialTable::None)
        return;
    const size_t w = static_cast<size_t>(g.gw), h = static_cast<size_t>(g.gh);
    for (size_t j = 0; j < h; ++j) {
        for (size_t i = 0; i < w; ++i) {
            const uint8_t id = dungeonSpecialId(table, g.micro[j * w + i]);
            if (id != 0)
                g.special[j * w + i] = id;
        }
    }
    // drlg_l2.cpp:1672-1681: dPiece 131/134/138 also marks neighbouring cells.
    if (table == DungeonSpecialTable::Catacombs) {
        for (size_t j = 0; j < h; ++j) {
            for (size_t i = 0; i < w; ++i) {
                const uint16_t mi = g.micro[j * w + i];
                if (mi == 131) {
                    if (j + 1 < h) g.special[(j + 1) * w + i] = 2;
                    if (j + 2 < h) g.special[(j + 2) * w + i] = 1;
                } else if (mi == 134 || mi == 138) {
                    if (i + 1 < w) g.special[j * w + i + 1] = 3;
                    if (i + 2 < w) g.special[j * w + i + 2] = 4;
                }
            }
        }
    }
}

// DevilutionX LoadL2Dungeon (drlg_l2.cpp:2855) calls AddL2Objs() AFTER Pass3().
// AddL2Objs (objects.cpp:3786-3797) creates OBJ_L2LDOOR / OBJ_L2RDOOR for the
// doorway micros; AddDoor -> SetDoorStateClosed (objects.cpp:1138-1147) then
// replaces dPiece with the CLOSED door-leaf micro and clears dSpecial.
// scene.build has no object layer, so emulate just that final dPiece state:
// micro 12/540 (left doorway) -> 537, 16/541 (right doorway) -> 539.
// Without this the .dun's doorway stubs stay empty (no door leaf).
void applyL2ClosedDoors(SceneGrid& g)
{
    for (size_t i = 0; i < g.micro.size(); ++i) {
        const uint16_t mi = g.micro[i];
        uint16_t door = 0;
        if (mi == 12 || mi == 540)
            door = 537; // OBJ_L2LDOOR
        else if (mi == 16 || mi == 541)
            door = 539; // OBJ_L2RDOOR
        if (door == 0)
            continue;
        g.micro[i] = door;
        g.special[i] = 0; // SetDoorStateClosed clears overlapping arches
    }
}

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
                            const DunFile& dun,
                            bool dirtBorder)
{
    SceneGrid g;
    // Crop to the .dun bounding box by default: every cell is the preset room.
    // The Dirt border is a SOLID tile that renders above the room floor and
    // hid the room in a full-scene view. dirtBorder=true restores the 32-micro
    // DRLG_LPass3 prefill + (16,16) offset for faithful dPiece reproduction.
    // sceneGridDims() reports the default (cropped) dims.
    const int border = dirtBorder ? 32 : 0;
    const int off = dirtBorder ? 16 : 0;
    if (dun.width && dun.height) {
        g.gw = static_cast<int>(dun.width) * 2 + border;
        g.gh = static_cast<int>(dun.height) * 2 + border;
    } else {
        const auto dims = sceneGridDims(spec);
        g.gw = dims.first + border;
        g.gh = dims.second + border;
    }
    const size_t cells = static_cast<size_t>(g.gw) * g.gh;
    g.megaId.assign(cells, 0);
    g.special.assign(cells, 0);

    // Fallback prefill (DRLG_LPass3's Dirt megatile): with the bbox crop the
    // .dun covers every cell, so this only fills words whose TIL megatile
    // index is out of range (and, with dirtBorder, IS the border itself).
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

    // Overlay the .dun tile layer: at (0,0) when cropped, (16,16) with border.
    if (dun.width && dun.height) {
        for (uint16_t j = 0; j < dun.height; ++j) {
            for (uint16_t i = 0; i < dun.width; ++i) {
                const uint16_t word = dun.tileLayer[static_cast<size_t>(j) * dun.width + i];
                const int px = off + static_cast<int>(i) * 2;
                const int py = off + static_cast<int>(j) * 2;
                if (word == 0) {
                    // DevilutionX LoadDungeonBase -> PlaceDunTiles(dunData,
                    // {0,0}, floorId): a zero DUN cell is replaced by the
                    // level's Floor megatile, not left as the Dirt prefill.
                    // These cells are the room's walkable floor / passages.
                    const uint16_t floor = spec.floorMegaTile;
                    if (floor > 0 && static_cast<size_t>(floor - 1) < megatiles.size())
                        putMega(g, px, py, megatiles[floor - 1], floor);
                    continue;
                }
                const size_t ti = static_cast<size_t>(word - 1);
                if (ti >= megatiles.size())
                    continue;
                putMega(g, px, py, megatiles[ti], word);
            }
        }
    }
    // Pass3() -> InitDungeonPieces(): mark arch / door cells for the special
    // CEL overlay (town does the equivalent via townSpecialFrameId).
    applyDungeonSpecial(g, spec.specialTable);
    // AddL2Objs() -> SetDoorStateClosed(): catacombs doorways become closed
    // door leaves. This must follow InitDungeonPieces (which marks 540/541).
    if (spec.specialTable == DungeonSpecialTable::Catacombs)
        applyL2ClosedDoors(g);
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
            uint16_t prefill;                  // Dirt megatile (1-based)
            uint16_t floorId;                  // PlaceDunTiles floorId (1-based)
            const char* specialCel;            // dSpecial overlay CEL, "" = none
            DungeonSpecialTable specialTable;
            uint8_t ambient;
            uint8_t lightStyle;
        };
        // floorId / special CEL from DevilutionX:
        //   drlg_l1.cpp:1330 LoadDungeonBase(path, spawn, Floor=13, Dirt=22)
        //   drlg_l2.cpp:2851 (3, 12)   drlg_l3.cpp:2207 (7, 8)
        //   drlg_l4.cpp:1249 (6, 30)
        //   diablo.cpp:1389-1408 LoadLvlGFX: Cathedral/Caves -> l1s, Catacombs/Hell -> l2s
        // Only Cathedral and Catacombs call InitDungeonPieces() in Pass3();
        // Caves / Hell have no dSpecial source, so their specialCel stays empty.
        const DunScene dunScenes[] = {
            { "l1_rnd2", "大教堂 L1 · rnd2 房间", "levels/l1data",
              "levels/l1data/rnd2.dun", 11, 11, "levels/l1data/l1_2.pal",
              10, 10, 22, 13, "levels/l1data/l1s.cel",
              DungeonSpecialTable::Cathedral, 15, 0 },
            { "l1_skngdo", "大教堂 L1 · skngdo 大厅", "levels/l1data",
              "levels/l1data/skngdo.dun", 7, 7, "levels/l1data/l1_2.pal",
              10, 10, 22, 13, "levels/l1data/l1s.cel",
              DungeonSpecialTable::Cathedral, 15, 0 },
            { "l2_blind", "地下墓穴 L2 · blind 房间", "levels/l2data",
              "levels/l2data/blind1.dun", 11, 11, "levels/l2data/l2_2.pal",
              10, 10, 12, 3, "levels/l2data/l2s.cel",
              DungeonSpecialTable::Catacombs, 15, 0 },
            { "l3_anvil", "洞穴 L3 · anvil 房间", "levels/l3data",
              "levels/l3data/anvil.dun", 9, 9, "levels/l3data/l3pfoul.pal",
              10, 10, 8, 7, "", DungeonSpecialTable::None, 15, 0 },
            { "l4_diab1", "地狱 L4 · diab1 房间", "levels/l4data",
              "levels/l4data/diab1.dun", 6, 6, "levels/l4data/l4_1.pal",
              12, 16, 30, 6, "", DungeonSpecialTable::None, 15, 1 },
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
            s.floorMegaTile = ds.floorId;
            s.specialCel = ds.specialCel;
            s.specialTable = ds.specialTable;
            s.ambientLight = ds.ambient;
            s.lightStyle = ds.lightStyle;
            s.dunW = ds.dunW;
            s.dunH = ds.dunH;
            s.gridW = ds.dunW * 2;
            s.gridH = ds.dunH * 2;
            v.push_back(s);
        }

        // Phase 5/P6: seed-generated levels, one per dlvl that carries a
        // vanilla fixed room (plus L1 as a clean Cathedral baseline). These
        // have NO fixed .dun: scene.random builds a full 40x40 megatile grid
        // (-> 80x80 micro grid) with the matching drlg_l1..l4 generator and
        // injects it through loadSceneAssets' generatedDun override. The id
        // suffix is the exact game dlvl; fixedRoom is an i18n key naming the
        // embedded set piece so the UI can surface it. Appended LAST so
        // catalog[1] == l1_rnd2 stays valid.
        // Reference: DevilutionX questdat.tsv + drlg_l1.cpp::initSetPiece,
        // drlg_l2.cpp::InitSetPiece, drlg_l3.cpp, drlg_l4.cpp::InitSetPiece.
        struct RandomScene {
            const char* id;
            const char* title;
            const char* dataDir;   // levels/lNdata
            const char* palette;
            uint8_t microLen, blocks;
            uint16_t dirt, floor;
            const char* specialCel;
            DungeonSpecialTable specialTable;
            uint8_t ambient, lightStyle;
            int level;             // exact game dlvl generated
            const char* fixedRoom; // i18n key; "" = no quest set piece
        };
        const RandomScene randomScenes[] = {
            // Cathedral L1-L4 (l1data; special l1s.cel table).
            { "random_l1", "随机地牢 L1 · 大教堂（按种子生成）", "levels/l1data",
              "levels/l1data/l1_2.pal", 10, 10, 22, 13, "levels/l1data/l1s.cel",
              DungeonSpecialTable::Cathedral, 15, 0, 1, "" },
            { "random_l2", "随机地牢 L2 · 大教堂 · 屠夫（按种子生成）", "levels/l1data",
              "levels/l1data/l1_2.pal", 10, 10, 22, 13, "levels/l1data/l1s.cel",
              DungeonSpecialTable::Cathedral, 15, 0, 2, "butcher" },
            { "random_l3", "随机地牢 L3 · 大教堂 · 骷髅王（按种子生成）", "levels/l1data",
              "levels/l1data/l1_2.pal", 10, 10, 22, 13, "levels/l1data/l1s.cel",
              DungeonSpecialTable::Cathedral, 15, 0, 3, "skelking" },
            { "random_l4", "随机地牢 L4 · 大教堂 · Ogden 招牌（按种子生成）", "levels/l1data",
              "levels/l1data/l1_2.pal", 10, 10, 22, 13, "levels/l1data/l1s.cel",
              DungeonSpecialTable::Cathedral, 15, 0, 4, "banner" },
            // Catacombs L5-L7 (l2data; special l2s.cel table). Quest dlvls from
            // questdat.tsv: Blood=5, Chamber of Bone=6, Blind=7.
            { "random_l5", "随机地牢 L5 · 地下墓穴 · 血石（按种子生成）", "levels/l2data",
              "levels/l2data/l2_2.pal", 10, 10, 12, 3, "levels/l2data/l2s.cel",
              DungeonSpecialTable::Catacombs, 15, 0, 5, "blood" },
            { "random_l6", "随机地牢 L6 · 地下墓穴 · 骸骨密室（按种子生成）", "levels/l2data",
              "levels/l2data/l2_2.pal", 10, 10, 12, 3, "levels/l2data/l2s.cel",
              DungeonSpecialTable::Catacombs, 15, 0, 6, "schamb" },
            { "random_l7", "随机地牢 L7 · 地下墓穴 · 盲眼大厅（按种子生成）", "levels/l2data",
              "levels/l2data/l2_2.pal", 10, 10, 12, 3, "levels/l2data/l2s.cel",
              DungeonSpecialTable::Catacombs, 15, 0, 7, "blind" },
            // Caves L10 (l3data; no special CEL table). Anvil of Fury = dlvl 10.
            { "random_l10", "随机地牢 L10 · 洞穴 · 愤怒铁砧（按种子生成）", "levels/l3data",
              "levels/l3data/l3_2.pal", 10, 10, 8, 7, "",
              DungeonSpecialTable::None, 15, 0, 10, "anvil" },
            // Hell L13/L15/L16 (l4data; no special table, hell light style).
            // Warlord=13, Betrayer/Vile=15, Diablo boss room=16 (hell gate 15).
            { "random_l13", "随机地牢 L13 · 地狱 · 血之领主（按种子生成）", "levels/l4data",
              "levels/l4data/l4_2.pal", 12, 16, 30, 6, "",
              DungeonSpecialTable::None, 15, 1, 13, "warlord" },
            { "random_l15", "随机地牢 L15 · 地狱 · 背叛者（按种子生成）", "levels/l4data",
              "levels/l4data/l4_2.pal", 12, 16, 30, 6, "",
              DungeonSpecialTable::None, 15, 1, 15, "betrayer" },
            { "random_l16", "随机地牢 L16 · 地狱 · 迪亚波罗（按种子生成）", "levels/l4data",
              "levels/l4data/l4_2.pal", 12, 16, 30, 6, "",
              DungeonSpecialTable::None, 15, 1, 16, "diablo" },
        };
        for (const RandomScene& rs : randomScenes) {
            SceneSpec s;
            s.id = rs.id;
            s.title = rs.title;
            const std::string dir = rs.dataDir;
            const char lv = dir[dir.find_last_of('/') + 2]; // '1'..'4' (after 'l')
            s.cel = dir + "/l" + lv + ".cel";
            s.til = dir + "/l" + lv + ".til";
            s.min = dir + "/l" + lv + ".min";
            s.sol = dir + "/l" + lv + ".sol";
            s.palette = rs.palette;
            s.specialCel = rs.specialCel;
            s.specialCelWidth = 64;
            s.layout = SceneLayout::Dungeon;
            s.microLen = rs.microLen;
            s.blocks = rs.blocks;
            s.prefillMegaTile = rs.dirt;
            s.floorMegaTile = rs.floor;
            s.specialTable = rs.specialTable;
            s.ambientLight = rs.ambient;
            s.lightStyle = rs.lightStyle;
            s.dunW = 40;
            s.dunH = 40;
            s.gridW = 80;
            s.gridH = 80;
            s.randomLevel = rs.level;
            s.fixedRoom = rs.fixedRoom;
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
    // Default (cropped) dims. scene.build with dirt_border:true adds 32 micro
    // on each axis; its returned grid_width/height reflect the actual render.
    if (spec.layout == SceneLayout::Dungeon && spec.dunW > 0 && spec.dunH > 0)
        return { spec.dunW * 2, spec.dunH * 2 };
    return { spec.gridW, spec.gridH };
}

} // namespace decoders
