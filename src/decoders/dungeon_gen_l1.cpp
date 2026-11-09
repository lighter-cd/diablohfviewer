/**
 * @file dungeon_gen_l1.cpp
 *
 * Faithful port of DevilutionX `Source/levels/drlg_l1.cpp` (Cathedral / Crypt).
 * Only the Cathedral (non-Crypt) branch is implemented so far.
 *
 * Every function below keeps the same name, order of operations and RNG call
 * sequence as the reference so that a given seed reproduces the same 40x40
 * megatile grid.
 */
#include "decoders/dungeon_gen.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>

namespace dhfc {
namespace {

constexpr int DMAXX = DungeonTileGrid::Width;
constexpr int DMAXY = DungeonTileGrid::Height;

/** Megatile ids used by the Cathedral generator (DevilutionX `enum Tile`). */
enum Tile : uint8_t {
    VWall = 1,
    HWall = 2,
    Corner = 3,
    DWall = 4,
    DArch = 5,
    VWallEnd = 6,
    HWallEnd = 7,
    HArchEnd = 8,
    VArchEnd = 9,
    HArchVWall = 10,
    VArch = 11,
    HArch = 12,
    Floor = 13,
    HWallVArch = 14,
    Pillar = 15,
    VCorner = 16,
    HCorner = 17,
    DirtHwall = 18,
    DirtVwall = 19,
    VDirtCorner = 20,
    HDirtCorner = 21,
    Dirt = 22,
    DirtHwallEnd = 23,
    DirtVwallEnd = 24,
    VDoor = 25,
    HDoor = 26,
    HFenceVWall = 27,
    HDoorVDoor = 28,
    DFence = 29,
    VDoorEnd = 30,
    HDoorEnd = 31,
    VFenceEnd = 32,
    VArchEnd2 = 33,
    HArchVWall2 = 34,
    VFence = 35,
    HFence = 36,
    HWallVFence = 37,
    HArchVFence = 38,
    HArchVDoor = 39,
    HArchVWall3 = 40,
    DWall2 = 41,
    HWallVArch2 = 42,
    DWall3 = 43,
    EntranceStairs = 64,
    VWall2 = 79,
    HWall2 = 80,
    DWall4 = 82,
    VWallEnd2 = 84,
    VWall4 = 89,
    VWall5 = 90,
    HWall4 = 91,
    HWall5 = 92,
    VWall8 = 100,
    Floor12 = 139,
    Floor13 = 140,
    Floor14 = 141,
    Floor15 = 142,
    Floor16 = 143,
    Floor17 = 144,
    Floor18 = 145,
    VWall17 = 146,
    VArch5 = 147,
    HWallShadow = 148,
    HArchShadow = 149,
    Floor19 = 150,
    Floor20 = 151,
    Floor21 = 152,
    HArchShadow2 = 153,
    HWallShadow2 = 154,
    Floor22 = 162,
    Floor23 = 163,
    DirtHWall2 = 199,
    DirtVWall2 = 200,
    DirtCorner2 = 202,
    DirtHWallEnd2 = 204,
    DirtVWallEnd2 = 205,
};

struct Miniset {
    int width;
    int height;
    /** Indexed [y][x]; 0 means "don't care". */
    uint8_t search[6][6];
    /** Indexed [y][x]; 0 means "leave untouched". */
    uint8_t replace[6][6];
};

/** Miniset: stairs up on a corner wall (Cathedral). */
const Miniset STAIRSUP {
    4, 4,
    {
        { 13, 13, 13, 13 },
        { 2, 2, 2, 2 },
        { 13, 13, 13, 13 },
        { 13, 13, 13, 13 },
    },
    {
        { 0, 66, 6, 0 },
        { 63, 64, 65, 0 },
        { 0, 67, 68, 0 },
        { 0, 0, 0, 0 },
    }
};

/** Miniset: stairs up used when the player starts in the "original" Cathedral. */
const Miniset L5STAIRSUP {
    4, 4,
    {
        { 22, 22, 22, 22 },
        { 2, 2, 2, 2 },
        { 13, 13, 13, 13 },
        { 13, 13, 13, 13 },
    },
    {
        { 0, 66, 23, 0 },
        { 63, 64, 65, 0 },
        { 0, 67, 68, 0 },
        { 0, 0, 0, 0 },
    }
};

/** Miniset: stairs down. */
const Miniset STAIRSDOWN {
    4, 3,
    {
        { 13, 13, 13, 13 },
        { 13, 13, 13, 13 },
        { 13, 13, 13, 13 },
    },
    {
        { 62, 57, 58, 0 },
        { 61, 59, 60, 0 },
        { 0, 0, 0, 0 },
    }
};

/** Miniset: candlestick. */
const Miniset LAMPS {
    2, 2,
    {
        { 13, 0 },
        { 13, 13 },
    },
    {
        { 129, 0 },
        { 130, 128 },
    }
};

/** Miniset: Poisoned Water Supply entrance. */
const Miniset PWATERIN {
    6, 6,
    {
        { 13, 13, 13, 13, 13, 13 },
        { 13, 13, 13, 13, 13, 13 },
        { 13, 13, 13, 13, 13, 13 },
        { 13, 13, 13, 13, 13, 13 },
        { 13, 13, 13, 13, 13, 13 },
        { 13, 13, 13, 13, 13, 13 },
    },
    {
        { 0, 0, 0, 0, 0, 0 },
        { 0, 202, 200, 200, 84, 0 },
        { 0, 199, 203, 203, 83, 0 },
        { 0, 85, 206, 80, 81, 0 },
        { 0, 0, 134, 135, 0, 0 },
        { 0, 0, 0, 0, 0, 0 },
    }
};

struct ShadowStruct {
    uint8_t strig;
    uint8_t s1;
    uint8_t s2;
    uint8_t s3;
    uint8_t nv1;
    uint8_t nv2;
    uint8_t nv3;
};

/** Contains shadows for 2x2 blocks of base tile IDs in the Cathedral. */
const ShadowStruct ShadowPatterns[37] = {
    { HWallEnd, Floor, 0, Floor, Floor17, 0, Floor15 },
    { VCorner, Floor, 0, Floor, Floor17, 0, Floor15 },
    { Pillar, Floor, 0, Floor, Floor18, 0, Floor15 },
    { DArch, Floor, Floor, Floor, Floor21, Floor13, Floor12 },
    { DArch, Floor, VWall, Floor, Floor16, VWall17, Floor12 },
    { DArch, Floor, Floor, HWall, Floor16, Floor13, HWallShadow },
    { DArch, 0, VWall, HWall, 0, VWall17, HWallShadow },
    { DArch, Floor, VArch, Floor, Floor16, VArch5, Floor12 },
    { DArch, Floor, Floor, HArch, Floor16, Floor13, HArchShadow },
    { DArch, Floor, VArch, HArch, Floor19, VArch5, HArchShadow },
    { DArch, Floor, VWall, HArch, Floor16, VWall17, HArchShadow },
    { DArch, Floor, VArch, HWall, Floor16, VArch5, HWallShadow },
    { VArchEnd, Floor, Floor, Floor, Floor17, Floor13, Floor15 },
    { VArchEnd, Floor, VWall, Floor, Floor17, VWall17, Floor15 },
    { VArchEnd, Floor, VArch, Floor, Floor20, VArch5, Floor15 },
    { HArchEnd, Floor, 0, Floor, Floor17, 0, Floor12 },
    { HArchEnd, Floor, 0, HArch, Floor16, 0, HArchShadow },
    { HArchEnd, 0, 0, HWall, 0, 0, HWallShadow },
    { VArch, 0, 0, Floor, 0, 0, Floor12 },
    { VArch, Floor, 0, Floor, Floor12, 0, Floor12 },
    { VArch, HWall, 0, Floor, HWallShadow, 0, Floor12 },
    { VArch, HArch, 0, Floor, HArchShadow, 0, Floor12 },
    { VArch, Floor, VArch, HArch, Floor12, 0, HArchShadow },
    { HWallVArch, 0, 0, Floor, 0, 0, Floor12 },
    { HWallVArch, Floor, 0, Floor, Floor12, 0, Floor12 },
    { HWallVArch, HWall, 0, Floor, HWallShadow, 0, Floor12 },
    { HWallVArch, HArch, 0, Floor, HArchShadow, 0, Floor12 },
    { HWallVArch, Floor, VArch, HArch, Floor12, 0, HArchShadow },
    { HArchVWall, 0, Floor, 0, 0, Floor13, 0 },
    { HArchVWall, Floor, Floor, 0, Floor13, Floor13, 0 },
    { HArchVWall, 0, VWall, 0, 0, VWall17, 0 },
    { HArchVWall, Floor, VArch, 0, Floor13, VArch5, 0 },
    { HArch, 0, Floor, 0, 0, Floor13, 0 },
    { HArch, Floor, Floor, 0, Floor13, Floor13, 0 },
    { HArch, 0, VWall, 0, 0, VWall17, 0 },
    { HArch, Floor, VArch, 0, Floor13, VArch5, 0 },
    { Corner, Floor, VArch, HArch, Floor19, 0, 0 },
};

/** Maps tile IDs to their corresponding base tile ID. */
const uint8_t BaseTypes[207] = {
    0,
    VWall, HWall, Corner, DWall, DArch, VWallEnd, HWallEnd, HArchEnd, VArchEnd,
    HArchVWall, VArch, HArch, Floor, HWallVArch, Pillar, VCorner, HCorner,
    0, 0, 0, 0, 0, 0, 0,
    VWall, HWall, HArchVWall, DWall, DArch, VWallEnd, HWallEnd, HArchEnd,
    VArchEnd, HArchVWall, VArch, HArch, HWallVArch, DArch, HWallVArch,
    HArchVWall, DWall, HWallVArch, DWall, DArch,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0,
    VWall, HWall, Corner, DWall, VWall, VWallEnd, HWallEnd, VCorner, HCorner,
    HWall, VWall, VWall, HWall, HWall, VWall, VWall, HWall, HWall, HWall, HWall,
    HWall, VWall, VWall, VArch, VWall, Floor, Floor, Floor, VWall, HWall, VWall,
    HWall, VWall, HWall, VWall, HWall, HWall, HWall, HWall, HArch,
    0, 0,
    VArch, VWall, VArch, VWall, Floor,
    0, 0, 0, 0, 0, 0, 0,
    Floor, Floor, Floor, Floor, Floor, Floor, Floor, Floor, Floor, Floor, Floor,
    Floor, Floor, VWall, VArch, HWall, HArch, Floor, Floor, Floor, HArch, HWall,
    VWall, HWall, HWall, DWall, HWallVArch, DWall, HArchVWall, Floor, Floor,
    DWall, DWall, VWall, VWall, DWall, HWall, HWall, Floor, Floor, Floor, Floor,
    VDoor, HDoor, HDoorVDoor, VDoorEnd, HDoorEnd, DWall2, DWall3, HArchVWall3,
    DWall2, HWallVArch2, DWall3, VDoor, DWall2, DWall3, HDoorVDoor, HDoorVDoor,
    VWall, HWall, VDoor, HDoor, Dirt, Dirt, VDoor, HDoor,
    0, 0, 0, 0, 0, 0, 0, 0
};

/** Maps tile IDs to their corresponding undecorated tile ID. */
const uint8_t TileDecorations[207] = {
    0,
    VWall, HWall, Corner, DWall, DArch, VWallEnd, HWallEnd, HArchEnd, VArchEnd,
    HArchVWall, VArch, HArch, Floor, HWallVArch, Pillar, VCorner, HCorner,
    0, 0, 0, 0, 0, 0, 0,
    VDoor, HDoor,
    0,
    HDoorVDoor,
    0,
    VDoorEnd, HDoorEnd,
    0, 0, 0, 0, 0, 0, 0, 0,
    HArchVWall3, DWall2, HWallVArch2, DWall3,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    VWall2, HWall2,
    0,
    DWall4,
    0, 0, 0, 0, 0, 0,
    VWall2,
    0,
    HWall2,
    0, 0,
    VWall2, HWall2,
    0,
    HWall, HWall, HWall, VWall, VWall, VArch, VDoor, Floor, Floor, Floor, VWall,
    HWall, VWall, HWall, VWall, HWall, VWall, HWall, HWall, HWall, HWall, HArch,
    0, 0,
    VArch, VWall, VArch, VWall, Floor,
    0, 0, 0, 0, 0, 0, 0,
    Floor, Floor, Floor, Floor, Floor, Floor,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0,
};

/** Bounds-checked 2D bit set, matching `Bitset2d<DMAXX, DMAXY>`. */
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

struct Rect {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
};

class CathedralGenerator {
public:
    explicit CathedralGenerator(const CathedralOptions& options)
        : options_(options)
        , rng_(options.seed)
    {
    }

