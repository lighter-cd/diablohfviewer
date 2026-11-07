// src/decoders/clx.cpp

#include "decoders/clx.h"
#include "decoders/endian_util.h"

#include <cstddef>

namespace decoders {

void decodeClxFrameData(const uint8_t* src, size_t srcSize,
                        uint16_t width, uint16_t height,
                        const uint8_t* palRGB,
                        std::vector<uint8_t>& outRGBA)
{
    outRGBA.assign(static_cast<size_t>(width) * height * 4, 0); // all transparent
    if (width == 0 || height == 0 || srcSize == 0)
        return;

    const uint8_t* p   = src;
    const uint8_t* end = src + srcSize;
    int curX = 0, curY = 0;

    while (p < end && curY < static_cast<int>(height)) {
        int remaining = static_cast<int>(width) - curX;
        while (remaining > 0 && p < end) {
            uint8_t control = *p++;
            if (control < 0x80) {
                // transparent run
                int w = control; // 0..127
                curX      += w;
                remaining -= w;
            } else if (control <= 0xBE) {
                // opaque fill run: width = 0xBF - control, then 1 color byte
                int w     = 0xBF - control; // 1..63
                uint8_t color = *p++;
                for (int k = 0; k < w && curX < static_cast<int>(width) && curY < static_cast<int>(height); ++k) {
                    // CLX/CEL/CL2 store rows bottom-to-top (DevilutionX renders
                    // "backwards"); map the first data row to the bottom of the
                    // buffer so the RGBA output is top-down (row 0 = top).
                    int outRow = (static_cast<int>(height) - 1) - curY;
                    int idx          = (outRow * static_cast<int>(width) + curX) * 4;
                    outRGBA[idx]     = palRGB[static_cast<size_t>(color) * 3];
                    outRGBA[idx + 1] = palRGB[static_cast<size_t>(color) * 3 + 1];
                    outRGBA[idx + 2] = palRGB[static_cast<size_t>(color) * 3 + 2];
                    outRGBA[idx + 3] = 255;
                    ++curX;
                }
                remaining -= w;
            } else {
                // opaque pixels run: width = -(int8)control, then `width` color bytes
                int w = -static_cast<int>(static_cast<int8_t>(control)); // 1..65
                for (int k = 0; k < w && curX < static_cast<int>(width) && curY < static_cast<int>(height); ++k) {
                    uint8_t color = *p++;
                    // CLX/CEL/CL2 store rows bottom-to-top (DevilutionX renders
                    // "backwards"); map the first data row to the bottom of the
                    // buffer so the RGBA output is top-down (row 0 = top).
                    int outRow = (static_cast<int>(height) - 1) - curY;
                    int idx          = (outRow * static_cast<int>(width) + curX) * 4;
                    outRGBA[idx]     = palRGB[static_cast<size_t>(color) * 3];
                    outRGBA[idx + 1] = palRGB[static_cast<size_t>(color) * 3 + 1];
                    outRGBA[idx + 2] = palRGB[static_cast<size_t>(color) * 3 + 2];
                    outRGBA[idx + 3] = 255;
                    ++curX;
                }
                remaining -= w;
            }
        }

        // Scanline finished. If we overran, carry the run onto the next line(s).
        if (remaining == 0) {
            ++curY;
            curX = 0;
        } else {
            unsigned overrun = static_cast<unsigned>(-remaining);
            int lines = static_cast<int>(overrun / static_cast<unsigned>(width) + 1);
            int xOff  = static_cast<int>(overrun % static_cast<unsigned>(width));
            curY += lines;
            curX  = xOff;
        }
    }
}

uint16_t clxNumLists(const std::vector<uint8_t>& buf)
{
    if (buf.size() < 8)
        return 0;
    const uint32_t maybeNumFrames = LoadLE32(buf.data());
    // If the last frame offset equals the file size, it's a plain list (0 lists);
    // otherwise it's a sheet whose first dword is the group-offset table size.
    if (maybeNumFrames * 4 + 4 + 4 > buf.size())
        return 0;
    if (LoadLE32(buf.data() + static_cast<size_t>(maybeNumFrames) * 4 + 4) != buf.size())
        return static_cast<uint16_t>(maybeNumFrames / 4);
    return 0;
}

std::vector<DecodedFrame> decodeClxList(const std::vector<uint8_t>& buf,
                                        const std::vector<uint8_t>& palette,
                                        uint32_t listOffset)
{
    std::vector<DecodedFrame> frames;
    if (buf.size() < listOffset + 8)
        return frames;
    const uint8_t* base = buf.data() + listOffset;

    const uint32_t numSprites = LoadLE32(base);
    const uint32_t entryCount = numSprites + 1; // offsets + trailing list-size dword
    if (listOffset + 4 + entryCount * 4 > buf.size())
        return frames;

    const uint8_t* palRGB = palette.empty() ? nullptr : palette.data();

    for (uint32_t i = 0; i < numSprites; ++i) {
        uint32_t off0 = LoadLE32(base + 4 + i * 4);
        uint32_t off1 = LoadLE32(base + 4 + (i + 1) * 4);
        if (off0 >= off1 || listOffset + off1 > buf.size())
            break;

        const uint8_t* fhdr     = base + off0;
        uint16_t headerSize     = LoadLE16(fhdr);
        uint16_t width          = LoadLE16(fhdr + 2);
        uint16_t height         = LoadLE16(fhdr + 4);
        uint32_t pixelOffset    = off0 + headerSize;
        uint32_t pixelSize      = off1 - pixelOffset;

        DecodedFrame frame;
        frame.width  = width;
        frame.height = height;
        if (palRGB != nullptr)
            decodeClxFrameData(base + pixelOffset, pixelSize, width, height, palRGB, frame.rgba);
        else
            frame.rgba.assign(static_cast<size_t>(width) * height * 4, 0);
        frames.push_back(std::move(frame));
    }
    return frames;
}

std::vector<DecodedFrame> decodeClx(const std::vector<uint8_t>& buf,
                                    const std::vector<uint8_t>& palette)
{
    std::vector<DecodedFrame> all;
    uint16_t lists = clxNumLists(buf);
    if (lists == 0) {
        auto f = decodeClxList(buf, palette, 0);
        all.insert(all.end(), f.begin(), f.end());
        return all;
    }
    for (uint16_t s = 0; s < lists; ++s) {
        if (4 + static_cast<size_t>(s + 1) * 4 > buf.size())
            break;
        uint32_t off = LoadLE32(buf.data() + 4 * static_cast<size_t>(s));
        auto f = decodeClxList(buf, palette, off);
        all.insert(all.end(), f.begin(), f.end());
    }
    return all;
}

} // namespace decoders
