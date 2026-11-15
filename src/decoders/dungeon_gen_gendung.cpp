/**
 * @file dungeon_gen_gendung.cpp
 *
 * Shared global state and helpers for the Catacombs / Caves / Hell faithful
 * ports (drlg_l2/l3/l4). Transcribed from DevilutionX Source/levels/gendung.cpp
 * (the tile-layer-affecting parts). Everything here operates on the shared
 * globals declared in dungeon_gen_common.h so the per-level generators can be
 * transcribed verbatim.
 *
 * Rendering-only state (dPiece / pMegaTiles / dSpecial / dFlags) is intentionally
 * omitted: it never modifies `dungeon`. Transparency (dTransVal) is kept as a
 * faithful call sequence but is not part of the output and does not affect the
 * megatile grid.
 */
#include "decoders/dungeon_gen_common.h"

#include <cstring>

namespace dhfc {
namespace gen {

// ----------------------------------------------------------------------- RNG
DiabloRng g_rng(0);

int32_t GenerateRnd(int32_t v) { return g_rng.generateRnd(v); }
bool FlipCoin(unsigned frequency) { return g_rng.flipCoin(frequency); }
void DiscardRandomValues(unsigned count) { g_rng.discard(count); }
uint32_t GetLCGEngineState() { return g_rng.state(); }
void SetRndSeed(uint32_t seed) { g_rng.seed(seed); }

int RandomIntBetween(int low, int high) { return GenerateRnd(high - low + 1) + low; }

// -------------------------------------------------------------- grid + flags
uint8_t dungeon[DMAXX][DMAXY] {};
BitGrid ProtectedBits {};
int TransVal = 1;
int8_t dTransVal[128][128] {};

QuestState Quests[NUM_QUESTS] {};
bool g_useMultiplayerQuests = false;

dungeon_type leveltype = DTYPE_CATHEDRAL;
int currlevel = 0;
lvl_entry entry = ENTRY_MAIN;
WorldTileRectangle SetPiece {};
WorldTileRectangle SetPieceRoom {};

struct ThemeLoc {
    WorldTileRectangle room {};
    int ttval = 0;
};
// DevilutionX declares `themeLoc[MAXTHEMES]` with MAXTHEMES == 50; the
// generator can emit more than 4 theme rooms, so the array must be sized to
// match or CreateThemeRoom() reads past the end.
ThemeLoc themeLoc[50] {};
int themeCount = 0;

// ------------------------------------------------------------- Miniset table
bool minisetMatches(const Miniset& miniset, int x, int y, bool respectProtected)
{
    for (int yy = 0; yy < miniset.height; ++yy) {
        for (int xx = 0; xx < miniset.width; ++xx) {
            if (miniset.search[yy][xx] != 0
                && dungeon[x + xx][y + yy] != miniset.search[yy][xx])
                return false;
            if (respectProtected && ProtectedBits.test(x + xx, y + yy))
                return false;
        }
    }
    return true;
}

void minisetPlace(const Miniset& miniset, int x, int y)
{
    for (int yy = 0; yy < miniset.height; ++yy) {
        for (int xx = 0; xx < miniset.width; ++xx) {
            if (miniset.replace[yy][xx] == 0)
                continue;
            dungeon[x + xx][y + yy] = miniset.replace[yy][xx];
        }
    }
}

std::optional<std::pair<int, int>> placeMiniSet(const Miniset& miniset, int tries, bool drlg1Quirk)
{
    const int sw = miniset.width;
    const int sh = miniset.height;
    int x = GenerateRnd(DMAXX - sw);
    int y = GenerateRnd(DMAXY - sh);

    for (int i = 0; i < tries; i++, x++) {
        if (x == DMAXX - sw) {
            x = 0;
            y++;
            if (y == DMAXY - sh)
                y = 0;
        }

        // Limit the position of SetPieces for compatibility with the Diablo bug.
        if (drlg1Quirk) {
            bool valid = true;
            if (x <= 12) {
                x++;
                valid = false;
            }
            if (y <= 12) {
                y++;
                valid = false;
            }
            if (!valid)
                continue;
        }

        if (SetPieceRoom.contains({ x, y }))
            continue;
        if (!minisetMatches(miniset, x, y))
            continue;

        minisetPlace(miniset, x, y);
        return std::pair { x, y };
    }

    // Fallback: if the random-start scan above missed every match (possible when
    // only a few candidate positions exist and the RNG-derived start lands away
    // from them), do a deterministic full-grid scan so a valid placement is never
    // skipped. Does not consume RNG, so it only affects callers where the 199-try
    // random scan would otherwise fail.
    for (int yy = 0; yy < DMAXY - sh; yy++) {
        for (int xx = 0; xx < DMAXX - sw; xx++) {
            if (SetPieceRoom.contains({ xx, yy }))
                continue;
            if (!minisetMatches(miniset, xx, yy))
                continue;
            minisetPlace(miniset, xx, yy);
            return std::pair { xx, yy };
        }
    }
    return {};
}

// --------------------------------------------------------- shared gendung.cpp
namespace {
constexpr WorldTileDisplacement South { 0, 1 };
} // namespace

WorldTilePosition ViewPosition {};

void DRLG_InitTrans()
{
    memset(dTransVal, 0, sizeof(dTransVal));
    TransVal = 1;
}

void DRLG_RectTrans(WorldTileRectangle area)
{
    const WorldTilePosition position = area.position;
    const WorldTileSize size = area.size;
    for (int j = position.y; j <= position.y + size.height; j++) {
        for (int i = position.x; i <= position.x + size.width; i++) {
            if (i >= 0 && i < 128 && j >= 0 && j < 128)
                dTransVal[i][j] = static_cast<int8_t>(TransVal);
        }
    }
    TransVal++;
}

void DRLG_MRectTrans(WorldTileRectangle area)
{
    DRLG_RectTrans({ area.position.megaToWorld() + South, area.size * 2 - 1 });
}

void DRLG_MRectTrans(WorldTilePosition origin, WorldTilePosition extent)
{
    DRLG_MRectTrans({ origin, WorldTileSize { extent.x - origin.x, extent.y - origin.y } });
}

void DRLG_CopyTrans(int sx, int sy, int dx, int dy)
{
    if (sx >= 0 && sx < 128 && sy >= 0 && sy < 128 && dx >= 0 && dx < 128 && dy >= 0 && dy < 128)
        dTransVal[dx][dy] = dTransVal[sx][sy];
}

WorldTileSize GetDunSize(const uint16_t* dunData)
{
    return WorldTileSize { static_cast<int>(dunData[0]), static_cast<int>(dunData[1]) };
}

std::vector<uint16_t> buildDunBuffer(const DunTileLayer& l)
{
    std::vector<uint16_t> buf(2 + l.tiles.size());
    buf[0] = static_cast<uint16_t>(l.width);
    buf[1] = static_cast<uint16_t>(l.height);
    for (size_t i = 0; i < l.tiles.size(); i++)
        buf[2 + i] = l.tiles[i];
    return buf;
}

void PlaceDunTiles(const uint16_t* dunData, int px, int py, int floorId)
{
    const WorldTileSize size = GetDunSize(dunData);
    const uint16_t* tileLayer = &dunData[2];
    for (int j = 0; j < size.height; j++) {
        for (int i = 0; i < size.width; i++) {
            const uint16_t raw = tileLayer[j * size.width + i];
            // Swap16LE for the little-endian .dun files on disk.
            const uint8_t tileId = static_cast<uint8_t>(
                ((raw & 0xFF) << 8) | ((raw >> 8) & 0xFF));
            if (tileId != 0) {
                dungeon[px + i][py + j] = tileId;
                ProtectedBits.set(px + i, py + j);
            } else if (floorId != 0) {
                dungeon[px + i][py + j] = static_cast<uint8_t>(floorId);
            }
        }
    }
}

void Make_SetPC(WorldTileRectangle area)
{
    // Populates dFlags in the reference; dFlags is rendering-only and not part of
    // our megatile-grid output, so this is intentionally a no-op.
    (void)area;
}

std::optional<WorldTileSize> GetSizeForThemeRoom(uint8_t floor, WorldTilePosition origin, int minSize, int maxSize)
{
    if (origin.x + maxSize > DMAXX && origin.y + maxSize > DMAXY)
        return {}; // Original broken bounds check, avoids lower-right corner.
    if (IsNearThemeRoom(origin))
        return {};

    const int maxWidth = std::min(maxSize, DMAXX - origin.x);
    const int maxHeight = std::min(maxSize, DMAXY - origin.y);

    WorldTileSize room { maxWidth, maxHeight };

    for (int i = 0; i < maxSize; i++) {
        int width = i < room.height ? i : 0;
        if (i < maxHeight) {
            while (width < room.width) {
                if (dungeon[origin.x + width][origin.y + i] != floor)
                    break;
                width++;
            }
        }

        int height = i < room.width ? i : 0;
        if (i < maxWidth) {
            while (height < room.height) {
                if (dungeon[origin.x + i][origin.y + height] != floor)
                    break;
                height++;
            }
        }

        if (width < minSize || height < minSize) {
            if (i < minSize)
                return {};
            break;
        }

        room = { std::min(room.width, width), std::min(room.height, height) };
    }

    return room - 2;
}

void CreateThemeRoom(int themeIndex)
{
    const int lx = themeLoc[themeIndex].room.position.x;
    const int ly = themeLoc[themeIndex].room.position.y;
    const int hx = lx + themeLoc[themeIndex].room.size.width;
    const int hy = ly + themeLoc[themeIndex].room.size.height;

    for (int yy = ly; yy < hy; yy++) {
        for (int xx = lx; xx < hx; xx++) {
            if (leveltype == DTYPE_CATACOMBS) {
                if (yy == ly || yy == hy - 1) {
                    dungeon[xx][yy] = 2;
                } else if (xx == lx || xx == hx - 1) {
                    dungeon[xx][yy] = 1;
                } else {
                    dungeon[xx][yy] = 3;
                }
            }
            if (IsAnyOf(leveltype, DTYPE_CAVES, DTYPE_NEST)) {
                if (yy == ly || yy == hy - 1) {
                    dungeon[xx][yy] = 134;
                } else if (xx == lx || xx == hx - 1) {
                    dungeon[xx][yy] = 137;
                } else {
                    dungeon[xx][yy] = 7;
                }
            }
            if (leveltype == DTYPE_HELL) {
                if (yy == ly || yy == hy - 1) {
                    dungeon[xx][yy] = 2;
                } else if (xx == lx || xx == hx - 1) {
                    dungeon[xx][yy] = 1;
                } else {
                    dungeon[xx][yy] = 6;
                }
            }
        }
    }

    if (leveltype == DTYPE_CATACOMBS) {
        dungeon[lx][ly] = 8;
        dungeon[hx - 1][ly] = 7;
        dungeon[lx][hy - 1] = 9;
        dungeon[hx - 1][hy - 1] = 6;
    }
    if (IsAnyOf(leveltype, DTYPE_CAVES, DTYPE_NEST)) {
        dungeon[lx][ly] = 150;
        dungeon[hx - 1][ly] = 151;
        dungeon[lx][hy - 1] = 152;
        dungeon[hx - 1][hy - 1] = 138;
    }
    if (leveltype == DTYPE_HELL) {
        dungeon[lx][ly] = 9;
        dungeon[hx - 1][ly] = 16;
        dungeon[lx][hy - 1] = 15;
        dungeon[hx - 1][hy - 1] = 12;
    }

    if (leveltype == DTYPE_CATACOMBS) {
        if (FlipCoin()) {
            dungeon[hx - 1][(ly + hy) / 2] = 4;
        } else {
            dungeon[(lx + hx) / 2][hy - 1] = 5;
        }
    }
    if (IsAnyOf(leveltype, DTYPE_CAVES, DTYPE_NEST)) {
        if (FlipCoin()) {
            dungeon[hx - 1][(ly + hy) / 2] = 147;
        } else {
            dungeon[(lx + hx) / 2][hy - 1] = 146;
        }
    }
    if (leveltype == DTYPE_HELL) {
        if (FlipCoin()) {
            const int yy = (ly + hy) / 2;
            dungeon[hx - 1][yy - 1] = 53;
            dungeon[hx - 1][yy] = 6;
            dungeon[hx - 1][yy + 1] = 52;
            dungeon[hx - 2][yy - 1] = 54;
        } else {
            const int xx = (lx + hx) / 2;
            dungeon[xx - 1][hy - 1] = 57;
            dungeon[xx][hy - 1] = 6;
            dungeon[xx + 1][hy - 1] = 56;
            dungeon[xx][hy - 2] = 59;
            dungeon[xx - 1][hy - 2] = 58;
        }
    }
}

bool IsNearThemeRoom(WorldTilePosition testPosition)
{
    for (int i = 0; i < themeCount; i++) {
        if (WorldTileRectangle { themeLoc[i].room.position - WorldTileDisplacement { 2, 2 },
                themeLoc[i].room.size + 5 }
                .contains(testPosition))
            return true;
    }
    return false;
}

void DRLG_PlaceThemeRooms(int minSize, int maxSize, int floor, int freq, bool rndSize)
{
    themeCount = 0;
    memset(themeLoc, 0, sizeof(*themeLoc));
    for (int j = 0; j < DMAXY; j++) {
        for (int i = 0; i < DMAXX; i++) {
            if (dungeon[i][j] == static_cast<uint8_t>(floor) && FlipCoin(freq)) {
                std::optional<WorldTileSize> themeSize = GetSizeForThemeRoom(
                    static_cast<uint8_t>(floor), { i, j }, minSize, maxSize);
                if (!themeSize)
                    continue;

                if (rndSize) {
                    const int min = minSize - 2;
                    const int max = maxSize - 2;
                    themeSize->width = min + GenerateRnd(GenerateRnd(themeSize->width - min + 1));
                    if (themeSize->width < min || themeSize->width > max)
                        themeSize->width = min;
                    themeSize->height = min + GenerateRnd(GenerateRnd(themeSize->height - min + 1));
                    if (themeSize->height < min || themeSize->height > max)
                        themeSize->height = min;
                }

                ThemeLoc& theme = themeLoc[themeCount];
                theme.room = { WorldTilePosition { i, j } + South, *themeSize };
                if (IsAnyOf(leveltype, DTYPE_CAVES, DTYPE_NEST)) {
                    DRLG_RectTrans({ (theme.room.position + South).megaToWorld(), theme.room.size * 2 - 5 });
                } else {
                    DRLG_MRectTrans({ theme.room.position, theme.room.size - 1 });
                }
                theme.ttval = TransVal - 1;
                CreateThemeRoom(themeCount);
                themeCount++;
            }
        }
    }
}

} // namespace gen
} // namespace dhfc
