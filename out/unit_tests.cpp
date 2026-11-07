// Consolidated unit tests for DiabloHFConverter backend modules.
//
// Covers:
//   - decoders: CLX list decode, CEL->CLX and CL2->CLX round trips, PCX decode
//   - decoders/palette: loadPalette + paletteToRGBA
//   - tools/base64: RFC4648 encode/decode round trip
//
// Build & run (MinGW-w64, from repo root):
//   g++ -I src -std=c++17 out/unit_tests.cpp \
//       src/decoders/clx.cpp src/decoders/cel.cpp src/decoders/cl2.cpp \
//       src/decoders/pcx.cpp src/decoders/palette.cpp \
//       -o out/unit_tests.exe && out/unit_tests.exe
//
// Exit code 0 = all pass, 1 = at least one failure.

#include "decoders/clx.h"
#include "decoders/cel.h"
#include "decoders/cl2.h"
#include "decoders/pcx.h"
#include "decoders/endian_util.h"
#include "decoders/palette.h"
#include "tools/base64.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace decoders;
namespace base64 = mcp::base64;

static bool allOk = true;

static void chkPx(const std::vector<uint8_t>& rgba, int w, int x, int y,
                  int r, int g, int b, int a)
{
    int o = (y * w + x) * 4;
    if (o + 3 >= (int)rgba.size() ||
        rgba[o] != r || rgba[o + 1] != g || rgba[o + 2] != b || rgba[o + 3] != a) {
        allOk = false;
        int have = (o + 3 < (int)rgba.size()) ? rgba[o + 3] : -1;
        printf("  px(%d,%d) mismatch: got (%d,%d,%d,%d) expected (%d,%d,%d,%d)\n",
               x, y, (o + 0 < (int)rgba.size() ? rgba[o] : -1),
               (o + 1 < (int)rgba.size() ? rgba[o + 1] : -1),
               (o + 2 < (int)rgba.size() ? rgba[o + 2] : -1), have, r, g, b, a);
    }
}

// ---------------------------------------------------------------------------
// decoders: CLX list decode
// ---------------------------------------------------------------------------
static void testClxDecode()
{
    // Identity-ish palette: entry i -> (i, i+1, i+2).
    uint8_t pal[256 * 3];
    for (int i = 0; i < 256 * 3; ++i) pal[i] = static_cast<uint8_t>(i);

    std::vector<uint8_t> buf(33);
    auto w32 = [](std::vector<uint8_t>& b, size_t o, uint32_t v) { WriteLE32(&b[o], v); };
    auto w16 = [](std::vector<uint8_t>& b, size_t o, uint16_t v) { WriteLE16(&b[o], v); };
    w32(buf, 0, 1);                 // numSprites
    w32(buf, 4, 16);                // offset[0]  (frame table ends at byte 16)
    w32(buf, 8, 33);                // offset[1] == listSize
    w32(buf, 12, 33);               // listSize
    w16(buf, 16, 6);                // frame headerSize
    w16(buf, 18, 4);                // width
    w16(buf, 20, 2);                // height
    uint8_t stream[] = {
        0xFF, 5,                 // opaque pixels: width 1 -> color 5  (at x=0)
        0x01,                    // transparent run width 1           (at x=1)
        0xFE, 7, 7,              // opaque pixels: width 2 -> 7,7     (at x=2,3)
        0xFC, 9, 9, 9, 9         // row 1: width 4 -> 9 x4
    };
    memcpy(&buf[22], stream, sizeof(stream));

    auto frames = decodeClxList(buf, std::vector<uint8_t>(pal, pal + 256 * 3), 0);
    if (frames.size() != 1) { printf("CLX decode: FAIL (frames=%zu)\n", frames.size()); allOk = false; return; }
    const auto& f = frames[0];
    if (f.width != 4 || f.height != 2) { printf("CLX decode: FAIL dims %dx%d\n", f.width, f.height); allOk = false; }
    if (f.rgba.size() != 4 * 2 * 4) { printf("CLX decode: FAIL rgba size %zu\n", f.rgba.size()); allOk = false; }

    chkPx(f.rgba, 4, 0, 0, 15, 16, 17, 255);  // color 5
    chkPx(f.rgba, 4, 1, 0, 0, 0, 0, 0);        // transparent
    chkPx(f.rgba, 4, 2, 0, 21, 22, 23, 255);   // color 7
    chkPx(f.rgba, 4, 3, 0, 21, 22, 23, 255);
    chkPx(f.rgba, 4, 0, 1, 27, 28, 29, 255);   // color 9
    chkPx(f.rgba, 4, 1, 1, 27, 28, 29, 255);
    chkPx(f.rgba, 4, 2, 1, 27, 28, 29, 255);
    chkPx(f.rgba, 4, 3, 1, 27, 28, 29, 255);

    // CL2-frame "fill" run: control 0xBE (fill width 1) with a color byte.
    std::vector<uint8_t> buf2(27);
    w32(buf2, 0, 1); w32(buf2, 4, 16); w32(buf2, 8, 27); w32(buf2, 12, 27);
    w16(buf2, 16, 6); w16(buf2, 18, 4); w16(buf2, 20, 1);
    uint8_t stream2[] = { 0xBE, 3, 0x01, 0xBE, 200 };
    memcpy(&buf2[22], stream2, sizeof(stream2));
    auto f2 = decodeClxList(buf2, std::vector<uint8_t>(pal, pal + 256 * 3), 0);
    if (f2.empty()) { printf("CLX fill-run: FAIL (empty)\n"); allOk = false; return; }
    chkPx(f2[0].rgba, 4, 0, 0, 9, 10, 11, 255);    // color 3  -> (9,10,11)
    chkPx(f2[0].rgba, 4, 1, 0, 0, 0, 0, 0);         // transparent
    chkPx(f2[0].rgba, 4, 2, 0, 88, 89, 90, 255);    // color 200 -> mod 256 (88,89,90)

    printf("CLX decode: %s\n", allOk ? "PASS" : "FAIL");
}

