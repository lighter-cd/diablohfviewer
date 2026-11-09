// src/tools/ui_tools.h
//
// Stage 5 (goal 4): MCP tools that expose the DiabloUI layout catalog and render
// real in-game UI art into a composited 640x480 PNG.
//
//   ui.list    -> {count, categories, screens[]}                 (no MPQ)
//   ui.layout  -> full structured layout of one screen            (no MPQ)
//   ui.build   -> decode real MPQ art, composite a 640x480 PNG +
//                 aligned element-rectangle overlay, write out/ui/ artifacts,
//                 return base64 PNGs + per-element decode metadata.
//
// Text is NOT burned into the backend PNG: the structured text/font/color/align
// fields in ui.layout / ui.build.elements drive the frontend's approximate-font
// text layer (see web/src/main.js drawGuiText). The backend PNG stays a pure
// "real art + overlay boxes" verifiable rendering.

#pragma once

#include "tools/registry.h"

namespace mcp {
namespace tools {

void registerUiTools(ToolRegistry& reg);

} // namespace tools
} // namespace mcp
