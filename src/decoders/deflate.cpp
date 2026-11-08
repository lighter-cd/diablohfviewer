// src/decoders/deflate.cpp
//
// Minimal zlib/DEFLATE compressor + decompressor (RFC 1950 + RFC 1951).
// Fixed-Huffman block type + greedy LZ77 match finder. Output is always a
// valid, decodeable zlib stream. No external zlib is linked, so PNG/bitmap
// output from the backend stays small enough to embed in a JSON/WS response
// as a data: URL (Firefox caps data: URIs at ~2 MB).
//
// Bit conventions (RFC 1951):
//   * The whole stream is packed into bytes LSB-first (bit 0 of a byte is the
//     first bit written).
//   * Huffman codes (literal/length and the 5-bit distance code) are emitted
//     MSB-first -> see BitWriter::put.
//   * length/distance extra bits are emitted LSB-first -> see BitWriter::putExtra.
//   * Length/distance base+extra tables follow RFC 1951 §3.2.5 EXACTLY: codes
//     257-264 carry 0 extra bits, then 1,1,1,1 / 2,2,2,2 / 3,3,3,3 / 4,4,4,4 /
//     5,5,5,5, and 285 carries 0. Getting these off by even one entry corrupts
//     the stream (the decoder then reads the wrong number of extra bits and
//     desyncs, surfacing as "invalid literal/length code").

#include "decoders/deflate.h"

#include <cstring>
#include <map>
#include <utility>
#include <vector>
#include <algorithm>

