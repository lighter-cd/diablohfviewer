// src/decoders/inv_widths.h
//
// Per-frame widths for Diablo 1's inventory cursor CELs, transcribed verbatim
// from DevilutionX's width tables.
//
// `data\inv\objcurs.cel` (179 frames, base game) and `data\inv\objcurs2.cel`
// (61 frames, Hellfire) are the ICONS shown in the inventory / on the cursor,
// NOT the `items\*.cel` ground-drop animations. Every frame has a DIFFERENT
// width (28px for a 1x1 item, 56px for a 2x2 item, ...), and CEL does not
// store widths, so a single frameWidth cannot decode them. DevilutionX loads
// them with a width ARRAY (Source/cursor.cpp:445-451):
//
//   LoadCel("data\\inv\\objcurs", ReadWidths("data\\inv\\objcurs-widths.txt").data())
//
// Those -widths.txt files are DevilutionX source assets (not MPQ contents), so
// we mirror them here as compile-time constants. This keeps sprite.decode
// independent of an on-disk DevilutionX tree and makes the table trivially
// unit-testable (same header-only pattern as src/tools/spell_tables.h).
//
// Source of truth:
//   DevilutionX/assets/data/inv/objcurs-widths.txt    (179 values)
//   DevilutionX/mods/hf/data/inv/objcurs2-widths.txt (61 values)

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace decoders {
namespace inv_widths {

inline constexpr std::array<uint16_t, 179> kObjcursWidths = {{
    33, 32, 32, 32, 32, 32, 32, 32, 32, 32, 23, 28, 28, 28, 28, 28,
    28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28,
    28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28,
    28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28,
    28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28,
    28, 28, 28, 28, 28, 28, 56, 56, 56, 56, 56, 56, 56, 56, 56, 56,
    56, 56, 56, 56, 56, 56, 56, 56, 56, 56, 56, 56, 56, 56, 56, 56,
    56, 56, 56, 56, 56, 56, 56, 56, 56, 56, 56, 56, 56, 56, 56, 56,
    56, 56, 56, 56, 56, 56, 56, 56, 56, 56, 56, 56, 56, 56, 56, 56,
    56, 56, 56, 56, 56, 56, 56, 56, 56, 56, 56, 56, 56, 56, 56, 56,
    56, 56, 56, 56, 56, 56, 56, 56, 56, 56, 56, 56, 56, 56, 56, 56,
    56, 56, 56
}};

inline constexpr std::array<uint16_t, 61> kObjcurs2Widths = {{
    28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28,
    28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 56, 56,
    28, 28, 28, 56, 56, 56, 56, 56, 56, 56, 56, 56, 56, 56, 56, 56,
    56, 56, 56, 56, 56, 56, 56, 56, 56, 56, 56, 56, 56
}};

// Non-owning view over one of the tables above.
struct WidthTable {
    const uint16_t* data = nullptr;
    std::size_t count = 0;

    bool empty() const { return count == 0; }

    std::vector<uint16_t> toVector() const {
        if (empty()) return {};
        return std::vector<uint16_t>(data, data + count);
    }
};

// Match by lowercased basename so the identical objcurs.cel in DIABDAT.MPQ and
// spawn.mpq resolve to the same table. Panel backgrounds (inv*.cel) and any
// other name return an empty table -- they use a single fixed width instead.
inline WidthTable invWidthsForName(const std::string& name) {
    std::string base = name;
    const std::size_t slash = base.find_last_of("\\/");
    if (slash != std::string::npos)
        base = base.substr(slash + 1);
    for (char& c : base)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    if (base == "objcurs.cel")
        return { kObjcursWidths.data(), kObjcursWidths.size() };
    if (base == "objcurs2.cel")
        return { kObjcurs2Widths.data(), kObjcurs2Widths.size() };
    return {};
}

} // namespace inv_widths
} // namespace decoders
