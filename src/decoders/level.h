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

// Sentinel for a micro cell that lies OUTSIDE the scene (no tile at all).
// DevilutionX uses 0 as a perfectly valid micro index (DPieceMicros[0]), so 0
// cannot double as "empty": we need an explicit out-of-band value.
constexpr uint16_t kNoMicro = 0xFFFF;

// Options for the composite map render. Defaults produce a fully opaque,
// faithfully-composited scene (DevilutionX two-pass: floors, then walls).
struct MapRenderOptions {
    // Background RGBA. DevilutionX clears the view to black; filling every pixel
    // (rather than leaving alpha=0) is what removes the "black blocks" that a
    // viewer shows for transparent gaps.
    uint8_t background[4] = { 0, 0, 0, 255 };
    // A cell whose micro index is kNoMicro is drawn with this micro instead of
    // being skipped. DevilutionX prefills every dPiece cell with a floor micro,
    // so a real scene has no empty cells. Set to kNoMicro to leave holes.
    uint16_t prefillMicro = kNoMicro;
};

// Isometric bounding box of a (gw x gh) micro grid with `microLen` blocks per
// micro. Shared by the renderer and the scene overlay so both place cells with
// exactly the same projection.
struct SceneProjection {
    long minX = 0, minY = 0, width = 0, height = 0;
};
SceneProjection sceneProjection(int gw, int gh, int microLen);

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

// Parse a .sol buffer -> MAXTILES flags, indexed by MICRO piece id (verified
// against DevilutionX `SOLData[dPiece[x][y]]`, dun_tile_data.hpp:219-221).
// The on-disk file is only `pieceCount` bytes (l1.sol=453, town.sol=1258), so
// it is copied into a MAXTILES-sized, zero-filled table; missing entries stay 0.
// Returns an empty vector only for an empty input.
std::vector<uint8_t> loadSol(const std::vector<uint8_t>& data);

// Assemble the micro-tile grid (the `dPiece` grid) from the DUN megatile layer
// and the megatile table (TIL). The grid is (2*width) x (2*height); each cell
// holds the TIL megatile -> micro index.
//
// INDEXING (verified against DevilutionX `gendung.cpp:607-610`, `town.cpp:52-55`,
// `scrollrt.cpp:551-553`): the micro index is copied VERBATIM from TIL and used
// 0-based into `micros` (`micros[mi]`). `0` is a perfectly valid micro index
// (DPieceMicros[0]) — it is NOT "empty". The only "empty" marker is the
// out-of-band sentinel `kNoMicro`. This is a DIFFERENT convention from
// `LevelCelBlock.frame()`, which is 1-based into CEL frames; the two must not
// be conflated. Verified with real data: l1.til micros 0..452 <-> l1.min 453
// pieces (0..452); town.til micros 0..1257 <-> town.min 1258 pieces.
// Megatile ids in the DUN layer are 1-based into `megatiles` (0 = empty).
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

// Decode a frame forcing a specific TileType (DevilutionX DrawFloorTile always
// renders mt[0]/mt[1] as Left/RightTriangle regardless of the block's own type
// bits). Same layout as decodeReencodedFrame, but the shape decoder is chosen
// by `forced` instead of cel.frameTypes[frameIndex].
int decodeReencodedFrameAs(const ReencodedLevelCel& cel, uint32_t frameIndex,
                           TileType forced, const uint8_t* pal,
                           std::vector<uint8_t>& rgba, int& outH);

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
// 0-based micro-index grid (kNoMicro = truly outside the scene) from
// buildMicroGrid / buildTownScene. `solFlags` is the optional per-MICRO SOL
// table (MAXTILES bytes, indexed by the resolved micro piece id, as in
// DevilutionX `SOLData[dPiece[x][y]]`); when empty, no SOL metadata is
// produced. `transparency` is the optional DUN micro-scale transparency layer.
// `pal` is a 256-entry RGBA table (1024 bytes) from paletteToRGBA.
//
// Faithful to DevilutionX DrawGame: Pass 1 draws every cell's floor as two
// triangles (mt[0]=LeftTriangle, mt[1]=RightTriangle) forming a 64x32 diamond;
// Pass 2 draws walls (mt[2..microLen-1]) back-to-front, each pair 32px higher.
// Blocks with !hasValue() are skipped. Every pixel is filled with
// opts.background first, so the result has no transparent gaps.
MapRenderResult renderMap(const ReencodedLevelCel& cel,
                          const std::vector<MicroTileRef>& micros,
                          const std::vector<uint16_t>& microGrid, int gw, int gh,
                          const std::vector<uint8_t>& solFlags,
                          const std::vector<int8_t>& transparency,
                          const uint8_t* pal, int microLen,
                          const MapRenderOptions& opts = {});

} // namespace decoders
