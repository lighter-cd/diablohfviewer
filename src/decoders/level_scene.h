// src/decoders/level_scene.h
//
// Scene assembly: build a full `dPiece` micro grid for a real Diablo 1 /
// Hellfire SCENE (as opposed to rendering a single .dun preset in isolation).
//
// Faithful to DevilutionX Source/levels/town.cpp + gendung.cpp:
//   * Town (Tristram): DrlgTPass3() prefills dPiece[112][112] with micro 426,
//     then FillSector() lays four sector .dun files at fixed offsets. A DUN
//     word of 0 -> micro 218; a word >= 1 -> pMegaTiles[word-1].micro1..4.
//   * Dungeon: DRLG_LPass3() prefills from a "Dirt" megatile, then overlays the
//     DUN tile layer at (16,16).
//
// The output SceneGrid.micro holds 0-based micro indices (kNoMicro = outside),
// matching level_render.cpp's renderMap. SceneGrid.megaId holds 1-based
// megatile ids purely for referencing / overlay metadata.

#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "decoders/level.h"

namespace decoders {

enum class SceneLayout { Town, Dungeon };

struct SceneSpec {
    std::string id;
    std::string title;
    // In-archive asset names (forward or back slashes both work; the Archive
    // layer normalizes them).
    std::string cel, til, min, sol, palette, dun;
    std::vector<std::string> sectorDuns;              // town only
    std::vector<std::pair<int, int>> sectorOffsets;   // town only, micro coords
    SceneLayout layout = SceneLayout::Town;
    uint8_t microLen = 16;                            // town 16, L1 10, L4 12
    uint8_t blocks = 16;                              // MIN stride
    uint16_t prefillMicro = 426;                      // town DrlgTPass3 default
    uint16_t emptyMicro = 218;                        // town FillSector word 0
    uint16_t prefillMegaTile = 22;                    // dungeon Dirt megatile (1-based)
    uint32_t background = 0xFF000000;                 // opaque black RGBA
    int gridW = 112, gridH = 112;                     // dPiece dimensions
    int dunW = 0, dunH = 0;                           // known .dun dims (for catalog)
    // Static lighting defaults (DevilutionX gendung.cpp: town 0, dungeon 15).
    // `lightStyle` is a decoders::LightStyle value (0 normal, 1 hell, 2 soft).
    uint8_t ambientLight = 0;
    uint8_t lightStyle = 0;
};

struct SceneGrid {
    int gw = 0, gh = 0;
    std::vector<uint16_t> micro;   // 0-based into micros; kNoMicro = outside
    std::vector<uint16_t> megaId;  // 1-based megatile id (0 = none/prefill)
};

// Assemble the Tristram town grid. `sectorDunBufs[i]` is the raw bytes of
// sectorDuns[i]. `megatiles` comes from loadTil(town.til).
SceneGrid buildTownScene(const SceneSpec& spec,
                         const std::vector<MegaTile>& megatiles,
                         const std::vector<std::vector<uint8_t>>& sectorDunBufs);

// Assemble a dungeon grid: prefill every cell with spec.prefillMicro (Dirt),
// then overlay the .dun tile layer at (16,16) using `megatiles`.
SceneGrid buildDungeonScene(const SceneSpec& spec,
                            const std::vector<MegaTile>& megatiles,
                            const DunFile& dun);

// Built-in scene catalog (Tristram + two L1 presets). Static, no MPQ needed.
const std::vector<SceneSpec>& sceneCatalog();
const SceneSpec* findScene(const std::string& id);

// Effective dPiece grid dims for a spec (dungeon scenes derive from the .dun
// size; town scenes use the fixed grid). Used by scene.list and scene.build so
// the reported dimensions always match the rendered grid.
std::pair<int, int> sceneGridDims(const SceneSpec& spec);

} // namespace decoders
