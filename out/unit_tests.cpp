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
    } else {
        printf("  (sprite_width autoWidth: SKIP, DevilutionX not found at %s)\n", dev.string().c_str());
    }

    printf("sprite_width: %s\n", ok ? "PASS" : "FAIL");
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
    testPngWrite();
    testSpriteWidth();
    printf("=== %s ===\n", allOk ? "ALL PASS" : "FAILURES PRESENT");
    return allOk ? 0 : 1;
}
