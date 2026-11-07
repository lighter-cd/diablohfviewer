// src/decoders/png.h
//
// Minimal, dependency-free PNG writer for the decoded RGBA sprite frames.
//
// It emits a single truecolor+alpha (color type 6, 8-bit) PNG. The IDAT is a
// *stored* (uncompressed, BTYPE=00) zlib stream, so no zlib/deflate library is
// required -- we compute the zlib header, adler32 and chunk CRC32 ourselves.
// This keeps the native build self-contained (no -lz needed) while still
// producing files any standard PNG viewer/decoder will open.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace decoders {

// Write `rgba` (width*height*4, RGBA8, row-major) to `path` as a PNG.
// Returns true on success. `width`/`height` must be > 0.
bool writePNG(const std::string& path, uint16_t width, uint16_t height,
              const std::vector<uint8_t>& rgba);

} // namespace decoders
