// src/decoders/cel.h
//
// Convert a Diablo 1 CEL sprite into a CLX buffer (so it can be rendered by the
// shared CLX decoder in clx.cpp). Faithful port of DevilutionX CelToClx.
//
// CEL files store each scanline as a sequence of control bytes:
//   control >= 0x80 -> transparent run of width = -(int8)control  (1..128)
//   control <  0x80 -> opaque run of `control` palette indices     (1..127)
// Each frame may begin with a 10-byte header (signalled by its first uint16 == 10)
// which we skip. The frame width is not stored in the file and must be supplied.
//
// A CEL may be a single sprite list or a "group" sheet (a list of lists).

#pragma once

#include <cstdint>
#include <vector>

namespace decoders {

// Returns a CLX byte buffer. `frameWidth` is the width of every frame.
std::vector<uint8_t> celToClx(const uint8_t* data, size_t size, uint16_t frameWidth);

} // namespace decoders
