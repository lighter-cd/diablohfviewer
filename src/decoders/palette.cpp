// src/decoders/palette.cpp

#include "decoders/palette.h"

#include <cstddef>

namespace decoders {

std::vector<uint8_t> loadPalette(const std::vector<uint8_t>& data)
{
    std::vector<uint8_t> rgb;
    const size_t n = data.size();
    if (n == 256 * 3) {
        rgb = data;
    } else if (n == 256 * 4) {
        rgb.reserve(256 * 3);
        for (int i = 0; i < 256; ++i) {
            rgb.push_back(data[static_cast<size_t>(i) * 4 + 0]);
            rgb.push_back(data[static_cast<size_t>(i) * 4 + 1]);
            rgb.push_back(data[static_cast<size_t>(i) * 4 + 2]);
        }
    } else if (n >= 256 * 3) {
        rgb.assign(data.begin(), data.begin() + static_cast<std::ptrdiff_t>(256 * 3));
    }
    return rgb;
}

std::vector<uint8_t> paletteToRGBA(const std::vector<uint8_t>& rgb)
{
    std::vector<uint8_t> out;
    out.reserve(256 * 4);
    for (int i = 0; i < 256; ++i) {
        uint8_t r = rgb.size() > static_cast<size_t>(i) * 3     ? rgb[static_cast<size_t>(i) * 3]     : 0;
        uint8_t g = rgb.size() > static_cast<size_t>(i) * 3 + 1 ? rgb[static_cast<size_t>(i) * 3 + 1] : 0;
        uint8_t b = rgb.size() > static_cast<size_t>(i) * 3 + 2 ? rgb[static_cast<size_t>(i) * 3 + 2] : 0;
        out.push_back(r);
        out.push_back(g);
        out.push_back(b);
        out.push_back(255);
    }
    return out;
}

} // namespace decoders
