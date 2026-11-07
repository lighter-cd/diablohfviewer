// src/tools/base64.h
//
// Minimal base64 encoder (RFC 4648) for embedding binary tool output
// (e.g. decoded sprites) inside JSON. Only encoding is needed server-side.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace mcp {
namespace base64 {

inline std::string encode(const uint8_t* data, size_t len) {
    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t b = (uint32_t)data[i] << 16;
        if (i + 1 < len) b |= (uint32_t)data[i + 1] << 8;
        if (i + 2 < len) b |= (uint32_t)data[i + 2];
        out.push_back(tbl[(b >> 18) & 0x3F]);
        out.push_back(tbl[(b >> 12) & 0x3F]);
        if (i + 1 < len) out.push_back(tbl[(b >> 6) & 0x3F]);
        else             out.push_back('=');
        if (i + 2 < len) out.push_back(tbl[b & 0x3F]);
        else             out.push_back('=');
    }
    return out;
}

inline std::string encode(const std::vector<uint8_t>& v) {
    return encode(v.data(), v.size());
}

// Decode a base64 string (RFC 4648, with or without trailing '=' padding) back
// into bytes. Invalid characters are ignored. Used to accept palette blobs the
// frontend may have obtained elsewhere (e.g. palette.load output).
inline std::vector<uint8_t> decode(const std::string& in) {
    static const int8_t inv[256] = {
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1, -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63, 52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
        -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14, 15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
        -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40, 41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1, -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1, -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1, -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1, -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1
    };
    std::vector<uint8_t> out;
    out.reserve((in.size() * 3) / 4);
    int acc = 0, bits = 0;
    for (unsigned char c : in) {
        int v = inv[c];
        if (v < 0) continue; // skip '=' and anything non-base64
        acc = (acc << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((acc >> bits) & 0xFF));
        }
    }
    return out;
}

} // namespace base64
} // namespace mcp
