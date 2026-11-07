// src/tools/sprite_tools.h
//
// Sprite/palette MCP tools for the frontend. These wrap the decoders in
// src/decoders/ and expose them over the shared ToolRegistry (stdio + WS):
//
//   palette.load  {archive, name}                       -> {count, colors, rgba_base64}
//   sprite.decode {archive, name, palette_*, width?, frame?}
//                                                    -> {format, width, height, frames[]}
//
// `sprite.decode` auto-detects the format by extension (.clx/.cl2/.cel/.pcx) and
// returns every frame as an RGBA base64 blob the browser can upload as a texture.

#pragma once

#include "tools/registry.h"

namespace mcp {
namespace tools {

void registerSpriteTools(ToolRegistry& reg);

} // namespace tools
} // namespace mcp
