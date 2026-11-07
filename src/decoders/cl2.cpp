// src/decoders/cl2.cpp

#include "decoders/cl2.h"
#include "decoders/clx_encode.h"
#include "decoders/endian_util.h"

#include <cstddef>
#include <vector>

namespace decoders {

static bool IsClxOpaque(uint8_t control)            { return control >= 0x80; }
static bool IsClxOpaqueFill(uint8_t control)        { return control <= 0xBE; }
static uint8_t GetClxOpaqueFillWidth(uint8_t c)     { return static_cast<uint8_t>(0xBF - c); }
static uint8_t GetClxOpaquePixelsWidth(uint8_t c)   { return static_cast<uint8_t>(-static_cast<int8_t>(c)); }

std::vector<uint8_t> cl2ToClx(const uint8_t* data, size_t size, uint16_t frameWidth)
{
    uint32_t numGroups = 1;
    const uint32_t maybeNumFrames = LoadLE32(data);
    const uint8_t* groupBegin = data;

    std::vector<uint8_t> out;
    if (LoadLE32(data + static_cast<size_t>(maybeNumFrames) * 4 + 4) != size) {
        numGroups = maybeNumFrames / 4;
        out.resize(maybeNumFrames); // reserve room for the sheet's group-offset table
    }

    std::vector<uint8_t> pixels;
    pixels.reserve(4096);

    for (uint32_t group = 0; group < numGroups; ++group) {
        uint32_t numFrames;
        if (numGroups == 1) {
            numFrames = maybeNumFrames;
        } else {
            groupBegin = &data[LoadLE32(&data[4 * static_cast<size_t>(group)])];
            numFrames  = LoadLE32(groupBegin);
            WriteLE32(&out[4 * static_cast<size_t>(group)], static_cast<uint32_t>(out.size()));
        }

        const size_t clxOffset = out.size();
        out.resize(out.size() + 4 * (2 + static_cast<size_t>(numFrames)));
        WriteLE32(&out[clxOffset], numFrames);

        const uint8_t* frameEnd = &groupBegin[LoadLE32(&groupBegin[4])];
        for (uint32_t frame = 1; frame <= numFrames; ++frame) {
            WriteLE32(&out[clxOffset + 4 * static_cast<size_t>(frame)],
                      static_cast<uint32_t>(out.size() - clxOffset));

            const uint8_t* frameBegin = frameEnd;
            frameEnd = &groupBegin[LoadLE32(&groupBegin[4 * static_cast<size_t>(frame + 1)])];

            const size_t frameHeaderPos = out.size();
            out.resize(out.size() + ClxFrameHeaderSize);
            WriteLE16(&out[frameHeaderPos], static_cast<uint16_t>(ClxFrameHeaderSize));
            WriteLE16(&out[frameHeaderPos + 2], frameWidth);

            unsigned transparentRunWidth = 0;
            int xOffset = 0;
            size_t frameHeight = 0;
            const uint8_t* src = frameBegin + LoadLE16(frameBegin);

            while (src != frameEnd) {
                int remainingWidth = static_cast<int>(frameWidth) - xOffset;
                while (remainingWidth > 0) {
                    uint8_t control = *src++;
                    if (!IsClxOpaque(control)) {
                        if (!pixels.empty()) {
                            AppendClxPixelsOrFillRun(pixels.data(), pixels.size(), out);
                            pixels.clear();
                        }
                        transparentRunWidth += control;
                        remainingWidth      -= control;
                    } else if (IsClxOpaqueFill(control)) {
                        AppendClxTransparentRun(transparentRunWidth, out);
                        transparentRunWidth = 0;
                        uint8_t w     = GetClxOpaqueFillWidth(control);
                        uint8_t color = *src++;
                        pixels.insert(pixels.end(), w, color);
                        remainingWidth -= w;
                    } else {
                        AppendClxTransparentRun(transparentRunWidth, out);
                        transparentRunWidth = 0;
                        uint8_t w = GetClxOpaquePixelsWidth(control);
                        pixels.insert(pixels.end(), src, src + w);
                        src += w;
                        remainingWidth -= w;
                    }
                }

                // Only transparent runs may continue across a scanline boundary.
                if (!pixels.empty()) {
                    AppendClxPixelsOrFillRun(pixels.data(), pixels.size(), out);
                    pixels.clear();
                }
                int wholeLines, xOff;
                GetSkipSize(remainingWidth, static_cast<int>(frameWidth), wholeLines, xOff);
                xOffset     = xOff;
                frameHeight += static_cast<size_t>(wholeLines);
            }

            if (!pixels.empty()) {
                AppendClxPixelsOrFillRun(pixels.data(), pixels.size(), out);
                pixels.clear();
            }
            AppendClxTransparentRun(transparentRunWidth, out);
            WriteLE16(&out[frameHeaderPos + 4], static_cast<uint16_t>(frameHeight));
        }

        WriteLE32(&out[clxOffset + 4 * (1 + static_cast<size_t>(numFrames))],
                  static_cast<uint32_t>(out.size() - clxOffset));
    }

    return out;
}

} // namespace decoders
