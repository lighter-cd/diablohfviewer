// Consolidated unit tests for DiabloHFConverter backend modules.
//
// Covers:
//   - decoders: CLX list decode, CEL->CLX and CL2->CLX round trips, PCX decode
//   - decoders/palette: loadPalette + paletteToRGBA
//   - decoders/png: writePNG -> re-read and verify (round trip)
//   - tools/base64: RFC4648 encode/decode round trip
//
// Build & run (MinGW-w64, from repo root):
//   g++ -I src -std=c++17 out/unit_tests.cpp \
//       src/decoders/clx.cpp src/decoders/cel.cpp src/decoders/cl2.cpp \
//       src/decoders/pcx.cpp src/decoders/palette.cpp src/decoders/png.cpp \
//       src/decoders/sprite_width.cpp \
//       -static-libgcc -static-libstdc++ \
//       -o out/unit_tests.exe && out/unit_tests.exe
//
// Exit code 0 = all pass, 1 = at least one failure.

#include "decoders/clx.h"
#include "decoders/cel.h"
#include "decoders/cl2.h"
#include "decoders/pcx.h"
#include "decoders/endian_util.h"
#include "decoders/palette.h"
#include "decoders/png.h"
#include "decoders/sprite_width.h"
#include "tools/base64.h"
#include "tools/spell_tables.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>
#include <filesystem>
#include <system_error>

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

    // CLX rows are stored bottom-to-top: data row 0 (color 5/7) lands at output
    // BOTTOM (row 1); data row 1 (color 9) lands at output TOP (row 0).
    chkPx(f.rgba, 4, 0, 1, 15, 16, 17, 255);  // color 5 (data row 0 -> bottom)
    chkPx(f.rgba, 4, 1, 1, 0, 0, 0, 0);        // transparent
    chkPx(f.rgba, 4, 2, 1, 21, 22, 23, 255);   // color 7
    chkPx(f.rgba, 4, 3, 1, 21, 22, 23, 255);
    chkPx(f.rgba, 4, 0, 0, 27, 28, 29, 255);   // color 9 (data row 1 -> top)
    chkPx(f.rgba, 4, 1, 0, 27, 28, 29, 255);
    chkPx(f.rgba, 4, 2, 0, 27, 28, 29, 255);
    chkPx(f.rgba, 4, 3, 0, 27, 28, 29, 255);

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
    // Flipped rows: old data row0 ([5,5,5,5]) -> new row1; old row1 ([.,.,7,7]) -> new row0.
    chkPx(fr[0].rgba, 4, 0, 0, 0, 0, 0, 0);
    chkPx(fr[0].rgba, 4, 1, 0, 0, 0, 0, 0);
    chkPx(fr[0].rgba, 4, 2, 0, 21, 22, 23, 255);
    chkPx(fr[0].rgba, 4, 3, 0, 21, 22, 23, 255);
    chkPx(fr[0].rgba, 4, 0, 1, 15, 16, 17, 255);
    chkPx(fr[0].rgba, 4, 3, 1, 15, 16, 17, 255);
    if (fr[0].width != 4 || fr[0].height != 2) { printf("CEL round-trip: FAIL dims\n"); allOk = false; }
    printf("CEL round-trip: %s\n", allOk ? "PASS" : "FAIL");
}

// ---------------------------------------------------------------------------
// decoders: malformed CEL / wrong frameWidth must be rejected, never SIGSEGV.
// Regression for towners\animals\cow.cel decoded with width 64/96/160.
// ---------------------------------------------------------------------------
static void testCelRejectsMalformed()
{
    auto w32 = [](std::vector<uint8_t>& b, size_t o, uint32_t v) { WriteLE32(&b[o], v); };
    auto w16 = [](std::vector<uint8_t>& b, size_t o, uint16_t v) { WriteLE16(&b[o], v); };
    auto expectThrow = [](const std::vector<uint8_t>& b, uint16_t w, const char* label) {
        bool threw = false;
        try {
            auto clx = celToClx(b.data(), b.size(), w);
            (void)clx;
        } catch (const std::runtime_error&) {
            threw = true;
        }
        if (!threw) {
            printf("  %s: FAIL (no exception)\n", label);
            allOk = false;
        }
    };

    expectThrow(std::vector<uint8_t>(0), 4, "empty CEL rejected");
    expectThrow(std::vector<uint8_t>(3, 0), 4, "3-byte CEL rejected");

    // The same valid 4-wide CEL as testCelRoundTrip, but with a wrong width:
    // src used to run past srcEnd and crash (SIGSEGV).
    std::vector<uint8_t> cb(35);
    w32(cb, 0, 1); w32(cb, 4, 16); w32(cb, 8, 35); w32(cb, 12, 35);
    w16(cb, 16, 10);
    uint8_t celpix[] = { 0x04, 5, 5, 5, 5, 0xFE, 0x02, 7, 7 };
    memcpy(&cb[26], celpix, sizeof(celpix));
    expectThrow(cb, 64, "wrong frameWidth rejected (no SIGSEGV)");

    // Non-monotonic frame offsets.
    std::vector<uint8_t> nonMono(24, 0);
    w32(nonMono, 0, 2); w32(nonMono, 4, 16);
    w32(nonMono, 8, 8); w32(nonMono, 12, 24);
    expectThrow(nonMono, 4, "non-monotonic frame offsets rejected");

    // Frame offset past EOF.
    std::vector<uint8_t> pastEof(16, 0);
    w32(pastEof, 0, 1); w32(pastEof, 4, 999999);
    w32(pastEof, 8, 16); w32(pastEof, 12, 16);
    expectThrow(pastEof, 4, "frame offset past EOF rejected");

    printf("CEL malformed-input: %s\n", allOk ? "PASS" : "FAIL");
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
    // Flipped rows: old data row0 ([5,5,5,5]) -> new row1; old row1 ([.,.,7,7]) -> new row0.
    chkPx(fr[0].rgba, 4, 0, 0, 0, 0, 0, 0);
    chkPx(fr[0].rgba, 4, 1, 0, 0, 0, 0, 0);
    chkPx(fr[0].rgba, 4, 2, 0, 21, 22, 23, 255);
    chkPx(fr[0].rgba, 4, 3, 0, 21, 22, 23, 255);
    chkPx(fr[0].rgba, 4, 0, 1, 15, 16, 17, 255);
    chkPx(fr[0].rgba, 4, 3, 1, 15, 16, 17, 255);
    if (fr[0].width != 4 || fr[0].height != 2) { printf("CL2 round-trip: FAIL dims\n"); allOk = false; }
    printf("CL2 round-trip: %s\n", allOk ? "PASS" : "FAIL");
}