    DungeonTileGrid run()
    {
        size_t minarea = 761;
        switch (options_.level) {
        case 1:
            minarea = 533;
            break;
        case 2:
            minarea = 693;
            break;
        default:
            break;
        }

        while (true) {
            do {
                firstRoom();
            } while (findArea() < minarea);

            initDungeonFlags();
            makeDmt();
            fillChambers();
            fixTilesPatterns();
            addWall();
            if (placeStairs())
                break;
        }

        fixDirtTiles();
        fixCornerTiles();
        substitution();
        applyShadowsPatterns();

        const int numt = rng_.generateRnd(5) + 5;
        for (int i = 0; i < numt; ++i)
            placeMiniSet(LAMPS, DMAXX * DMAXY, true);

        fillFloor();
        return grid_;
    }

private:
    // ----------------------------------------------------------------- utils

    int32_t rnd(int32_t v) { return rng_.generateRnd(v); }
    bool flipCoin(unsigned frequency = 2) { return rng_.flipCoin(frequency); }
    uint8_t tile(int x, int y) const
    {
        if (x < 0 || x >= DMAXX || y < 0 || y >= DMAXY)
            return 0;
        return grid_.at(x, y);
    }
    void setTile(int x, int y, uint8_t value)
    {
        if (x < 0 || x >= DMAXX || y < 0 || y >= DMAXY)
            return;
        grid_.at(x, y) = value;
    }

