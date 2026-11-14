// src/tools/scene_tools.h
//
// MCP tools that assemble and render complete Diablo 1 / Hellfire SCENES
// (as opposed to map.build, which renders a single .dun preset in isolation).
//
//   scene.list   -> [{id,title,layout,grid_width,grid_height,micro_len,...}]
//                   (no MPQ; random scenes carry randomizable/random_level)
//   scene.build  -> {id,archive,archives?,out?,preview_max_width?} renders a
//                   full scene (town sectors or Dirt-prefilled dungeon), writes
//                   a native PNG to disk, and returns a bounded base64 preview
//                   plus an aligned overlay PNG and per-cell SOL metadata.
//   scene.tiles  -> {id,archive,...} front-end-renderable index atlas + layout.
//   scene.random -> {action,id,...} seed-generated Cathedral dungeon levels
//                   (generate / save / list), same tiles output as scene.tiles.

#pragma once

#include "tools/registry.h"

namespace mcp {
namespace tools {

void registerSceneTools(ToolRegistry& reg);

} // namespace tools
} // namespace mcp