// ---------------------------------------------------------------------------
// decoders: CEL -> CLX -> decode round trip
// ---------------------------------------------------------------------------
static void testCelRoundTrip()
{
    uint8_t pal[256 * 3];
    for (int i = 0; i < 256 * 3; ++i) pal[i] = static_cast<uint8_t>(i);
    auto w32 = [](std::vector<uint8_t>& b, size_t o, uint32_t v) { WriteLE32(&b[o], v); };
    auto w16 = [](std::vector<uint8_t>& b, size_t o, uint16_t v) { WriteLE16(&b[o], v); };

    std::vector<uint8_t> cb(35);
    w32(cb, 0, 1); w32(cb, 4, 16); w32(cb, 8, 35); w32(cb, 12, 35);
    w16(cb, 16, 10);                // CEL frame-header marker @16
    uint8_t celpix[] = { 0x04, 5, 5, 5, 5, 0xFE, 0x02, 7, 7 };
    memcpy(&cb[26], celpix, sizeof(celpix));
    auto clx = celToClx(cb.data(), cb.size(), 4);
    if (clx.empty()) { printf("CEL round-trip: FAIL (empty CLX)\n"); allOk = false; return; }
    auto fr = decodeClx(clx, std::vector<uint8_t>(pal, pal + 256 * 3));
    if (fr.size() != 1) { printf("CEL round-trip: FAIL (frames=%zu)\n", fr.size()); allOk = false; return; }
    chkPx(fr[0].rgba, 4, 0, 0, 15, 16, 17, 255);
    chkPx(fr[0].rgba, 4, 3, 0, 15, 16, 17, 255);
    chkPx(fr[0].rgba, 4, 0, 1, 0, 0, 0, 0);
    chkPx(fr[0].rgba, 4, 1, 1, 0, 0, 0, 0);
    chkPx(fr[0].rgba, 4, 2, 1, 21, 22, 23, 255);
    chkPx(fr[0].rgba, 4, 3, 1, 21, 22, 23, 255);
    if (fr[0].width != 4 || fr[0].height != 2) { printf("CEL round-trip: FAIL dims\n"); allOk = false; }
    printf("CEL round-trip: %s\n", allOk ? "PASS" : "FAIL");
}

