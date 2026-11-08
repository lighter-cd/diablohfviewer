// src/decoders/png.cpp
//
// Self-contained PNG writer. See png.h for the design notes.

#include "decoders/png.h"
#include "decoders/deflate.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <vector>

namespace decoders {

namespace {

// ---- CRC32 (IEEE 802.3, reflected) -----------------------------------------
uint32_t crc32Table[256];
bool crc32Init = false;

void initCrc32() {
    for (uint32_t n = 0; n < 256; ++n) {
        uint32_t c = n;
        for (int k = 0; k < 8; ++k)
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        crc32Table[n] = c;
    }
    crc32Init = true;
}

uint32_t crc32(const uint8_t* data, size_t len) {
    if (!crc32Init) initCrc32();
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i)
        c = crc32Table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

// Append a big-endian 32-bit value to a buffer.
void putU32(std::vector<uint8_t>& b, uint32_t v) {
    b.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
    b.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    b.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    b.push_back(static_cast<uint8_t>(v & 0xFF));
}

// Build one PNG chunk: length + type + data + CRC(type+data).
void appendChunk(std::vector<uint8_t>& out, const char* type,
                 const std::vector<uint8_t>& data) {
    putU32(out, static_cast<uint32_t>(data.size()));
    size_t typeStart = out.size();
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(type[i]));
    out.insert(out.end(), data.begin(), data.end());
    uint32_t crc = crc32(out.data() + typeStart, data.size() + 4);
    putU32(out, crc);
}

// Build one PNG chunk: length + type + data + CRC(type+data).  (declared above)
// encodePNG uses deflate::compressZlib for the IDAT (see below).

} // namespace

std::vector<uint8_t> encodePNG(uint16_t width, uint16_t height,
                               const std::vector<uint8_t>& rgba) {
    std::vector<uint8_t> out;
    if (width == 0 || height == 0) return out;
    if (rgba.size() != static_cast<size_t>(width) * height * 4) return out;

    // Raw image data: each scanline prefixed with a filter byte (0 = none).
    std::vector<uint8_t> raw;
    raw.reserve(static_cast<size_t>(height) * (1 + static_cast<size_t>(width) * 4));
    const size_t stride = static_cast<size_t>(width) * 4;
    for (uint16_t y = 0; y < height; ++y) {
        raw.push_back(0);  // filter type: none
        const uint8_t* row = rgba.data() + static_cast<size_t>(y) * stride;
        raw.insert(raw.end(), row, row + stride);
    }

    // 8-byte PNG signature.
    const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    out.insert(out.end(), sig, sig + 8);

    // IHDR.
    std::vector<uint8_t> ihdr;
    putU32(ihdr, width);
    putU32(ihdr, height);
    ihdr.push_back(8);    // bit depth
    ihdr.push_back(6);    // color type: RGBA
    ihdr.push_back(0);    // compression
    ihdr.push_back(0);    // filter
    ihdr.push_back(0);    // interlace
    appendChunk(out, "IHDR", ihdr);

    // IDAT: real zlib/DEFLATE (fixed-Huffman + greedy LZ77) via deflate:: so the
    // PNG stays small enough to embed as a data: URL in the WS scene response
    // (Firefox caps data: URIs at ~2 MB).
    std::vector<uint8_t> idat = deflate::compressZlib(raw);
    appendChunk(out, "IDAT", idat);

    // IEND.
    std::vector<uint8_t> iend;
    appendChunk(out, "IEND", iend);
    return out;
}

bool writePNG(const std::string& path, uint16_t width, uint16_t height,
              const std::vector<uint8_t>& rgba) {
    const std::vector<uint8_t> out = encodePNG(width, height, rgba);
    if (out.empty()) return false;
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(out.data()),
            static_cast<std::streamsize>(out.size()));
    return static_cast<bool>(f);
}

} // namespace decoders
