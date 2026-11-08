// src/decoders/level_cel.cpp
//
// Level CEL handling for the map renderer.
//
// A level .cel is a standard CEL: a little-endian uint32 frame count followed by
// (count+1) little-endian uint32 offsets, then the frame pixel data. Each frame
// is a 32x32 micro-tile primitive, but its on-disk layout depends on the tile
// shape (DevilutionX TileType):
//
//   * Square          -> 1024 raw palette indices (no compression)
//   * TransparentSquare-> RLE (control byte: >= 0x80 = transparent run of -byte
//                          pixels, otherwise `byte` literal pixels follow)
//   * Left/RightTriangle, Left/RightTrapezoid
//                     -> the rows carry 2 "bloat" padding bytes before/after
//                        every even row; the reencode step strips those so the
//                        renderer can read the triangle runs directly.
//
// We re-encode every frame 1:1 (keeping the original frame index, so a
// LevelCelBlock.frame()-1 still addresses it) and then decode each frame into a
// 32 x H RGBA buffer (H = 32 for square/trapezoid, 31 for triangle). This is a
// faithful port of DevilutionX's ReencodeDungeonCels + RenderTileFrame (FullyLit
// path; we do not apply per-pixel lighting or the half-tile transparency masks,
// which are lighting/visibility effects rather than tile shape).

#include "decoders/level.h"

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>

#include "decoders/endian_util.h"