// ---------------------------------------------------------------------------
// decoders: CL2 -> CLX -> decode round trip
// ---------------------------------------------------------------------------
static void testCl2RoundTrip()
{
    uint8_t pal[256 * 3];
    for (int i = 0; i < 256 * 3; ++i) pal[i] = static_cast<uint8_t>(i);
    auto w32 = [](std::vector<uint8_t>& b, size_t o, uint32_t v) { WriteLE32(&b[o], v); };
    auto w16 = [](std::vector<uint8_t>& b, size_t o, uint16_t v) { WriteLE16(&b[o], v); };

    std::vector<uint8_t> cb(27);
    w32(cb, 0, 1); w32(cb, 4, 16); w32(cb, 8, 27); w32(cb, 12, 27);
    w16(cb, 16, 2);                 // pixel data starts 2 bytes into the frame
    uint8_t cl2pix[] = { 0xFC, 5, 5, 5, 5, 0x02, 0xFE, 7, 7 };
    memcpy(&cb[18], cl2pix, sizeof(cl2pix));
    auto clx = cl2ToClx(cb.data(), cb.size(), 4);
    auto fr = decodeClx(clx, std::vector<uint8_t>(pal, pal + 256 * 3));
    if (fr.size() != 1) { printf("CL2 round-trip: FAIL (frames=%zu)\n", fr.size()); allOk = false; return; }
    chkPx(fr[0].rgba, 4, 0, 0, 15, 16, 17, 255);
    chkPx(fr[0].rgba, 4, 3, 0, 15, 16, 17, 255);
    chkPx(fr[0].rgba, 4, 0, 1, 0, 0, 0, 0);
    chkPx(fr[0].rgba, 4, 1, 1, 0, 0, 0, 0);
    chkPx(fr[0].rgba, 4, 2, 1, 21, 22, 23, 255);
    chkPx(fr[0].rgba, 4, 3, 1, 21, 22, 23, 255);
    if (fr[0].width != 4 || fr[0].height != 2) { printf("CL2 round-trip: FAIL dims\n"); allOk = false; }
    printf("CL2 round-trip: %s\n", allOk ? "PASS" : "FAIL");
}

// ---------------------------------------------------------------------------
// decoders: PCX decode (hand-built 4x2, embedded palette)
// ---------------------------------------------------------------------------
static void testPcxDecode()
{
    std::vector<uint8_t> pcx(128 + 7 + 1 + 256 * 3);
    pcx[0] = 0x0A;   // manufacturer
    pcx[1] = 5;      // version
    pcx[2] = 1;      // encoding (RLE)
    pcx[3] = 8;      // bits per pixel
    WriteLE16(&pcx[8], 3);   // xmax = W-1
    WriteLE16(&pcx[10], 1);  // ymax = H-1
    pcx[65] = 1;             // nplanes
    WriteLE16(&pcx[66], 4);  // bytesPerLine = W
    size_t pos = 128;
    uint8_t row0[] = { 1, 2, 3, 4 };                 // literals
    uint8_t row1[] = { 0xC3, 5, 7 };                  // run: 3x5, then literal 7
    memcpy(&pcx[pos], row0, sizeof(row0)); pos += sizeof(row0);
    memcpy(&pcx[pos], row1, sizeof(row1)); pos += sizeof(row1);
    pcx[pos++] = 0x0C;                                // palette separator
    for (int i = 0; i < 256; ++i) {
        pcx[pos++] = static_cast<uint8_t>(i);
        pcx[pos++] = static_cast<uint8_t>(i + 1);
        pcx[pos++] = static_cast<uint8_t>(i + 2);
    }
    DecodedFrame fr;
    std::vector<uint8_t> embPal;
    bool ok = decodePcx(pcx, fr, &embPal);
    if (!ok || fr.width != 4 || fr.height != 2 || embPal.size() != 256 * 3) {
        printf("PCX decode: FAIL (ok=%d w=%d h=%d pal=%zu)\n", ok, fr.width, fr.height, embPal.size());
        allOk = false;
    } else {
        auto px = [&](int x, int y) -> const uint8_t* { return &fr.rgba[(y * 4 + x) * 4]; };
        auto expect = [&](int x, int y, int r, int g, int b) {
            const uint8_t* p = px(x, y);
            if (p[0] != r || p[1] != g || p[2] != b || p[3] != 255) {
                allOk = false;
                printf("  PCX px(%d,%d) got (%d,%d,%d,%d) expected (%d,%d,%d,255)\n",
                       x, y, p[0], p[1], p[2], p[3], r, g, b);
            }
        };
        expect(0, 0, 1, 2, 3); expect(1, 0, 2, 3, 4); expect(2, 0, 3, 4, 5); expect(3, 0, 4, 5, 6);
        expect(0, 1, 5, 6, 7); expect(1, 1, 5, 6, 7); expect(2, 1, 5, 6, 7); expect(3, 1, 7, 8, 9);
    }
    printf("PCX decode: %s\n", allOk ? "PASS" : "FAIL");
}

