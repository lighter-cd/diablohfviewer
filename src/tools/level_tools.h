// src/tools/level_tools.h
#pragma once

#include "tools/registry.h"

namespace mcp {
namespace tools {

// level.inspect: decode/inspect a level tileset (TIL/MIN/CEL) and render
// individual megatiles / micro pieces / CEL frames. Used by the level viewer.
void registerLevelTools(ToolRegistry& reg);

} // namespace tools
} // namespace mcp
