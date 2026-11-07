// src/decoders/cel.cpp

#include "decoders/cel.h"
#include "decoders/clx_encode.h"
#include "decoders/endian_util.h"

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

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
    // Same class of hardening as cl2ToClx: every offset comes from the file and
    // is untrusted. A wrong `frameWidth` (or truncated CEL) used to make `src`
    // run past `srcEnd` and crash in *src++ / AppendClxPixelsOrFillRun (SIGSEGV).
    auto inRange = [size](size_t off, size_t n) -> bool {
        return off <= size && n <= size - off;
    };
    auto fail = [](const char* what) -> void {
        throw std::runtime_error(std::string("invalid CEL: ") + what);
    };
    auto u32At = [&](size_t off) -> uint32_t {
        if (!inRange(off, 4)) fail("offset out of bounds");
        return LoadLE32(data + off);
    };
    auto u16At = [&](size_t off) -> uint16_t {
        if (!inRange(off, 2)) fail("offset out of bounds");
        return LoadLE16(data + off);
    };

    if (!inRange(0, 4)) fail("file too small for header");
    const uint32_t maybeNumFrames = LoadLE32(data);

    std::vector<uint8_t> out;
    out.reserve(size + 4445); // CLX is usually smaller; hf_logo3.cel is the lone exception

    uint32_t numGroups = 1;
    size_t pOff = 0;   // current group starts at data + pOff
    const size_t probeOff = static_cast<size_t>(maybeNumFrames) * 4 + 4;
    const bool singleGroup = inRange(probeOff, 4) &&
        (LoadLE32(data + probeOff) == static_cast<uint32_t>(size));
    if (singleGroup) {
        if (maybeNumFrames == 0) fail("single-group CEL has zero frames");
    } else {
        if (maybeNumFrames < 4 || (maybeNumFrames % 4) != 0)
            fail("bad group offset table size");
        if (!inRange(0, maybeNumFrames))
            fail("group offset table out of bounds");
        numGroups = maybeNumFrames / 4;
        pOff = maybeNumFrames;
        out.resize(maybeNumFrames);
    }

    for (uint32_t group = 0; group < numGroups; ++group) {
        uint32_t numFrames;
        if (numGroups == 1) {
            numFrames = maybeNumFrames;
        } else {
            numFrames = u32At(pOff);
            WriteLE32(&out[4 * static_cast<size_t>(group)],
                      static_cast<uint32_t>(out.size()));
        }

        // frameOffset[1..numFrames+1] is read from pOff+4 onwards; the last
        // entry is the end offset of the final frame.
        const size_t frameTableBytes =
            4 * (static_cast<size_t>(numFrames) + 1) + 4;
        if (!inRange(pOff, frameTableBytes))
            fail("frame offset table out of bounds");
        auto frameOff = [&](uint32_t index) -> size_t {
            const uint32_t v = u32At(pOff + 4 * static_cast<size_t>(index));
            if (static_cast<size_t>(v) > size - pOff)
                fail("frame offset past end of file");
            return static_cast<size_t>(v);
        };

        const size_t clxOffset = out.size();
        out.resize(out.size() + 4 * (2 + static_cast<size_t>(numFrames)));
        WriteLE32(&out[clxOffset], numFrames);

        size_t srcEndRel = frameOff(1);
        for (uint32_t frame = 1; frame <= numFrames; ++frame) {
            const size_t srcBeginRel = srcEndRel;
            srcEndRel = frameOff(frame + 1);
            if (srcEndRel < srcBeginRel)
                fail("frame offsets are not monotonic");
            WriteLE32(&out[clxOffset + 4 * static_cast<size_t>(frame)],
                      static_cast<uint32_t>(out.size() - clxOffset));

            size_t srcAbs = pOff + srcBeginRel;
            const size_t srcEndAbs = pOff + srcEndRel;
            // Skip a 10-byte CEL frame header if present.
            if (u16At(srcAbs) == 10) {
                srcAbs += 10;
                if (srcAbs > srcEndAbs)
                    fail("frame header overruns the frame boundary");
            }

            const size_t frameHeaderPos = out.size();
            out.resize(out.size() + ClxFrameHeaderSize);
            WriteLE16(&out[frameHeaderPos], static_cast<uint16_t>(ClxFrameHeaderSize));
            WriteLE16(&out[frameHeaderPos + 2], frameWidth);

            const uint8_t* src = data + srcAbs;
            const uint8_t* const srcEnd = data + srcEndAbs;

            unsigned transparentRunWidth = 0;
            size_t frameHeight = 0;
            // `<` (not `!=`): once src passes srcEnd the loop must stop. `int`
            // remaining (not unsigned) so an over-wide run cannot wrap around
            // and keep the inner loop spinning. All reads are bounds-checked.
            while (src < srcEnd) {
                int remaining = static_cast<int>(frameWidth);
                while (remaining > 0) {
                    if (src >= srcEnd)
                        fail("frame data overruns the frame boundary");
                    uint8_t val = *src++;
                    if (IsCelTransparent(val)) {
                        val = GetCelTransparentWidth(val);
                        transparentRunWidth += val;
                    } else {
                        AppendClxTransparentRun(transparentRunWidth, out);
                        transparentRunWidth = 0;
                        if (static_cast<size_t>(srcEnd - src) < val)
                            fail("frame data overruns the frame boundary");
                        AppendClxPixelsOrFillRun(src, val, out);
                        src += val;
                    }
                    remaining -= val;
                }
                ++frameHeight;
            }
            if (src > srcEnd)
                fail("frame data overruns the frame boundary");
            WriteLE16(&out[frameHeaderPos + 4], static_cast<uint16_t>(frameHeight));
            AppendClxTransparentRun(transparentRunWidth, out);
        }

        WriteLE32(&out[clxOffset + 4 * (1 + static_cast<size_t>(numFrames))],
                  static_cast<uint32_t>(out.size() - clxOffset));
        pOff += srcEndRel;   // next group starts where the last frame ended
    }

    return out;
}

} // namespace decoders
