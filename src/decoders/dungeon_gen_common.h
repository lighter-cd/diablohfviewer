/**
 * @file dungeon_gen_common.h
 *
 * Shared state and helpers for the Catacombs / Caves / Hell faithful ports
 * (drlg_l2/l3/l4). Modeled on DevilutionX's global layout
 * (Source/levels/gendung.cpp) so the per-level generators can be transcribed
 * line-by-line: they reference `dungeon`, `ProtectedBits`, `leveltype`, etc. as
 * globals exactly as the reference does.
 *
 * Only the 40x40 megatile grid (`dungeon`) matters for our output: every
 * generator's golden .dun fixture IS that grid. The rendering-only state
 * (dPiece / pMegaTiles / dSpecial / dFlags) is intentionally NOT reproduced —
 * it never modifies `dungeon`. Transparency values (dTransVal) are kept as
 * cheap members so the call sequence matches, but they are not part of the
 * output and do not affect `dungeon`.
 *
 * RNG call order is part of the format: all randomness flows through the shared
 * `g_rng` via the free functions GenerateRnd/FlipCoin/DiscardRandomValues,
 * mirroring DevilutionX's sglGameSeed path.
 */
#pragma once

#include "decoders/dungeon_gen.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <optional>
#include <utility>
#include <vector>

