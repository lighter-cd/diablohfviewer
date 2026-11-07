// src/decoders/cl2.cpp

#include "decoders/cl2.h"
#include "decoders/clx_encode.h"
#include "decoders/endian_util.h"

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace decoders {

static bool IsClxOpaque(uint8_t control)            { return control >= 0x80; }
static bool IsClxOpaqueFill(uint8_t control)        { return control <= 0xBE; }
static uint8_t GetClxOpaqueFillWidth(uint8_t c)     { return static_cast<uint8_t>(0xBF - c); }
static uint8_t GetClxOpaquePixelsWidth(uint8_t c)   { return static_cast<uint8_t>(-static_cast<int8_t>(c)); }

std::vector<uint8_t> cl2ToClx(const uint8_t* data, size_t size, uint16_t frameWidth)
{
    // Every offset in a CL2 comes from the file and is untrusted. A truncated /
    // corrupt CL2 (e.g. the 96-byte monsters\darkmage\dmagew.cl2 stub) used to
    // make `src` run far past the buffer and crash in memmove / *src++ (SIGSEGV).
    // Validate all reads against `size` and reject bad data with an exception;
    // the tool layer turns that into a clean JSON-RPC error instead of a crash.
    auto inRange = [size](size_t off, size_t n) -> bool {
        return off <= size && n <= size - off;
    };
    auto fail = [](const char* what) -> void {
        throw std::runtime_error(std::string("invalid CL2: ") + what);
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

    uint32_t numGroups = 1;
    std::vector<uint8_t> out;

    // Single-group layout: u32 numFrames, u32 frameOffset[numFrames],
    // u32 fileSize. Only probe the trailing size field when it is in bounds.
    const size_t probeOff = static_cast<size_t>(maybeNumFrames) * 4 + 4;
    const bool singleGroup = inRange(probeOff, 4) &&
        (LoadLE32(data + probeOff) == static_cast<uint32_t>(size));
    if (singleGroup) {
        if (maybeNumFrames == 0) fail("single-group CL2 has zero frames");
    } else {
        // Multi-group layout: u32 groupTableBytes (= 4 * numGroups), then
        // numGroups u32 offsets to each group's CL2 sub-block.
        if (maybeNumFrames < 4 || (maybeNumFrames % 4) != 0)
            fail("bad group offset table size");
        if (!inRange(0, maybeNumFrames))
            fail("group offset table out of bounds");
        numGroups = maybeNumFrames / 4;
        out.resize(maybeNumFrames);
    }

    std::vector<uint8_t> pixels;
    pixels.reserve(4096);

    for (uint32_t group = 0; group < numGroups; ++group) {
        size_t groupOff = 0;   // offset of this group's CL2 sub-block from `data`
        uint32_t numFrames;
        if (numGroups == 1) {
            numFrames = maybeNumFrames;
        } else {
            groupOff = u32At(4 * static_cast<size_t>(group));
            if (groupOff < maybeNumFrames)
                fail("group offset overlaps the group table");
            numFrames = u32At(groupOff);
            WriteLE32(&out[4 * static_cast<size_t>(group)],
                      static_cast<uint32_t>(out.size()));
        }

        // frameOffset[1..numFrames+1] is read from groupOff+4 onwards; the last
        // entry is the end offset of the final frame.
        const size_t frameTableBytes =
            4 * (static_cast<size_t>(numFrames) + 1) + 4;
        if (!inRange(groupOff, frameTableBytes))
            fail("frame offset table out of bounds");

        // Frame offsets are relative to the group sub-block start.
        auto frameOff = [&](uint32_t index) -> size_t {
            const uint32_t v = u32At(groupOff + 4 * static_cast<size_t>(index));
            if (static_cast<size_t>(v) > size - groupOff)
                fail("frame offset past end of file");
            return static_cast<size_t>(v);
        };

        const size_t clxOffset = out.size();
        out.resize(out.size() + 4 * (2 + static_cast<size_t>(numFrames)));
        WriteLE32(&out[clxOffset], numFrames);

        size_t frameEndRel = frameOff(1);
        for (uint32_t frame = 1; frame <= numFrames; ++frame) {
            WriteLE32(&out[clxOffset + 4 * static_cast<size_t>(frame)],
                      static_cast<uint32_t>(out.size() - clxOffset));

            const size_t frameBeginRel = frameEndRel;
            frameEndRel = frameOff(frame + 1);
            if (frameEndRel < frameBeginRel)
                fail("frame offsets are not monotonic");

            const size_t frameHeaderPos = out.size();
            out.resize(out.size() + ClxFrameHeaderSize);
            WriteLE16(&out[frameHeaderPos], static_cast<uint16_t>(ClxFrameHeaderSize));
            WriteLE16(&out[frameHeaderPos + 2], frameWidth);

            // CL2 frames begin with a u16 header size; pixel data follows it.
            const size_t frameBeginAbs = groupOff + frameBeginRel;
            const size_t frameEndAbs   = groupOff + frameEndRel;
            const size_t srcAbs = frameBeginAbs + u16At(frameBeginAbs);
            if (srcAbs > frameEndAbs)
                fail("frame header overruns the frame boundary");

            const uint8_t* src = data + srcAbs;
            const uint8_t* const frameEnd = data + frameEndAbs;

            unsigned transparentRunWidth = 0;
            int xOffset = 0;
            size_t frameHeight = 0;

            // `<` (not `!=`): once src passes frameEnd the loop must stop. Any
            // overrun is reported below instead of reading into unmapped memory.
            while (src < frameEnd) {
                int remainingWidth = static_cast<int>(frameWidth) - xOffset;
                while (remainingWidth > 0) {
                    if (src >= frameEnd)
                        fail("frame data overruns the frame boundary");
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
                        if (src >= frameEnd)
                            fail("frame data overruns the frame boundary");
                        uint8_t w     = GetClxOpaqueFillWidth(control);
                        uint8_t color = *src++;
                        pixels.insert(pixels.end(), w, color);
                        remainingWidth -= w;
                    } else {
                        AppendClxTransparentRun(transparentRunWidth, out);
                        transparentRunWidth = 0;
                        uint8_t w = GetClxOpaquePixelsWidth(control);
                        if (static_cast<size_t>(frameEnd - src) < w)
                            fail("frame data overruns the frame boundary");
                        pixels.insert(pixels.end(), src, src + w);
                        src += w;
                        remainingWidth -= w;
                    }
                }

                if (!pixels.empty()) {
                    AppendClxPixelsOrFillRun(pixels.data(), pixels.size(), out);
                    pixels.clear();
                }
                int wholeLines, xOff;
                GetSkipSize(remainingWidth, static_cast<int>(frameWidth), wholeLines, xOff);
                xOffset     = xOff;
                frameHeight += static_cast<size_t>(wholeLines);
            }
            if (src > frameEnd)
                fail("frame data overruns the frame boundary");

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
