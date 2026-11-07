// mpq-info/render_sprite.cpp
//
// Render one extracted Diablo 1 sprite (CL2 or CEL) to a PNG, using the
// src/decoders pipeline: CL2/CEL -> CLX -> RGBA frame(s) -> PNG.
//
// The CL2/CEL pixel layout does NOT store the frame width, so it must be passed
// in (it comes from DevilutionX's animation tables; e.g. warrior stand = 96).
// Palette is a 256*3 (or 256*4) .pal file.
//
// Build (mirrors recover_phase0.cpp / batch_extract.cpp):
//   g++ -I 3rdparty -I 3rdparty/StormLib/src -I src -std=c++17 \
//       mpq-info/render_sprite.cpp \
//       src/decoders/clx.cpp src/decoders/cl2.cpp src/decoders/cel.cpp \
//       src/decoders/pcx.cpp src/decoders/palette.cpp src/decoders/png.cpp \
//       -o out/render_sprite.exe
//
// Run:
//   out/render_sprite.exe <sprite> <palette.pal> <width> [out.png] [frameIndex]
//
// Example (warrior, light armour, unarmed, Stand; width 96):
//   out/render_sprite.exe \
//     out/extracted/DIABDAT/plrgfx/warrior/wlu/wluas.cl2 \
//     out/extracted/devilutionx/ui_art/diablo.pal 96 \
//     out/render/warrior_stand.png

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "decoders/cl2.h"
#include "decoders/cel.h"
#include "decoders/clx.h"
#include "decoders/palette.h"
#include "decoders/png.h"

namespace {

bool readFile(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    std::streamoff sz = f.tellg();
    if (sz <= 0) return false;
    f.seekg(0, std::ios::beg);
    out.resize(static_cast<size_t>(sz));
    f.read(reinterpret_cast<char*>(out.data()), sz);
    return static_cast<bool>(f) || f.eof();
}

bool endsWith(const std::string& s, const std::string& suf) {
    return s.size() >= suf.size() &&
           s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        fprintf(stderr,
                "usage: %s <sprite> <palette.pal> <width> [out.png] [frameIndex]\n",
                argv[0]);
        return 1;
    }
    const std::string spritePath = argv[1];
    const std::string palPath    = argv[2];
    const int width              = atoi(argv[3]);
    const std::string outPath    = (argc > 4) ? argv[4] : "out/render/sprite.png";
    const size_t frameIdx        = (argc > 5) ? static_cast<size_t>(atoi(argv[5])) : 0;

    if (width <= 0 || width > 65535) {
        fprintf(stderr, "invalid width %d\n", width);
        return 1;
    }

    std::vector<uint8_t> sprite, palBytes;
    if (!readFile(spritePath, sprite)) {
        fprintf(stderr, "cannot read sprite %s\n", spritePath.c_str());
        return 1;
    }
    if (!readFile(palPath, palBytes)) {
        fprintf(stderr, "cannot read palette %s\n", palPath.c_str());
        return 1;
    }

    std::vector<uint8_t> pal = decoders::loadPalette(palBytes);
    if (pal.size() < 256 * 3) {
        fprintf(stderr, "palette too small (%zu bytes)\n", palBytes.size());
        return 1;
    }

    // CL2/CEL -> CLX.
    std::vector<uint8_t> clx;
    const bool isCel = endsWith(spritePath, ".cel") || endsWith(spritePath, ".CEL");
    if (isCel)
        clx = decoders::celToClx(sprite.data(), sprite.size(),
                                 static_cast<uint16_t>(width));
    else
        clx = decoders::cl2ToClx(sprite.data(), sprite.size(),
                                 static_cast<uint16_t>(width));

    if (clx.empty()) {
        fprintf(stderr, "conversion to CLX failed (wrong width?)\n");
        return 1;
    }

    std::vector<decoders::DecodedFrame> frames = decoders::decodeClx(clx, pal);
    if (frames.empty()) {
        fprintf(stderr, "no frames decoded\n");
        return 1;
    }
    if (frameIdx >= frames.size()) {
        fprintf(stderr, "frame %zu out of range (have %zu frames)\n",
                frameIdx, frames.size());
        return 1;
    }

    const decoders::DecodedFrame& fr = frames[frameIdx];
    printf("decoded %zu frame(s); rendering frame %zu: %ux%u\n",
           frames.size(), frameIdx, fr.width, fr.height);

    if (!decoders::writePNG(outPath, fr.width, fr.height, fr.rgba)) {
        fprintf(stderr, "writePNG failed: %s\n", outPath.c_str());
        return 1;
    }
    printf("wrote %s\n", outPath.c_str());
    return 0;
}