    bool minisetMatches(const Miniset& miniset, int x, int y) const
    {
        for (int yy = 0; yy < miniset.height; ++yy) {
            for (int xx = 0; xx < miniset.width; ++xx) {
                if (miniset.search[yy][xx] != 0 && tile(x + xx, y + yy) != miniset.search[yy][xx])
                    return false;
                if (protected_.test(x + xx, y + yy))
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
                setTile(x + xx, y + yy, miniset.replace[yy][xx]);
            }
        }
    }

    std::optional<std::pair<int, int>> placeMiniSet(const Miniset& miniset, int tries, bool drlg1Quirk)
    {
        int x = rnd(DMAXX - miniset.width);
        int y = rnd(DMAXY - miniset.height);

        for (int i = 0; i < tries; i++, x++) {
            if (x == DMAXX - miniset.width) {
                x = 0;
                y++;
                if (y == DMAXY - miniset.height)
                    y = 0;
            }

            // Reproduce the vanilla Cathedral set-piece placement restriction.
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

            if (!minisetMatches(miniset, x, y))
                continue;

            minisetPlace(miniset, x, y);
            return std::pair { x, y };
        }

        return std::nullopt;
    }

    // -------------------------------------------------------------- layout

    void mapRoom(const Rect& room)
    {
        for (int y = 0; y < room.h; ++y) {
            for (int x = 0; x < room.w; ++x)
                mask_.set(room.x + x, room.y + y);
        }
    }

    bool checkRoom(const Rect& room) const
    {
        for (int j = 0; j < room.h; ++j) {
            for (int i = 0; i < room.w; ++i) {
                if (i + room.x < 0 || i + room.x >= DMAXX || j + room.y < 0 || j + room.y >= DMAXY)
                    return false;
                if (mask_.test(i + room.x, j + room.y))
                    return false;
            }
        }
        return true;
    }

    void generateRoom(Rect room, bool verticalLayout)
    {
        const bool rotate = flipCoin(4);
        verticalLayout = (!verticalLayout && rotate) || (verticalLayout && !rotate);

        bool placeRoom1 = false;
        Rect room1;

        for (int num = 0; num < 20; ++num) {
            const int32_t randomWidth = (rnd(5) + 2) & ~1;
            const int32_t randomHeight = (rnd(5) + 2) & ~1;
            room1.w = randomWidth;
            room1.h = randomHeight;
            room1.x = areaOf(room).x;
            room1.y = areaOf(room).y;
            if (verticalLayout) {
                room1.x += -room1.w;
                room1.y += (room.h / 2) - (room1.h / 2);
                // BUGFIX workaround kept for compatibility: height/width are swapped.
                placeRoom1 = checkRoom({ room1.x - 1, room1.y - 1, room1.h + 2, room1.w + 1 });
            } else {
                room1.x += (room.w / 2) - (room1.w / 2);
                room1.y += -room1.h;
                placeRoom1 = checkRoom({ room1.x - 1, room1.y - 1, room1.w + 2, room1.h + 1 });
            }
            if (placeRoom1)
                break;
        }

        if (placeRoom1) {
            mapRoom({ room1.x,
                room1.y,
                std::min(DMAXX - room1.x, room1.w),
                std::min(DMAXY - room1.y, room1.h) });
        }

        bool placeRoom2 = false;
        Rect room2 = room1;
        if (verticalLayout) {
            room2.x = room.x + room.w;
            placeRoom2 = checkRoom({ room2.x, room2.y - 1, room2.w + 1, room2.h + 2 });
        } else {
            room2.y = room.y + room.h;
            placeRoom2 = checkRoom({ room2.x - 1, room2.y, room2.w + 2, room2.h + 1 });
        }

        if (placeRoom2)
            mapRoom(room2);
        if (placeRoom1)
            generateRoom(room1, !verticalLayout);
        if (placeRoom2)
            generateRoom(room2, !verticalLayout);
    }