// ---------------------------------------------------------------------------
// decoders: malformed / truncated CL2 must be rejected, never SIGSEGV.
// Regression for monsters\darkmage\dmagew.cl2 (a 96-byte stub in DIABDAT.MPQ)
// whose unvalidated offsets used to make `src` run past the buffer in cl2ToClx.
// ---------------------------------------------------------------------------
static void testCl2RejectsMalformed()
{
    auto w32 = [](std::vector<uint8_t>& b, size_t o, uint32_t v) { WriteLE32(&b[o], v); };
    auto expectThrow = [](const std::vector<uint8_t>& b, uint16_t w, const char* label) {
        bool threw = false;
        try {
            auto clx = cl2ToClx(b.data(), b.size(), w);
            (void)clx;
        } catch (const std::runtime_error&) {
            threw = true;
        }
        if (!threw) {
            printf("  %s: FAIL (no exception)\n", label);
            allOk = false;
        }
    };

    // (a) header too small to even read maybeNumFrames.
    expectThrow(std::vector<uint8_t>(0), 128, "empty CL2 rejected");
    expectThrow(std::vector<uint8_t>(3, 0), 128, "3-byte CL2 rejected");

    // (b) exact shape of monsters\darkmage\dmagew.cl2 from DIABDAT.MPQ:
    //     96 bytes, first u32 = 32 (misread as a group-offset table), then the
    //     8 offsets 32/39/46/53/60/67/74/81, then repeating 0/8 pairs.
    std::vector<uint8_t> stub(96, 0);
    const uint32_t head[8] = {32, 39, 46, 53, 60, 67, 74, 81};
    for (int i = 0; i < 8; ++i) w32(stub, static_cast<size_t>(4 * i), head[i]);
    for (size_t o = 32; o + 4 <= stub.size(); o += 8) w32(stub, o, 8);
    expectThrow(stub, 128, "96-byte dmagew stub rejected");

    // (c) valid single-group header, but a frame offset points past EOF.
    std::vector<uint8_t> pastEof(16, 0);
    w32(pastEof, 0, 1); w32(pastEof, 4, 999999);
    w32(pastEof, 8, 16); w32(pastEof, 12, 16);
    expectThrow(pastEof, 4, "frame offset past EOF rejected");

    // (d) non-monotonic frame offsets (the corrupt shape in dmagew group 4):
    //     frame 2 ends before frame 1, which `while (src != frameEnd)` used to
    //     turn into an unbounded overrun.
    std::vector<uint8_t> nonMono(24, 0);
    w32(nonMono, 0, 2); w32(nonMono, 4, 16);
    w32(nonMono, 8, 8); w32(nonMono, 12, 24);
    expectThrow(nonMono, 4, "non-monotonic frame offsets rejected");

    printf("CL2 malformed-input: %s\n", allOk ? "PASS" : "FAIL");
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

// ---------------------------------------------------------------------------
// decoders/png: writePNG -> re-read the file and verify (round trip)
// ---------------------------------------------------------------------------
namespace {
uint32_t testCrc32(const uint8_t* data, size_t len) {
    static uint32_t tbl[256];
    static bool init = false;
    if (!init) {
        for (uint32_t n = 0; n < 256; ++n) {
            uint32_t c = n;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            tbl[n] = c;
        }
        init = true;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) c = tbl[(c ^ data[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

// Inflate a zlib stream made only of stored (BTYPE=00) blocks.
bool inflateStored(const std::vector<uint8_t>& z, std::vector<uint8_t>& out) {
    if (z.size() < 2) return false;          // skip zlib header
    size_t p = 2;
    while (p < z.size()) {
        uint8_t b = z[p++];
        bool bfinal = (b & 1) != 0;
        int btype = (b >> 1) & 3;
        if (btype != 0) return false;        // only stored supported
        if (p + 4 > z.size()) return false;
        uint16_t len = z[p] | (z[p + 1] << 8);
        uint16_t nlen = z[p + 2] | (z[p + 3] << 8);
        p += 4;
        if ((len ^ nlen) != 0xFFFF) return false;
        if (p + len > z.size()) return false;
        out.insert(out.end(), z.begin() + p, z.begin() + p + len);
        p += len;
        if (bfinal) break;
    }
    return true;
}
} // namespace

static void testPngWrite()
{
    const uint16_t W = 4, H = 3;
    std::vector<uint8_t> rgba(static_cast<size_t>(W) * H * 4);
    for (uint16_t y = 0; y < H; ++y)
        for (uint16_t x = 0; x < W; ++x) {
            size_t o = (static_cast<size_t>(y) * W + x) * 4;
            rgba[o] = static_cast<uint8_t>(x * 20);
            rgba[o + 1] = static_cast<uint8_t>(y * 30);
            rgba[o + 2] = static_cast<uint8_t>((x + y) * 10);
            rgba[o + 3] = 255;
        }

    const char* path = "out/__png_test.png";
    if (!writePNG(path, W, H, rgba)) { printf("PNG write: FAIL (writePNG false)\n"); allOk = false; return; }

    std::ifstream f(path, std::ios::binary);
    std::vector<uint8_t> png((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());

    static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    if (png.size() < 8 || memcmp(png.data(), sig, 8) != 0) { printf("PNG write: FAIL signature\n"); allOk = false; return; }

    uint32_t ihdrW = 0, ihdrH = 0; bool haveIend = false;
    std::vector<uint8_t> idat;
    size_t p = 8;
    while (p + 8 <= png.size()) {
        uint32_t len = (png[p] << 24) | (png[p + 1] << 16) | (png[p + 2] << 8) | png[p + 3];
        std::string type(reinterpret_cast<const char*>(&png[p + 4]), 4);
        const uint8_t* data = &png[p + 8];
        uint32_t crc = (png[p + 8 + len] << 24) | (png[p + 9 + len] << 16) |
                       (png[p + 10 + len] << 8) | png[p + 11 + len];
        if (testCrc32(&png[p + 4], 4 + len) != crc) { printf("PNG write: FAIL chunk CRC\n"); allOk = false; return; }
        if (type == "IHDR") {
            if (len < 13) { printf("PNG write: FAIL IHDR len\n"); allOk = false; return; }
            ihdrW = (data[0] << 24) | (data[1] << 16) | (data[2] << 8) | data[3];
            ihdrH = (data[4] << 24) | (data[5] << 16) | (data[6] << 8) | data[7];
        } else if (type == "IDAT") {
            idat.insert(idat.end(), data, data + len);
        } else if (type == "IEND") {
            haveIend = true;
        }
        p += 12 + len;
    }
    if (!haveIend) { printf("PNG write: FAIL no IEND\n"); allOk = false; return; }
    if (ihdrW != W || ihdrH != H) { printf("PNG write: FAIL dimensions\n"); allOk = false; return; }

    std::vector<uint8_t> raw;
    if (!inflateStored(idat, raw)) { printf("PNG write: FAIL inflate\n"); allOk = false; return; }
    std::vector<uint8_t> px;
    size_t stride = static_cast<size_t>(W) * 4;
    for (uint16_t y = 0; y < H; ++y) {
        size_t rowStart = static_cast<size_t>(y) * (1 + stride);
        if (raw.size() < rowStart + 1 + stride) { printf("PNG write: FAIL raw size\n"); allOk = false; return; }
        if (raw[rowStart] != 0) { printf("PNG write: FAIL filter byte\n"); allOk = false; return; }
        px.insert(px.end(), raw.begin() + rowStart + 1, raw.begin() + rowStart + 1 + stride);
    }
    if (px != rgba) { printf("PNG write: FAIL pixel mismatch\n"); allOk = false; return; }

    printf("PNG write: PASS\n");
}

// ---------------------------------------------------------------------------
// decoders/sprite_width: type-code mapping + auto frame-width lookup
// ---------------------------------------------------------------------------
static void testSpriteWidth()
{
    bool ok = true;

    // 1) pure type-code -> column mapping (verbatim from DevilutionX player.cpp)
    auto expectCol = [&](const std::string& type, char w, const std::string& exp) {
        std::string c;
        if (!decoders::sprite_width::playerTypeToColumn(type, w, c) || c != exp) {
            allOk = false; ok = false;
            printf("  playerTypeToColumn(%s,%c) expected '%s' got '%s'\n",
                   type.c_str(), w, exp.c_str(), c.c_str());
        }
    };
    expectCol("as", 'l', "stand");
    expectCol("st", 'l', "stand");
    expectCol("aw", 'l', "walk");
    expectCol("wl", 'l', "walk");
    expectCol("at", 'n', "attack");
    expectCol("at", 'b', "bow");   // weapon 'b' = Bow -> bow width
    expectCol("ht", 'l', "swHit");
    expectCol("lm", 'l', "lightning");
    expectCol("fm", 'l', "fire");
    expectCol("qm", 'l', "magic");
    expectCol("dt", 'l', "death");
    expectCol("bl", 'l', "block");
    { std::string c; if (decoders::sprite_width::playerTypeToColumn("zz", 'l', c)) { allOk = false; ok = false; } }

    // 2) autoWidth against real DevilutionX data tables (SKIP if not present)
    std::filesystem::path dev("D:/work/xt/diablo/DevilutionX");
    std::error_code ec;
    bool haveDev = std::filesystem::exists(
        dev / "assets" / "txtdata" / "classes" / "warrior" / "sprites.tsv", ec);
    if (haveDev) {
        std::string dbg;
        int w = decoders::sprite_width::autoWidth("plrgfx\\warrior\\wlu\\wluas.cl2", dev, dbg);
        if (w != 96) { allOk = false; ok = false; printf("  autoWidth warrior stand expected 96 got %d (%s)\n", w, dbg.c_str()); }
        int wm = decoders::sprite_width::autoWidth("monsters\\acid\\acida.cl2", dev, dbg);
        if (wm <= 0) { allOk = false; ok = false; printf("  autoWidth monster expected >0 got %d (%s)\n", wm, dbg.c_str()); }
        int wi = decoders::sprite_width::autoWidth("items\\armor2.cel", dev, dbg);
        if (wi != -1) { allOk = false; ok = false; printf("  autoWidth items should fail (no width table)\n"); }
        // DevilutionX quirk: MPQ folder "sorceror" maps to class dir "sorcerer".
        int ws = decoders::sprite_width::autoWidth("plrgfx\\sorceror\\sha\\shaas.cl2", dev, dbg);
        if (ws != 96) { allOk = false; ok = false; printf("  autoWidth sorceror(->sorcerer) stand expected 96 got %d (%s)\n", ws, dbg.c_str()); }
        // monsterFramesPerDir: zombie stand 'n' -> frames[6] index 0 = 11.
        int fpdN = decoders::sprite_width::monsterFramesPerDir("monsters\\zombie\\zombien.cl2", dev, dbg);
        if (fpdN != 11) { allOk = false; ok = false; printf("  monsterFramesPerDir zombie n expected 11 got %d (%s)\n", fpdN, dbg.c_str()); }
        // zombie walk 'w' -> index 1 = 24; death 'd' -> index 4 = 16.
        int fpdW = decoders::sprite_width::monsterFramesPerDir("monsters\\zombie\\zombiew.cl2", dev, dbg);
        if (fpdW != 24) { allOk = false; ok = false; printf("  monsterFramesPerDir zombie w expected 24 got %d (%s)\n", fpdW, dbg.c_str()); }
        int fpdD = decoders::sprite_width::monsterFramesPerDir("monsters\\zombie\\zombied.cl2", dev, dbg);
        if (fpdD != 16) { allOk = false; ok = false; printf("  monsterFramesPerDir zombie d expected 16 got %d (%s)\n", fpdD, dbg.c_str()); }
    } else {
        printf("  (sprite_width autoWidth: SKIP, DevilutionX not found at %s)\n", dev.string().c_str());
    }

    printf("sprite_width: %s\n", ok ? "PASS" : "FAIL");
}

// ---------------------------------------------------------------------------
// tools/spell_tables.h: pure data-table joins (no MPQ / no TSV files on disk)
// ---------------------------------------------------------------------------
static void testSpellTables()
{
    using namespace spell_tables;

    auto bad = [](const char* label) {
        printf("  %s: FAIL\n", label);
        allOk = false;
    };

    // (a) parseTsvText: CRLF, trimmed cells, empty trailing columns.
    const std::string tsv =
        "id\twidth\tnumFrames\tname\tframeLength\r\n"
        "Fireball\t96\t16\tfireba\t14,14,14\r\n"
        "Lightning\t96\t1\tlghning\t8\r\n";
    Table t = parseTsvText(tsv);
    if (t.size() != 2) bad("parse rows");
    else if (val(t[0], "id") != "Fireball" || val(t[1], "name") != "lghning")
        bad("parse CRLF/values");

    // (b) element -> cast action
    if (elementToAction(firstElement("Fire,Targeted")) != "fm") bad("Fire -> fm");
    if (elementToAction(firstElement("Lightning,Targeted")) != "lm") bad("Lightning -> lm");
    if (elementToAction(firstElement("Magic")) != "qm") bad("Magic -> qm");

    // (c) numbered sprite files vs single-frame file
    SpriteInfo s1 = spriteFilesFor(t[0]);
    if (s1.files.size() != 16 || s1.files.front() != "missiles\\fireba1.cl2" ||
        s1.files.back() != "missiles\\fireba16.cl2" || s1.width != 96 || s1.frames != 14)
        bad("Fireball 16 numbered files / width / frames");
    SpriteInfo s2 = spriteFilesFor(t[1]);
    if (s2.files.size() != 1 || s2.files[0] != "missiles\\lghning.cl2")
        bad("Lightning single .cl2 (no numeric suffix)");
    Row invalid;
    invalid["name"] = "?"; invalid["width"] = "96"; invalid["numFrames"] = "1";
    if (spriteFilesFor(invalid).valid()) bad("'?' name rejected");

    // (d) AI -> missile (real GetMissileType switch)
    bool def = false;
    if (aiToMissile("Torchant", def) != "Fireball" || def) bad("Torchant -> Fireball");
    if (aiToMissile("Magma", def) != "MagmaBall" || def) bad("Magma -> MagmaBall");
    if (aiToMissile("SkeletonRanged", def) != "Arrow" || !def) bad("SkeletonRanged -> default Arrow");
    if (!rangedAiSet().count("SkeletonRanged")) bad("rangedAiSet contains SkeletonRanged");

    // (d2) attack forms that bypass GetMissileType() (AI picks the missile itself)
    {
        int counselor = 0, bat = 0, mega = 0, hork = 0;
        bool hasFlash = false, hasFireballForm = false;
        for (const auto& f : monsterAttackForms()) {
            if (f.ai == "Counselor") {
                ++counselor;
                if (f.variant == "Flash") {
                    hasFlash = true;
                    if (f.missiles != "FlashBottom,FlashTop") bad("Counselor Flash missiles");
                }
                if (f.variant == "Fireball") {
                    hasFireballForm = true;
                    if (f.missiles != "Fireball") bad("Counselor Fireball missile");
                }
            } else if (f.ai == "Bat") ++bat;
            else if (f.ai == "Mega") ++mega;
            else if (f.ai == "HorkDemon") ++hork;
        }
        if (counselor != 5) bad("Counselor has 5 attack forms (4 missiles + Flash)");
        if (!hasFlash || !hasFireballForm) bad("Counselor Flash/Fireball forms present");
        if (bat != 2) bad("Bat has 2 attack forms (Lightning + Gloom charge)");
        if (mega != 1) bad("Mega has 1 attack form (Inferno)");
        if (hork != 1) bad("HorkDemon has 1 attack form (HorkSpawn, unique AI)");

        // Unique-monster AIs that delegate to another AI body.
        if (normalizedMonsterAi("Zhar") != "Counselor") bad("Zhar -> Counselor");
        if (normalizedMonsterAi("Lazarus") != "Counselor") bad("Lazarus -> Counselor");
        if (normalizedMonsterAi("Succubus") != "Succubus") bad("plain AI unchanged");

        // intelligence / monster-id narrowing mirrors the AI code
        const AttackForm* fireball = nullptr;
        const AttackForm* charged = nullptr;
        const AttackForm* batLightning = nullptr;
        const AttackForm* gloomCharge = nullptr;
        for (const auto& f : monsterAttackForms()) {
            if (f.ai == "Counselor" && f.variant == "Fireball") fireball = &f;
            if (f.ai == "Counselor" && f.variant == "ChargedBolt") charged = &f;
            if (f.ai == "Bat" && f.variant == "Lightning") batLightning = &f;
            if (f.ai == "Bat" && f.variant == "Charge") gloomCharge = &f;
        }
        if (!fireball || !charged || !batLightning || !gloomCharge) bad("attack forms located");
        else {
            if (!attackFormMatches(*fireball, "MT_ADVOCATE", 3))
                bad("Counselor/Fireball matches Advocate (intel 3)");
            if (attackFormMatches(*fireball, "MT_COUNSLR", 0))
                bad("Counselor/Fireball rejects Counselor (intel 0)");
            if (!attackFormMatches(*charged, "MT_MAGISTR", 1))
                bad("Counselor/ChargedBolt matches Magistrate (intel 1)");
            if (!attackFormMatches(*batLightning, "MT_FAMILIAR", 3))
                bad("Bat/Lightning matches Familiar");
            if (attackFormMatches(*batLightning, "MT_GLOOM", 2))
                bad("Bat/Lightning rejects Gloom");
            if (!attackFormMatches(*gloomCharge, "MT_GLOOM", 2))
                bad("Bat/Charge matches Gloom");
        }
    }

    // (d3) engine-level monster abilities (no missile attack of their own)
    {
        const auto& ab = monsterAbilities();
        if (ab.size() != 3) bad("3 monster abilities registered");
        bool blink = false, acid = false, hidden = false;
        for (const auto& a : ab) {
            if (a.id == "Blink") blink = true;
            if (a.id == "AcidDeathPuddle") acid = true;
            if (a.id == "Hidden") hidden = true;
        }
        if (!blink || !acid || !hidden) bad("Blink/AcidDeathPuddle/Hidden present");
        for (const auto& a : ab) {
            if (a.id == "Blink" && !abilityMatches(a, "Bat", "MT_BLINK"))
                bad("Blink matches MT_BLINK");
            if (a.id == "Blink" && abilityMatches(a, "Bat", "MT_FAMILIAR"))
                bad("Blink rejects MT_FAMILIAR");
            if (a.id == "AcidDeathPuddle" && !abilityMatches(a, "Acid", "MT_XACID"))
                bad("Acid puddle matches MT_XACID");
            if (a.id == "Hidden" && !abilityMatches(a, "Sneak", "MT_SNEAK"))
                bad("Hidden matches Sneak AI");
            if (a.id == "Hidden" && abilityMatches(a, "Zombie", "MT_NZOMBIE"))
                bad("Hidden rejects Zombie");
        }
    }

    // (e) impact / status classification
    if (impactOverride("Fireball") != "MagmaBallExplosion") bad("Fireball impact override");
    if (!isImpactGraphic("MagmaBallExplosion")) bad("isImpactGraphic");
    if (!isStatusGraphic("ManaShield") || isStatusGraphic("Fireball"))
        bad("isStatusGraphic");

    // (f) deriveEntry with tiny in-memory tables
    Table mis = parseTsvText(
        "id\tgraphic\tflags\taddFn\tprocessFn\n"
        "Fireball\tFireball\tFire\tAddFireball\tProcessFireball\n"
        "FireWallControl\tFireWall\tFire\tAddWallControl\tProcessWallControl\n"
        "StoneCurse\t\tMagic,Invisible\tAddStoneCurse\tProcessStoneCurse\n"
        "FlashBottom\tFlashBottom\tMagic\tAddFlashBottom\tProcessFlashBottom\n"
        "FlashTop\tFlashTop\tMagic\tAddFlashTop\tProcessFlashTop\n");
    Table spr = parseTsvText(
        "id\twidth\tnumFrames\tname\tframeLength\n"
        "Fireball\t96\t16\tfireba\t14\n"
        "FireWall\t128\t2\tfirewal\t13,11\n"
        "MagmaBallExplosion\t128\t1\tmagblos\t16\n"
        "StoneCurseShatter\t128\t1\tshatter1\t1\n"
        "FlashBottom\t160\t1\tbluexfr\t1\n"
        "FlashTop\t160\t1\tbluexbk\t1\n");
    std::map<std::string, Row> misById;
    for (const auto& r : mis) misById[val(r, "id")] = r;
    std::map<std::string, SpriteInfo> sprById;
    for (const auto& r : spr) sprById[val(r, "id")] = spriteFilesFor(r);

    Row fb;
    fb["id"] = "Fireball"; fb["name"] = "Fireball";
    fb["flags"] = "Fire,Targeted"; fb["missiles"] = "Fireball";
    Entry e = deriveEntry(fb, misById, sprById, "player");
    if (e.stages.size() != 4) bad("Fireball 4 stages");
    else {
        if (e.stages[0].layers.size() != 1 || e.stages[0].layers[0].action != "fm")
            bad("Fireball cast stage");
        if (e.stages[1].layers.size() != 1 ||
            e.stages[1].layers[0].files.size() != 16 ||
            e.stages[1].layers[0].width != 96)
            bad("Fireball projectile stage");
        if (e.stages[2].layers.size() != 1 ||
            e.stages[2].layers[0].graphic_id != "MagmaBallExplosion")
            bad("Fireball impact stage");
        if (!e.stages[3].layers.empty()) bad("Fireball debuff stage empty");
    }

    // Flash: two comma-separated missiles -> two projectile layers.
    Row flash;
    flash["id"] = "Flash"; flash["name"] = "Flash";
    flash["flags"] = "Magic"; flash["missiles"] = "FlashBottom,FlashTop";
    Entry ef = deriveEntry(flash, misById, sprById, "player");
    if (ef.missiles.size() != 2 || ef.stages[1].layers.size() != 2)
        bad("Flash two missiles -> two projectile layers");

    // Stone Curse: TRN debuff + impact shatter.
    Row sc;
    sc["id"] = "StoneCurse"; sc["name"] = "Stone Curse";
    sc["flags"] = "Magic"; sc["missiles"] = "StoneCurse";
    Entry e2 = deriveEntry(sc, misById, sprById, "player");
    if (e2.stages[2].layers.empty() ||
        e2.stages[2].layers[0].graphic_id != "StoneCurseShatter")
        bad("Stone Curse impact");
    bool hasTrn = false;
    for (const auto& l : e2.stages[3].layers)
        if (l.kind == "trn" && l.trn_preset == "stone" && l.anchor == "monster")
            hasTrn = true;
    if (!hasTrn) bad("Stone Curse TRN debuff layer");

    // (g) Chinese name + synthesized description (no disk; map provided directly)
    std::map<std::string, std::string> zh;
    zh["Firebolt"] = "火栓";
    Row fbz;
    fbz["id"] = "Firebolt"; fbz["name"] = "Firebolt";
    fbz["flags"] = "Fire,Targeted"; fbz["manaCost"] = "8";
    fbz["minIntelligence"] = "15"; fbz["missiles"] = "Fireball";
    Entry ez = deriveEntry(fbz, misById, sprById, "player", &zh);
    if (ez.name_zh != "火栓") bad("Firebolt name_zh from po map");
    if (ez.description.empty()) bad("description synthesized");
    if (ez.description.find("火系法术") == std::string::npos) bad("description has element word");
    // missing translation -> English fallback, still has description
    Row nov;
    nov["id"] = "Nova"; nov["name"] = "Nova";
    nov["flags"] = "Lightning"; nov["missiles"] = "LightningControl";
    Entry en = deriveEntry(nov, misById, sprById, "player", &zh);
    if (en.name_zh != "Nova") bad("Nova falls back to English name_zh");
    if (en.description.empty()) bad("Nova description present on fallback");

    // (h) spell-level parameters from spelldat.tsv (level / mana_by_level)
    Row fw;
    fw["id"] = "FireWall"; fw["name"] = "Fire Wall";
    fw["flags"] = "Fire,Targeted"; fw["missiles"] = "FireWallControl";
    fw["manaCost"] = "28"; fw["bookLevel"] = "3";
    fw["manaMultiplier"] = "2"; fw["minMana"] = "16";
    Entry efw = deriveEntry(fw, misById, sprById, "player");
    if (efw.level != "3") bad("FireWall level == 3");
    if (efw.mana_multiplier != "2") bad("FireWall mana_multiplier == 2");
    if (efw.mana_by_level.size() != 3) bad("FireWall mana_by_level has 3 levels");
    else {
        // cost = 28 - (lvl-1)*2, floored at minMana=16
        if (efw.mana_by_level[0].first != 1 || efw.mana_by_level[0].second != 28)
            bad("FireWall L1 mana == 28");
        if (efw.mana_by_level[1].first != 2 || efw.mana_by_level[1].second != 26)
            bad("FireWall L2 mana == 26");
        if (efw.mana_by_level[2].first != 3 || efw.mana_by_level[2].second != 24)
            bad("FireWall L3 mana == 24");
    }
    if (!efw.description.empty() &&
        efw.description.find("法术等级 3") == std::string::npos)
        bad("FireWall description mentions spell level");
    // 帮助函数：取某级别的视觉参数
    auto findP = [](const Entry& e, int lvl, const std::string& k) -> std::string {
        for (const auto& kv : e.visual_by_level)
            if (kv.first == lvl) {
                auto it = kv.second.find(k);
                return it == kv.second.end() ? "" : it->second;
            }
        return "";
    };
    // visual_by_level: FireWall duration = 10*(lvl+1)*16, no radius/count/speed keys
    if (efw.visual_by_level.size() != 3) bad("FireWall visual_by_level has 3 levels");
    if (findP(efw, 3, "duration_ticks") != "640") bad("FireWall L3 duration_ticks == 640");
    if (findP(efw, 1, "duration_ticks") != "320") bad("FireWall L1 duration_ticks == 320");
    if (!findP(efw, 1, "radius_tiles").empty()) bad("FireWall has no radius_tiles");
    if (findP(efw, 1, "_note").empty()) bad("FireWall visual params carry _note");
    // FlameWave: segments = (lvl/2)+2, wall_length = 2*seg+1
    Row flw; flw["id"] = "FlameWave"; flw["name"] = "Flame Wave";
    flw["flags"] = "Fire"; flw["missiles"] = "FlameWaveControl"; flw["bookLevel"] = "9";
    Entry eflw = deriveEntry(flw, misById, sprById, "player");
    if (eflw.visual_by_level.size() != 9) bad("FlameWave visual_by_level has 9 levels");
    if (findP(eflw, 9, "segments") != "6") bad("FlameWave L9 segments == 6");
    if (findP(eflw, 9, "wall_length_tiles") != "13") bad("FlameWave L9 wall_length == 13");
    if (findP(eflw, 9, "speed") != "16") bad("FlameWave speed == 16");
    // ChargedBolt: count = (lvl/2)+3
    Row cb; cb["id"] = "ChargedBolt"; cb["name"] = "Charged Bolt";
    cb["flags"] = ""; cb["missiles"] = "ChargedBolt"; cb["bookLevel"] = "1";
    Entry ecb = deriveEntry(cb, misById, sprById, "player");
    if (ecb.visual_by_level.empty()) bad("ChargedBolt has visual_by_level");
    else if (findP(ecb, 1, "count") != "3") bad("ChargedBolt L1 count == 3");
    // HolyBolt: speed = 16 + min(lvl*2,47)
    Row hb; hb["id"] = "HolyBolt"; hb["name"] = "Holy Bolt";
    hb["flags"] = ""; hb["missiles"] = "HolyBolt"; hb["bookLevel"] = "1";
    Entry ehb = deriveEntry(hb, misById, sprById, "player");
    if (ehb.visual_by_level.empty()) bad("HolyBolt has visual_by_level");
    else if (findP(ehb, 1, "speed") != "18") bad("HolyBolt L1 speed == 18");
    // Guardian: duration depends on refChar (50) -> 9+25=34 clamp30 <<4 =480
    Row gd; gd["id"] = "Guardian"; gd["name"] = "Guardian";
    gd["flags"] = ""; gd["missiles"] = "Guardian"; gd["bookLevel"] = "9";
    Entry egd = deriveEntry(gd, misById, sprById, "player");
    if (egd.visual_by_level.empty()) bad("Guardian has visual_by_level");
    else if (findP(egd, 9, "duration_ticks") != "480")
        bad("Guardian L9 duration_ticks == 480 (refChar=50)");
    // Identify (utility, bookLevel=-1): no visual_by_level
    Row idn; idn["id"] = "Identify"; idn["name"] = "Identify";
    idn["flags"] = ""; idn["missiles"] = ""; idn["bookLevel"] = "-1";
    Entry eid = deriveEntry(idn, misById, sprById, "player");
    if (!eid.visual_by_level.empty()) bad("Identify has no visual_by_level");
    // Stone Curse: duration = min(lvl+6,15) << 4
    Row scRow; scRow["id"] = "StoneCurse"; scRow["name"] = "Stone Curse";
    scRow["flags"] = ""; scRow["missiles"] = "StoneCurse"; scRow["bookLevel"] = "6";
    Entry esc = deriveEntry(scRow, misById, sprById, "player");
    if (esc.visual_by_level.empty()) bad("Stone Curse has visual_by_level");
    else {
        if (findP(esc, 1, "duration_ticks") != "112") bad("Stone Curse L1 duration == 112 (min(7,15)<<4)");
        if (findP(esc, 6, "duration_ticks") != "192") bad("Stone Curse L6 duration == 192 (min(12,15)<<4)");
    }
    // Inferno: radius_tiles = min(lvl+3, MaxCrawlRadius=18)
    Row inf; inf["id"] = "Inferno"; inf["name"] = "Inferno";
    inf["flags"] = ""; inf["missiles"] = "InfernoControl"; inf["bookLevel"] = "3";
    Entry einf = deriveEntry(inf, misById, sprById, "player");
    if (einf.visual_by_level.empty()) bad("Inferno has visual_by_level");
    else if (findP(einf, 3, "radius_tiles") != "6") bad("Inferno L3 radius_tiles == 6");
    // Firebolt (player): constant duration 256, speed 26
    Row fbz2; fbz2["id"] = "Firebolt"; fbz2["name"] = "Firebolt";
    fbz2["flags"] = ""; fbz2["missiles"] = "Firebolt"; fbz2["bookLevel"] = "1";
    Entry efbz = deriveEntry(fbz2, misById, sprById, "player");
    if (efbz.visual_by_level.empty()) bad("Firebolt has visual_by_level");
    else if (findP(efbz, 1, "speed") != "26") bad("Firebolt L1 speed == 26 (player constant)");
    // Firebolt halves the per-level mana adjustment (game special-case)
    Row fb2;
    fb2["id"] = "Firebolt"; fb2["name"] = "Firebolt";
    fb2["flags"] = "Fire"; fb2["manaCost"] = "6";
    fb2["bookLevel"] = "1"; fb2["manaMultiplier"] = "1"; fb2["minMana"] = "3";
    Entry efb = deriveEntry(fb2, misById, sprById, "player");
    if (efb.mana_by_level.size() != 1 || efb.mana_by_level[0].second != 6)
        bad("Firebolt mana_by_level (halved adj) == 6");
    // Monster entries carry no spell-level params
    Row mon;
    mon["id"] = "Acid"; mon["name"] = "Acid"; mon["flags"] = ""; mon["missiles"] = "Acid";
    Entry em = deriveEntry(mon, misById, sprById, "monster");
    if (!em.mana_by_level.empty()) bad("monster entry has no mana_by_level");

    // Multi-form monster attacks: variant / condition land in the entry and the
    // synthesized description, while the missile chain stays per form.
    Row form;
    form["id"] = "Counselor.Fireball"; form["name"] = "Counselor";
    form["flags"] = "Fire"; form["missiles"] = "Fireball";
    Entry efm = deriveEntry(form, misById, sprById, "monster", nullptr, "Fireball",
                            "智力 3 的顾问远程弹");
    if (efm.variant != "Fireball") bad("attack form variant stored");
    if (efm.condition.empty()) bad("attack form condition stored");
    if (efm.description.find("Counselor / Fireball") == std::string::npos)
        bad("attack form description names the form");
    if (efm.description.find("智力 3") == std::string::npos)
        bad("attack form description carries the condition");
    if (efm.stages[1].layers.empty()) bad("attack form has a projectile layer");
    if (efm.cast_action != "fm") bad("attack form cast action follows the missile element");

    printf("spell_tables (pure joins): %s\n", allOk ? "PASS" : "FAIL");
}

int main()
{
    printf("=== DiabloHFConverter unit tests ===\n");
    testClxDecode();
    testCelRoundTrip();
    testCelRejectsMalformed();
    testCl2RoundTrip();
    testCl2RejectsMalformed();
    testPcxDecode();
    testPalette();
    testBase64();
    testPngWrite();
    testSpriteWidth();
    testSpellTables();
    printf("=== %s ===\n", allOk ? "ALL PASS" : "FAILURES PRESENT");
    return allOk ? 0 : 1;
}
