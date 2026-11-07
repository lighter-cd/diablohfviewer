// src/decoders/cl2.h
//
// Convert a Diablo 1 CL2 sprite into a CLX buffer (rendered by clx.cpp). Faithful
// port of DevilutionX Cl2ToClx.
//
// CL2 frame pixel data uses the same control-byte encoding as CLX pixel data
// (see clx_encode.h), so once converted the shared CLX decoder handles it.
//
// A CL2 file is: uint32 numFrames; uint32 offset[i] (i in 0..numFrames); then,
// per frame, a 2-byte header whose value is the offset (within the frame) to the
// pixel data. Frame width is not stored and must be supplied. CL2 may also be a
// "group" sheet (list of lists).

#pragma once

#include <cstdint>
#include <vector>

namespace decoders {

// Returns a CLX byte buffer. `frameWidth` is the width of every frame.
std::vector<uint8_t> cl2ToClx(const uint8_t* data, size_t size, uint16_t frameWidth);

} // namespace decoders
