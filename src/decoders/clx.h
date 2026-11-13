// src/decoders/clx.h
//
// CLX sprite decoder. CLX is DevilutionX's runtime sprite format and, crucially,
// its per-frame pixel data uses the *same* control-byte encoding as CL2 frame
// data. That means a single pixel decoder here also renders CEL/CL2 sprites once
// they have been converted to CLX (see cel.cpp / cl2.cpp).
//
// CLX buffer layout (a "sprite list"):
//   uint32  numSprites
//   uint32  offset[i]            for i in 0..numSprites   (frame start offsets)
//   uint32  listSize             (offset[numSprites+1] == total size)
//   per frame:
//      uint16 headerSize         (offset within the frame to pixel data)
//      uint16 width
//      uint16 height
//      ...     pixel data        (CL2-frame-encoded, length = frameSize-headerSize)
//
// A "sprite sheet" is a list of lists: it starts with numLists uint32 offsets to
// each contained sprite list.

#pragma once

#include <cstdint>
#include <vector>

#include "decoders/decoders_common.h"

namespace decoders {

// Decode one frame's CL2-frame-encoded pixel data into an RGBA buffer.
//   src / srcSize : the frame's pixel data (after the 6-byte header)
//   width/height  : from the frame header
//   palRGB        : 256*3 bytes (r,g,b per palette index)
//   outRGBA       : filled with width*height*4 bytes; alpha 0 = transparent.
void decodeClxFrameData(const uint8_t* src, size_t srcSize,
                        uint16_t width, uint16_t height,
                        const uint8_t* palRGB,
                        std::vector<uint8_t>& outRGBA);

// Index-output variant: writes the palette index into R (A=255 where drawn,
// A=0 for transparent) instead of resolved RGBA. Used by the web tile atlas so
// the frontend can re-apply dLight shading per cell.
void decodeClxFrameDataIndex(const uint8_t* src, size_t srcSize,
                             uint16_t width, uint16_t height,
                             std::vector<uint8_t>& outRGBA);

// Decode a whole CLX buffer (list or sheet) into all of its frames, with each
// frame's pixels stored as palette indices (index form, see
// decodeClxFrameDataIndex). Returns frames whose `rgba` carries the index data.
std::vector<DecodedFrame> decodeClxIndex(const std::vector<uint8_t>& buf);

// Returns the number of lists if `buf` is a sprite sheet, or 0 for a plain list.
uint16_t clxNumLists(const std::vector<uint8_t>& buf);

// Decode a single CLX sprite list whose header starts at `listOffset`.
std::vector<DecodedFrame> decodeClxList(const std::vector<uint8_t>& buf,
                                        const std::vector<uint8_t>& palette,
                                        uint32_t listOffset = 0);

// Index-output counterpart of decodeClxList (see decodeClxFrameDataIndex).
std::vector<DecodedFrame> decodeClxListIndex(const std::vector<uint8_t>& buf,
                                            uint32_t listOffset = 0);

// Decode a whole CLX buffer (list or sheet) into all of its frames.
std::vector<DecodedFrame> decodeClx(const std::vector<uint8_t>& buf,
                                    const std::vector<uint8_t>& palette);

} // namespace decoders
