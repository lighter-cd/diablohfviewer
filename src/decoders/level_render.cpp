// src/decoders/level_render.cpp
//
// Composite level-map rendering, ported from DevilutionX's DrawGame ->
// DrawFloor + DrawTileContent/DrawCell + worldToScreen.
//
// A scene is a (gw x gh) grid of MICRO indices ("dPiece"). Each micro expands
// into a stack of 32x32 CEL frames (the LevelCelBlocks of a .min piece):
//   * Pass 1 (floors): mt[0] + mt[1] are drawn as a Left + Right triangle
//     forming a 64x32 diamond at the cell base. DevilutionX forces the
//     triangle shape regardless of the block's stored TileType (DrawFloorTile).
//     EVERY cell gets a floor diamond, so the ground plane is complete.
//   * Pass 2 (walls): mt[2..microLen-1] are drawn in pairs, each pair 32px
//     higher, back-to-front (descending i+j), with hasValue() guards.
//
// The isometric transform is DevilutionX's screen axes:
//   screenX = (x - y) * 32
//   screenY = (x + y) * 16      (base of the cell; stacks grow upward)
//
// Every pixel is filled with opts.background first, so the output has NO
// transparent gaps. Previously the buffer was left at (0,0,0,0), which any
// viewer composited as black, producing the "black blocks" / incomplete look.

#include "decoders/level.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

#include "decoders/level_light.h"