namespace dhfc {
namespace gen {

constexpr int DMAXX = DungeonTileGrid::Width;   // 40
constexpr int DMAXY = DungeonTileGrid::Height; // 40

// ----------------------------------------------------------------------- RNG
// Shared LCG, mirroring DevilutionX sglGameSeed. generateCathedral uses its own
// rng instance (it is self-contained in dungeon_gen_l1.cpp and must not change).
extern DiabloRng g_rng;
int32_t GenerateRnd(int32_t v);
bool FlipCoin(unsigned frequency = 2);
void DiscardRandomValues(unsigned count);
uint32_t GetLCGEngineState();
void SetRndSeed(uint32_t seed);
int RandomIntBetween(int low, int high);

/** Pick a random element from an initializer list (DevilutionX PickRandomlyAmong). */
template <typename T>
T PickRandomlyAmong(std::initializer_list<T> list)
{
    if (list.size() == 0)
        return T();
    return *(list.begin() + GenerateRnd(static_cast<int>(list.size())));
}

// -------------------------------------------------------------- grid + flags
extern uint8_t dungeon[DMAXX][DMAXY];

/** Bounds-checked 2D bit set, matching DevilutionX Bitset2d<DMAXX, DMAXY>. */
class BitGrid {
public:
    bool test(int x, int y) const
    {
        if (x < 0 || x >= DMAXX || y < 0 || y >= DMAXY)
            return false;
        return bits_[index(x, y)];
    }
    void set(int x, int y, bool value = true)
    {
        if (x < 0 || x >= DMAXX || y < 0 || y >= DMAXY)
            return;
        bits_[index(x, y)] = value;
    }
    void reset() { bits_.fill(false); }
    void reset(int x, int y) { set(x, y, false); }
    size_t count() const
    {
        size_t n = 0;
        for (bool b : bits_)
            n += b ? 1 : 0;
        return n;
    }

private:
    static size_t index(int x, int y) { return static_cast<size_t>(y) * DMAXX + x; }
    std::array<bool, DMAXX * DMAXY> bits_ {};
};
extern BitGrid ProtectedBits;

// Transparency values (rendering-only; kept so call sequences match).
extern int TransVal;
extern int8_t dTransVal[128][128];

// ------------------------------------------------------------------- level
enum dungeon_type : int {
    DTYPE_NONE = -1,
    DTYPE_TOWN = 0,
    DTYPE_CATHEDRAL = 1,
    DTYPE_CATACOMBS = 2,
    DTYPE_CAVES = 3,
    DTYPE_HELL = 4,
    DTYPE_NEST = 5,
    DTYPE_CRYPT = 6,
};
extern dungeon_type leveltype;
extern int currlevel;

/** Entry direction (does not affect the megatile grid; default ENTRY_MAIN). */
enum lvl_entry {
    ENTRY_MAIN = 0,
    ENTRY_PREV = 1,
    ENTRY_SETLVL = 2,
    ENTRY_RTNLVL = 3,
    ENTRY_LOAD = 4,
    ENTRY_WARPLVL = 5,
    ENTRY_TWARPDN = 6,
    ENTRY_TWARPUP = 7,
    ENTRY_MWARPDN = 8,
};
extern lvl_entry entry;

/** World tile position (x,y in dungeon coords; megatiles are at 2x scale). */
struct WorldTilePosition {
    int x = 0;
    int y = 0;
    WorldTilePosition() = default;
    constexpr WorldTilePosition(int x_, int y_) : x(x_), y(y_) {}
    WorldTilePosition megaToWorld() const { return { x * 2 + 16, y * 2 + 16 }; }
};
struct WorldTileDisplacement {
    int x = 0;
    int y = 0;
    WorldTileDisplacement() = default;
    constexpr WorldTileDisplacement(int x_, int y_) : x(x_), y(y_) {}
};
inline WorldTilePosition operator+(WorldTilePosition a, WorldTileDisplacement d)
{
    return { a.x + d.x, a.y + d.y };
}
inline WorldTilePosition& operator+=(WorldTilePosition& a, WorldTileDisplacement d)
{
    a.x += d.x;
    a.y += d.y;
    return a;
}
struct WorldTileSize {
    int width = 0;
    int height = 0;
    WorldTileSize() = default;
    constexpr WorldTileSize(int w, int h) : width(w), height(h) {}
};
inline WorldTileSize operator*(WorldTileSize s, int k) { return { s.width * k, s.height * k }; }
inline WorldTileSize operator-(WorldTileSize s, int k) { return { s.width - k, s.height - k }; }
inline WorldTileSize operator+(WorldTileSize s, int k) { return { s.width + k, s.height + k }; }
inline WorldTilePosition operator-(WorldTilePosition p, WorldTileDisplacement d)
{
    return { p.x - d.x, p.y - d.y };
}
inline WorldTileDisplacement operator-(WorldTilePosition a, WorldTilePosition b)
{
    return { a.x - b.x, a.y - b.y };
}
inline bool operator==(WorldTilePosition a, WorldTilePosition b)
{
    return a.x == b.x && a.y == b.y;
}
inline bool operator!=(WorldTilePosition a, WorldTilePosition b)
{
    return !(a == b);
}

/** World spawn position (world tiles), set by PlaceStairs. Not part of the
 *  megatile grid output but reported for spawn/entry consistency. */
extern WorldTilePosition ViewPosition;

/** Templated membership helpers (mirrors DevilutionX utils/is_of.hpp). */
template <typename T, typename... Ts>
bool IsAnyOf(T value, Ts... matches)
{
    return ((value == matches) || ...);
}
template <typename T, typename... Ts>
bool IsNoneOf(T value, Ts... matches)
{
    return !IsAnyOf(value, matches...);
}
struct WorldTileRectangle {
    WorldTilePosition position;
    WorldTileSize size;
    bool contains(WorldTilePosition p) const
    {
        return p.x >= position.x && p.x < position.x + size.width
            && p.y >= position.y && p.y < position.y + size.height;
    }
};
extern WorldTileRectangle SetPiece;
extern WorldTileRectangle SetPieceRoom;

// ------------------------------------------------------------- quest state (Hell/L4)
// Minimal quest state needed by the Hell (L4) generator. Mirrors the small slice
// of DevilutionX `Quests[]` that drlg_l4.cpp reads; the full quest system is not
// ported. `_qactive` uses the DevilutionX ordering: 0=NOTAVAIL, 1=INIT, 2=ACTIVE,
// 3=DONE. `IsAvailable()` returns true when the quest can be placed.
enum quest_id {
    Q_WARLORD = 0,
    Q_BETRAYER = 1,
    Q_DIABLO = 2,
    NUM_QUESTS
};
struct QuestState {
    int _qactive = 0;
    int _qlevel = 0;
    int position_x = 0;
    int position_y = 0;
    bool IsAvailable() const { return _qactive != 0; }
};
extern QuestState Quests[NUM_QUESTS];
extern bool g_useMultiplayerQuests;
inline bool UseMultiplayerQuests() { return g_useMultiplayerQuests; }

// ------------------------------------------------------------- Miniset table
struct Miniset {
    int width = 0;
    int height = 0;
    /** Indexed [y][x]; 0 means "don't care" (DevilutionX 0 = wildcard). */
    uint8_t search[6][6] {};
    /** Indexed [y][x]; 0 means "leave untouched". */
    uint8_t replace[6][6] {};
};

bool minisetMatches(const Miniset& miniset, int x, int y, bool respectProtected = true);
void minisetPlace(const Miniset& miniset, int x, int y);
std::optional<std::pair<int, int>> placeMiniSet(const Miniset& miniset, int tries, bool drlg1Quirk);

// --------------------------------------------------------- shared gendung.cpp
void DRLG_InitTrans();
void DRLG_RectTrans(WorldTileRectangle area);
void DRLG_MRectTrans(WorldTileRectangle area);
void DRLG_MRectTrans(WorldTilePosition origin, WorldTilePosition extent);
void DRLG_CopyTrans(int sx, int sy, int dx, int dy);

/** Place a .dun tile layer at (px,py); non-zero tiles overwrite and become
 *  Protected; zero tiles fall back to floorId (0 = leave as-is). Mirrors
 *  DevilutionX PlaceDunTiles. */
void PlaceDunTiles(const uint16_t* dunData, int px, int py, int floorId);
WorldTileSize GetDunSize(const uint16_t* dunData);

/** Build a `uint16_t[width, height, tiles...]` buffer (on-disk little-endian
 *  layout that PlaceDunTiles expects) from a parsed .dun layer. Shared by the
 *  l2/l3/l4 set-piece placers. */
std::vector<uint16_t> buildDunBuffer(const DunTileLayer& l);

void Make_SetPC(WorldTileRectangle area);

void DRLG_PlaceThemeRooms(int minSize, int maxSize, int floor, int freq, bool rndSize);
void CreateThemeRoom(int themeIndex);
std::optional<WorldTileSize> GetSizeForThemeRoom(uint8_t floor, WorldTilePosition origin, int minSize, int maxSize);
bool IsNearThemeRoom(WorldTilePosition testPosition);

} // namespace gen
} // namespace dhfc
