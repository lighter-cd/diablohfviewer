// src/decoders/pcx.h
//
// PCX (ZSoft PC Paintbrush) decoder for Diablo 1 / Hellfire static images.
// Only 8-bit single-plane PCX is supported (what the game uses). Many Diablo PCX
// files embed a 256-colour palette (separator byte 0x0C followed by 256 RGB
// triplets) at the end of the file; that palette is used to colourise the image.
//
// If an explicit palette is supplied via `usePalette` it overrides the embedded
// one. `transparentColor` (a palette index) marks pixels that render transparent.

#pragma once

#include <cstdint>
#include <vector>

#include "decoders/decoders_common.h"

namespace decoders {

// Decode a PCX buffer into `out`. Embedded palette (if any) is returned via
// `outEmbeddedPalette` (768 bytes). If `usePalette` is non-null and at least 768
// bytes it is used for colourising instead of the embedded palette.
// Returns false on unsupported format / truncated input.
bool decodePcx(const std::vector<uint8_t>& buf,
               DecodedFrame& out,
               std::vector<uint8_t>* outEmbeddedPalette = nullptr,
               const std::vector<uint8_t>* usePalette = nullptr,
               int transparentColor = -1);

} // namespace decoders
