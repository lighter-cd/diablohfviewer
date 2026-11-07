// src/decoders/cel.cpp

#include "decoders/cel.h"
#include "decoders/clx_encode.h"
#include "decoders/endian_util.h"

#include <cstddef>

namespace decoders {

static bool IsCelTransparent(uint8_t control)
{
    return control >= 0x80;
}

static uint8_t GetCelTransparentWidth(uint8_t control)
{
    return static_cast<uint8_t>(-static_cast<int8_t>(control));
}

std::vector<uint8_t> celToClx(const uint8_t* data, size_t size, uint16_t frameWidth)
{
    std::vector<uint8_t> out;
    out.reserve(size + 4445); // CLX is usually smaller; hf_logo3.cel is the lone exception

    size_t groupsHeaderSize = 0;
    uint32_t numGroups = 1;
    const uint32_t maybeNumFrames = LoadLE32(data);
    const uint8_t* p = data;

    if (LoadLE32(data + static_cast<size_t>(maybeNumFrames) * 4 + 4) != size) {
        numGroups = maybeNumFrames / 4;
        groupsHeaderSize = maybeNumFrames;
        p += groupsHeaderSize;
        out.resize(groupsHeaderSize); // reserve room for the sheet's group-offset table
    }

    for (uint32_t group = 0; group < numGroups; ++group) {
        uint32_t numFrames;
        if (numGroups == 1) {
            numFrames = maybeNumFrames;
        } else {
            numFrames = LoadLE32(p);
            WriteLE32(&out[4 * static_cast<size_t>(group)], static_cast<uint32_t>(out.size()));
        }

        const size_t clxOffset = out.size();
        out.resize(out.size() + 4 * (2 + static_cast<size_t>(numFrames)));
        WriteLE32(&out[clxOffset], numFrames);

        const uint8_t* srcEnd = &p[LoadLE32(&p[4])];
        for (uint32_t frame = 1; frame <= numFrames; ++frame) {
            const uint8_t* src = srcEnd;
            srcEnd = &p[LoadLE32(&p[4 * static_cast<size_t>(frame + 1)])];
            WriteLE32(&out[clxOffset + 4 * static_cast<size_t>(frame)],
                      static_cast<uint32_t>(out.size() - clxOffset));

            // Skip a 10-byte CEL frame header if present.
            const bool hasHeader = (LoadLE16(src) == 10);
            if (hasHeader)
                src += 10;

            const size_t frameHeaderPos = out.size();
            out.resize(out.size() + ClxFrameHeaderSize);
            WriteLE16(&out[frameHeaderPos], static_cast<uint16_t>(ClxFrameHeaderSize));
            WriteLE16(&out[frameHeaderPos + 2], frameWidth);

            unsigned transparentRunWidth = 0;
            size_t frameHeight = 0;
            while (src != srcEnd) {
                for (unsigned remaining = frameWidth; remaining != 0;) {
                    uint8_t val = *src++;
                    if (IsCelTransparent(val)) {
                        val = GetCelTransparentWidth(val);
                        transparentRunWidth += val;
                    } else {
                        AppendClxTransparentRun(transparentRunWidth, out);
                        transparentRunWidth = 0;
                        AppendClxPixelsOrFillRun(src, val, out);
                        src += val;
                    }
                    remaining -= val;
                }
                ++frameHeight;
            }
            WriteLE16(&out[frameHeaderPos + 4], static_cast<uint16_t>(frameHeight));
            AppendClxTransparentRun(transparentRunWidth, out);
        }

        WriteLE32(&out[clxOffset + 4 * (1 + static_cast<size_t>(numFrames))],
                  static_cast<uint32_t>(out.size() - clxOffset));
        p = srcEnd;
    }

    return out;
}

} // namespace decoders
