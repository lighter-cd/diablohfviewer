#include "decoders/dungeon_gen.h"

#include <limits>

namespace dhfc {

uint32_t DiabloRng::advance()
{
    // std::linear_congruential_engine<uint32_t, 0x015A4E35, 1, 0>: modulo 2^32.
    state_ = state_ * UINT32_C(0x015A4E35) + 1;
    return state_;
}

int32_t DiabloRng::generateRnd(int32_t v)
{
    if (v <= 0)
        return 0;

    uint32_t r = advance();
    // AdvanceRndSeed: abs() of the signed interpretation, INT_MIN passes through.
    const int32_t signedValue = static_cast<int32_t>(r);
    int32_t value;
    if (signedValue == std::numeric_limits<int32_t>::min())
        value = std::numeric_limits<int32_t>::min();
    else
        value = signedValue < 0 ? -signedValue : signedValue;

    if (v <= 0x7FFF)
        return (value >> 16) % v;
    return value % v;
}

bool DiabloRng::flipCoin(unsigned frequency)
{
    return generateRnd(static_cast<int32_t>(frequency)) == 0;
}

void DiabloRng::discard(unsigned count)
{
    while (count-- != 0)
        advance();
}

DunTileLayer parseDunTileLayer(const std::vector<uint8_t>& bytes)
{
    DunTileLayer layer;
    if (bytes.size() < 4)
        return layer;

    const auto readU16 = [&bytes](size_t wordIndex) -> uint16_t {
        const size_t offset = wordIndex * 2;
        return static_cast<uint16_t>(bytes[offset] | (static_cast<uint16_t>(bytes[offset + 1]) << 8));
    };

    layer.width = readU16(0);
    layer.height = readU16(1);
    if (layer.width <= 0 || layer.height <= 0)
        return layer;

    const size_t count = static_cast<size_t>(layer.width) * static_cast<size_t>(layer.height);
    if (bytes.size() < 4 + count * 2)
        return layer;

    layer.tiles.resize(count);
    for (size_t i = 0; i < count; ++i)
        layer.tiles[i] = readU16(2 + i);
    return layer;
}

} // namespace dhfc
