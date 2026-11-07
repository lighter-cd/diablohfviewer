// src/decoders/decoders_common.h
//
// Shared types for the sprite/palette decoders. Output is always an uncompressed
// RGBA pixel buffer (alpha = 0 means transparent), which is exactly what a
// Canvas/WebGL frontend needs to upload as a texture.

#pragma once

#include <cstdint>
#include <vector>

namespace decoders {

// One decoded sprite frame: `width` x `height` RGBA, row-major, 4 bytes/pixel.
struct DecodedFrame {
    uint16_t width  = 0;
    uint16_t height = 0;
    std::vector<uint8_t> rgba; // width*height*4, RGBA8 (alpha 0 = transparent)

    size_t pixelCount() const { return static_cast<size_t>(width) * height; }
};

} // namespace decoders