    static Rect areaOf(const Rect& r) { return r; }

    void firstRoom()
    {
        mask_.reset();

        verticalLayout_ = flipCoin();
        hasChamber1_ = !flipCoin();
        hasChamber2_ = !flipCoin();
        hasChamber3_ = !flipCoin();

        if (!hasChamber1_ || !hasChamber3_)
            hasChamber2_ = true;

        Rect chamber1 { 1, 15, 10, 10 };
        const Rect chamber2 { 15, 15, 10, 10 };
        Rect chamber3 { 29, 15, 10, 10 };
        Rect hallway { 1, 17, 38, 6 };
        if (!hasChamber1_) {
            hallway.x += 17;
            hallway.w -= 17;
        }
        if (!hasChamber3_)
            hallway.w -= 16;
        if (verticalLayout_) {
            std::swap(chamber1.x, chamber1.y);
            std::swap(chamber3.x, chamber3.y);
            std::swap(hallway.x, hallway.y);
            std::swap(hallway.w, hallway.h);
        }

        if (hasChamber1_)
            mapRoom(chamber1);
        if (hasChamber2_)
            mapRoom(chamber2);
        if (hasChamber3_)
            mapRoom(chamber3);

        mapRoom(hallway);

        if (hasChamber1_)
            generateRoom(chamber1, verticalLayout_);
        if (hasChamber2_)
            generateRoom(chamber2, verticalLayout_);
        if (hasChamber3_)
            generateRoom(chamber3, verticalLayout_);
    }

    size_t findArea() const { return mask_.count(); }

    void makeDmt()
    {
        for (int j = 0; j < DMAXY - 1; ++j) {
            for (int i = 0; i < DMAXX - 1; ++i) {
                if (mask_.test(i, j))
                    setTile(i, j, Floor);
                else if (!mask_.test(i + 1, j + 1) && mask_.test(i, j + 1) && mask_.test(i + 1, j))
                    setTile(i, j, Floor); // Remove diagonal corners
                else if (mask_.test(i + 1, j + 1) && mask_.test(i, j + 1) && mask_.test(i + 1, j))
                    setTile(i, j, VCorner);
                else if (mask_.test(i, j + 1))
                    setTile(i, j, HWall);
                else if (mask_.test(i + 1, j))
                    setTile(i, j, VWall);
                else if (mask_.test(i + 1, j + 1))
                    setTile(i, j, DWall);
                else
                    setTile(i, j, Dirt);
            }
        }
    }

    int horizontalWallOk(int x, int y) const
    {
        int length = 1;
        for (;; ++length) {
            if (x + length >= DMAXX || tile(x + length, y) != Floor)
                break;
            if (tile(x + length, y - 1) != Floor || y + 1 >= DMAXY || tile(x + length, y + 1) != Floor
                || protected_.test(x + length, y) || chamber_.test(x + length, y))
                break;
        }

        if (length == 1)
            return -1;

        const auto tileId = static_cast<Tile>(tile(x + length, y));
        switch (tileId) {
        case Corner:
        case DWall:
        case DArch:
        case VWallEnd:
        case HWallEnd:
        case VCorner:
        case HCorner:
        case DirtHwall:
        case DirtVwall:
        case VDirtCorner:
        case HDirtCorner:
        case DirtHwallEnd:
        case DirtVwallEnd:
            return length;
        default:
            return -1;
        }
    }

    int verticalWallOk(int x, int y) const
    {
        int length = 1;
        for (;; ++length) {
            if (y + length >= DMAXY || tile(x, y + length) != Floor)
                break;
            if (x - 1 < 0 || tile(x - 1, y + length) != Floor || x + 1 >= DMAXX || tile(x + 1, y + length) != Floor
                || protected_.test(x, y + length) || chamber_.test(x, y + length))
                break;
        }

        if (length == 1)
            return -1;

        const auto tileId = static_cast<Tile>(tile(x, y + length));
        switch (tileId) {
        case Corner:
        case DWall:
        case DArch:
        case VWallEnd:
        case HWallEnd:
        case VCorner:
        case HCorner:
        case DirtHwall:
        case DirtVwall:
        case VDirtCorner:
        case HDirtCorner:
        case DirtHwallEnd:
        case DirtVwallEnd:
            return length;
        default:
            return -1;
        }
    }

    void horizontalWall(int x, int y, Tile start, int maxX)
    {
        Tile wallTile = HWall;
        Tile doorTile = HDoor;

        switch (rnd(4)) {
        case 2: // Add arch
            wallTile = HArch;
            doorTile = HArch;
            if (start == HWall)
                start = HArch;
            else if (start == DWall)
                start = HArchVWall;
            break;
        case 3: // Add Fence
            wallTile = HFence;
            if (start == HWall)
                start = HFence;
            else if (start == DWall)
                start = HFenceVWall;
            break;
        default:
            break;
        }

        if (rnd(6) == 5)
            doorTile = HArch;

        setTile(x, y, start);

        for (int i = 1; i < maxX; ++i)
            setTile(x + i, y, wallTile);

        const int doorX = rnd(maxX - 1) + 1;

        setTile(x + doorX, y, doorTile);
        if (doorTile == HDoor)
            protected_.set(x + doorX, y);
    }

