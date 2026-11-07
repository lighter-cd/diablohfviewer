// src/decoders/pcx.cpp

#include "decoders/pcx.h"
#include "decoders/endian_util.h"

#include <cstddef>
#include <cstring>

namespace decoders {

#pragma pack(push, 1)
struct PcxHeader {
    uint8_t  manufacturer;
    uint8_t  version;
    uint8_t  encoding;
    uint8_t  bitsPerPixel;
    uint16_t xmin, ymin, xmax, ymax;
    uint16_t hdpi, vdpi;
    uint8_t  colormap[48];
    uint8_t  reserved;
    uint8_t  nplanes;
    uint16_t bytesPerLine;
    uint16_t paletteInfo;
    uint16_t hscreenSize;
    uint16_t vscreenSize;
    uint8_t  filler[54];
};
#pragma pack(pop)

static const size_t PcxHeaderSize = 128;
static const uint8_t PcxPaletteSeparator = 0x0C;

bool decodePcx(const std::vector<uint8_t>& buf,
               DecodedFrame& out,
               std::vector<uint8_t>* outEmbeddedPalette,
               const std::vector<uint8_t>* usePalette,
               int transparentColor)
{
    if (buf.size() <= PcxHeaderSize)
        return false;

    const PcxHeader* h = reinterpret_cast<const PcxHeader*>(buf.data());
    if (h->manufacturer != 0x0A) // PCX signature
        return false;

    int width  = static_cast<int>(LoadLE16(reinterpret_cast<const uint8_t*>(&h->xmax)))
               - static_cast<int>(LoadLE16(reinterpret_cast<const uint8_t*>(&h->xmin))) + 1;
    int height = static_cast<int>(LoadLE16(reinterpret_cast<const uint8_t*>(&h->ymax)))
               - static_cast<int>(LoadLE16(reinterpret_cast<const uint8_t*>(&h->ymin))) + 1;
    uint8_t bpp     = h->bitsPerPixel;
    uint8_t nplanes = h->nplanes;
    unsigned bytesPerLine = LoadLE16(reinterpret_cast<const uint8_t*>(&h->bytesPerLine));

    if (bpp != 8 || nplanes != 1)
        return false;
    if (width <= 0 || height <= 0)
        return false;
    if (bytesPerLine < static_cast<unsigned>(width))
        return false;

    // Optional 256-colour palette at the very end of the file.
    std::vector<uint8_t> embedded;
    const uint8_t* dataPtr = buf.data() + PcxHeaderSize;
    const uint8_t* dataEnd = buf.data() + buf.size();
    const uint8_t* pixelEnd = dataEnd;
    if (buf.size() >= PcxHeaderSize + 1 + 256 * 3) {
        const uint8_t* sep = dataEnd - (1 + 256 * 3);
        if (*sep == PcxPaletteSeparator) {
            embedded.assign(sep + 1, sep + 1 + 256 * 3);
            pixelEnd = sep;
        }
    }
    if (outEmbeddedPalette)
        *outEmbeddedPalette = embedded;

    // Choose colourisation palette.
    const uint8_t* pal = nullptr;
    if (usePalette && usePalette->size() >= 256 * 3)
        pal = usePalette->data();
    else if (!embedded.empty())
        pal = embedded.data();

    out.width  = static_cast<uint16_t>(width);
    out.height = static_cast<uint16_t>(height);
    out.rgba.assign(static_cast<size_t>(width) * height * 4, 0);

    std::vector<uint8_t> lineBuffer(bytesPerLine, 0);
    size_t dstRow = 0;
    while (dstRow < static_cast<size_t>(height) && dataPtr < pixelEnd) {
        // Decode one RLE scanline.
        unsigned x = 0;
        while (x < bytesPerLine && dataPtr < pixelEnd) {
            uint8_t byte = *dataPtr++;
            if (byte <= 0xBF) {
                lineBuffer[x++] = byte;
            } else {
                uint8_t runLen = byte & 0x3F;
                if (runLen == 0) runLen = 1; // defensive: 0xC0 with count 0
                uint8_t val = (dataPtr < pixelEnd) ? *dataPtr++ : 0;
                for (uint8_t i = 0; i < runLen && x < bytesPerLine; ++i)
                    lineBuffer[x++] = val;
            }
        }

        // Colourise the row (top-to-bottom, as stored).
        for (int cx = 0; cx < width; ++cx) {
            uint8_t idx = lineBuffer[cx];
            int o = static_cast<int>(dstRow) * width + cx;
            o *= 4;
            bool transparent = (transparentColor >= 0 && idx == static_cast<uint8_t>(transparentColor));
            if (transparent) {
                // leave RGBA at 0 (transparent)
            } else if (pal) {
                out.rgba[o]     = pal[static_cast<size_t>(idx) * 3];
                out.rgba[o + 1] = pal[static_cast<size_t>(idx) * 3 + 1];
                out.rgba[o + 2] = pal[static_cast<size_t>(idx) * 3 + 2];
                out.rgba[o + 3] = 255;
            } else {
                out.rgba[o] = out.rgba[o + 1] = out.rgba[o + 2] = idx;
                out.rgba[o + 3] = 255;
            }
        }
        ++dstRow;
    }
    return true;
}

} // namespace decoders
