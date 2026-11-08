// src/decoders/level_decode.cpp
//
// Parsers for the Diablo 1 / Hellfire level files (.til / .min / .dun / .sol)
// and the megatile -> micro-tile grid assembly (DRLG_LPass3).

#include "decoders/level.h"

#include <algorithm>
#include <cstddef>
#include <stdexcept>

#include "decoders/endian_util.h"

namespace decoders {

namespace {

// Block source offset inside a MIN piece (DevilutionX SetDungeonMicros).
inline int minBlockSource(int block, int blocks)
{
    return (blocks - 2) + (block & 1) - (block & 0x0E);
}

} // namespace

std::vector<MegaTile> loadTil(const std::vector<uint8_t>& data)
{
    std::vector<MegaTile> out;
    if (data.size() < 8)
        return out;
    const size_t count = data.size() / 8;
    out.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        const uint8_t* p = data.data() + i * 8;
        MegaTile m;
        m.micro1 = LoadLE16(p + 0);
        m.micro2 = LoadLE16(p + 2);
        m.micro3 = LoadLE16(p + 4);
        m.micro4 = LoadLE16(p + 6);
        out.push_back(m);
    }
    return out;
}

std::vector<MicroTileRef> loadMin(const std::vector<uint8_t>& data,
                                 uint8_t blocks,
                                 uint8_t microLen)
{
    std::vector<MicroTileRef> out;
    if (data.size() < 2 || blocks == 0)
        return out;
    const size_t tileCount = data.size() / 2;
    const size_t pieceCount = tileCount / blocks;
    out.reserve(pieceCount);
    for (size_t piece = 0; piece < pieceCount; ++piece) {
        MicroTileRef ref;
        for (uint8_t b = 0; b < microLen && b < 16; ++b) {
            const int src = minBlockSource(b, blocks);
            const size_t idx = static_cast<size_t>(piece) * blocks + src;
            if (idx < tileCount) {
                ref.blocks[b].data = LoadLE16(data.data() + idx * 2);
            } else {
                ref.blocks[b].data = 0;
            }
        }
        out.push_back(ref);
    }
    return out;
}

DunFile loadDun(const std::vector<uint8_t>& data)
{
    DunFile out;
    if (data.size() < 4)
        return out;
    out.width = LoadLE16(data.data() + 0);
    out.height = LoadLE16(data.data() + 2);
    if (out.width == 0 || out.height == 0)
        return out;

    const size_t layerCount = static_cast<size_t>(out.width) * out.height;
    if (data.size() < 4 + layerCount * 2)
        return out; // truncated header/tile layer

    out.tileLayer.reserve(layerCount);
    for (size_t i = 0; i < layerCount; ++i)
        out.tileLayer.push_back(LoadLE16(data.data() + 4 + i * 2));

    // Transparency layer (micro-scale): located after the tile layer and three
    // other (2w*2h) layers. Read only if the file is large enough.
    const size_t microW = static_cast<size_t>(out.width) * 2;
    const size_t microH = static_cast<size_t>(out.height) * 2;
    const size_t transByteOffset = (2 + layerCount + 3 * microW * microH) * 2;
    const size_t transEnd = transByteOffset + microW * microH * 2;
    if (data.size() >= transEnd) {
        out.hasTransparency = true;
        out.transparency.reserve(microW * microH);
        for (size_t i = 0; i < microW * microH; ++i) {
            const uint16_t v = LoadLE16(data.data() + transByteOffset + i * 2);
            out.transparency.push_back(static_cast<int8_t>(v & 0xFF));
        }
    }
    return out;
}

std::vector<uint8_t> loadSol(const std::vector<uint8_t>& data)
{
    std::vector<uint8_t> out;
    if (data.empty())
        return out;
    // SOL is one byte per MICRO piece (dPiece index). The file is shorter than
    // MAXTILES; copy what is present and zero-fill the rest (DevilutionX loads
    // into a fixed SOLData[MAXTILES] array, leaving unused entries at 0).
    out.assign(MAXTILES, 0);
    const size_t n = data.size() < static_cast<size_t>(MAXTILES)
                         ? data.size() : static_cast<size_t>(MAXTILES);
    std::copy(data.data(), data.data() + n, out.data());
    return out;
}

void buildMicroGrid(const DunFile& dun,
                    const std::vector<MegaTile>& megatiles,
                    std::vector<uint16_t>& microGrid,
                    int& gw,
                    int& gh)
{
    gw = static_cast<int>(dun.width) * 2;
    gh = static_cast<int>(dun.height) * 2;
    // kNoMicro = truly outside the scene. `0` is a VALID micro index
    // (DPieceMicros[0]), so it cannot be used as the "empty" default.
    microGrid.assign(static_cast<size_t>(gw) * gh, kNoMicro);
    for (uint16_t j = 0; j < dun.height; ++j) {
        for (uint16_t i = 0; i < dun.width; ++i) {
            const uint16_t tileId = dun.tileLayer[static_cast<size_t>(j) * dun.width + i];
            if (tileId == 0)
                continue;
            if (tileId - 1 >= megatiles.size())
                continue;
            const MegaTile& m = megatiles[tileId - 1];
            const size_t base = static_cast<size_t>(j) * 2 * gw + static_cast<size_t>(i) * 2;
            // TIL megatile -> micro indices are copied VERBATIM and used
            // 0-based into `micros` (`micros[mi]`) by renderMap. Verified vs
            // DevilutionX gendung.cpp:607-610 / scrollrt.cpp:551-553 and real
            // l1.til (0..452) <-> l1.min (453 pieces).
            microGrid[base + 0] = m.micro1;
            microGrid[base + 1] = m.micro2;
            microGrid[base + gw] = m.micro3;
            microGrid[base + gw + 1] = m.micro4;
        }
    }
}

} // namespace decoders