    void verticalWall(int x, int y, Tile start, int maxY)
    {
        Tile wallTile = VWall;
        Tile doorTile = VDoor;

        switch (rnd(4)) {
        case 2: // Add arch
            wallTile = VArch;
            doorTile = VArch;
            if (start == VWall)
                start = VArch;
            else if (start == DWall)
                start = HWallVArch;
            break;
        case 3: // Add Fence
            wallTile = VFence;
            if (start == VWall)
                start = VFence;
            else if (start == DWall)
                start = HWallVFence;
            break;
        default:
            break;
        }

        if (rnd(6) == 5)
            doorTile = VArch;

        setTile(x, y, start);

        for (int i = 1; i < maxY; ++i)
            setTile(x, y + i, wallTile);

        const int doorY = rnd(maxY - 1) + 1;

        setTile(x, y + doorY, doorTile);
        if (doorTile == VDoor)
            protected_.set(x, y + doorY);
    }

    void addWall()
    {
        for (int j = 0; j < DMAXY; ++j) {
            for (int i = 0; i < DMAXX; ++i) {
                if (protected_.test(i, j) || chamber_.test(i, j))
                    continue;

                if (tile(i, j) == Corner) {
                    rng_.discard(1);
                    const int maxX = horizontalWallOk(i, j);
                    if (maxX != -1)
                        horizontalWall(i, j, HWall, maxX);
                }
                if (tile(i, j) == Corner) {
                    rng_.discard(1);
                    const int maxY = verticalWallOk(i, j);
                    if (maxY != -1)
                        verticalWall(i, j, VWall, maxY);
                }
                if (tile(i, j) == VWallEnd) {
                    rng_.discard(1);
                    const int maxX = horizontalWallOk(i, j);
                    if (maxX != -1)
                        horizontalWall(i, j, DWall, maxX);
                }
                if (tile(i, j) == HWallEnd) {
                    rng_.discard(1);
                    const int maxY = verticalWallOk(i, j);
                    if (maxY != -1)
                        verticalWall(i, j, DWall, maxY);
                }
                if (tile(i, j) == HWall) {
                    rng_.discard(1);
                    const int maxX = horizontalWallOk(i, j);
                    if (maxX != -1)
                        horizontalWall(i, j, HWall, maxX);
                }
                if (tile(i, j) == VWall) {
                    rng_.discard(1);
                    const int maxY = verticalWallOk(i, j);
                    if (maxY != -1)
                        verticalWall(i, j, VWall, maxY);
                }
            }
        }
    }

    // ------------------------------------------------------------- chamber

    void generateChamber(int x, int y, bool connectPrevious, bool connectNext, bool verticalLayout)
    {
        if (connectPrevious) {
            if (verticalLayout) {
                setTile(x + 2, y, HArch);
                setTile(x + 3, y, HArch);
                setTile(x + 4, y, Corner);
                setTile(x + 7, y, VArchEnd);
                setTile(x + 8, y, HArch);
                setTile(x + 9, y, HWall);
            } else {
                setTile(x, y + 2, VArch);
                setTile(x, y + 3, VArch);
                setTile(x, y + 4, Corner);
                setTile(x, y + 7, HArchEnd);
                setTile(x, y + 8, VArch);
                setTile(x, y + 9, VWall);
            }
        }
        if (connectNext) {
            if (verticalLayout) {
                y += 11;
                setTile(x + 2, y, HArchVWall);
                setTile(x + 3, y, HArch);
                setTile(x + 4, y, HArchEnd);
                setTile(x + 7, y, DArch);
                setTile(x + 8, y, HArch);
                if (tile(x + 9, y) != DWall)
                    setTile(x + 9, y, HDirtCorner);
                y -= 11;
            } else {
                x += 11;
                setTile(x, y + 2, HWallVArch);
                setTile(x, y + 3, VArch);
                setTile(x, y + 4, VArchEnd);
                setTile(x, y + 7, DArch);
                setTile(x, y + 8, VArch);
                if (tile(x, y + 9) != DWall)
                    setTile(x, y + 9, HDirtCorner);
                x -= 11;
            }
        }

        for (int yy = 1; yy < 11; ++yy) {
            for (int xx = 1; xx < 11; ++xx) {
                setTile(x + xx, y + yy, Floor);
                chamber_.set(x + xx, y + yy);
            }
        }

        setTile(x + 4, y + 4, Pillar);
        setTile(x + 7, y + 4, Pillar);
        setTile(x + 4, y + 7, Pillar);
        setTile(x + 7, y + 7, Pillar);
    }

    void generateHall(int startX, int startY, int length, bool verticalLayout)
    {
        if (verticalLayout) {
            for (int i = startY; i < startY + length; ++i) {
                setTile(startX, i, VArch);
                setTile(startX + 3, i, VArch);
            }
        } else {
            for (int i = startX; i < startX + length; ++i) {
                setTile(i, startY, HArch);
                setTile(i, startY + 3, HArch);
            }
        }
    }

    void initSetPiece()
    {
        const DunTileLayer* setPieceData = nullptr;
        if (options_.butcherQuest) {
            setPieceData = options_.butcherSetPiece;
        } else if (options_.skeletonKingQuest && !options_.multiplayerQuests) {
            setPieceData = options_.skeletonKingSetPiece;
        } else if (options_.bannerQuest) {
            setPieceData = options_.bannerSetPiece;
        } else {
            return; // no set piece needed for this level
        }

        if (setPieceData == nullptr)
            throw std::runtime_error("Cathedral set piece data is required for the active quest");

        const auto position = selectChamber();
        placeDunTiles(*setPieceData, position.first, position.second, Floor);
        setPiece_ = { position.first, position.second, setPieceData->width, setPieceData->height };
    }