namespace decoders {

namespace {

// Blit a decoded 32x32 frame into `dst` (RGBA, `dstW` x `dstH`, row pitch
// `dstPitch` bytes) at column offset `dx` and slot top `slotTopY`. CEL row 0 is
// written at the BOTTOM of the slot (vertical flip), matching the engine.
// Transparent source pixels (alpha == 0) are skipped. Bounds-checked so a cell
// near an image edge can never write out of the buffer.
void blitFrame(const uint8_t* frame, int frameW, int frameH,
               uint8_t* dst, int dstW, int dstH, int dstPitch,
               int dx, int slotTopY)
{
    for (int fr = 0; fr < frameH; ++fr) {
        const int dr = slotTopY + (frameH - 1 - fr); // vertical flip
        if (dr < 0 || dr >= dstH)
            continue;
        const uint8_t* src = frame + static_cast<size_t>(fr) * frameW * 4;
        uint8_t* row = dst + static_cast<size_t>(dr) * dstPitch;
        for (int fx = 0; fx < frameW; ++fx) {
            const int px = dx + fx;
            if (px < 0 || px >= dstW)
                continue;
            const uint8_t* p = src + fx * 4;
            if (p[3] != 0)
                std::memcpy(row + static_cast<size_t>(px) * 4, p, 4);
        }
    }
}

// Blit a decoded top-down sprite with its BOTTOM row at `bottomY` and its left
// edge at `dx`. Used for town `dSpecial` CEL overlays (DrawDungeon's pSpecialCels
// pass draws ClxDraw with the sprite's bottom at the tile base).
void blitSpriteBottom(const SpriteFrameRGBA& spr,
                      uint8_t* dst, int dstW, int dstH, int dstPitch,
                      int dx, int bottomY)
{
    if (spr.width <= 0 || spr.height <= 0)
        return;
    for (int sr = 0; sr < spr.height; ++sr) {
        const int dr = bottomY - (spr.height - 1) + sr;
        if (dr < 0 || dr >= dstH)
            continue;
        const uint8_t* srcRow = spr.rgba.data() + static_cast<size_t>(sr) * spr.width * 4;
        uint8_t* row = dst + static_cast<size_t>(dr) * dstPitch;
        for (int sx = 0; sx < spr.width; ++sx) {
            const int px = dx + sx;
            if (px < 0 || px >= dstW)
                continue;
            const uint8_t* p = srcRow + static_cast<size_t>(sx) * 4;
            if (p[3] != 0)
                std::memcpy(row + static_cast<size_t>(px) * 4, p, 4);
        }
    }
}

// A CEL block is drawable only if it has a value, a nonzero (1-based) frame,
// and that frame exists in the re-encoded CEL.
bool blockDrawable(const LevelCelBlock& b, const ReencodedLevelCel& cel)
{
    if (!b.hasValue() || b.frame() == 0)
        return false;
    return (b.frame() - 1) < cel.numFrames;
}

// Resolve the micro INDEX for a cell. `mi` is 0-based into `micros`
// (kNoMicro = truly outside the scene). kNoMicro / out-of-range cells fall
// back to opts.prefillMicro so a real scene has no holes (DevilutionX
// prefills every dPiece cell with a floor micro). The resolved index is also
// the SOL table index (SOLData[dPiece[x][y]]).
uint16_t resolveMicroIndex(const std::vector<MicroTileRef>& micros,
                           uint16_t mi, const MapRenderOptions& opts)
{
    uint16_t idx = mi;
    if (idx == kNoMicro || idx >= micros.size())
        idx = opts.prefillMicro;
    if (idx == kNoMicro || idx >= micros.size())
        return kNoMicro;
    return idx;
}

} // namespace

std::vector<TileType> buildFrameTypes(const std::vector<MicroTileRef>& micros, int microLen)
{
    uint32_t maxFrame = 0;
    for (const auto& m : micros) {
        for (int b = 0; b < microLen && b < 16; ++b) {
            if (!m.blocks[b].hasValue())
                continue;
            const uint32_t f = m.blocks[b].frame(); // 1-based
            if (f > 0 && f - 1 > maxFrame)
                maxFrame = f - 1;
        }
    }
    std::vector<TileType> types(maxFrame + 1, TileType::Square);
    for (const auto& m : micros) {
        for (int b = 0; b < microLen && b < 16; ++b) {
            if (!m.blocks[b].hasValue())
                continue;
            const uint32_t f = m.blocks[b].frame();
            if (f > 0)
                types[f - 1] = m.blocks[b].type();
        }
    }
    return types;
}

SceneProjection sceneProjection(int gw, int gh, int microLen)
{
    SceneProjection p;
    const int Hm = (microLen / 2) * 32;
    // DevilutionX screen axes: East = (x+1,y-1) moves +32 px X, and the
    // per-row step (x+1 or y+1) moves +16 px Y. Therefore:
    //   screenX = (x - y) * 32
    //   screenY = (x + y) * 16
    // (Our grid uses i=x, j=y; the previous (j-i, -(j+i)) was a 180° rotation.)
    p.minX = -static_cast<long>(gh - 1) * 32;
    p.minY = -Hm + 1;
    const long maxX = static_cast<long>(gw - 1) * 32 + 64;
    const long maxY = static_cast<long>(gw + gh - 2) * 16;
    p.width = maxX - p.minX + 1;
    p.height = maxY - p.minY + 1;
    return p;
}

void renderMicroTile(const ReencodedLevelCel& cel, const MicroTileRef& micro,
                     const uint8_t* pal, int microLen,
                     std::vector<uint8_t>& rgba, int& outW, int& outH)
{
    const int nRows = microLen / 2;
    const int H = nRows * 32;
    const int W = 64;
    rgba.assign(static_cast<size_t>(W) * H * 4, 0);
    const int pitch = W * 4;
    std::vector<uint8_t> frame(32 * 32 * 4);

    for (int b = 0; b < nRows; ++b) {
        const int slotTopY = H - (b + 1) * 32;
        for (int side = 0; side < 2; ++side) {
            const LevelCelBlock blk = micro.blocks[2 * b + side];
            if (!blockDrawable(blk, cel))
                continue;
            int fh = 0;
            decodeReencodedFrame(cel, blk.frame() - 1, pal, frame, fh);
            blitFrame(frame.data(), 32, 32, rgba.data(), W, H, pitch,
                      side * 32, slotTopY);
        }
    }
    outW = W;
    outH = H;
}

MapRenderResult renderMap(const ReencodedLevelCel& cel,
                          const std::vector<MicroTileRef>& micros,
                          const std::vector<uint16_t>& microGrid, int gw, int gh,
                          const std::vector<uint8_t>& solFlags,
                          const std::vector<int8_t>& transparency,
                          const uint8_t* pal, int microLen,
                          const MapRenderOptions& opts)
{
    MapRenderResult res;
    res.gridW = gw;
    res.gridH = gh;
    const size_t cells = static_cast<size_t>(gw) * gh;
    res.solid.assign(cells, 0);
    res.blockLight.assign(cells, 0);
    res.blockMissile.assign(cells, 0);
    res.transparent.assign(cells, 0);
    res.light.assign(cells, 0);

    // Optional faithful dLight shading. When lightLevels is supplied we bake
    // one RGBA palette per level (DevilutionX LightTables[level][paletteIndex])
    // and decode each cell's tiles through the cell's lit palette.
    const bool useLight = opts.lightLevels.size() == cells;
    LightTableView lightTables;
    std::array<std::vector<uint8_t>, kNumLightLevels> litPalettes;
    if (useLight) {
        const LightStyle style = static_cast<LightStyle>(
            opts.lightStyle <= static_cast<uint8_t>(LightStyle::Soft)
                ? opts.lightStyle
                : static_cast<uint8_t>(LightStyle::Normal));
        lightTables.build(style);
        for (int level = 0; level < kNumLightLevels; ++level) {
            litPalettes[static_cast<size_t>(level)].resize(256 * 4);
            lightTables.applyToPalette(static_cast<uint8_t>(level), pal,
                                       litPalettes[static_cast<size_t>(level)].data());
        }
        for (size_t i = 0; i < cells; ++i)
            res.light[i] = static_cast<uint8_t>(opts.lightLevels[i] & 0x0F);
    }

    const SceneProjection proj = sceneProjection(gw, gh, microLen);
    const int nRows = microLen / 2;
    const int Hm = nRows * 32;

    res.width = static_cast<int>(proj.width);
    res.height = static_cast<int>(proj.height);
    res.rgba.assign(static_cast<size_t>(res.width) * static_cast<size_t>(res.height) * 4, 0);
    // Fill the whole canvas with an OPAQUE background (no transparent gaps).
    const uint8_t bg[4] = { opts.background[0], opts.background[1],
                            opts.background[2], opts.background[3] };
    for (size_t i = 0; i < res.rgba.size(); i += 4)
        std::memcpy(res.rgba.data() + i, bg, 4);
    const int pitch = res.width * 4;

    std::vector<uint8_t> frame(32 * 32 * 4);

    // Screen position of a cell (DevilutionX screen axes: East=(+1,-1) so
    // X=(x-y), and rows advance (x+y) downward):
    //   screenX = (i - j) * 32, screenY = (i + j) * 16
    // colTop is the top of the whole (microLen/2 * 32) column. Slot b occupies
    // rows colTop + (Hm - (b+1)*32) .. +31, so b=0 (floor) is the bottom slot.
    const auto cellX = [&](int i, int j) {
        return static_cast<int>((static_cast<long>(i) - j) * 32 - proj.minX);
    };
    const auto colTop = [&](int i, int j) {
        const long baseY = (static_cast<long>(i) + j) * 16 - proj.minY;
        return static_cast<int>(baseY - Hm + 1);
    };
    const auto slotTop = [&](int ct, int b) {
        return ct + (Hm - (b + 1) * 32);
    };

    // Precompute per-cell SOL flags + resolved micro index. SOL is indexed by
    // the micro piece id (DevilutionX SOLData[dPiece[x][y]]).
    std::vector<uint8_t> flags(cells, 0);
    std::vector<uint16_t> cellMicroIdx(cells, kNoMicro);
    for (int j = 0; j < gh; ++j) {
        for (int i = 0; i < gw; ++i) {
            const size_t idx = static_cast<size_t>(j) * gw + i;
            const uint16_t mi = resolveMicroIndex(micros, microGrid[idx], opts);
            cellMicroIdx[idx] = mi;
            if (mi != kNoMicro && !solFlags.empty() && mi < solFlags.size())
                flags[idx] = solFlags[mi];
            res.solid[idx] = (flags[idx] & TileFlags::Solid) ? 1 : 0;
            res.blockLight[idx] = (flags[idx] & TileFlags::BlockLight) ? 1 : 0;
            res.blockMissile[idx] = (flags[idx] & TileFlags::BlockMissile) ? 1 : 0;
            if (!transparency.empty() && idx < transparency.size())
                res.transparent[idx] = (transparency[idx] > 0) ? 1 : 0;
        }
    }

    // DevilutionX IsFloor(tilePosition): not Solid and not BlockMissile.
    const auto isFloor = [&](size_t idx) {
        return !(flags[idx] & (TileFlags::Solid | TileFlags::BlockMissile));
    };

    // Palette used to decode a cell's tile frames. Without lighting this is the
    // plain palette; with lighting it is the cell's baked dLight shade table.
    const auto paletteAt = [&](size_t idx) -> const uint8_t* {
        if (!useLight)
            return pal;
        return litPalettes[static_cast<size_t>(res.light[idx] & 0x0F)].data();
    };

    const MicroTileRef* floorMicro =
        (opts.prefillMicro == kNoMicro || opts.prefillMicro >= micros.size())
            ? nullptr : &micros[opts.prefillMicro];

    const auto microAt = [&](size_t idx) -> const MicroTileRef* {
        const uint16_t mi = cellMicroIdx[idx];
        return (mi == kNoMicro) ? nullptr : &micros[mi];
    };

    // ---- Pass 1: floor diamonds for EVERY cell (complete ground plane) ----
    // Drawn far-to-near (ascending i+j: smaller screen Y first), so nearer
    // diamonds overlap farther ones correctly.
    // DevilutionX DrawFloorTile forces LeftTriangle / RightTriangle regardless
    // of the block's stored type; CopyRun writes opaque pixels, so even a frame
    // encoded as a square/trapezoid yields a filled (if stylized) diamond.
    // If a cell's own floor block is missing, fall back to the prefill floor.
    const int maxSum = (gw - 1) + (gh - 1);
    for (int s = 0; s <= maxSum; ++s) {
        for (int i = 0; i < gw; ++i) {
            const int j = s - i;
            if (j < 0 || j >= gh)
                continue;
            const size_t idx = static_cast<size_t>(j) * gw + i;
            // DevilutionX DrawFloor only draws floor cells; solid/wall cells
            // are rendered by DrawCell (real tile types + wall stack) in Pass 2.
            // Drawing a forced floor diamond for a solid cell decodes its
            // wall/foliage data as triangles and produces garbage.
            if (!isFloor(idx))
                continue;
            const int x = cellX(i, j);
            const int slot = slotTop(colTop(i, j), 0);
            for (int side = 0; side < 2; ++side) {
                const MicroTileRef* m = microAt(idx);
                LevelCelBlock blk = m ? m->blocks[side] : LevelCelBlock{};
                if (!blockDrawable(blk, cel) && floorMicro)
                    blk = floorMicro->blocks[side];
                if (!blockDrawable(blk, cel))
                    continue;
                int fh = 0;
                // TransparentSquare frames are RLE squares that combine floor
                // and foliage. Forcing them through the triangle decoder reads
                // RLE bytes as triangle pixels (garbage specks), so decode their
                // real shape here.
                if (blk.type() == TileType::TransparentSquare) {
                    decodeReencodedFrame(cel, blk.frame() - 1, paletteAt(idx), frame, fh);
                } else {
                    decodeReencodedFrameAs(cel, blk.frame() - 1,
                                           side == 0 ? TileType::LeftTriangle
                                                     : TileType::RightTriangle,
                                           paletteAt(idx), frame, fh);
                }
                blitFrame(frame.data(), 32, 32, res.rgba.data(), res.width,
                          res.height, pitch, x + side * 32, slot);
            }
        }
    }

    // ---- Pass 2: walls / objects, back-to-front (ascending i+j) ----
    for (int s = 0; s <= maxSum; ++s) {
        for (int i = 0; i < gw; ++i) {
            const int j = s - i;
            if (j < 0 || j >= gh)
                continue;
            const size_t idx = static_cast<size_t>(j) * gw + i;
            const MicroTileRef* m = microAt(idx);
            if (!m)
                continue;
            const int x = cellX(i, j);
            const int ct = colTop(i, j);

            // Non-floor cells: draw mt[0]/mt[1] with their REAL TileType at the
            // cell base (wall base / object), on top of the floor diamond.
            if (!isFloor(idx)) {
                for (int side = 0; side < 2; ++side) {
                    const LevelCelBlock blk = m->blocks[side];
                    if (!blockDrawable(blk, cel))
                        continue;
                    int fh = 0;
                    decodeReencodedFrame(cel, blk.frame() - 1, paletteAt(idx), frame, fh);
                    blitFrame(frame.data(), 32, 32, res.rgba.data(), res.width,
                              res.height, pitch, x + side * 32, slotTop(ct, 0));
                }
            }

            // Wall pairs mt[2..microLen-1], each pair one 32px slot higher.
            for (int b = 1; b < nRows; ++b) {
                const int slot = slotTop(ct, b);
                for (int side = 0; side < 2; ++side) {
                    const LevelCelBlock blk = m->blocks[2 * b + side];
                    if (!blockDrawable(blk, cel))
                        continue;
                    int fh = 0;
                    decodeReencodedFrame(cel, blk.frame() - 1, paletteAt(idx), frame, fh);
                    blitFrame(frame.data(), 32, 32, res.rgba.data(), res.width,
                              res.height, pitch, x + side * 32, slot);
                }
            }

            // Town dSpecial overlay (tree canopies, arches, etc.), drawn after
            // the cell content like DrawDungeon's pSpecialCels pass. The sprite
            // is bottom-anchored at the cell base.
            if (idx < opts.special.size() && opts.special[idx] != 0) {
                const size_t si = static_cast<size_t>(opts.special[idx] - 1);
                if (si < opts.specialSprites.size()) {
                    const int baseY = ct + Hm - 1;
                    const SpriteFrameRGBA& spr = opts.specialSprites[si];
                    const int sx = x + (64 - spr.width) / 2;
                    blitSpriteBottom(spr, res.rgba.data(), res.width, res.height,
                                     pitch, sx, baseY);
                }
            }
        }
    }

    return res;
}

} // namespace decoders
