// src/decoders/sprite_width.h
//
// Auto frame-width lookup for Diablo 1 / Hellfire sprites.
//
// CL2/CEL files do NOT store their frame width; it comes from DevilutionX's
// runtime animation tables. Given a sprite path (either a bare in-archive name
// like "plrgfx\warrior\wlu\wluas.cl2" or an extracted path with any prefix),
// autoWidth() detects the sprite family and looks the correct width up in the
// authoritative data tables shipped with DevilutionX:
//
//   plrgfx    -> assets/txtdata/classes/<classPath>/sprites.tsv
//   monsters  -> assets/txtdata/monsters/monstdat.tsv        (column "width")
//   missiles  -> assets/txtdata/missiles/missile_sprites.tsv (column "width")
//   objects   -> assets/txtdata/objects/objdat.tsv           (column "animWidth")
//   towners   -> assets/txtdata/towners/towners.tsv           (column "animWidth")
//
// items (CEL) have no width table and still require a manual width.
//
// The type-code -> column mapping is taken verbatim from DevilutionX:
//   Source/player.cpp:2098-2142 (type code) and :1479-1502 (graphic -> column),
//   player.h:176-186 (weapon char; 'b' = Bow).
//
// Shared by mpq-info/render_sprite.cpp (CLI) and src/tools/sprite_tools.cpp
// (MCP tool sprite.decode, width:"auto").

#ifndef DHFC_DECODERS_SPRITE_WIDTH_H
#define DHFC_DECODERS_SPRITE_WIDTH_H

#include <filesystem>
#include <string>

namespace decoders {
namespace sprite_width {

// Map a 2-char player sprite type code (e.g. "as") + weapon char to the
// sprites.tsv column holding its width. Returns false for unknown codes.
bool playerTypeToColumn(const std::string& type, char weaponChar, std::string& col);

// Auto-determine the frame width for a sprite path using DevilutionX data tables.
// Returns width (>0) on success, or <=0 on failure (dbg holds a human-readable
// reason). devRoot is the DevilutionX source root.
int autoWidth(const std::string& spritePath, const std::filesystem::path& devRoot,
              std::string& dbg);

} // namespace sprite_width
} // namespace decoders

#endif // DHFC_DECODERS_SPRITE_WIDTH_H
