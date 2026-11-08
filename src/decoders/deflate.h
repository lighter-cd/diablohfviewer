// src/decoders/deflate.h
//
// Minimal, dependency-free zlib/DEFLATE compressor (RFC 1950 + RFC 1951).
// Used so PNG output from the backend stays small enough to embed in a
// JSON/WS response as a data: URL (Firefox caps data: URIs at ~2 MB).
// No external zlib is linked; this implements the fixed-Huffman block type
// with a greedy LZ77 match finder — correct for any input, and compresses
// dark/repetitive scene bitmaps very well.

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace decoders {
namespace deflate {

// Compress `data` (len bytes) into a complete zlib stream (2-byte header +
// deflate body + 4-byte adler32). On success returns the bytes; the output is
// always valid and decodable by any RFC 1950/1951 decoder.
std::vector<uint8_t> compressZlib(const uint8_t* data, size_t len);

inline std::vector<uint8_t> compressZlib(const std::vector<uint8_t>& v) {
    return compressZlib(v.data(), v.size());
}

// Decompress a zlib stream (fixed-Huffman + stored blocks supported). Returns
// false if the stream is malformed or the adler32 check fails. Useful for
// round-trip tests and verification; the production encoder only emits
// fixed-Huffman blocks.
bool decompressZlib(const std::vector<uint8_t>& z, std::vector<uint8_t>& out);

}  // namespace deflate
}  // namespace decoders
