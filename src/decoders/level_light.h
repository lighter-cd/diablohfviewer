// src/decoders/level_light.h
//
// Faithful port of DevilutionX lighting for static level maps.
//
// DevilutionX stores one light level per world tile in `dLight[x][y]`
// (0 = fully lit, 15 = pitch black). The renderer does NOT scale RGB directly:
// it looks up `LightTables[level][paletteIndex]`, a 16-step shade table that
// walks each 16-colour palette ramp toward black. This module reproduces both
// halves of that model:
//
//   * buildLightMap()    -> per-cell light levels (ambient + point lights),
//                           ported from Source/lighting.cpp::DoLighting() and
//                           Source/engine/light_tables.cpp.
//   * LightTableView     -> the 16 palette-index translation tables, plus a
//                           helper that bakes one into an RGBA palette so the
//                           tile decoder can render lit pixels directly.
//
// The tables are generated from the same constants as the engine, so the
// result is dimmable / cave-like in the same way as the original game.

#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace decoders {

constexpr int kNumLightLevels = 16; // 0 (bright) .. 15 (dark)
constexpr int kNumLightRadiuses = 16;
constexpr int kLightTableSize = 256;

// Lighting style selected by the level type (DevilutionX MakeLightTable()).
enum class LightStyle : uint8_t {
    Normal = 0, // cathedral / catacombs / caves / town
    Hell = 1,   // DTYPE_HELL: blood-wall colour remap + lit ceiling
    Soft = 2,   // DTYPE_NEST / DTYPE_CRYPT: lava/over-exposed falloff
};

struct LightTableView {
    std::array<std::array<uint8_t, kLightTableSize>, kNumLightLevels> tables{};

    // Build the 16 shade tables exactly like DevilutionX MakeLightTable().
    void build(LightStyle style = LightStyle::Normal);

    // Apply one shade table to a 256-entry RGBA palette (1024 bytes) and write
    // the result to `outRgba` (also 1024 bytes). Alpha is preserved.
    void applyToPalette(uint8_t level, const uint8_t* paletteRgba,
                        uint8_t* outRgba) const;
};

// A static light source, in MICRO-TILE coordinates (the scene grid units used
// by buildMicroGrid / buildDungeonScene).
struct LightSource {
    int x = 0;
    int y = 0;
    uint8_t radius = 15; // 0..15, same range as DevilutionX NumLightRadiuses
    int8_t offsetX = 0;  // 0..7 (or negative, normalized by DoLighting)
    int8_t offsetY = 0;
};

struct LightMapResult {
    std::vector<uint8_t> levels; // gridW * gridH, row-major (y * gridW + x)
    uint8_t minLevel = 0;
    uint8_t maxLevel = 15;
    size_t litCells = 0; // cells with level 0 (fully lit)
};

// Build the per-cell dLight grid:
//   * every cell starts at `ambient` (DevilutionX: town 0, dungeon 15),
//   * each light lowers the cells in its cone/radius with `LightFalloffs`.
// Cells outside the grid are ignored. `softFalloff` enables the nest/crypt
// quadratic over-exposed curve used by DevilutionX.
LightMapResult buildLightMap(int gridW, int gridH, uint8_t ambient,
                             const std::vector<LightSource>& lights,
                             bool softFalloff = false);

// Exposed for unit tests: the generated falloff/cone tables, matching
// LightFalloffs[16][128] / LightConeInterpolations[8][8][16][16].
void makeLightFalloffs(bool softFalloff, uint8_t out[16][128]);
void makeLightConeInterpolations(uint8_t out[8][8][16][16]);

} // namespace decoders
