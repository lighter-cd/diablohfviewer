// src/decoders/clx_encode.h
//
// CLX / CL2 frame-data encoding helpers, ported 1:1 from DevilutionX
// (utils/clx_encode.hpp). The CEL->CLX and CL2->CLX converters below emit CLX
// sprite buffers using exactly these routines, so that a single CLX pixel
// decoder (see clx.cpp) can render CEL, CL2 and native CLX alike.
//
// CLX frame pixel data uses the CL2 control-byte scheme:
//   control < 0x80            -> transparent run of `control` pixels
//   control in [0x80,0xBE]    -> opaque fill run: width = 0xBF-control, then 1 color byte
//   control in [0xBF,0xFF]    -> opaque pixels run: width = -(int8)control, then `width` color bytes

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "decoders/endian_util.h"

namespace decoders {

constexpr size_t ClxFrameHeaderSize = 6; // uint16 headerSize, uint16 width, uint16 height

// Append a transparent run (width pixels drawn as fully transparent).
inline void AppendClxTransparentRun(unsigned width, std::vector<uint8_t>& out)
{
    while (width >= 0x7F) {
        out.push_back(0x7F);
        width -= 0x7F;
    }
    if (width == 0)
        return;
    out.push_back(static_cast<uint8_t>(width));
}

inline void AppendClxFillRun(uint8_t color, unsigned width, std::vector<uint8_t>& out)
{
    while (width >= 0x3F) {
        out.push_back(0x80);
        out.push_back(color);
        width -= 0x3F;
    }
    if (width == 0)
        return;
    out.push_back(static_cast<uint8_t>(0xBF - width));
    out.push_back(color);
}

inline void AppendClxPixelsRun(const uint8_t* src, unsigned width, std::vector<uint8_t>& out)
{
    while (width >= 0x41) {
        out.push_back(0xBF);
        for (size_t i = 0; i < 0x41; ++i)
            out.push_back(src[i]);
        width -= 0x41;
        src += 0x41;
    }
    if (width == 0)
        return;
    out.push_back(static_cast<uint8_t>(256 - width));
    for (size_t i = 0; i < width; ++i)
        out.push_back(src[i]);
}

// Append a mixed run of opaque pixels, choosing between a pixels-run and a
// fill-run (run-length) encoding to keep the output compact.
inline void AppendClxPixelsOrFillRun(const uint8_t* src, size_t length, std::vector<uint8_t>& out)
{
    const uint8_t* begin         = src;
    const uint8_t* prevColorBegin = src;
    unsigned prevColorRunLength = 1;
    uint8_t prevColor = *src++;
    while (--length > 0) {
        const uint8_t color = *src;
        if (prevColor == color) {
            ++prevColorRunLength;
        } else {
            // Minimum run length at which a fill run beats a pixels run.
            constexpr unsigned MinFillRunLength = 3;
            if (prevColorRunLength >= MinFillRunLength) {
                AppendClxPixelsRun(begin, static_cast<unsigned>(prevColorBegin - begin), out);
                AppendClxFillRun(prevColor, prevColorRunLength, out);
                begin = src;
            }
            prevColorBegin    = src;
            prevColorRunLength = 1;
            prevColor         = color;
        }
        ++src;
    }

    // At the end of a line a run of 2 is cheaper as a fill (2 bytes) than as a
    // pixels run (3 bytes), so use a lower threshold here.
    if (prevColorRunLength >= 2) {
        AppendClxPixelsRun(begin, static_cast<unsigned>(prevColorBegin - begin), out);
        AppendClxFillRun(prevColor, prevColorRunLength, out);
    } else {
        AppendClxPixelsRun(begin, static_cast<unsigned>(prevColorBegin - begin + prevColorRunLength), out);
    }
}

// Given a negative (or zero) remaining-width at the end of a scanline, compute
// how many whole lines the current run overran and the x-offset to resume at.
inline void GetSkipSize(int remainingWidth, int srcWidth, int& wholeLines, int& xOffset)
{
    if (remainingWidth == 0) {
        wholeLines = 1;
        xOffset    = 0;
        return;
    }
    const unsigned overrun    = static_cast<unsigned>(-remainingWidth);
    wholeLines = static_cast<int>(overrun / static_cast<unsigned>(srcWidth) + 1);
    xOffset    = static_cast<int>(overrun % static_cast<unsigned>(srcWidth));
}

} // namespace decoders
