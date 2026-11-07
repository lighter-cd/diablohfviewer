// src/decoders/endian_util.h
//
// Tiny little-endian readers/writers used by every decoder. Diablo 1 / Hellfire
// assets (MPQ, CEL/CL2/CLX/PCX) are all little-endian on disk; these helpers
// keep the byte order explicit and portable.

#pragma once

#include <cstdint>

namespace decoders {

inline uint16_t LoadLE16(const uint8_t* p)
{
    return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
}

inline uint32_t LoadLE32(const uint8_t* p)
{
    return static_cast<uint32_t>(p[0])
         | (static_cast<uint32_t>(p[1]) << 8)
         | (static_cast<uint32_t>(p[2]) << 16)
         | (static_cast<uint32_t>(p[3]) << 24);
}

inline void WriteLE16(uint8_t* p, uint16_t v)
{
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
}

inline void WriteLE32(uint8_t* p, uint32_t v)
{
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
    p[2] = static_cast<uint8_t>(v >> 16);
    p[3] = static_cast<uint8_t>(v >> 24);
}

} // namespace decoders
