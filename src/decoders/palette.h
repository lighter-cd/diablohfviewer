// src/decoders/palette.h
//
// Diablo 1 / Hellfire palette (.pal) loader.
//
// A .pal is 256 RGB triplets (768 bytes). The values are full 8-bit per channel
// -- DevilutionX copies the three bytes directly into an SDL_Color (r/g/b), so
// this is NOT the 6-bit-per-channel VGA palette some formats use.

#pragma once

#include <cstdint>
#include <vector>

namespace decoders {

// Parse a .pal buffer. Accepts:
//   * exactly 256*3 bytes  -> used as-is (RGB)
//   * exactly 256*4 bytes  -> RGBA, alpha dropped
//   * anything >= 256*3    -> first 256 triplets used
// Returns an empty vector if there are fewer than 256 triplets (caller should
// treat that as an error).
std::vector<uint8_t> loadPalette(const std::vector<uint8_t>& data);

// Expand a 768-byte RGB palette to 1024 bytes of RGBA (alpha forced to 255).
std::vector<uint8_t> paletteToRGBA(const std::vector<uint8_t>& rgb);

} // namespace decoders