namespace decoders {

namespace {

constexpr int kTileWidth = 32;     // DunFrameWidth
constexpr int kTileHeight = 32;    // DunFrameHeight
constexpr int kTriangleUpperHeight = 15; // DunFrameHeight/2 - 1
constexpr int kXStep = 2;          // pixels per triangle row step (palette units)
constexpr int kDstPitch = kTileWidth * 4; // RGBA stride
constexpr int kXStepRGBA = kXStep * 4;

// ---- reencode: strip triangle/trapezoid bloat ----------------------------

void ReencodeLeftTriangleLower(const uint8_t*& src, uint8_t*& dst)
{
    unsigned width = 0;
    for (unsigned i = 0; i < 8; ++i) {
        src += 2; // skip the two zero "bloat" bytes
        width += 2;
        std::memcpy(dst, src, width);
        src += width;
        dst += width;
        width += 2;
        std::memcpy(dst, src, width);
        src += width;
        dst += width;
    }
}

void ReencodeLeftTriangle(const uint8_t*& src, uint8_t*& dst)
{
    ReencodeLeftTriangleLower(src, dst);
    unsigned width = kTileWidth;
    for (unsigned i = 0; i < 7; ++i) {
        src += 2; // bloat
        width -= 2;
        std::memcpy(dst, src, width);
        src += width;
        dst += width;
        width -= 2;
        std::memcpy(dst, src, width);
        src += width;
        dst += width;
    }
    src += 2; // bloat
    width -= 2;
    std::memcpy(dst, src, width);
    dst += width;
}

void ReencodeRightTriangleLower(const uint8_t*& src, uint8_t*& dst)
{
    unsigned width = 0;
    for (unsigned i = 0; i < 8; ++i) {
        width += 2;
        std::memcpy(dst, src, width);
        src += width + 2; // skip bloat
        dst += width;
        width += 2;
        std::memcpy(dst, src, width);
        src += width;
        dst += width;
    }
}

void ReencodeRightTriangle(const uint8_t*& src, uint8_t*& dst)
{
    ReencodeRightTriangleLower(src, dst);
    unsigned width = kTileWidth;
    for (unsigned i = 0; i < 7; ++i) {
        width -= 2;
        std::memcpy(dst, src, width);
        src += width + 2; // bloat
        dst += width;
        width -= 2;
        std::memcpy(dst, src, width);
        src += width;
        dst += width;
    }
    width -= 2;
    std::memcpy(dst, src, width);
    dst += width;
}

void ReencodeLeftTrapezoid(const uint8_t*& src, uint8_t*& dst)
{
    ReencodeLeftTriangleLower(src, dst);
    std::memcpy(dst, src, kTileWidth * 16);
    dst += kTileWidth * 16;
}

void ReencodeRightTrapezoid(const uint8_t*& src, uint8_t*& dst)
{
    ReencodeRightTriangleLower(src, dst);
    std::memcpy(dst, src, kTileWidth * 16);
    dst += kTileWidth * 16;
}

// ---- decode: render a re-encoded frame into a 32 x H RGBA buffer -----------

inline void CopyRun(uint8_t* dst, const uint8_t* src, unsigned n, const uint8_t* pal)
{
    // `pal` is a 256-entry RGBA table (4 bytes/entry), produced by
    // paletteToRGBA. Index with a 4-byte stride.
    for (unsigned i = 0; i < n; ++i) {
        const uint8_t idx = src[i];
        const size_t o = static_cast<size_t>(i) * 4;
        const size_t pi = static_cast<size_t>(idx) * 4;
        dst[o] = pal[pi];
        dst[o + 1] = pal[pi + 1];
        dst[o + 2] = pal[pi + 2];
        dst[o + 3] = 255;
    }
}

inline void SkipRun(uint8_t*& dst, unsigned n)
{
    dst += n * 4;
}

// Square / Solid: copy 32x32 raw indices.
void DecodeSquare(const uint8_t* src, uint8_t* rgba, const uint8_t* pal)
{
    for (int y = 0; y < kTileHeight; ++y) {
        CopyRun(rgba + static_cast<size_t>(y) * kDstPitch, src + static_cast<size_t>(y) * kTileWidth, kTileWidth, pal);
    }
}

// TransparentSquare: RLE, Solid mask (transparent runs stay transparent).
void DecodeTransparentSquare(const uint8_t* src, const uint8_t* srcEnd, uint8_t* rgba, const uint8_t* pal)
{
    uint8_t* dst = rgba;
    for (int y = 0; y < kTileHeight && src < srcEnd; ++y) {
        int drawWidth = kTileWidth;
        while (drawWidth > 0 && src < srcEnd) {
            int8_t v = static_cast<int8_t>(*src++);
            if (v > 0) {
                if (src + v > srcEnd)
                    break;
                CopyRun(dst, src, static_cast<unsigned>(v), pal);
                src += v;
                dst += v * 4;
            } else {
                unsigned t = static_cast<unsigned>(-v);
                SkipRun(dst, t);
            }
            drawWidth -= v < 0 ? static_cast<int>(static_cast<unsigned>(-v)) : v;
        }
    }
}

// Write `width` palette-index pixels from `src` into row `rowIndex` of a 32-wide
// RGBA buffer at horizontal offset `xOff` (palette pixels).
inline void WriteRow(std::vector<uint8_t>& rgba, int rowIndex, int xOff, int width,
                     const uint8_t* src, const uint8_t* pal)
{
    if (width <= 0 || xOff < 0 || xOff + width > kTileWidth)
        return;
    uint8_t* dst = rgba.data() + static_cast<size_t>(rowIndex) * kDstPitch + static_cast<size_t>(xOff) * 4;
    CopyRun(dst, src, static_cast<unsigned>(width), pal);
}

// Decode a triangle/trapezoid frame (already re-encoded into packed runs) into a
// 32 x 32 RGBA buffer (top-to-bottom). Left/Right differ by horizontal placement;
// rows are 0..31 with 31 = bottom of the tile. Floor diamonds are two triangles.
void DecodeLeftTriangle(const uint8_t*& src, std::vector<uint8_t>& rgba, const uint8_t* pal)
{
    // Lower 16 rows (k=0 bottom .. k=15 top of lower): widths 2,4,...,32, shifted
    // left by 2px each step up (solid on the left). Buffer rows 31..16.
    for (int k = 0; k < 16; ++k) {
        const int width = 2 * (k + 1);
        const int xOff = 30 - 2 * k;
        const int rowIndex = 31 - k;
        WriteRow(rgba, rowIndex, xOff, width, src, pal);
        src += width;
    }
    // Upper 15 rows (k=0 .. k=14): widths 30,28,...,2, shifted right by 2px each
    // step up. Buffer rows 15..1 (k=0 at row 15; the lower apex row 16 holds the
    // full-width line).
    for (int k = 0; k < 15; ++k) {
        const int width = 30 - 2 * k;
        const int xOff = 2 + 2 * k;
        const int rowIndex = 15 - k;
        WriteRow(rgba, rowIndex, xOff, width, src, pal);
        src += width;
    }
}

void DecodeRightTriangle(const uint8_t*& src, std::vector<uint8_t>& rgba, const uint8_t* pal)
{
    for (int k = 0; k < 16; ++k) {
        const int width = 2 * (k + 1);
        const int rowIndex = 31 - k;
        WriteRow(rgba, rowIndex, 0, width, src, pal);
        src += width;
    }
    for (int k = 0; k < 15; ++k) {
        const int width = 30 - 2 * k;
        const int rowIndex = 15 - k;
        WriteRow(rgba, rowIndex, 0, width, src, pal);
        src += width;
    }
}

void DecodeLeftTrapezoid(const uint8_t*& src, std::vector<uint8_t>& rgba, const uint8_t* pal)
{
    // Triangle lower (rows 31..16) + flat 32x16 rectangle on top (rows 15..0).
    for (int k = 0; k < 16; ++k) {
        const int width = 2 * (k + 1);
        const int xOff = 30 - 2 * k;
        const int rowIndex = 31 - k;
        WriteRow(rgba, rowIndex, xOff, width, src, pal);
        src += width;
    }
    for (int y = 15; y >= 0; --y) {
        WriteRow(rgba, y, 0, kTileWidth, src, pal);
        src += kTileWidth;
    }
}

void DecodeRightTrapezoid(const uint8_t*& src, std::vector<uint8_t>& rgba, const uint8_t* pal)
{
    for (int k = 0; k < 16; ++k) {
        const int width = 2 * (k + 1);
        const int rowIndex = 31 - k;
        WriteRow(rgba, rowIndex, 0, width, src, pal);
        src += width;
    }
    for (int y = 15; y >= 0; --y) {
        WriteRow(rgba, y, 0, kTileWidth, src, pal);
        src += kTileWidth;
    }
}

} // namespace

// ---- public API ------------------------------------------------------------

struct RawCel {
    uint32_t numFrames = 0;
    std::vector<uint32_t> offsets; // absolute byte offsets into `data`
    const uint8_t* data = nullptr;
    size_t size = 0;
};

static RawCel parseRawCel(const std::vector<uint8_t>& data)
{
    RawCel c;
    if (data.size() < 4)
        return c;
    c.data = data.data();
    c.size = data.size();
    c.numFrames = LoadLE32(data.data());
    if (c.numFrames == 0 || c.numFrames > 100000)
        return c;
    const size_t need = 4 + (static_cast<size_t>(c.numFrames) + 1) * 4;
    if (data.size() < need)
        return c;
    c.offsets.reserve(c.numFrames + 1);
    for (uint32_t i = 0; i <= c.numFrames; ++i)
        c.offsets.push_back(LoadLE32(data.data() + 4 + i * 4));
    return c;
}

// Re-encode every frame 1:1, keeping the original frame index. `frameTypes`
// maps frame index -> TileType (default Square for unassigned frames). Returns
// the re-encoded CEL buffer (numFrames, offsets, frames) plus the per-frame type.
ReencodedLevelCel reencodeLevelCel(const std::vector<uint8_t>& rawCel,
                                   const std::vector<TileType>& frameTypes)
{
    ReencodedLevelCel out;
    RawCel c = parseRawCel(rawCel);
    out.numFrames = c.numFrames;
    if (c.numFrames == 0)
        return out;
    out.frameTypes.assign(c.numFrames, TileType::Square);
    for (size_t i = 0; i < frameTypes.size() && i < c.numFrames; ++i)
        out.frameTypes[i] = frameTypes[i];

    // First pass: compute output size.
    std::vector<size_t> frameSizes(c.numFrames, 0);
    for (uint32_t f = 0; f < c.numFrames; ++f) {
        const uint8_t* src = c.data + c.offsets[f];
        const uint8_t* srcEnd = c.data + c.offsets[f + 1];
        size_t sz = srcEnd - src;
        switch (out.frameTypes[f]) {
        case TileType::Square: sz = kTileWidth * kTileHeight; break;
        case TileType::TransparentSquare: break; // keep RLE as-is
        case TileType::LeftTriangle:
        case TileType::RightTriangle: sz = 512; break; // ReencodedTriangleFrameSize
        case TileType::LeftTrapezoid:
        case TileType::RightTrapezoid: sz = 784; break; // ReencodedTrapezoidFrameSize
        }
        frameSizes[f] = sz;
    }
    size_t total = (2 + c.numFrames) * 4;
    for (uint32_t f = 0; f < c.numFrames; ++f)
        total += frameSizes[f];

    out.buffer.resize(total);
    WriteLE32(out.buffer.data(), c.numFrames);
    uint8_t* lookup = out.buffer.data() + 4;
    uint8_t* o = out.buffer.data() + (2 + c.numFrames) * 4;
    for (uint32_t f = 0; f < c.numFrames; ++f) {
        WriteLE32(lookup, static_cast<uint32_t>(o - out.buffer.data()));
        lookup += 4;
        const uint8_t* src = c.data + c.offsets[f];
        const uint8_t* srcEnd = c.data + c.offsets[f + 1];
        switch (out.frameTypes[f]) {
        case TileType::Square:
            // Copy exactly kTileWidth*kTileHeight raw bytes (clamp if shorter).
            {
                size_t n = (srcEnd - src < kTileWidth * kTileHeight) ? (srcEnd - src) : static_cast<size_t>(kTileWidth * kTileHeight);
                std::memcpy(o, src, n);
                o += kTileWidth * kTileHeight;
            }
            break;
        case TileType::TransparentSquare:
            {
                size_t n = srcEnd - src;
                std::memcpy(o, src, n);
                o += n;
            }
            break;
        case TileType::LeftTriangle: ReencodeLeftTriangle(src, o); break;
        case TileType::RightTriangle: ReencodeRightTriangle(src, o); break;
        case TileType::LeftTrapezoid: ReencodeLeftTrapezoid(src, o); break;
        case TileType::RightTrapezoid: ReencodeRightTrapezoid(src, o); break;
        }
    }
    WriteLE32(lookup, static_cast<uint32_t>(total));
    return out;
}

// Decode one re-encoded frame into a 32 x H RGBA buffer using the given shape.
// Shared by decodeReencodedFrame (per-frame type) and decodeReencodedFrameAs
// (forced type, used for floor triangles).
static int decodeAs(const ReencodedLevelCel& cel, uint32_t frameIndex,
                    TileType shape, const uint8_t* pal,
                    std::vector<uint8_t>& rgba, int& outH)
{
    rgba.assign(static_cast<size_t>(kTileWidth) * kTileHeight * 4, 0);
    outH = kTileHeight;
    if (frameIndex >= cel.numFrames)
        return 0;
    const uint8_t* src = cel.buffer.data() + cel.frameOffsetsRel(frameIndex);
    switch (shape) {
    case TileType::Square: DecodeSquare(src, rgba.data(), pal); break;
    case TileType::TransparentSquare:
        {
            const uint8_t* srcEnd = cel.buffer.data() + cel.frameOffsetsRel(frameIndex + 1);
            DecodeTransparentSquare(src, srcEnd, rgba.data(), pal);
        }
        break;
    case TileType::LeftTriangle: DecodeLeftTriangle(src, rgba, pal); break;
    case TileType::RightTriangle: DecodeRightTriangle(src, rgba, pal); break;
    case TileType::LeftTrapezoid: DecodeLeftTrapezoid(src, rgba, pal); break;
    case TileType::RightTrapezoid: DecodeRightTrapezoid(src, rgba, pal); break;
    }
    return kTileWidth;
}

int decodeReencodedFrame(const ReencodedLevelCel& cel, uint32_t frameIndex,
                         const uint8_t* pal, std::vector<uint8_t>& rgba, int& outH)
{
    if (frameIndex >= cel.numFrames)
        return decodeAs(cel, frameIndex, TileType::Square, pal, rgba, outH);
    return decodeAs(cel, frameIndex, cel.frameTypes[frameIndex], pal, rgba, outH);
}

int decodeReencodedFrameAs(const ReencodedLevelCel& cel, uint32_t frameIndex,
                           TileType forced, const uint8_t* pal,
                           std::vector<uint8_t>& rgba, int& outH)
{
    return decodeAs(cel, frameIndex, forced, pal, rgba, outH);
}

} // namespace decoders
