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
// The isometric transform is worldToScreen:
//   screenX = (j - i) * 32
//   screenY = -(j + i) * 16      (base of the cell; stacks grow upward)
//
// Every pixel is filled with opts.background first, so the output has NO
// transparent gaps. Previously the buffer was left at (0,0,0,0), which any
// viewer composited as black, producing the "black blocks" / incomplete look.

#include "decoders/level.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

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
    p.minX = -(static_cast<long>(gw) - 1) * 32;
    p.minY = -(static_cast<long>(gw) + gh - 2) * 16 - Hm + 1;
    const long maxX = (static_cast<long>(gh) - 1) * 32 + 64;
    const long maxY = 0;
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

    // Screen position of a cell: x = left of the 64px diamond, colTop = top of
    // the whole (microLen/2 * 32) column. Slot b occupies rows
    // colTop + (Hm - (b+1)*32) .. +31, so b=0 (floor) is the bottom slot.
    const auto cellX = [&](int i, int j) {
        return static_cast<int>((static_cast<long>(j) - i) * 32 - proj.minX);
    };
    const auto colTop = [&](int i, int j) {
        const long baseY = -(static_cast<long>(j) + i) * 16 - proj.minY;
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

    const MicroTileRef* floorMicro =
        (opts.prefillMicro == kNoMicro || opts.prefillMicro >= micros.size())
            ? nullptr : &micros[opts.prefillMicro];

    const auto microAt = [&](size_t idx) -> const MicroTileRef* {
        const uint16_t mi = cellMicroIdx[idx];
        return (mi == kNoMicro) ? nullptr : &micros[mi];
    };

    // ---- Pass 1: floor diamonds for EVERY cell (complete ground plane) ----
    // Drawn far-to-near so nearer diamonds overlap farther ones correctly.
    // DevilutionX DrawFloorTile forces LeftTriangle / RightTriangle regardless
    // of the block's stored type; CopyRun writes opaque pixels, so even a frame
    // encoded as a square/trapezoid yields a filled (if stylized) diamond.
    // If a cell's own floor block is missing, fall back to the prefill floor.
    const int maxSum = (gw - 1) + (gh - 1);
    for (int s = maxSum; s >= 0; --s) {
        for (int i = 0; i < gw; ++i) {
            const int j = s - i;
            if (j < 0 || j >= gh)
                continue;
            const size_t idx = static_cast<size_t>(j) * gw + i;
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
                decodeReencodedFrameAs(cel, blk.frame() - 1,
                                       side == 0 ? TileType::LeftTriangle
                                                 : TileType::RightTriangle,
                                       pal, frame, fh);
                blitFrame(frame.data(), 32, 32, res.rgba.data(), res.width,
                          res.height, pitch, x + side * 32, slot);
            }
        }
    }

    // ---- Pass 2: walls / objects, back-to-front (descending i+j) ----
    for (int s = maxSum; s >= 0; --s) {
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
                    decodeReencodedFrame(cel, blk.frame() - 1, pal, frame, fh);
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
                    decodeReencodedFrame(cel, blk.frame() - 1, pal, frame, fh);
                    blitFrame(frame.data(), 32, 32, res.rgba.data(), res.width,
                              res.height, pitch, x + side * 32, slot);
                }
            }
        }
    }

    return res;
}

} // namespace decoders
