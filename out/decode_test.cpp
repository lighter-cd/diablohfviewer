// Unit test for the CLX/CL2 frame decoder. Builds a known CLX buffer by hand
// (the same byte layout celToClx/cl2ToClx emit) and checks decodeClxList output.
#include "decoders/clx.h"
#include "decoders/cel.h"
#include "decoders/cl2.h"
#include "decoders/pcx.h"
#include "decoders/endian_util.h"
#include <cstdio>
#include <cstring>
#include <vector>

using namespace decoders;

static bool allOk = true;

static void chkPx(const std::vector<uint8_t>& rgba, int w, int x, int y,
                  int r, int g, int b, int a)
{
    int o = (y * w + x) * 4;
    if (rgba[o] != r || rgba[o + 1] != g || rgba[o + 2] != b || rgba[o + 3] != a) {
        allOk = false;
        printf("  px(%d,%d) mismatch: got (%d,%d,%d,%d) expected (%d,%d,%d,%d)\n",
               x, y, rgba[o], rgba[o + 1], rgba[o + 2], rgba[o + 3], r, g, b, a);
    }
}

int main()
{
    // Identity-ish palette: entry i -> (i, i+1, i+2).
    uint8_t pal[256 * 3];
    for (int i = 0; i < 256 * 3; ++i) pal[i] = static_cast<uint8_t>(i);

    // Hand-built CLX list:
    //   numSprites = 1
    //   offset[0] = 8, offset[1] = 20   (frame spans bytes 8..20 => 12 bytes)
    //   listSize  = 20
    //   frame header (6 bytes @8): headerSize=6, width=4, height=2
    //   pixel data (6 bytes @14): a 4x2 image:
    //     row0: opaque[5], transparent, opaque[7,7]
    //     row1: opaque[9] x4
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
        // row 1: four opaque pixels of color 9
        0xFC, 9, 9, 9, 9         // opaque pixels: width 4 -> 9 x4
    };
    memcpy(&buf[22], stream, sizeof(stream));

    auto frames = decodeClxList(buf, std::vector<uint8_t>(pal, pal + 256 * 3), 0);
    if (frames.size() != 1) {
        printf("DEBUG buf.size=%zu numSprites=%u off0=%u off1=%u\n",
               buf.size(), LoadLE32(buf.data()), LoadLE32(buf.data() + 4), LoadLE32(buf.data() + 8));
        printf("FAIL: expected 1 frame, got %zu\n", frames.size());
        return 1;
    }
    const auto& f = frames[0];
    if (f.width != 4 || f.height != 2) { printf("FAIL: dims %dx%d\n", f.width, f.height); allOk = false; }
    if (f.rgba.size() != 4 * 2 * 4) { printf("FAIL: rgba size %zu\n", f.rgba.size()); allOk = false; }

    // Identity palette pal[i]=i  =>  color c renders as (3c, 3c+1, 3c+2).
    chkPx(f.rgba, 4, 0, 0, 15, 16, 17, 255);  // color 5
    chkPx(f.rgba, 4, 1, 0, 0, 0, 0, 0);        // transparent
    chkPx(f.rgba, 4, 2, 0, 21, 22, 23, 255);    // color 7
    chkPx(f.rgba, 4, 3, 0, 21, 22, 23, 255);    // color 7
    chkPx(f.rgba, 4, 0, 1, 27, 28, 29, 255);    // color 9
    chkPx(f.rgba, 4, 1, 1, 27, 28, 29, 255);
    chkPx(f.rgba, 4, 2, 1, 27, 28, 29, 255);
    chkPx(f.rgba, 4, 3, 1, 27, 28, 29, 255);

    // A CL2-frame "fill" run should also work: control 0x80 = fill width 0x3F(63),
    // but for a tiny test use control 0xBE (fill width 1) with a color byte.
    std::vector<uint8_t> buf2(27);
    w32(buf2, 0, 1); w32(buf2, 4, 16); w32(buf2, 8, 27); w32(buf2, 12, 27);
    w16(buf2, 16, 6); w16(buf2, 18, 4); w16(buf2, 20, 1);
    uint8_t stream2[] = { 0xBE, 3, 0x01, 0xBE, 200 }; // fill width1 color3; transp1; fill width1 color200
    memcpy(&buf2[22], stream2, sizeof(stream2));
    auto f2frames = decodeClxList(buf2, std::vector<uint8_t>(pal, pal + 256 * 3), 0);
    if (f2frames.empty()) {
        printf("DEBUG buf2 numSprites=%u off0=%u off1=%u size=%zu\n",
               LoadLE32(buf2.data()), LoadLE32(buf2.data() + 4), LoadLE32(buf2.data() + 8), buf2.size());
        return 1;
    }
    chkPx(f2frames[0].rgba, 4, 0, 0, 9, 10, 11, 255);   // color 3  -> (9,10,11)
    chkPx(f2frames[0].rgba, 4, 1, 0, 0, 0, 0, 0);        // transparent
    chkPx(f2frames[0].rgba, 4, 2, 0, 88, 89, 90, 255);    // color 200 -> (200,201,202) mod 256 = (88,89,90)

    printf("decode self-test: %s\n", allOk ? "PASS" : "FAIL");

    // ---- CEL -> CLX -> decode round trip ----------------------------------
    // CEL: numFrames=1, frame@16 with 10-byte header (marker 10) + 2x4 pixels.
    {
        std::vector<uint8_t> cb(35);
        w32(cb, 0, 1); w32(cb, 4, 16); w32(cb, 8, 35); w32(cb, 12, 35);
        w16(cb, 16, 10);                // CEL frame-header marker @16
        // pixel data @26: row0 = 4x color5 ; row1 = transp2 + 2x color7
        uint8_t celpix[] = { 0x04, 5, 5, 5, 5, 0xFE, 0x02, 7, 7 };
        memcpy(&cb[26], celpix, sizeof(celpix));
        auto clx = celToClx(cb.data(), cb.size(), 4);
        if (clx.empty()) {
            printf("CEL converter produced empty CLX (size=%zu)\n", clx.size());
            allOk = false;
        } else {
            printf("CEL clx: size=%zu numSprites=%u off0=%u off1=%u\n",
                   clx.size(), LoadLE32(clx.data()), LoadLE32(clx.data() + 4), LoadLE32(clx.data() + 8));
        }
        auto fr = decodeClx(clx, std::vector<uint8_t>(pal, pal + 256 * 3));
        if (fr.size() != 1) { printf("CEL round-trip: FAIL (frames=%zu)\n", fr.size()); allOk = false; }
        else {
            chkPx(fr[0].rgba, 4, 0, 0, 15, 16, 17, 255);
            chkPx(fr[0].rgba, 4, 3, 0, 15, 16, 17, 255);
            chkPx(fr[0].rgba, 4, 0, 1, 0, 0, 0, 0);            // transparent
            chkPx(fr[0].rgba, 4, 1, 1, 0, 0, 0, 0);            // transparent
            chkPx(fr[0].rgba, 4, 2, 1, 21, 22, 23, 255);       // color 7
            chkPx(fr[0].rgba, 4, 3, 1, 21, 22, 23, 255);
            if (fr[0].width != 4 || fr[0].height != 2) { printf("CEL round-trip: FAIL dims\n"); allOk = false; }
        }
    }

    // ---- CL2 -> CLX -> decode round trip ----------------------------------
    // CL2: numFrames=1, frame@16: 2-byte header(=2) + 2x4 CL2-frame pixels.
    {
        std::vector<uint8_t> cb(27);
        w32(cb, 0, 1); w32(cb, 4, 16); w32(cb, 8, 27); w32(cb, 12, 27);
        w16(cb, 16, 2);                 // pixel data starts 2 bytes into the frame
        // CL2-frame codec: row0 = 4x color5 ; row1 = transp2 + 2x color7
        uint8_t cl2pix[] = { 0xFC, 5, 5, 5, 5, 0x02, 0xFE, 7, 7 };
        memcpy(&cb[18], cl2pix, sizeof(cl2pix));
        auto clx = cl2ToClx(cb.data(), cb.size(), 4);
        auto fr = decodeClx(clx, std::vector<uint8_t>(pal, pal + 256 * 3));
        if (fr.size() != 1) { printf("CL2 round-trip: FAIL (frames=%zu)\n", fr.size()); allOk = false; }
        else {
            chkPx(fr[0].rgba, 4, 0, 0, 15, 16, 17, 255);
            chkPx(fr[0].rgba, 4, 3, 0, 15, 16, 17, 255);
            chkPx(fr[0].rgba, 4, 0, 1, 0, 0, 0, 0);
            chkPx(fr[0].rgba, 4, 1, 1, 0, 0, 0, 0);
            chkPx(fr[0].rgba, 4, 2, 1, 21, 22, 23, 255);
            chkPx(fr[0].rgba, 4, 3, 1, 21, 22, 23, 255);
            if (fr[0].width != 4 || fr[0].height != 2) { printf("CL2 round-trip: FAIL dims\n"); allOk = false; }
        }
    }

    printf("converter round-trip: %s\n", allOk ? "PASS" : "FAIL");

    // ---- PCX decode (hand-built 4x2, embedded palette) ----------------------
    {
        std::vector<uint8_t> pcx(128 + 7 + 1 + 256 * 3);
        pcx[0] = 0x0A;   // manufacturer
        pcx[1] = 5;      // version
        pcx[2] = 1;      // encoding (RLE)
        pcx[3] = 8;      // bits per pixel
        WriteLE16(&pcx[8], 3);   // xmax = W-1
        WriteLE16(&pcx[10], 1);  // ymax = H-1
        pcx[65] = 1;              // nplanes
        WriteLE16(&pcx[66], 4);   // bytesPerLine = W
        size_t pos = 128;
        uint8_t row0[] = { 1, 2, 3, 4 };                 // literals
        uint8_t row1[] = { 0xC3, 5, 7 };                  // run: 3x5, then literal 7
        memcpy(&pcx[pos], row0, sizeof(row0)); pos += sizeof(row0);
        memcpy(&pcx[pos], row1, sizeof(row1)); pos += sizeof(row1);
        pcx[pos++] = 0x0C;                                // palette separator
        for (int i = 0; i < 256; ++i) { pcx[pos++] = static_cast<uint8_t>(i);
                                         pcx[pos++] = static_cast<uint8_t>(i + 1);
                                         pcx[pos++] = static_cast<uint8_t>(i + 2); }
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

    return allOk ? 0 : 1;
}
