// src/decoders/level_render.cpp
//
// Composite level-map rendering, ported from DevilutionX's DrawCell +
// worldToScreen. A micro-tile (one dPiece cell) is drawn as a stack of up to
// `microLen/2` rows of two 32x32 CEL frames (left block at the base, right
// block at +32px X), each row 32px higher. The CEL row 0 is drawn at the BOTTOM
// of each frame (vertical flip), exactly as the engine does. The whole map is
// composited with the isometric transform:
//
//   screenX = (j - i) * 32
//   screenY = -(j + i) * 16      (base of the cell; stacks grow upward)
//
// Cells are painted back-to-front (descending i+j) so nearer tiles overwrite
// farther ones.

#include "decoders/level.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

#include "decoders/endian_util.h"

namespace decoders {

namespace {

// Blit a decoded 32x32 frame into `dst` (RGBA, row-major, `dstPitch` bytes/row)
// at column offset `dx` and slot top `slotTopY`. The frame is written with its
// CEL row 0 at the bottom of the slot (vertical flip), matching the engine.
// Transparent pixels (alpha == 0) are skipped so lower layers show through.
void blitFrame(const uint8_t* frame, int frameW, int frameH,
               uint8_t* dst, int dstPitch, int dx, int slotTopY)
{
    for (int fr = 0; fr < frameH; ++fr) {
        const int dr = slotTopY + (frameH - 1 - fr); // vertical flip
        if (dr < 0)
            continue;
        const uint8_t* src = frame + static_cast<size_t>(fr) * frameW * 4;
        uint8_t* row = dst + static_cast<size_t>(dr) * dstPitch + static_cast<size_t>(dx) * 4;
        for (int fx = 0; fx < frameW; ++fx) {
            const uint8_t* p = src + fx * 4;
            if (p[3] != 0)
                std::memcpy(row + fx * 4, p, 4);
        }
    }
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
        const int slotTopY = H - (b + 1) * 32; // top of this row's 32px slot
        // Left block.
        {
            const LevelCelBlock blk = micro.blocks[2 * b];
            if (blk.hasValue()) {
                int fh = 0;
                decodeReencodedFrame(cel, blk.frame() - 1, pal, frame, fh);
                blitFrame(frame.data(), 32, 32, rgba.data(), pitch, 0, slotTopY);
            }
        }
        // Right block (+32px X).
        {
            const LevelCelBlock blk = micro.blocks[2 * b + 1];
            if (blk.hasValue()) {
                int fh = 0;
                decodeReencodedFrame(cel, blk.frame() - 1, pal, frame, fh);
                blitFrame(frame.data(), 32, 32, rgba.data(), pitch, 32, slotTopY);
            }
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
                          const std::vector<uint16_t>& tileLayer, int tileW, int tileH,
                          const uint8_t* pal, int microLen)
{
    MapRenderResult res;
    res.gridW = gw;
    res.gridH = gh;
    const size_t cells = static_cast<size_t>(gw) * gh;
    res.solid.assign(cells, 0);
    res.blockLight.assign(cells, 0);
    res.blockMissile.assign(cells, 0);
    res.transparent.assign(cells, 0);

    const int nRows = microLen / 2;
    const int Hm = nRows * 32;

    // Bounding box in screen space (long to avoid overflow for large grids).
    const long minX = -(static_cast<long>(gw) - 1) * 32;
    const long minY = -(static_cast<long>(gw) + gh - 2) * 16 - Hm + 1;
    const long maxX = (static_cast<long>(gh) - 1) * 32 + 64;
    const long maxY = 0;
    const long width = maxX - minX + 1;
    const long height = maxY - minY + 1;

    res.width = static_cast<int>(width);
    res.height = static_cast<int>(height);
    res.rgba.assign(static_cast<size_t>(width) * static_cast<size_t>(height) * 4, 0);
    const int pitch = res.width * 4;
    const long HmLong = Hm;
    const long minXLong = minX;

    std::vector<uint8_t> frame(32 * 32 * 4);

    // Painter's order: draw nearest cells first (largest i+j, bottom of the
    // screen) and farther cells last (smallest i+j, top). In this level's
    // isometric projection the farther cells sit higher on screen and their wall
    // bases are correctly occluded by the nearer cells drawn underneath.
    const int maxSum = (gw - 1) + (gh - 1);
    for (int s = maxSum; s >= 0; --s) {
        for (int i = 0; i < gw; ++i) {
            const int j = s - i;
            if (j < 0 || j >= gh)
                continue;
            const size_t idx = static_cast<size_t>(j) * gw + i;
            const uint16_t mi = microGrid[idx];
            if (mi == 0)
                continue;

            const MicroTileRef& micro = micros[mi - 1];
            const long baseX = (static_cast<long>(j) - i) * 32 - minXLong;
            const long baseY = -(static_cast<long>(j) + i) * 16 - minY;
            const long colTop = baseY - HmLong + 1;

            // Collision / lighting / transparency metadata for this cell.
            uint8_t flags = 0;
            if (i < 2 * tileW && j < 2 * tileH) {
                const uint16_t mega = tileLayer[static_cast<size_t>(j / 2) * tileW + (i / 2)];
                if (mega > 0 && !solFlags.empty() && mega - 1 < solFlags.size())
                    flags = solFlags[mega - 1];
            }
            res.solid[idx] = (flags & TileFlags::Solid) ? 1 : 0;
            res.blockLight[idx] = (flags & TileFlags::BlockLight) ? 1 : 0;
            res.blockMissile[idx] = (flags & TileFlags::BlockMissile) ? 1 : 0;
            if (!transparency.empty() && idx < transparency.size()) {
                const int8_t t = transparency[idx];
                // DUN transparency layer: 0 = fully solid, > 0 = transparent.
                res.transparent[idx] = (t > 0) ? 1 : 0;
            }

            // Draw the block stack back-to-front within the cell.
            for (int b = 0; b < nRows; ++b) {
                const long slotTopY = colTop + (HmLong - (b + 1) * 32);
                // Left block at column x = 0.
                {
                    const LevelCelBlock blk = micro.blocks[2 * b];
                    if (blk.hasValue()) {
                        int fh = 0;
                        decodeReencodedFrame(cel, blk.frame() - 1, pal, frame, fh);
                        blitFrame(frame.data(), 32, 32, res.rgba.data(), pitch,
                                  static_cast<int>(baseX), static_cast<int>(slotTopY));
                    }
                }
                // Right block at column x = 32.
                {
                    const LevelCelBlock blk = micro.blocks[2 * b + 1];
                    if (blk.hasValue()) {
                        int fh = 0;
                        decodeReencodedFrame(cel, blk.frame() - 1, pal, frame, fh);
                        blitFrame(frame.data(), 32, 32, res.rgba.data(), pitch,
                                  static_cast<int>(baseX) + 32, static_cast<int>(slotTopY));
                    }
                }
            }
        }
    }

    return res;
}

} // namespace decoders