    std::pair<int, int> selectChamber()
    {
        int chamber = 0;
        if (hasChamber1_ && hasChamber2_ && hasChamber3_) {
            chamber = rnd(3) + 1;
        } else if (hasChamber1_ && hasChamber2_) {
            // Reverse order to match vanilla
            chamber = pickRandomlyAmong({ 2, 1 });
        } else if (hasChamber1_ && hasChamber3_) {
            chamber = pickRandomlyAmong({ 3, 1 });
        } else if (hasChamber2_ && hasChamber3_) {
            chamber = pickRandomlyAmong({ 2, 3 });
        } else {
            chamber = 2;
        }

        switch (chamber) {
        case 1:
            return verticalLayout_ ? std::pair { 16, 2 } : std::pair { 2, 16 };
        case 3:
            return verticalLayout_ ? std::pair { 16, 30 } : std::pair { 30, 16 };
        default:
            return { 16, 16 };
        }
    }

    int pickRandomlyAmong(std::initializer_list<int> values)
    {
        const auto index = std::max<int32_t>(rnd(static_cast<int32_t>(values.size())), 0);
        return *(values.begin() + index);
    }

    void placeDunTiles(const DunTileLayer& data, int posX, int posY, int floorId)
    {
        for (int j = 0; j < data.height; ++j) {
            for (int i = 0; i < data.width; ++i) {
                const auto tileId = static_cast<uint8_t>(data.at(i, j));
                if (tileId != 0) {
                    setTile(posX + i, posY + j, tileId);
                    protected_.set(posX + i, posY + j);
                } else if (floorId != 0) {
                    setTile(posX + i, posY + j, static_cast<uint8_t>(floorId));
                }
            }
        }
    }

    void fillChambers()
    {
        int chamber1X = 0, chamber1Y = 14;
        int chamber3X = 28, chamber3Y = 14;
        int hall1X = 12, hall1Y = 18;
        int hall2X = 26, hall2Y = 18;
        if (verticalLayout_) {
            std::swap(chamber1X, chamber1Y);
            std::swap(chamber3X, chamber3Y);
            std::swap(hall1X, hall1Y);
            std::swap(hall2X, hall2Y);
        }

        if (hasChamber1_)
            generateChamber(chamber1X, chamber1Y, false, true, verticalLayout_);
        if (hasChamber2_)
            generateChamber(14, 14, hasChamber1_, hasChamber3_, verticalLayout_);
        if (hasChamber3_)
            generateChamber(chamber3X, chamber3Y, true, false, verticalLayout_);

        if (hasChamber2_) {
            if (hasChamber1_)
                generateHall(hall1X, hall1Y, 2, verticalLayout_);
            if (hasChamber3_)
                generateHall(hall2X, hall2Y, 2, verticalLayout_);
        } else {
            generateHall(hall1X, hall1Y, 16, verticalLayout_);
        }

        initSetPiece();
    }

    // -------------------------------------------------------------- fixups

