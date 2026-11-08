// src/decoders/level_light.cpp
//
// Port of DevilutionX Source/engine/light_tables.cpp (MakeLightTable) and
// Source/lighting.cpp (DoLighting) for offline/static map rendering.

#include "decoders/level_light.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace decoders {

namespace {

// DevilutionX LightTables colour-ramp partition. 256 palette entries are
// divided into 18 ramps (`steps`) so shading can walk each ramp separately.
constexpr int kRampSteps[18] = { 16, 16, 16, 16, 16, 16, 16, 16,
                                 8,  8,  8,  8,  16, 16, 16, 16, 16, 16 };

constexpr int kRotationSines[4] = { 0, 1, 0, -1 };

int rotateX(int x, int y, int quadrants)
{
    const int q = ((quadrants % 4) + 4) % 4;
    const int sine = kRotationSines[q];
    const int cosine = kRotationSines[(q + 1) % 4];
    return x * cosine - y * sine;
}

int rotateY(int x, int y, int quadrants)
{
    const int q = ((quadrants % 4) + 4) % 4;
    const int sine = kRotationSines[q];
    const int cosine = kRotationSines[(q + 1) % 4];
    return x * sine + y * cosine;
}

bool inBounds(int x, int y, int w, int h)
{
    return x >= 0 && x < w && y >= 0 && y < h;
}

} // namespace

void LightTableView::build(LightStyle style)
{
    // MakeLightTable(): 16 gradually darker lookup tables.
    uint8_t shade = 0;
    for (auto& table : tables) {
        uint8_t colorIndex = 0;
        for (const int steps : kRampSteps) {
            const uint8_t shading = static_cast<uint8_t>(shade * steps / 16);
            const uint8_t shadeStart = colorIndex;
            const uint8_t shadeEnd = static_cast<uint8_t>(shadeStart + steps - 1);
            for (int step = 0; step < steps; ++step) {
                if (colorIndex == 0) {
                    table[colorIndex++] = 0;
                    continue;
                }
                int color = shadeStart + step + shading;
                if (color > shadeEnd || color == 255)
                    color = 0;
                table[colorIndex++] = static_cast<uint8_t>(color);
            }
        }
        shade++;
    }

    tables[15].fill(0); // last shade is pitch black

    if (style == LightStyle::Hell) {
        // Blood-wall lighting (MakeLightTable's DTYPE_HELL branch).
        const int shades = static_cast<int>(tables.size() - 1); // 15
        constexpr int Range = 16;
        for (int i = 0; i < shades; ++i) {
            auto& table = tables[static_cast<size_t>(i)];
            for (int j = 0; j < Range; ++j) {
                uint8_t color = static_cast<uint8_t>(
                    ((Range - 1) << 4) / shades * (shades - i) / Range * (j + 1));
                color = static_cast<uint8_t>(1 + (color >> 4));
                table[static_cast<size_t>(j + 1)] = color;
                table[static_cast<size_t>(31 - j)] = color;
            }
        }
    } else if (style == LightStyle::Soft) {
        // Make the lava fully bright (DTYPE_NEST / DTYPE_CRYPT branch).
        for (auto& table : tables) {
            for (int i = 0; i < 16; ++i)
                table[static_cast<size_t>(i)] = static_cast<uint8_t>(i);
        }
        tables[15][0] = 0;
        for (int i = 1; i < 16; ++i)
            tables[15][static_cast<size_t>(i)] = 1;
    }
}

void LightTableView::applyToPalette(uint8_t level, const uint8_t* paletteRgba,
                                    uint8_t* outRgba) const
{
    if (!paletteRgba || !outRgba)
        return;
    const auto& table = tables[static_cast<size_t>(level & 0x0F)];
    for (int i = 0; i < kLightTableSize; ++i) {
        const uint8_t mapped = table[static_cast<size_t>(i)];
        const uint8_t* src = paletteRgba + static_cast<size_t>(mapped) * 4;
        uint8_t* dst = outRgba + static_cast<size_t>(i) * 4;
        dst[0] = src[0];
        dst[1] = src[1];
        dst[2] = src[2];
        dst[3] = paletteRgba[static_cast<size_t>(i) * 4 + 3];
    }
}

void makeLightFalloffs(bool softFalloff, uint8_t out[16][128])
{
    // LightFalloffs[radius][distance]
    constexpr float maxDarkness = 15.0F;
    constexpr float maxBrightness = 0.0F;
    for (unsigned radius = 0; radius < kNumLightRadiuses; ++radius) {
        const unsigned maxDistance = (radius + 1) * 8;
        for (unsigned distance = 0; distance < 128; ++distance) {
            uint8_t value;
            if (distance > maxDistance) {
                value = 15;
            } else {
                const float factor =
                    static_cast<float>(distance) / static_cast<float>(maxDistance);
                float scaled;
                if (softFalloff) {
                    // Quadratic falloff with over-exposure (nest/crypt).
                    const float brightness = static_cast<float>(radius) * 1.25F;
                    scaled = factor * factor * brightness + (maxDarkness - brightness);
                    scaled = std::max(maxBrightness, scaled);
                } else {
                    scaled = factor * maxDarkness;
                }
                scaled += 0.5F; // round up
                value = static_cast<uint8_t>(scaled);
            }
            out[radius][distance] = value;
        }
    }
}