namespace decoders {
namespace deflate {

namespace {

// Shared length/distance tables (RFC 1951 §3.2.5).
const int kLenBase[29] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
const int kLenExtra[29] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
const int kDistBase[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
const int kDistExtra[30] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};

uint32_t adler32(const uint8_t* d, size_t n) {
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < n; i++) {
        a = (a + d[i]) % 65521;
        b = (b + a) % 65521;
    }
    return (b << 16) | a;
}

struct BitWriter {
    std::vector<uint8_t>& out;
    uint32_t acc = 0;
    int nbits = 0;
    explicit BitWriter(std::vector<uint8_t>& o) : out(o) {}
    // Huffman code: most-significant bit first.
    void put(uint32_t code, int len) {
        for (int i = len - 1; i >= 0; --i) {
            acc |= ((code >> i) & 1u) << nbits;
            nbits++;
            if (nbits == 8) {
                out.push_back(static_cast<uint8_t>(acc));
                acc = 0;
                nbits = 0;
            }
        }
    }
    // Raw value: least-significant bit first (length/distance extras).
    void putExtra(uint32_t value, int len) {
        for (int i = 0; i < len; ++i) {
            acc |= ((value >> i) & 1u) << nbits;
            nbits++;
            if (nbits == 8) {
                out.push_back(static_cast<uint8_t>(acc));
                acc = 0;
                nbits = 0;
            }
        }
    }
    void flush() {
        if (nbits) {
            out.push_back(static_cast<uint8_t>(acc));
            acc = 0;
            nbits = 0;
        }
    }
};

// Emit a literal/length symbol with its DEFLATE fixed-Huffman code.
void emitSym(BitWriter& bw, int sym) {
    if (sym < 144)       bw.put(0x30 + sym, 8);
    else if (sym < 256)  bw.put(0x190 + (sym - 144), 9);
    else if (sym < 280)  bw.put(sym - 256, 7);
    else                 bw.put(0xC0 + (sym - 280), 8);
}

void emitLength(BitWriter& bw, int len) {
    int code = 28;
    while (code > 0 && len < kLenBase[code]) code--;
    emitSym(bw, 257 + code);
    if (kLenExtra[code]) bw.putExtra(static_cast<uint32_t>(len - kLenBase[code]), kLenExtra[code]);
}

void emitDistance(BitWriter& bw, int dist) {
    int code = 29;
    while (code > 0 && dist < kDistBase[code]) code--;
    bw.put(static_cast<uint32_t>(code), 5);  // distance uses 5-bit fixed codes
    if (kDistExtra[code]) bw.putExtra(static_cast<uint32_t>(dist - kDistBase[code]), kDistExtra[code]);
}

}  // namespace

std::vector<uint8_t> compressZlib(const uint8_t* data, size_t len) {
    std::vector<uint8_t> out;
    out.push_back(0x78);
    out.push_back(0x9C);  // zlib: deflate, 32 KB window, default compression

    BitWriter bw(out);
    bw.put(1, 1);              // BFINAL = 1
    bw.put(1, 1); bw.put(0, 1);  // BTYPE = 01 (fixed Huffman). 2-bit field is LSB-first:
                                 // B1 (first) = 1, B2 (second) = 0 -> value 1.

    // Greedy LZ77 with a 3-byte hash chain, 32 KB window.
    const size_t kWindow = 32768;
    const int kHashBits = 16;
    const int kHashSize = 1 << kHashBits;
    const uint32_t kHashMask = kHashSize - 1;

    std::vector<int> head(kHashSize, -1);
    std::vector<int> prev(len);
    std::fill(prev.begin(), prev.end(), -1);

    auto hash3 = [&](size_t p) -> uint32_t {
        return (static_cast<uint32_t>(data[p]) << 16 |
                static_cast<uint32_t>(data[p + 1]) << 8 |
                static_cast<uint32_t>(data[p + 2])) & kHashMask;
    };

    size_t i = 0;
    while (i < len) {
        int bestLen = 0;
        int bestDist = 0;
        if (i + 3 <= len) {
            uint32_t h = hash3(i);
            int cand = head[h];
            int limit = (i > kWindow) ? static_cast<int>(i - kWindow) : 0;
            while (cand >= limit) {
                if (data[cand] == data[i] && data[cand + 1] == data[i + 1] &&
                    data[cand + 2] == data[i + 2]) {
                    int l = 3;
                    while (l < 258 && i + l < len && data[cand + l] == data[i + l]) l++;
                    if (l > bestLen) {
                        bestLen = l;
                        bestDist = static_cast<int>(i - cand);
                        if (l == 258) break;
                    }
                }
                cand = prev[cand];
            }
        }

        if (bestLen >= 3) {
            emitLength(bw, bestLen);
            emitDistance(bw, bestDist);
            for (int k = 0; k < bestLen; k++) {
                if (i + k + 3 <= len) {
                    uint32_t h = hash3(i + k);
                    prev[i + k] = head[h];
                    head[h] = static_cast<int>(i + k);
                }
            }
            i += static_cast<size_t>(bestLen);
        } else {
            emitSym(bw, data[i]);
            if (i + 3 <= len) {
                uint32_t h = hash3(i);
                prev[i] = head[h];
                head[h] = static_cast<int>(i);
            }
            i++;
        }
    }

    emitSym(bw, 256);  // end-of-block
    bw.flush();

    uint32_t ad = adler32(data, len);
    out.push_back(static_cast<uint8_t>((ad >> 24) & 0xFF));
    out.push_back(static_cast<uint8_t>((ad >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((ad >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(ad & 0xFF));
    return out;
}

// ---------------------------------------------------------------------------
// Decompressor (fixed-Huffman + stored blocks). Sufficient for streams produced
// by compressZlib; also tolerates stored blocks. Used by the test suite and any
// round-trip verification.
// ---------------------------------------------------------------------------

namespace {

// Build the fixed-Huffman literal/length decode map (canonical assignment).
int fixedLitLenDecode(int code, int len) {
    static std::map<std::pair<int, int>, int> m;
    static bool built = false;
    if (!built) {
        int lenOf[288];
        for (int i = 0; i < 288; i++) {
            if (i < 144) lenOf[i] = 8;
            else if (i < 256) lenOf[i] = 9;
            else if (i < 280) lenOf[i] = 7;
            else lenOf[i] = 8;
        }
        std::vector<std::pair<int, int>> items;
        for (int i = 0; i < 288; i++) items.push_back({lenOf[i], i});
        std::sort(items.begin(), items.end());
        int c = 0, last = -1;
        for (auto& it : items) {
            if (it.first > last) { c <<= (it.first - last); last = it.first; }
            m[{c, it.first}] = it.second;
            c++;
        }
        built = true;
    }
    auto it = m.find({code, len});
    return it == m.end() ? -1 : it->second;
}

}  // namespace

bool decompressZlib(const std::vector<uint8_t>& z, std::vector<uint8_t>& out) {
    out.clear();
    if (z.size() < 6) return false;

    size_t bytePos = 2;  // skip zlib header
    uint32_t acc = 0;
    int nbits = 0;
    auto getBit = [&]() -> int {
        if (nbits == 0) {
            if (bytePos >= z.size()) return -1;
            acc = z[bytePos++];
            nbits = 8;
        }
        int b = static_cast<int>(acc & 1u);
        acc >>= 1;
        nbits--;
        return b;
    };

    bool bfinal = false;
    while (!bfinal) {
        int b0 = getBit();
        if (b0 < 0) return false;
        bfinal = (b0 != 0);
        int b1 = getBit();
        int b2 = getBit();
        if (b1 < 0 || b2 < 0) return false;
        int btype = b1 | (b2 << 1);  // 2-bit field is LSB-first: B1 = low bit

        if (btype == 0) {
            // Stored block: align to next byte boundary.
            if (nbits != 0) { nbits = 0; acc = 0; }
            if (bytePos + 4 > z.size()) return false;
            uint32_t blen = z[bytePos] | (z[bytePos + 1] << 8);
            uint32_t bnlen = z[bytePos + 2] | (z[bytePos + 3] << 8);
            bytePos += 4;
            if ((blen ^ bnlen) != 0xFFFF) return false;
            if (bytePos + blen > z.size()) return false;
            out.insert(out.end(), z.begin() + bytePos, z.begin() + bytePos + blen);
            bytePos += blen;
            continue;
        }
        if (btype != 1) return false;  // dynamic not supported by this decoder

        // Fixed-Huffman block.
        while (true) {
            int cur = 0, l = 0, sym = -1;
            while (true) {
                int b = getBit();
                if (b < 0) return false;
                cur = (cur << 1) | b;
                l++;
                sym = fixedLitLenDecode(cur, l);
                if (sym >= 0) break;
                if (l > 9) return false;
            }
            if (sym == 256) break;  // end of block
            if (sym < 256) {
                out.push_back(static_cast<uint8_t>(sym));
                continue;
            }
            int lc = sym - 257;
            if (lc < 0 || lc > 28) return false;
            int length = kLenBase[lc];
            if (kLenExtra[lc]) {
                int ext = 0;
                for (int k = 0; k < kLenExtra[lc]; k++) {
                    int b = getBit();
                    if (b < 0) return false;
                    ext |= b << k;  // extra bits are LSB-first
                }
                length += ext;
            }
            int dc = 0;
            for (int k = 0; k < 5; k++) {
                int b = getBit();
                if (b < 0) return false;
                dc = (dc << 1) | b;
            }
            if (dc > 29) return false;
            int dist = kDistBase[dc];
            if (kDistExtra[dc]) {
                int ext = 0;
                for (int k = 0; k < kDistExtra[dc]; k++) {
                    int b = getBit();
                    if (b < 0) return false;
                    ext |= b << k;  // extra bits are LSB-first
                }
                dist += ext;
            }
            if (dist > static_cast<int>(out.size())) return false;
            for (int k = 0; k < length; k++) out.push_back(out[out.size() - dist]);
        }
    }

    // Verify adler32.
    if (bytePos + 4 > z.size()) return false;
    uint32_t ad = (z[bytePos] << 24) | (z[bytePos + 1] << 16) |
                  (z[bytePos + 2] << 8) | z[bytePos + 3];
    if (ad != adler32(out.data(), out.size())) return false;
    return true;
}

}  // namespace deflate
}  // namespace decoders