    void fixTilesPatterns()
    {
        for (int j = 0; j < DMAXY; ++j) {
            for (int i = 0; i < DMAXX; ++i) {
                if (i + 1 < DMAXX) {
                    if (tile(i, j) == HWall && tile(i + 1, j) == Dirt)
                        setTile(i + 1, j, DirtHwallEnd);
                    if (tile(i, j) == Floor && tile(i + 1, j) == Dirt)
                        setTile(i + 1, j, DirtHwall);
                    if (tile(i, j) == Floor && tile(i + 1, j) == HWall)
                        setTile(i + 1, j, HWallEnd);
                    if (tile(i, j) == VWallEnd && tile(i + 1, j) == Dirt)
                        setTile(i + 1, j, DirtVwallEnd);
                }
                if (j + 1 < DMAXY) {
                    if (tile(i, j) == VWall && tile(i, j + 1) == Dirt)
                        setTile(i, j + 1, DirtVwallEnd);
                    if (tile(i, j) == Floor && tile(i, j + 1) == VWall)
                        setTile(i, j + 1, VWallEnd);
                    if (tile(i, j) == Floor && tile(i, j + 1) == Dirt)
                        setTile(i, j + 1, DirtVwall);
                }
            }
        }

        for (int j = 0; j < DMAXY; ++j) {
            for (int i = 0; i < DMAXX; ++i) {
                if (i + 1 < DMAXX) {
                    if (tile(i, j) == Floor && tile(i + 1, j) == DirtVwall)
                        setTile(i + 1, j, HDirtCorner);
                    if (tile(i, j) == Floor && tile(i + 1, j) == Dirt)
                        setTile(i + 1, j, VDirtCorner);
                    if (tile(i, j) == HWallEnd && tile(i + 1, j) == Dirt)
                        setTile(i + 1, j, DirtHwallEnd);
                    if (tile(i, j) == Floor && tile(i + 1, j) == DirtVwallEnd)
                        setTile(i + 1, j, HDirtCorner);
                    if (tile(i, j) == DirtVwall && tile(i + 1, j) == Dirt)
                        setTile(i + 1, j, VDirtCorner);
                    if (tile(i, j) == HWall && tile(i + 1, j) == DirtVwall)
                        setTile(i + 1, j, HDirtCorner);
                    if (tile(i, j) == DirtVwall && tile(i + 1, j) == VWall)
                        setTile(i + 1, j, VWallEnd);
                    if (tile(i, j) == HWallEnd && tile(i + 1, j) == DirtVwall)
                        setTile(i + 1, j, HDirtCorner);
                    if (tile(i, j) == HWall && tile(i + 1, j) == VWall)
                        setTile(i + 1, j, VWallEnd);
                    if (tile(i, j) == Corner && tile(i + 1, j) == Dirt)
                        setTile(i + 1, j, DirtVwallEnd);
                    if (tile(i, j) == HDirtCorner && tile(i + 1, j) == VWall)
                        setTile(i + 1, j, VWallEnd);
                    if (tile(i, j) == HWallEnd && tile(i + 1, j) == VWall)
                        setTile(i + 1, j, VWallEnd);
                    if (tile(i, j) == HWallEnd && tile(i + 1, j) == DirtVwallEnd)
                        setTile(i + 1, j, HDirtCorner);
                    if (tile(i, j) == DWall && tile(i + 1, j) == VCorner)
                        setTile(i + 1, j, HCorner);
                    if (tile(i, j) == HWallEnd && tile(i + 1, j) == Floor)
                        setTile(i + 1, j, HCorner);
                    if (tile(i, j) == HWall && tile(i + 1, j) == DirtVwallEnd)
                        setTile(i + 1, j, HDirtCorner);
                    if (tile(i, j) == HWall && tile(i + 1, j) == Floor)
                        setTile(i + 1, j, HCorner);
                }
                if (i > 0) {
                    if (tile(i, j) == DirtHwallEnd && tile(i - 1, j) == Dirt)
                        setTile(i - 1, j, DirtVwall);
                    if (tile(i, j) == DirtVwall && tile(i - 1, j) == DirtHwallEnd)
                        setTile(i - 1, j, HDirtCorner);
                    if (tile(i, j) == VWallEnd && tile(i - 1, j) == Dirt)
                        setTile(i - 1, j, DirtVwallEnd);
                    if (tile(i, j) == VWallEnd && tile(i - 1, j) == DirtHwallEnd)
                        setTile(i - 1, j, HDirtCorner);
                }
                if (j + 1 < DMAXY) {
                    if (tile(i, j) == VWall && tile(i, j + 1) == HWall)
                        setTile(i, j + 1, HWallEnd);
                    if (tile(i, j) == VWallEnd && tile(i, j + 1) == DirtHwall)
                        setTile(i, j + 1, HDirtCorner);
                    if (tile(i, j) == DirtHwall && tile(i, j + 1) == HWall)
                        setTile(i, j + 1, HWallEnd);
                    if (tile(i, j) == VWallEnd && tile(i, j + 1) == HWall)
                        setTile(i, j + 1, HWallEnd);
                    if (tile(i, j) == HDirtCorner && tile(i, j + 1) == HWall)
                        setTile(i, j + 1, HWallEnd);
                    if (tile(i, j) == VWallEnd && tile(i, j + 1) == Dirt)
                        setTile(i, j + 1, DirtVwallEnd);
                    if (tile(i, j) == VWallEnd && tile(i, j + 1) == Floor)
                        setTile(i, j + 1, VCorner);
                    if (tile(i, j) == VWall && tile(i, j + 1) == Floor)
                        setTile(i, j + 1, VCorner);
                    if (tile(i, j) == Floor && tile(i, j + 1) == VCorner)
                        setTile(i, j + 1, HCorner);
                }
                if (j > 0) {
                    if (tile(i, j) == VWallEnd && tile(i, j - 1) == Dirt)
                        setTile(i, j - 1, HWallEnd);
                    if (tile(i, j) == VWallEnd && tile(i, j - 1) == Dirt)
                        setTile(i, j - 1, DirtVwallEnd);
                    if (tile(i, j) == HWallEnd && tile(i, j - 1) == DirtVwallEnd)
                        setTile(i, j - 1, HDirtCorner);
                    if (tile(i, j) == DirtHwall && tile(i, j - 1) == DirtVwallEnd)
                        setTile(i, j - 1, HDirtCorner);
                }
            }
        }

        for (int j = 0; j < DMAXY; ++j) {
            for (int i = 0; i < DMAXX; ++i) {
                if (j + 1 < DMAXY && tile(i, j) == DWall && tile(i, j + 1) == HWall)
                    setTile(i, j + 1, HWallEnd);
                if (i + 1 < DMAXX && tile(i, j) == HWall && tile(i + 1, j) == DirtVwall)
                    setTile(i + 1, j, HDirtCorner);
                if (j + 1 < DMAXY && tile(i, j) == DirtHwall && tile(i, j + 1) == Dirt)
                    setTile(i, j + 1, VDirtCorner);
            }
        }
    }

    void substitution()
    {
        for (int y = 0; y < DMAXY; ++y) {
            for (int x = 0; x < DMAXX; ++x) {
                if (!flipCoin(4))
                    continue;
                const uint8_t c = TileDecorations[tile(x, y)];
                if (c == 0 || protected_.test(x, y))
                    continue;

                int rv = rnd(16);
                int i = -1;
                while (rv >= 0) {
                    ++i;
                    if (i == static_cast<int>(sizeof(TileDecorations))) {
                        i = 0;
                    }
                    if (c == TileDecorations[i])
                        --rv;
                }

                if (i == VWall4 && y > 0) {
                    if (TileDecorations[tile(x, y - 1)] != VWall2 || protected_.test(x, y - 1))
                        i = VWall2;
                    else
                        setTile(x, y - 1, VWall5);
                }
                if (i == HWall4 && x + 1 < DMAXX) {
                    if (TileDecorations[tile(x + 1, y)] != HWall2 || protected_.test(x + 1, y))
                        i = HWall2;
                    else
                        setTile(x + 1, y, HWall5);
                }
                setTile(x, y, static_cast<uint8_t>(i));
            }
        }
    }