void makeLightConeInterpolations(uint8_t out[8][8][16][16])
{
    for (int offsetY = 0; offsetY < 8; ++offsetY) {
        for (int offsetX = 0; offsetX < 8; ++offsetX) {
            for (int y = 0; y < 16; ++y) {
                for (int x = 0; x < 16; ++x) {
                    const int a = (8 * x) - offsetX;
                    const int b = (8 * y) - offsetY;
                    out[offsetX][offsetY][x][y] =
                        static_cast<uint8_t>(std::sqrt(static_cast<double>(a * a + b * b)));
                }
            }
        }
    }
}

LightMapResult buildLightMap(int gridW, int gridH, uint8_t ambient,
                             const std::vector<LightSource>& lights,
                             bool softFalloff)
{
    LightMapResult result;
    if (gridW <= 0 || gridH <= 0)
        return result;

    const size_t cells = static_cast<size_t>(gridW) * static_cast<size_t>(gridH);
    result.levels.assign(cells, static_cast<uint8_t>(std::min<int>(ambient, 15)));

    uint8_t falloffs[16][128];
    uint8_t cone[8][8][16][16];
    makeLightFalloffs(softFalloff, falloffs);
    makeLightConeInterpolations(cone);

    const auto get = [&](int x, int y) -> uint8_t {
        return result.levels[static_cast<size_t>(y) * gridW + x];
    };
    const auto set = [&](int x, int y, uint8_t v) {
        result.levels[static_cast<size_t>(y) * gridW + x] = v;
    };

    for (const LightSource& light : lights) {
        int px = light.x;
        int py = light.y;
        if (!inBounds(px, py, gridW, gridH))
            continue;
        const int radius = std::clamp<int>(light.radius, 0, kNumLightRadiuses - 1);
        int offsetX = light.offsetX;
        int offsetY = light.offsetY;
        if (offsetX < 0) {
            offsetX += 8;
            px -= 1;
        }
        if (offsetY < 0) {
            offsetY += 8;
            py -= 1;
        }
        // DoLighting() normalizes offsets to [0, 7]; guard against callers that
        // pass something larger so table indices stay in range.
        offsetX &= 7;
        offsetY &= 7;

        int minX = 15;
        if (px - 15 < 0)
            minX = px + 1;
        int maxX = 15;
        if (px + 15 > gridW)
            maxX = gridW - px;
        int minY = 15;
        if (py - 15 < 0)
            minY = py + 1;
        int maxY = 15;
        if (py + 15 > gridH)
            maxY = gridH - py;
        minX = std::max(0, minX);
        minY = std::max(0, minY);
        maxX = std::min(16, maxX);
        maxY = std::min(16, maxY);

        if (softFalloff) {
            if (inBounds(px, py, gridW, gridH) && get(px, py) > falloffs[radius][0])
                set(px, py, falloffs[radius][0]);
        } else if (inBounds(px, py, gridW, gridH)) {
            set(px, py, 0);
        }

        int distX = offsetX, distY = offsetY;
        int lightX = 0, lightY = 0;
        int blockX = 0, blockY = 0;
        int coneOffX = offsetX, coneOffY = offsetY;

        for (int i = 0; i < 4; ++i) {
            const int yBound = (i > 0 && i < 3) ? maxY : minY;
            const int xBound = (i < 2) ? maxX : minX;
            for (int y = 0; y < yBound; ++y) {
                for (int x = 1; x < xBound; ++x) {
                    const int linearDistance =
                        cone[coneOffX][coneOffY][x + blockX][y + blockY];
                    if (linearDistance >= 128)
                        continue;
                    const int tx = px + rotateX(x, y, -i);
                    const int ty = py + rotateY(x, y, -i);
                    if (!inBounds(tx, ty, gridW, gridH))
                        continue;
                    const uint8_t v = falloffs[radius][linearDistance];
                    if (v < get(tx, ty))
                        set(tx, ty, v);
                }
            }

            // RotateRadius() from lighting.cpp. `offset` itself is reassigned
            // each quarter-turn (it is used by the next cone lookup), so keep
            // its value in coneOffX/coneOffY.
            const int ndistX = 7 - distY;
            const int ndistY = distX;
            const int nlightX = 7 - lightY;
            const int nlightY = lightX;
            distX = ndistX;
            distY = ndistY;
            lightX = nlightX;
            lightY = nlightY;
            int offX = distX - lightX;
            int offY = distY - lightY;
            blockX = 0;
            if (offX < 0) {
                offX += 8;
                blockX = 1;
            }
            blockY = 0;
            if (offY < 0) {
                offY += 8;
                blockY = 1;
            }
            coneOffX = offX;
            coneOffY = offY;
        }
    }

    result.minLevel = 15;
    result.maxLevel = 0;
    for (const uint8_t level : result.levels) {
        result.minLevel = std::min(result.minLevel, level);
        result.maxLevel = std::max(result.maxLevel, level);
        if (level == 0)
            ++result.litCells;
    }
    if (result.levels.empty()) {
        result.minLevel = 0;
        result.maxLevel = 0;
    }
    return result;
}

} // namespace decoders
