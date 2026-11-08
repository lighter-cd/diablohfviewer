// src/decoders/level.h
//
// Diablo 1 / Hellfire level (map) decoding and assembly.
//
// The level map is built from four data sources, all faithfully ported from
// DevilutionX (Source/levels/ + Source/engine/render/dun_render.cpp):
//
//   * .til  (MegaTile)      -> each megatile is 4 micro-tile indices (TL, TR, BL, BR)
//   * .min  (MicroTileRef)  -> per micro-tile, `blocks` CEL block references
//                              (LevelCelBlock: a 1-based frame index into the level
//                              .cel + a TileType). `blocks` is 10 normally, 16 for Hell.
//   * .cel  (the level CEL) -> the actual 32x32 micro-tile pixel frames.
//   * .dun  (DUN)           -> a grid of megatile indices (the floor layout) plus an
//                              optional transparency layer.
//   * .sol  (TileProperties)-> per megatile collision / lighting flags.
//
// Assembly (DRLG_LPass3): every megatile in the DUN expands to a 2x2 block of
// micro-tile indices in the `dPiece` grid. Each micro index is then drawn as a
// stack of up to `microLen` 32x32 CEL frames (microLen = 10 normally, 12 for Hell).
//
// Tile shapes follow DevilutionX's TileType: Square, TransparentSquare,
// LeftTriangle, RightTriangle, LeftTrapezoid, RightTrapezoid.

#pragma once

#include <cstdint>
#include <vector>

#include "decoders/endian_util.h"

namespace decoders {

// Mirrors DevilutionX TileType (dun_tile.hpp).
enum class TileType : uint8_t {
    Square = 0,
    TransparentSquare = 1,
    LeftTriangle = 2,
    RightTriangle = 3,
    LeftTrapezoid = 4,
    RightTrapezoid = 5,
};

// One megatile: its four micro-tile indices (TopLeft, TopRight, BottomLeft,
// BottomRight). Stored little-endian in the .til file as 4 x uint16.
struct MegaTile {
    uint16_t micro1 = 0; // TopLeft
    uint16_t micro2 = 0; // TopRight
    uint16_t micro3 = 0; // BottomLeft
    uint16_t micro4 = 0; // BottomRight
};

// A level-Cel block reference (DevilutionX LevelCelBlock). `data` is the raw
// 16-bit value: bits 12-14 = TileType, bits 0-11 = 1-based frame index.
struct LevelCelBlock {
    uint16_t data = 0;
    bool hasValue() const { return data != 0; }
    TileType type() const { return static_cast<TileType>((data & 0x7000) >> 12); }
    uint16_t frame() const { return static_cast<uint16_t>(data & 0x0FFF); } // 1-based
};

// Per micro-tile reference: up to 16 CEL blocks (index 0..microLen-1 are drawn).
struct MicroTileRef {
    LevelCelBlock blocks[16];
};

// Per megatile collision / lighting flags (DevilutionX TileProperties bitfield).
// We keep them as the raw uint8 bitmask; helpers below interpret them.
struct TileFlags {
    static constexpr uint8_t None = 0;
    static constexpr uint8_t Solid = 1 << 0;
    static constexpr uint8_t BlockLight = 1 << 1;
    static constexpr uint8_t BlockMissile = 1 << 2;
    static constexpr uint8_t Transparent = 1 << 3;
    static constexpr uint8_t TransparentLeft = 1 << 4;
    static constexpr uint8_t TransparentRight = 1 << 5;
    static constexpr uint8_t Trap = 1 << 7;
};

constexpr int MAXTILES = 1379;

// ---- Parsers ----------------------------------------------------------------

// Parse a .til buffer (each entry = 4 little-endian uint16). Returns one
// MegaTile per 8 bytes. On a short/empty buffer returns an empty vector.
std::vector<MegaTile> loadTil(const std::vector<uint8_t>& data);

// Parse a .min buffer. `blocks` is the stride per piece (10 normally, 16 for
// Hell). `microLen` is how many blocks are actually drawn (10 normally, 12 for
// Hell); only blocks 0..microLen-1 are read. Each piece's block b is taken from
// source offset `blocks - 2 + (b & 1) - (b & 0xE)` (DevilutionX's layout).
// Returns one MicroTileRef per piece.
std::vector<MicroTileRef> loadMin(const std::vector<uint8_t>& data,
                                 uint8_t blocks,
                                 uint8_t microLen);

// Parse a .dun buffer. Header = 2 little-endian uint16 (width, height). Then a
// (width*height) grid of little-endian uint16 megatile indices (0 = empty).
// If the file is large enough it also carries a transparency layer at
//   2 + (width*height) + 3 * ((2*width)*(2*height))
// (micro-scale), which is read if present.
struct DunFile {
    uint16_t width = 0;
    uint16_t height = 0;
    std::vector<uint16_t> tileLayer; // width*height, 0 = empty
    bool hasTransparency = false;
    std::vector<int8_t> transparency; // (2*width)*(2*height), -1 = solid/unset
};
DunFile loadDun(const std::vector<uint8_t>& data);

// Parse a .sol buffer -> MAXTILES flags. Returns an empty vector if the buffer
// does not contain MAXTILES bytes; otherwise exactly MAXTILES bytes.
std::vector<uint8_t> loadSol(const std::vector<uint8_t>& data);

// Assemble the micro-tile grid (the `dPiece` grid) from the DUN megatile layer
// and the megatile table (TIL). The grid is (2*width) x (2*height); each cell
// holds the TIL megatile -> micro index, which is 1-based into `micros`
// (0 = empty), matching DevilutionX's 1-based CEL/MIN convention. renderMap
// reads `micros[mi - 1]` and treats mi == 0 as an empty cell. `gw`/`gh` are set
// to 2*width / 2*height. Megatile index i in the DUN is 1-based into
// `megatiles`.
void buildMicroGrid(const DunFile& dun,
                    const std::vector<MegaTile>& megatiles,
                    std::vector<uint16_t>& microGrid,
                    int& gw,
                    int& gh);

// ---- Level CEL re-encode / frame decode ----------------------------------
//
// A raw level .cel is a standard CEL (uint32 numFrames + (numFrames+1) offsets
// + frames). Each frame is a 32xH micro-tile primitive whose on-disk layout
// depends on its TileType. We re-encode every frame into a fixed-layout buffer
// (DevilutionX's ReencodeDungeonCels) so the renderer can read it directly:
//   - Square          -> 1024 raw palette indices
//   - TransparentSquare-> kept as-is RLE
//   - Triangle        -> 512 bytes, runs packed bottom-to-top, no bloat
//   - Trapezoid       -> 784 bytes (triangle run + flat 32x16 rectangle)
// The re-encoded buffer layout is: uint32 numFrames, then (numFrames+1) uint32
// relative offsets (the last one is the total size), then the frames.

struct ReencodedLevelCel {
    uint32_t numFrames = 0;
    std::vector<TileType> frameTypes;
    std::vector<uint8_t> buffer;