    void applyShadowsPatterns()
    {
        uint8_t slice[2][2];

        for (int y = 1; y < DMAXY; ++y) {
            for (int x = 1; x < DMAXX; ++x) {
                slice[0][0] = BaseTypes[tile(x, y)];
                slice[1][0] = BaseTypes[tile(x - 1, y)];
                slice[0][1] = BaseTypes[tile(x, y - 1)];
                slice[1][1] = BaseTypes[tile(x - 1, y - 1)];

                for (const auto& shadow : ShadowPatterns) {
                    if (shadow.strig != slice[0][0])
                        continue;
                    if (shadow.s1 != 0 && shadow.s1 != slice[1][1])
                        continue;
                    if (shadow.s2 != 0 && shadow.s2 != slice[0][1])
                        continue;
                    if (shadow.s3 != 0 && shadow.s3 != slice[1][0])
                        continue;

                    if (shadow.nv1 != 0 && !protected_.test(x - 1, y - 1))
                        setTile(x - 1, y - 1, shadow.nv1);
                    if (shadow.nv2 != 0 && !protected_.test(x, y - 1))
                        setTile(x, y - 1, shadow.nv2);
                    if (shadow.nv3 != 0 && !protected_.test(x - 1, y))
                        setTile(x - 1, y, shadow.nv3);
                }
            }
        }

        for (int y = 1; y < DMAXY; ++y) {
            for (int x = 1; x < DMAXX; ++x) {
                if (protected_.test(x - 1, y))
                    continue;

                const auto isFence = [this, x, y] {
                    const uint8_t t = tile(x, y);
                    return t == DFence || t == VFenceEnd || t == VFence || t == HWallVFence
                        || t == HArchVFence || t == HArchVDoor;
                };

                if (tile(x - 1, y) == Floor12)
                    setTile(x - 1, y, isFence() ? Floor14 : Floor12);
                if (tile(x - 1, y) == HArchShadow)
                    setTile(x - 1, y, isFence() ? HArchShadow2 : HArchShadow);
                if (tile(x - 1, y) == HWallShadow)
                    setTile(x - 1, y, isFence() ? HWallShadow2 : HWallShadow);
            }
        }
    }

    void fillFloor()
    {
        for (int j = 0; j < DMAXY; ++j) {
            for (int i = 0; i < DMAXX; ++i) {
                if (tile(i, j) != Floor || protected_.test(i, j))
                    continue;

                const int rv = std::max<int32_t>(rnd(3), 0);
                if (rv == 1)
                    setTile(i, j, Floor22);
                else if (rv == 2)
                    setTile(i, j, Floor23);
            }
        }
    }

    void fixDirtTiles()
    {
        for (int j = 0; j < DMAXY - 1; ++j) {
            for (int i = 0; i < DMAXX - 1; ++i) {
                if (tile(i, j) == HDirtCorner && tile(i + 1, j) != DirtVwall)
                    setTile(i, j, DirtCorner2);
                if (tile(i, j) == DirtVwall && tile(i + 1, j) != DirtVwall)
                    setTile(i, j, DirtVWall2);
                if (tile(i, j) == DirtVwallEnd && tile(i + 1, j) != DirtVwall)
                    setTile(i, j, DirtVWallEnd2);
                if (tile(i, j) == DirtHwall && tile(i, j + 1) != DirtHwall)
                    setTile(i, j, DirtHWall2);
                if (tile(i, j) == HDirtCorner && tile(i, j + 1) != DirtHwall)
                    setTile(i, j, DirtCorner2);
                if (tile(i, j) == DirtHwallEnd && tile(i, j + 1) != DirtHwall)
                    setTile(i, j, DirtHWallEnd2);
            }
        }
    }

    void fixCornerTiles()
    {
        for (int j = 1; j < DMAXY - 1; ++j) {
            for (int i = 1; i < DMAXX - 1; ++i) {
                if (!protected_.test(i, j) && tile(i, j) == HCorner && tile(i - 1, j) == Floor
                    && tile(i, j - 1) == VWall) {
                    setTile(i, j, VCorner);
                }
                if (tile(i, j) == DirtCorner2 && tile(i + 1, j) == Floor && tile(i, j + 1) == VWall)
                    setTile(i, j, HArchEnd);
                if (tile(i, j) == DirtCorner2 && tile(i, j + 1) == Floor && tile(i + 1, j) == HWall)
                    setTile(i, j, VArchEnd);
            }
        }
    }

    // -------------------------------------------------------------- stairs

    bool placeCathedralStairs()
    {
        bool success = true;

        // Place poison water entrance
        if (options_.poisonWaterQuest) {
            if (!placeMiniSet(PWATERIN, DMAXX * DMAXY, true))
                success = false;
        }

        // Place stairs up
        const Miniset& stairsUp = (options_.originalCathedral && !options_.bannerQuest) ? L5STAIRSUP
                                                                                        : STAIRSUP;
        if (!placeMiniSet(stairsUp, DMAXX * DMAXY, true)) {
            if (options_.originalCathedral)
                return false;
            success = false;
        }

        // Place stairs down
        if (!options_.bannerQuest) {
            if (!placeMiniSet(STAIRSDOWN, DMAXX * DMAXY, true))
                success = false;
        }

        return success;
    }

    bool placeStairs() { return placeCathedralStairs(); }

    void initDungeonFlags()
    {
        grid_.tiles.fill(Dirt);
        protected_.reset();
        chamber_.reset();
    }

    CathedralOptions options_;
    DiabloRng rng_;
    DungeonTileGrid grid_ {};
    BitGrid protected_;
    BitGrid chamber_;
    BitGrid mask_;

    bool verticalLayout_ = false;
    bool hasChamber1_ = false;
    bool hasChamber2_ = false;
    bool hasChamber3_ = false;

    struct SetPiece {
        int x = 0;
        int y = 0;
        int w = 0;
        int h = 0;
    } setPiece_ {};
};

} // namespace

void applyVanillaCathedralQuests(CathedralOptions& options)
{
    options.butcherQuest = false;
    options.poisonWaterQuest = false;
    options.skeletonKingQuest = false;
    options.bannerQuest = false;

    switch (options.level) {
    case 2:
        options.butcherQuest = true;
        options.poisonWaterQuest = true;
        break;
    case 3:
        options.skeletonKingQuest = true;
        break;
    case 4:
        options.bannerQuest = true;
        break;
    default:
        break;
    }
}

DungeonTileGrid generateCathedral(const CathedralOptions& options)
{
    CathedralGenerator generator(options);
    return generator.run();
}

} // namespace dhfc