// ---------------------------------------------------------------------------
// decoders/palette: loadPalette + paletteToRGBA
// ---------------------------------------------------------------------------
static void testPalette()
{
    // 768-byte RGB palette: entry i -> (i, 255-i, i/2).
    std::vector<uint8_t> rgb(256 * 3);
    for (int i = 0; i < 256; ++i) {
        rgb[i * 3 + 0] = static_cast<uint8_t>(i);
        rgb[i * 3 + 1] = static_cast<uint8_t>(255 - i);
        rgb[i * 3 + 2] = static_cast<uint8_t>(i / 2);
    }
    auto loaded = loadPalette(rgb);
    if (loaded.size() != 256 * 3) { printf("palette.loadPalette(RGB): FAIL size %zu\n", loaded.size()); allOk = false; }
    else if (loaded != rgb) { printf("palette.loadPalette(RGB): FAIL content\n"); allOk = false; }

    // 1024-byte RGBA input: should drop alpha, return 768.
    std::vector<uint8_t> rgba(256 * 4);
    for (int i = 0; i < 256; ++i) {
        rgba[i * 4 + 0] = static_cast<uint8_t>(i);
        rgba[i * 4 + 1] = static_cast<uint8_t>(255 - i);
        rgba[i * 4 + 2] = static_cast<uint8_t>(i / 2);
        rgba[i * 4 + 3] = 0; // alpha ignored
    }
    auto fromRgba = loadPalette(rgba);
    if (fromRgba.size() != 256 * 3) { printf("palette.loadPalette(RGBA): FAIL size %zu\n", fromRgba.size()); allOk = false; }
    else if (fromRgba != rgb) { printf("palette.loadPalette(RGBA): FAIL content\n"); allOk = false; }

    // expand to RGBA, alpha must be 255.
    auto expanded = paletteToRGBA(rgb);
    if (expanded.size() != 256 * 4) { printf("palette.paletteToRGBA: FAIL size %zu\n", expanded.size()); allOk = false; }
    else {
        bool alphaOk = true;
        for (int i = 0; i < 256; ++i) if (expanded[i * 4 + 3] != 255) alphaOk = false;
        if (!alphaOk) { printf("palette.paletteToRGBA: FAIL alpha not 255\n"); allOk = false; }
        if (expanded[0] != rgb[0] || expanded[1] != rgb[1] || expanded[2] != rgb[2])
        { printf("palette.paletteToRGBA: FAIL rgb mismatch\n"); allOk = false; }
    }

    // too-short input -> empty.
    if (!loadPalette(std::vector<uint8_t>(10)).empty()) { printf("palette.loadPalette(short): FAIL expected empty\n"); allOk = false; }

    printf("palette: %s\n", allOk ? "PASS" : "FAIL");
}

// ---------------------------------------------------------------------------
// tools/base64: RFC4648 encode / decode round trip
// ---------------------------------------------------------------------------
static void testBase64()
{
    struct Case { const char* in; const char* out; };
    Case cases[] = {
        {"", ""},
        {"f", "Zg=="},
        {"fo", "Zm8="},
        {"foo", "Zm9v"},
        {"foob", "Zm9vYg=="},
        {"fooba", "Zm9vYmE="},
        {"foobar", "Zm9vYmFy"},
        {"Man", "TWFu"},
        {"Diablo 1 Hellfire", "RGlhYmxvIDEgSGVsbGZpcmU="},
    };
    for (auto& c : cases) {
        std::string e = base64::encode(reinterpret_cast<const uint8_t*>(c.in), strlen(c.in));
        if (e != c.out) { printf("base64.encode('%s'): got '%s' expected '%s'\n", c.in, e.c_str(), c.out); allOk = false; }
        auto d = base64::decode(e);
        if (d.size() != strlen(c.in) || memcmp(d.data(), c.in, d.size()) != 0) {
            printf("base64.decode round-trip FAIL for '%s'\n", c.in); allOk = false;
        }
    }
    // decode must ignore '=' padding and stray whitespace.
    // "Zm 9v\nYg==" -> "Zm9vYg==" -> "foob" (4 bytes).
    auto d = base64::decode("Zm 9v\nYg==");
    if (d.size() != 4 || memcmp(d.data(), "foob", 4) != 0) { printf("base64.decode(loose): FAIL\n"); allOk = false; }

    // binary round trip (all 256 byte values).
    std::vector<uint8_t> bin(256);
    for (int i = 0; i < 256; ++i) bin[i] = static_cast<uint8_t>(i);
    auto back = base64::decode(base64::encode(bin));
    if (back != bin) { printf("base64.binary round-trip: FAIL\n"); allOk = false; }

    printf("base64: %s\n", allOk ? "PASS" : "FAIL");
}

int main()
{
    printf("=== DiabloHFConverter unit tests ===\n");
    testClxDecode();
    testCelRoundTrip();
    testCl2RoundTrip();
    testPcxDecode();
    testPalette();
    testBase64();
    printf("=== %s ===\n", allOk ? "ALL PASS" : "FAILURES PRESENT");
    return allOk ? 0 : 1;
}