    // Relative byte offset of frame `index` into `buffer` (0-based). The entry
    // at index `numFrames` holds the total buffer size.
    size_t frameOffsetsRel(uint32_t index) const
    {
        return static_cast<size_t>(LoadLE32(buffer.data() + 4 + static_cast<size_t>(index) * 4));
    }
};

// Re-encode a raw level CEL into the fixed-layout form. `frameTypes` maps frame
// index -> TileType (frames not listed default to Square).
ReencodedLevelCel reencodeLevelCel(const std::vector<uint8_t>& rawCel,
                                   const std::vector<TileType>& frameTypes);

// Decode one re-encoded frame into a 32 x H RGBA buffer (H = 32 for
// square/trapezoid, 31 for triangle). Returns the pixel width (32); `outH` is
// set to the used height.
int decodeReencodedFrame(const ReencodedLevelCel& cel, uint32_t frameIndex,
                         const uint8_t* pal, std::vector<uint8_t>& rgba, int& outH);

// Build the per-frame TileType table needed by reencodeLevelCel, from every
// CEL block referenced by every micro-tile (DevilutionX's "frame type is a
// property of the CEL frame"). Frames not referenced default to Square.
std::vector<TileType> buildFrameTypes(const std::vector<MicroTileRef>& micros,
                                     int microLen);

// ---- Composite map rendering --------------------------------------------
//
// Faithful port of DevilutionX's DrawCell + worldToScreen: a micro-tile is drawn
// as a stack of up to `microLen/2` rows of two 32x32 CEL frames (left at the
// base, right at +32px X), each row 32px higher; the CEL row 0 is drawn at the
// BOTTOM of each frame (vertical flip), matching the engine.

struct MapRenderResult {
    int width = 0;   // output RGBA width
    int height = 0;  // output RGBA height
    std::vector<uint8_t> rgba;       // width*height*4, game-oriented
    int gridW = 0;                    // micro grid width
    int gridH = 0;                    // micro grid height
    std::vector<uint8_t> solid;       // gridW*gridH (0/1), from SOL
    std::vector<uint8_t> blockLight;  // gridW*gridH (0/1)
    std::vector<uint8_t> blockMissile;// gridW*gridH (0/1)
    std::vector<uint8_t> transparent; // gridW*gridH (0/1), from DUN micro-scale
};

// Render a single micro-tile (one dPiece cell) into a 64 x H RGBA column
// (H = microLen/2 * 32), game-oriented. `pal` is a 256-entry RGBA table (1024
// bytes) from paletteToRGBA. Used directly by tests and by renderMap.
void renderMicroTile(const ReencodedLevelCel& cel, const MicroTileRef& micro,
                     const uint8_t* pal, int microLen,
                     std::vector<uint8_t>& rgba, int& outW, int& outH);

// Render a whole level map from the micro grid. `microGrid` is the (gw x gh)
// 1-based micro-index grid (0 = empty) from buildMicroGrid. `tileLayer`/
// `tileW`/`tileH` map each micro cell back to its megatile (1-based) so SOL flags
// can be attached; `solFlags` is the optional per-megatile SOL table (MAXTILES);
// `transparency` is the optional DUN micro-scale transparency layer. `pal` is a
// 256-entry RGBA table (1024 bytes) from paletteToRGBA, used to color the frames.
MapRenderResult renderMap(const ReencodedLevelCel& cel,
                          const std::vector<MicroTileRef>& micros,
                          const std::vector<uint16_t>& microGrid, int gw, int gh,
                          const std::vector<uint8_t>& solFlags,
                          const std::vector<int8_t>& transparency,
                          const std::vector<uint16_t>& tileLayer, int tileW, int tileH,
                          const uint8_t* pal, int microLen);

} // namespace decoders
