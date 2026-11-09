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

} // namespace dhfc
