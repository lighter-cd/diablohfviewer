/**
 * @file dungeon_gen.h
 *
 * Native reimplementation of Diablo 1 / Hellfire dungeon generation.
 *
 * The rules are a faithful transcription of DevilutionX's `Source/levels/drlg_l1.cpp`
 * (Cathedral), including its vanilla quirks: the RNG call order is part of the
 * format, so any deviation changes the whole level.
 *
 * See docs/MAP_GENERATION.md for the full rule description and the
 * DevilutionX-fixture based acceptance procedure.
 */
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace dhfc {

/**
 * @brief Borland C/C++ LCG used by Diablo 1 for deterministic generation.
 *
 * `state_{n+1} = state_n * 0x015A4E35 + 1 (mod 2^32)`.
 */
class DiabloRng {
public:
    explicit DiabloRng(uint32_t seed = 0) : state_(seed) {}

    void seed(uint32_t value) { state_ = value; }
    uint32_t state() const { return state_; }

    /** Advance one step and return the new state (DevilutionX GenerateRandomNumber). */
    uint32_t advance();

    /** GenerateRnd: uses the high bits for limits <= 0x7FFF. */
    int32_t generateRnd(int32_t v);

    /** FlipCoin(freq): true 1/freq of the time. */
    bool flipCoin(unsigned frequency = 2);

    /** Discard `count` values (DevilutionX DiscardRandomValues). */
    void discard(unsigned count);

private:
    uint32_t state_;
};

/** A 40x40 megatile layer parsed from a .dun file. */
struct DunTileLayer {
    int width = 0;
    int height = 0;
    std::vector<uint16_t> tiles; // [y * width + x]

    bool valid() const
    {
        return width > 0 && height > 0
            && tiles.size() == static_cast<size_t>(width) * static_cast<size_t>(height);
    }

    uint16_t at(int x, int y) const { return tiles[static_cast<size_t>(y) * width + x]; }
};

/**
 * @brief Parse the megatile layer of a .dun blob.
 *
 * Layout: `uint16 width, uint16 height, width*height uint16 tiles` (row-major,
 * y outer). Any extra micro layers are ignored.
 */
DunTileLayer parseDunTileLayer(const std::vector<uint8_t>& bytes);

/**
 * @brief A generated 40x40 megatile grid, identical in meaning to DevilutionX
 * `dungeon[DMAXX][DMAXY]`.
 */
struct DungeonTileGrid {
    static constexpr int Width = 40;
    static constexpr int Height = 40;

    std::array<uint8_t, Width * Height> tiles {};

    uint8_t& at(int x, int y) { return tiles[static_cast<size_t>(y) * Width + x]; }
    uint8_t at(int x, int y) const { return tiles[static_cast<size_t>(y) * Width + x]; }
};

/** Options for `generateCathedral`. */
struct CathedralOptions {
    /** Dungeon level (1..4). */
    int level = 1;
    /** `DungeonSeeds[level]`. */
    uint32_t seed = 0;
    /** `MyPlayer->pOriginalCathedral` (true for Diablo, false for Hellfire). */
    bool originalCathedral = true;
    /** `UseMultiplayerQuests()`; false for single-player full quests. */
    bool multiplayerQuests = false;

    // Resolved quest availability for the current level (Quest::IsAvailable()).
    bool butcherQuest = false;      // Q_BUTCHER, level 2
    bool poisonWaterQuest = false;  // Q_PWATER, level 2
    bool skeletonKingQuest = false; // Q_SKELKING, level 3
    bool bannerQuest = false;       // Q_LTBANNER, level 4

    // Set-piece megatile layers (levels/l1data/*.dun). Only consulted when the
    // matching quest is available; a missing pointer throws std::runtime_error.
    const DunTileLayer* butcherSetPiece = nullptr;      // rnd6.dun
    const DunTileLayer* skeletonKingSetPiece = nullptr; // skngdo.dun
    const DunTileLayer* bannerSetPiece = nullptr;       // banner2.dun
};

/**
 * @brief Fill `options` with vanilla single-player quest availability for a
 * Cathedral level: L2 -> Butcher + Poisoned Water, L3 -> Skeleton King,
 * L4 -> Ogden's Sign (banner). L1 has no quests.
 */
void applyVanillaCathedralQuests(CathedralOptions& options);

/**
 * @brief Generate a Cathedral level (L1-L4, Hellfire Cathedral).
 *
 * Deterministic: the same options always produce the same grid, matching
 * DevilutionX `CreateDungeon` + `Pass3` tile layer.
 */
DungeonTileGrid generateCathedral(const CathedralOptions& options);

// ---------------------------------------------------------------------------
// Phase 6: Catacombs / Caves / Hell generators (DevilutionX drlg_l2/l3/l4).
// Each is a faithful, self-contained port keeping the exact RNG call order, so
// a given (level, seed, quest state) reproduces the golden .dun tile layer.
// The three level types use DIFFERENT megatile-id sets (L2 Floor=3/Dirt=12,
// L3 7/8, L4 6/30); DungeonTileGrid just stores the ids, so the caller must
// pair the grid with the matching level's .til (see SceneSpec.til).
// ---------------------------------------------------------------------------

/** Which dungeon generator a scene uses. */
enum class DungeonType {
    Cathedral,  // drlg_l1, levels 1-4  (l1data)
    Catacombs,  // drlg_l2, levels 5-8  (l2data)
    Caves,      // drlg_l3, levels 9-12 (l3data)
    Hell,       // drlg_l4, levels 13-16(l4data)
};

/** Options for `generateCatacombs` (DevilutionX CreateL2Dungeon). */
struct CatacombsOptions {
    int level = 5;                 // game level 5..8
    uint32_t seed = 0;
    bool multiplayerQuests = false;

    // Resolved Quest::IsAvailable() for this level.
    bool bloodQuest = false;       // Q_BLOOD
    bool schambQuest = false;      // Q_SCHAMB (bonestr)
    bool blindQuest = false;       // Q_BLIND
    bool warlordNotAvailable = false; // affects DRLG_CheckQuests iteration

    // Set-piece megatile layers (levels/l2data/*.dun). Injected by the caller;
    // a missing pointer disables that quest's set piece (graceful fallback).
    const DunTileLayer* blood1 = nullptr;   // InitSetPiece Q_BLOOD
    const DunTileLayer* blood2 = nullptr;   // DrawBlood
    const DunTileLayer* bonestr1 = nullptr; // DrawSChamber
    const DunTileLayer* bonestr2 = nullptr; // InitSetPiece Q_SCHAMB
    const DunTileLayer* blind1 = nullptr;   // InitSetPiece Q_BLIND
};

/** Apply vanilla single-player quest availability for a Catacombs level. */
void applyVanillaCatacombsQuests(CatacombsOptions& options);

/** Generate a Catacombs level (L5-L8). Deterministic; matches CreateL2Dungeon tile layer. */
DungeonTileGrid generateCatacombs(const CatacombsOptions& options);

/** Options for `generateCaves` (DevilutionX CreateL3Dungeon, Diablo branch). */
struct CavesOptions {
    int level = 9;                 // game level 9..12
    uint32_t seed = 0;
    bool multiplayerQuests = false;
    bool anvilQuest = false;       // Q_ANVIL
    int anvilQuestLevel = 0;       // Quests[Q_ANVIL]._qlevel (10 when active)
    const DunTileLayer* anvil = nullptr; // levels/l3data/anvil.dun
};

/** Apply vanilla quest availability for a Caves level. */
void applyVanillaCavesQuests(CavesOptions& options);

/** Generate a Caves level (L9-L12, Diablo branch). Deterministic. */
DungeonTileGrid generateCaves(const CavesOptions& options);

/** Options for `generateHell` (DevilutionX CreateL4Dungeon). */
struct HellOptions {
    int level = 13;                // game level 13..16
    uint32_t seed = 0;
    bool multiplayerQuests = false;
    bool warlordQuest = false;     // Q_WARLORD
    bool betrayerQuest = false;    // Q_BETRAYER / L4PENTA
    bool diabloQuest = false;      // Q_DIABLO gate (level 16)

    const DunTileLayer* warlord = nullptr;  // levels/l4data/warlord.dun
    const DunTileLayer* warlord2 = nullptr;// levels/l4data/warlord2.dun
    const DunTileLayer* vile1 = nullptr;   // levels/l4data/vile1.dun
    // LoadDiabQuads pieces (level 16): diab1, diab2a/b, diab3a/b, diab4a/b.
    const DunTileLayer* diab[7] = { nullptr, nullptr, nullptr, nullptr,
                                    nullptr, nullptr, nullptr };
};

/** Apply vanilla quest availability for a Hell level. */
void applyVanillaHellQuests(HellOptions& options);

/** Generate a Hell level (L13-L16). Deterministic; matches CreateL4Dungeon tile layer. */
DungeonTileGrid generateHell(const HellOptions& options);

} // namespace dhfc
