// src/tools/map_tools.h
//
// Map (level) MCP tool: decodes the Diablo 1 / Hellfire level files (TIL / MIN /
// DUN / SOL / level .cel) and renders a complete isometric map (tiles) plus
// collision / lighting / transparency metadata.
//
//   map.build {archive, archives?, cel, til, min, sol, dun, palette?,
//              micro_len?, blocks?, out?}
//           -> {image_path, preview_base64, width, height, grid_width, grid_height,
//               micro_len, frame_count, solid_count, transparent_count, meta_path}

#pragma once

#include "tools/registry.h"

namespace mcp {
namespace tools {

void registerMapTools(ToolRegistry& reg);

} // namespace tools
} // namespace mcp
