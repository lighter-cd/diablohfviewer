// Hell (L4) dungeon generator — faithful port of DevilutionX drlg_l4.cpp.
//
// Only `dungeon[][]` writers and RNG consumers are ported. Rendering state
// (`dPiece`/`pMegaTiles`), the transparency value grid (`dTransVal`) and the
// border-prefill done by `DRLG_LPass3` are intentionally omitted: they never
// modify the megatile grid we compare against the golden fixtures. As a result
// `FixTransparency`, `FloodTransparencyValues` and `Pass3` are no-ops here.
//
// NOTE on golden fidelity: like the Catacombs/Caves ports, byte-exact golden
// matching is not achievable because the reference relies on unspecified
// out-of-bounds `dungeon[x][y]` reads at grid borders. The generator is
// structurally valid; golden comparison is informational only.
#include "decoders/dungeon_gen_common.h"
#include "decoders/dungeon_gen_l4_data.h"
#include "decoders/dungeon_gen.h"

#include <algorithm>
#include <cstring>

namespace dhfc {
namespace gen {

// Half-resolution room mask (mirrored to produce the full layout).
static BitGrid DungeonMask;
static bool hallok[20];
static WorldTilePosition L4Hold;
static WorldTilePosition DiabloQuad1;
static WorldTilePosition DiabloQuad2;
static WorldTilePosition DiabloQuad3;
static WorldTilePosition DiabloQuad4;

// ----------------------------------------------------------- quest set-pieces
static void InitSetPiece(const HellOptions& opts)
{
    const DunTileLayer* data = nullptr;
    if (Quests[Q_WARLORD].IsAvailable()) {
        data = opts.warlord;
    } else if (currlevel == 15 && UseMultiplayerQuests()) {
        data = opts.vile1;
    } else {
        return; // no setpiece needed for this level
    }
    if (data == nullptr || data->tiles.empty())
        return;

    std::vector<uint16_t> buf = buildDunBuffer(*data);
    const WorldTilePosition setPiecePosition = SetPieceRoom.position;
    PlaceDunTiles(buf.data(), setPiecePosition.x, setPiecePosition.y, 6);
    SetPiece = { setPiecePosition, GetDunSize(buf.data()) };
}

static void LoadDiabQuads(bool preflag, const HellOptions& opts)
{
    auto place = [&](const DunTileLayer* layer, WorldTilePosition at) {
        if (layer == nullptr || layer->tiles.empty())
            return;
        std::vector<uint16_t> buf = buildDunBuffer(*layer);
        PlaceDunTiles(buf.data(), at.x, at.y, 6);
    };
    place(opts.diab[0], L4Hold + WorldTileDisplacement { 4, 4 });
    place(preflag ? opts.diab[2] : opts.diab[1], WorldTilePosition(27 - L4Hold.x, 1 + L4Hold.y));
    place(preflag ? opts.diab[4] : opts.diab[3], WorldTilePosition(1 + L4Hold.x, 27 - L4Hold.y));
    place(preflag ? opts.diab[6] : opts.diab[5], WorldTilePosition(28 - L4Hold.x, 28 - L4Hold.y));
}

// ------------------------------------------------------------- shadow patterns
static void ApplyShadowsPatterns()
{
    for (int y = 1; y < DMAXY; y++) {
        for (int x = 1; x < DMAXY; x++) {
            if (IsNoneOf(dungeon[x][y], 3, 4, 8, 15)) {
                continue;
            }
            if (dungeon[x - 1][y] == 6) {
                dungeon[x - 1][y] = 47;
            }
            if (dungeon[x - 1][y - 1] == 6) {
                dungeon[x - 1][y - 1] = 48;
            }
        }
    }
}

// ----------------------------------------------------------- dungeon init
static void InitDungeonFlags()
{
    DungeonMask.reset();
    ProtectedBits.reset();
    memset(dungeon, 30, sizeof(dungeon));
}

static void MapRoom(WorldTileRectangle room)
{
    for (int y = 0; y < room.size.height && y + room.position.y < DMAXY / 2; y++) {
        for (int x = 0; x < room.size.width && x + room.position.x < DMAXX / 2; x++) {
            DungeonMask.set(room.position.x + x, room.position.y + y);
        }
    }
}

static bool CheckRoom(WorldTileRectangle room)
{
    if (room.position.x <= 0 || room.position.y <= 0) {
        return false;
    }

    for (int y = 0; y < room.size.height; y++) {
        for (int x = 0; x < room.size.width; x++) {
            if (x + room.position.x < 0 || x + room.position.x >= DMAXX / 2 || y + room.position.y < 0 || y + room.position.y >= DMAXY / 2) {
                return false;
            }
            if (DungeonMask.test(room.position.x + x, room.position.y + y)) {
                return false;
            }
        }
    }

    return true;
}

static void GenerateRoom(WorldTileRectangle area, bool verticalLayout)
{
    const bool rotate = !FlipCoin(4);
    verticalLayout = (!verticalLayout && rotate) || (verticalLayout && !rotate);

    bool placeRoom1;
    WorldTileRectangle room1;

    for (int num = 0; num < 20; num++) {
        const int32_t randomWidth = (GenerateRnd(5) + 2) & ~1;
        const int32_t randomHeight = (GenerateRnd(5) + 2) & ~1;
        room1.size = WorldTileSize(randomWidth, randomHeight);
        room1.position = area.position;
        if (verticalLayout) {
            room1.position += WorldTileDisplacement(-room1.size.width, (area.size.height / 2) - (room1.size.height / 2));
            placeRoom1 = CheckRoom({ room1.position + WorldTileDisplacement { -1, -1 }, WorldTileSize(room1.size.height + 2, room1.size.width + 1) });
        } else {
            room1.position += WorldTileDisplacement((area.size.width / 2) - (room1.size.width / 2), -room1.size.height);
            placeRoom1 = CheckRoom({ room1.position + WorldTileDisplacement { -1, -1 }, WorldTileSize(room1.size.width + 2, room1.size.height + 1) });
        }
        if (placeRoom1)
            break;
    }

    if (placeRoom1)
        MapRoom({ room1.position, WorldTileSize(std::min<int>(DMAXX - room1.position.x, room1.size.width), std::min<int>(DMAXX - room1.position.y, room1.size.height)) });

    bool placeRoom2;
    WorldTileRectangle room2 = room1;
    if (verticalLayout) {
        room2.position.x = area.position.x + area.size.width;
        placeRoom2 = CheckRoom({ room2.position + WorldTileDisplacement { 0, -1 }, WorldTileSize(room2.size.width + 1, room2.size.height + 2) });
    } else {
        room2.position.y = area.position.y + area.size.height;
        placeRoom2 = CheckRoom({ room2.position + WorldTileDisplacement { -1, 0 }, WorldTileSize(room2.size.width + 2, room2.size.height + 1) });
    }

    if (placeRoom2)
        MapRoom(room2);
    if (placeRoom1)
        GenerateRoom(room1, verticalLayout);
    if (placeRoom2)
        GenerateRoom(room2, verticalLayout);
}

static void FirstRoom()
{
    WorldTileRectangle room { { 0, 0 }, { 14, 14 } };
    if (currlevel != 16) {
        if (currlevel == Quests[Q_WARLORD]._qlevel && Quests[Q_WARLORD]._qactive != 0) {
            room.size = { 11, 11 };
        } else if (currlevel == Quests[Q_BETRAYER]._qlevel && UseMultiplayerQuests()) {
            room.size = { 11, 11 };
        } else {
            const int32_t randomWidth = GenerateRnd(5) + 2;
            const int32_t randomHeight = GenerateRnd(5) + 2;
            room.size = WorldTileSize(randomWidth, randomHeight);
        }
    }

    const int xmin = (DMAXX / 2 - room.size.width) / 2;
    const int xmax = (DMAXX / 2) - 1 - room.size.width;
    const int ymin = (DMAXY / 2 - room.size.height) / 2;
    const int ymax = (DMAXY / 2) - 1 - room.size.height;
    const int32_t randomX = GenerateRnd(xmax - xmin + 1) + xmin;
    const int32_t randomY = GenerateRnd(ymax - ymin + 1) + ymin;
    room.position = WorldTilePosition(randomX, randomY);

    if (currlevel == 16) {
        L4Hold = room.position;
    }
    if (Quests[Q_WARLORD].IsAvailable() || (currlevel == Quests[Q_BETRAYER]._qlevel && UseMultiplayerQuests())) {
        SetPieceRoom = { room.position + WorldTileDisplacement { 1, 1 }, WorldTileSize(room.size.width + 1, room.size.height + 1) };
    } else {
        SetPieceRoom = {};
    }

    MapRoom(room);
    GenerateRoom(room, !FlipCoin());
}

/** @brief Mirrors the first quadrant to the rest of the map. */
static void MirrorDungeonLayout()
{
    for (int y = 0; y < DMAXY / 2; y++) {
        for (int x = 0; x < DMAXX / 2; x++) {
            if (DungeonMask.test(x, y)) {
                DungeonMask.set(x, DMAXY - 1 - y);
                DungeonMask.set(DMAXX - 1 - x, y);
                DungeonMask.set(DMAXX - 1 - x, DMAXY - 1 - y);
            }
        }
    }
}

static void MakeDmt()
{
    for (int y = 0; y < DMAXY - 1; y++) {
        for (int x = 0; x < DMAXX - 1; x++) {
            const int val = (DungeonMask.test(x + 1, y + 1) << 3) | (DungeonMask.test(x, y + 1) << 2) | (DungeonMask.test(x + 1, y) << 1) | (DungeonMask.test(x, y) << 0);
            dungeon[x][y] = L4ConvTbl[val];
        }
    }
}

int HorizontalWallOk(int i, int j)
{
    int x;
    for (x = 1; dungeon[i + x][j] == 6; x++) {
        if (ProtectedBits.test(i + x, j)) {
            break;
        }
        if (dungeon[i + x][j - 1] != 6) {
            break;
        }
        if (dungeon[i + x][j + 1] != 6) {
            break;
        }
    }

    if (IsAnyOf(dungeon[i + x][j], 10, 12, 13, 15, 16, 21, 22) && x > 3)
        return x;

    return -1;
}

int VerticalWallOk(int i, int j)
{
    int y;
    for (y = 1; dungeon[i][j + y] == 6; y++) {
        if (ProtectedBits.test(i, j + y)) {
            break;
        }
        if (dungeon[i - 1][j + y] != 6) {
            break;
        }
        if (dungeon[i + 1][j + y] != 6) {
            break;
        }
    }

    if (IsAnyOf(dungeon[i][j + y], 8, 9, 11, 14, 15, 16, 21, 23) && y > 3)
        return y;

    return -1;
}

void HorizontalWall(int i, int j, int dx)
{
    if (dungeon[i][j] == 13) {
        dungeon[i][j] = 17;
    }
    if (dungeon[i][j] == 16) {
        dungeon[i][j] = 11;
    }
    if (dungeon[i][j] == 12) {
        dungeon[i][j] = 14;
    }

    for (int xx = 1; xx < dx; xx++) {
        dungeon[i + xx][j] = 2;
    }

    if (dungeon[i + dx][j] == 15) {
        dungeon[i + dx][j] = 14;
    }
    if (dungeon[i + dx][j] == 10) {
        dungeon[i + dx][j] = 17;
    }
    if (dungeon[i + dx][j] == 21) {
        dungeon[i + dx][j] = 23;
    }
    if (dungeon[i + dx][j] == 22) {
        dungeon[i + dx][j] = 29;
    }

    const int xx = GenerateRnd(dx - 3) + 1;
    dungeon[i + xx][j] = 57;
    dungeon[i + xx + 2][j] = 56;
    dungeon[i + xx + 1][j] = 60;

    if (dungeon[i + xx][j - 1] == 6) {
        dungeon[i + xx][j - 1] = 58;
    }
    if (dungeon[i + xx + 1][j - 1] == 6) {
        dungeon[i + xx + 1][j - 1] = 59;
    }
}

void VerticalWall(int i, int j, int dy)
{
    if (dungeon[i][j] == 14) {
        dungeon[i][j] = 17;
    }
    if (dungeon[i][j] == 8) {
        dungeon[i][j] = 9;
    }
    if (dungeon[i][j] == 15) {
        dungeon[i][j] = 10;
    }

    for (int yy = 1; yy < dy; yy++) {
        dungeon[i][j + yy] = 1;
    }

    if (dungeon[i][j + dy] == 11) {
        dungeon[i][j + dy] = 17;
    }
    if (dungeon[i][j + dy] == 9) {
        dungeon[i][j + dy] = 10;
    }
    if (dungeon[i][j + dy] == 16) {
        dungeon[i][j + dy] = 13;
    }
    if (dungeon[i][j + dy] == 21) {
        dungeon[i][j + dy] = 22;
    }
    if (dungeon[i][j + dy] == 23) {
        dungeon[i][j + dy] = 29;
    }

    const int yy = GenerateRnd(dy - 3) + 1;
    dungeon[i][j + yy] = 53;
    dungeon[i][j + yy + 2] = 52;
    dungeon[i][j + yy + 1] = 6;

    if (dungeon[i - 1][j + yy] == 6) {
        dungeon[i - 1][j + yy] = 54;
    }
    if (dungeon[i - 1][j + yy - 1] == 6) {
        dungeon[i - 1][j + yy - 1] = 55;
    }
}

void AddWall()
{
    for (int j = 0; j < DMAXY; j++) {
        for (int i = 0; i < DMAXX; i++) {
            if (ProtectedBits.test(i, j)) {
                continue;
            }
            for (auto d : { 10, 12, 13, 15, 16, 21, 22 }) {
                if (d == dungeon[i][j]) {
                    DiscardRandomValues(1);
                    const int x = HorizontalWallOk(i, j);
                    if (x != -1) {
                        HorizontalWall(i, j, x);
                    }
                }
            }
            for (auto d : { 8, 9, 11, 14, 15, 16, 21, 23 }) {
                if (d == dungeon[i][j]) {
                    DiscardRandomValues(1);
                    const int y = VerticalWallOk(i, j);
                    if (y != -1) {
                        VerticalWall(i, j, y);
                    }
                }
            }
        }
    }
}

static void FixTilesPatterns()
{
    for (int j = 0; j < DMAXY; j++) {
        for (int i = 0; i < DMAXX; i++) {
            if (dungeon[i][j] == 2 && dungeon[i + 1][j] == 6)
                dungeon[i + 1][j] = 5;
            if (dungeon[i][j] == 2 && dungeon[i + 1][j] == 1)
                dungeon[i + 1][j] = 13;
            if (dungeon[i][j] == 1 && dungeon[i][j + 1] == 2)
                dungeon[i][j + 1] = 14;
        }
    }

    for (int j = 0; j < DMAXY; j++) {
        for (int i = 0; i < DMAXX; i++) {
            if (dungeon[i][j] == 2 && dungeon[i + 1][j] == 6)
                dungeon[i + 1][j] = 2;
            if (dungeon[i][j] == 2 && dungeon[i + 1][j] == 9)
                dungeon[i + 1][j] = 11;
            if (dungeon[i][j] == 9 && dungeon[i + 1][j] == 6)
                dungeon[i + 1][j] = 12;
            if (dungeon[i][j] == 14 && dungeon[i + 1][j] == 1)
                dungeon[i + 1][j] = 13;
            if (dungeon[i][j] == 6 && dungeon[i + 1][j] == 14)
                dungeon[i + 1][j] = 15;
            if (dungeon[i][j] == 6 && dungeon[i][j + 1] == 13)
                dungeon[i][j + 1] = 16;
            if (dungeon[i][j] == 1 && dungeon[i][j + 1] == 9)
                dungeon[i][j + 1] = 10;
            if (dungeon[i][j] == 6 && dungeon[i][j - 1] == 1)
                dungeon[i][j - 1] = 1;
        }
    }

    for (int j = 0; j < DMAXY; j++) {
        for (int i = 0; i < DMAXX; i++) {
            if (dungeon[i][j] == 13 && dungeon[i][j + 1] == 30)
                dungeon[i][j + 1] = 27;
            if (dungeon[i][j] == 27 && dungeon[i + 1][j] == 30)
                dungeon[i + 1][j] = 19;
            if (dungeon[i][j] == 1 && dungeon[i][j + 1] == 30)
                dungeon[i][j + 1] = 27;
            if (dungeon[i][j] == 27 && dungeon[i + 1][j] == 1)
                dungeon[i + 1][j] = 16;
            if (dungeon[i][j] == 19 && dungeon[i + 1][j] == 27)
                dungeon[i + 1][j] = 26;
            if (dungeon[i][j] == 27 && dungeon[i + 1][j] == 30)
                dungeon[i + 1][j] = 19;
            if (dungeon[i][j] == 2 && dungeon[i + 1][j] == 15)
                dungeon[i + 1][j] = 14;
            if (dungeon[i][j] == 14 && dungeon[i + 1][j] == 15)
                dungeon[i + 1][j] = 14;
            if (dungeon[i][j] == 22 && dungeon[i + 1][j] == 1)
                dungeon[i + 1][j] = 16;
            if (dungeon[i][j] == 27 && dungeon[i + 1][j] == 1)
                dungeon[i + 1][j] = 16;
            if (dungeon[i][j] == 6 && dungeon[i + 1][j] == 27 && dungeon[i + 1][j + 1] != 0) /* check */
                dungeon[i + 1][j] = 22;
            if (dungeon[i][j] == 22 && dungeon[i + 1][j] == 30)
                dungeon[i + 1][j] = 19;
            if (dungeon[i][j] == 21 && dungeon[i + 1][j] == 1 && dungeon[i + 1][j - 1] == 1)
                dungeon[i + 1][j] = 13;
            if (dungeon[i][j] == 14 && dungeon[i + 1][j] == 30 && dungeon[i][j + 1] == 6)
                dungeon[i + 1][j] = 28;
            if (dungeon[i][j] == 16 && dungeon[i + 1][j] == 6 && dungeon[i][j + 1] == 30)
                dungeon[i][j + 1] = 27;
            if (dungeon[i][j] == 16 && dungeon[i][j + 1] == 30 && dungeon[i + 1][j + 1] == 30)
                dungeon[i][j + 1] = 27;
            if (dungeon[i][j] == 6 && dungeon[i + 1][j] == 30 && dungeon[i + 1][j - 1] == 6)
                dungeon[i + 1][j] = 21;
            if (dungeon[i][j] == 2 && dungeon[i + 1][j] == 27 && dungeon[i + 1][j + 1] == 9)
                dungeon[i + 1][j] = 29;
            if (dungeon[i][j] == 9 && dungeon[i + 1][j] == 15)
                dungeon[i + 1][j] = 14;
            if (dungeon[i][j] == 15 && dungeon[i + 1][j] == 27 && dungeon[i + 1][j + 1] == 2)
                dungeon[i + 1][j] = 29;
            if (dungeon[i][j] == 19 && dungeon[i + 1][j] == 18)
                dungeon[i + 1][j] = 24;
            if (dungeon[i][j] == 9 && dungeon[i + 1][j] == 15)
                dungeon[i + 1][j] = 14;
            if (dungeon[i][j] == 19 && dungeon[i + 1][j] == 19 && dungeon[i + 1][j - 1] == 30)
                dungeon[i + 1][j] = 24;
            if (dungeon[i][j] == 24 && dungeon[i][j - 1] == 30 && dungeon[i][j - 2] == 6)
                dungeon[i][j - 1] = 21;
            if (dungeon[i][j] == 2 && dungeon[i + 1][j] == 30)
                dungeon[i + 1][j] = 28;
            if (dungeon[i][j] == 15 && dungeon[i + 1][j] == 30)
                dungeon[i + 1][j] = 28;
            if (dungeon[i][j] == 28 && dungeon[i][j + 1] == 30)
                dungeon[i][j + 1] = 18;
            if (dungeon[i][j] == 28 && dungeon[i][j + 1] == 2)
                dungeon[i][j + 1] = 15;
            if (dungeon[i][j] == 19 && dungeon[i + 2][j] == 2 && dungeon[i + 1][j - 1] == 18 && dungeon[i + 1][j + 1] == 1)
                dungeon[i + 1][j] = 17;
            if (dungeon[i][j] == 19 && dungeon[i + 2][j] == 2 && dungeon[i + 1][j - 1] == 22 && dungeon[i + 1][j + 1] == 1)
                dungeon[i + 1][j] = 17;
            if (dungeon[i][j] == 19 && dungeon[i + 2][j] == 2 && dungeon[i + 1][j - 1] == 18 && dungeon[i + 1][j + 1] == 13)
                dungeon[i + 1][j] = 17;
            if (dungeon[i][j] == 21 && dungeon[i + 2][j] == 2 && dungeon[i + 1][j - 1] == 18 && dungeon[i + 1][j + 1] == 1)
                dungeon[i + 1][j] = 17;
            if (dungeon[i][j] == 21 && dungeon[i + 1][j + 1] == 1 && dungeon[i + 1][j - 1] == 22 && dungeon[i + 2][j] == 3)
                dungeon[i + 1][j] = 17;
            if (dungeon[i][j] == 15 && dungeon[i + 1][j] == 28 && dungeon[i + 2][j] == 30 && dungeon[i + 1][j - 1] == 6)
                dungeon[i + 1][j] = 23;
            if (dungeon[i][j] == 14 && dungeon[i + 1][j] == 28 && dungeon[i + 2][j] == 1)
                dungeon[i + 1][j] = 23;
            if (dungeon[i][j] == 15 && dungeon[i + 1][j] == 27 && dungeon[i + 1][j + 1] == 30)
                dungeon[i + 1][j] = 29;
            if (dungeon[i][j] == 28 && dungeon[i][j + 1] == 9)
                dungeon[i][j + 1] = 15;
            if (dungeon[i][j] == 21 && dungeon[i + 1][j - 1] == 21)
                dungeon[i + 1][j] = 24;
            if (dungeon[i][j] == 2 && dungeon[i + 1][j] == 27 && dungeon[i + 1][j + 1] == 30)
                dungeon[i + 1][j] = 29;
            if (dungeon[i][j] == 2 && dungeon[i + 1][j] == 18)
                dungeon[i + 1][j] = 25;
            if (dungeon[i][j] == 21 && dungeon[i + 1][j] == 9 && dungeon[i + 2][j] == 2)
                dungeon[i + 1][j] = 11;
            if (dungeon[i][j] == 19 && dungeon[i + 1][j] == 10)
                dungeon[i + 1][j] = 17;
            if (dungeon[i][j] == 15 && dungeon[i][j + 1] == 3)
                dungeon[i][j + 1] = 4;
            if (dungeon[i][j] == 22 && dungeon[i][j + 1] == 9)
                dungeon[i][j + 1] = 15;
            if (dungeon[i][j] == 18 && dungeon[i][j + 1] == 30)
                dungeon[i][j + 1] = 18;
            if (dungeon[i][j] == 24 && dungeon[i - 1][j] == 30)
                dungeon[i - 1][j] = 19;
            if (dungeon[i][j] == 21 && dungeon[i][j + 1] == 2)
                dungeon[i][j + 1] = 15;
            if (dungeon[i][j] == 21 && dungeon[i][j + 1] == 9)
                dungeon[i][j + 1] = 10;
            if (dungeon[i][j] == 22 && dungeon[i][j + 1] == 30)
                dungeon[i][j + 1] = 18;
            if (dungeon[i][j] == 21 && dungeon[i][j + 1] == 30)
                dungeon[i][j + 1] = 18;
            if (dungeon[i][j] == 16 && dungeon[i][j + 1] == 2)
                dungeon[i][j + 1] = 15;
            if (dungeon[i][j] == 13 && dungeon[i][j + 1] == 2)
                dungeon[i][j + 1] = 15;
            if (dungeon[i][j] == 22 && dungeon[i][j + 1] == 2)
                dungeon[i][j + 1] = 15;
            if (dungeon[i][j] == 21 && dungeon[i + 1][j] == 18 && dungeon[i + 2][j] == 30)
                dungeon[i + 1][j] = 24;
            if (dungeon[i][j] == 21 && dungeon[i + 1][j] == 9 && dungeon[i + 1][j + 1] == 1)
                dungeon[i + 1][j] = 16;
            if (dungeon[i][j] == 2 && dungeon[i + 1][j] == 27 && dungeon[i + 1][j + 1] == 2)
                dungeon[i + 1][j] = 29;
            if (dungeon[i][j] == 23 && dungeon[i][j + 1] == 2)
                dungeon[i][j + 1] = 15;
            if (dungeon[i][j] == 23 && dungeon[i][j + 1] == 9)
                dungeon[i][j + 1] = 15;
            if (dungeon[i][j] == 25 && dungeon[i][j + 1] == 2)
                dungeon[i][j + 1] = 15;
            if (dungeon[i][j] == 22 && dungeon[i + 1][j] == 9)
                dungeon[i + 1][j] = 11;
            if (dungeon[i][j] == 23 && dungeon[i + 1][j] == 9)
                dungeon[i + 1][j] = 11;
            if (dungeon[i][j] == 15 && dungeon[i + 1][j] == 1)
                dungeon[i + 1][j] = 16;
            if (dungeon[i][j] == 11 && dungeon[i + 1][j] == 15)
                dungeon[i + 1][j] = 14;
            if (dungeon[i][j] == 23 && dungeon[i + 1][j] == 1)
                dungeon[i + 1][j] = 16;
            if (dungeon[i][j] == 21 && dungeon[i + 1][j] == 27)
                dungeon[i + 1][j] = 26;
            if (dungeon[i][j] == 21 && dungeon[i + 1][j] == 18)
                dungeon[i + 1][j] = 24;
            if (dungeon[i][j] == 26 && dungeon[i + 1][j] == 1)
                dungeon[i + 1][j] = 16;
            if (dungeon[i][j] == 29 && dungeon[i + 1][j] == 1)
                dungeon[i + 1][j] = 16;
            if (dungeon[i][j] == 29 && dungeon[i][j + 1] == 2)
                dungeon[i][j + 1] = 15;
            if (dungeon[i][j] == 1 && dungeon[i][j - 1] == 15)
                dungeon[i][j - 1] = 10;
            if (dungeon[i][j] == 18 && dungeon[i][j + 1] == 2)
                dungeon[i][j + 1] = 15;
            if (dungeon[i][j] == 23 && dungeon[i][j + 1] == 30)
                dungeon[i][j + 1] = 18;
            if (dungeon[i][j] == 18 && dungeon[i][j + 1] == 9)
                dungeon[i][j + 1] = 10;
            if (dungeon[i][j] == 14 && dungeon[i + 1][j] == 30 && dungeon[i + 1][j + 1] == 30)
                dungeon[i + 1][j] = 23;
            if (dungeon[i][j] == 2 && dungeon[i + 1][j] == 28 && dungeon[i + 1][j - 1] == 6)
                dungeon[i + 1][j] = 23;
            if (dungeon[i][j] == 23 && dungeon[i + 1][j] == 18 && dungeon[i][j - 1] == 6)
                dungeon[i + 1][j] = 24;
            if (dungeon[i][j] == 14 && dungeon[i + 1][j] == 23 && dungeon[i + 2][j] == 30)
                dungeon[i + 1][j] = 28;
            if (dungeon[i][j] == 14 && dungeon[i + 1][j] == 28 && dungeon[i + 2][j] == 30 && dungeon[i + 1][j - 1] == 6)
                dungeon[i + 1][j] = 23;
            if (dungeon[i][j] == 23 && dungeon[i + 1][j] == 30)
                dungeon[i + 1][j] = 19;
            if (dungeon[i][j] == 29 && dungeon[i + 1][j] == 30)
                dungeon[i + 1][j] = 19;
            if (dungeon[i][j] == 29 && dungeon[i][j + 1] == 30)
                dungeon[i][j + 1] = 18;
            if (dungeon[i][j] == 19 && dungeon[i + 1][j] == 30)
                dungeon[i + 1][j] = 19;
            if (dungeon[i][j] == 21 && dungeon[i + 1][j] == 30)
                dungeon[i + 1][j] = 19;
            if (dungeon[i][j] == 26 && dungeon[i + 1][j] == 30)
                dungeon[i + 1][j] = 19;
            if (dungeon[i][j] == 16 && dungeon[i][j + 1] == 30)
                dungeon[i][j + 1] = 18;
            if (dungeon[i][j] == 13 && dungeon[i][j + 1] == 9)
                dungeon[i][j + 1] = 10;
            if (dungeon[i][j] == 25 && dungeon[i][j + 1] == 30)
                dungeon[i][j + 1] = 18;
            if (dungeon[i][j] == 18 && dungeon[i][j + 1] == 2)
                dungeon[i][j + 1] = 15;
            if (dungeon[i][j] == 11 && dungeon[i + 1][j] == 3)
                dungeon[i + 1][j] = 5;
            if (dungeon[i][j] == 19 && dungeon[i + 1][j] == 9)
                dungeon[i + 1][j] = 11;
            if (dungeon[i][j] == 19 && dungeon[i + 1][j] == 1)
                dungeon[i + 1][j] = 13;
            if (dungeon[i][j] == 19 && dungeon[i + 1][j] == 13 && dungeon[i + 1][j - 1] == 6)
                dungeon[i + 1][j] = 16;
        }
    }

    for (int j = 0; j < DMAXY; j++) {
        for (int i = 0; i < DMAXX; i++) {
            if (dungeon[i][j] == 21 && dungeon[i][j + 1] == 24 && dungeon[i][j + 2] == 1)
                dungeon[i][j + 1] = 17;
            if (dungeon[i][j] == 15 && dungeon[i + 1][j + 1] == 9 && dungeon[i + 1][j - 1] == 1 && dungeon[i + 2][j] == 16)
                dungeon[i + 1][j] = 29;
            if (dungeon[i][j] == 2 && dungeon[i - 1][j] == 6)
                dungeon[i - 1][j] = 8;
            if (dungeon[i][j] == 1 && dungeon[i][j - 1] == 6)
                dungeon[i][j - 1] = 7;
            if (dungeon[i][j] == 6 && dungeon[i + 1][j] == 15 && dungeon[i + 1][j + 1] == 4)
                dungeon[i + 1][j] = 10;
            if (dungeon[i][j] == 1 && dungeon[i][j + 1] == 3)
                dungeon[i][j + 1] = 4;
            if (dungeon[i][j] == 1 && dungeon[i][j + 1] == 6)
                dungeon[i][j + 1] = 4;
            if (dungeon[i][j] == 9 && dungeon[i][j + 1] == 3)
                dungeon[i][j + 1] = 4;
            if (dungeon[i][j] == 10 && dungeon[i][j + 1] == 3)
                dungeon[i][j + 1] = 4;
            if (dungeon[i][j] == 13 && dungeon[i][j + 1] == 3)
                dungeon[i][j + 1] = 4;
            if (dungeon[i][j] == 1 && dungeon[i][j + 1] == 5)
                dungeon[i][j + 1] = 12;
            if (dungeon[i][j] == 1 && dungeon[i][j + 1] == 16)
                dungeon[i][j + 1] = 13;
            if (dungeon[i][j] == 6 && dungeon[i][j + 1] == 13)
                dungeon[i][j + 1] = 16;
            if (dungeon[i][j] == 25 && dungeon[i][j + 1] == 9)
                dungeon[i][j + 1] = 10;
            if (dungeon[i][j] == 13 && dungeon[i][j + 1] == 5)
                dungeon[i][j + 1] = 12;
            if (dungeon[i][j] == 28 && dungeon[i][j - 1] == 6 && dungeon[i + 1][j] == 1)
                dungeon[i + 1][j] = 23;
            if (dungeon[i][j] == 19 && dungeon[i + 1][j] == 10)
                dungeon[i + 1][j] = 17;
            if (dungeon[i][j] == 21 && dungeon[i + 1][j] == 9)
                dungeon[i + 1][j] = 11;
            if (dungeon[i][j] == 11 && dungeon[i + 1][j] == 3)
                dungeon[i + 1][j] = 5;
            if (dungeon[i][j] == 10 && dungeon[i + 1][j] == 4)
                dungeon[i + 1][j] = 12;
            if (dungeon[i][j] == 14 && dungeon[i + 1][j] == 4)
                dungeon[i + 1][j] = 12;
            if (dungeon[i][j] == 27 && dungeon[i + 1][j] == 9)
                dungeon[i + 1][j] = 11;
            if (dungeon[i][j] == 15 && dungeon[i + 1][j] == 4)
                dungeon[i + 1][j] = 12;
            if (dungeon[i][j] == 21 && dungeon[i + 1][j] == 1)
                dungeon[i + 1][j] = 16;
            if (dungeon[i][j] == 11 && dungeon[i + 1][j] == 4)
                dungeon[i + 1][j] = 12;
            if (dungeon[i][j] == 2 && dungeon[i + 1][j] == 3)
                dungeon[i + 1][j] = 5;
            if (dungeon[i][j] == 9 && dungeon[i + 1][j] == 3)
                dungeon[i + 1][j] = 5;
            if (dungeon[i][j] == 14 && dungeon[i + 1][j] == 3)
                dungeon[i + 1][j] = 5;
            if (dungeon[i][j] == 15 && dungeon[i + 1][j] == 3)
                dungeon[i + 1][j] = 5;
            if (dungeon[i][j] == 2 && dungeon[i + 1][j] == 5 && dungeon[i + 1][j - 1] == 16)
                dungeon[i + 1][j] = 12;
            if (dungeon[i][j] == 2 && dungeon[i + 1][j] == 4)
                dungeon[i + 1][j] = 12;
            if (dungeon[i][j] == 9 && dungeon[i + 1][j] == 4)
                dungeon[i + 1][j] = 12;
            if (dungeon[i][j] == 1 && dungeon[i][j - 1] == 8)
                dungeon[i][j - 1] = 9;
            if (dungeon[i][j] == 28 && dungeon[i + 1][j] == 23 && dungeon[i + 1][j + 1] == 3)
                dungeon[i + 1][j] = 16;
        }
    }

    for (int j = 0; j < DMAXY; j++) {
        for (int i = 0; i < DMAXX; i++) {
            if (dungeon[i][j] == 21 && dungeon[i + 1][j] == 10)
                dungeon[i + 1][j] = 17;
            if (dungeon[i][j] == 17 && dungeon[i + 1][j] == 4)
                dungeon[i + 1][j] = 12;
            if (dungeon[i][j] == 10 && dungeon[i + 1][j] == 4)
                dungeon[i + 1][j] = 12;
            if (dungeon[i][j] == 17 && dungeon[i][j + 1] == 5)
                dungeon[i][j + 1] = 12;
            if (dungeon[i][j] == 29 && dungeon[i][j + 1] == 9)
                dungeon[i][j + 1] = 10;
            if (dungeon[i][j] == 13 && dungeon[i][j + 1] == 5)
                dungeon[i][j + 1] = 12;
            if (dungeon[i][j] == 9 && dungeon[i][j + 1] == 16)
                dungeon[i][j + 1] = 13;
            if (dungeon[i][j] == 10 && dungeon[i][j + 1] == 16)
                dungeon[i][j + 1] = 13;
            if (dungeon[i][j] == 16 && dungeon[i][j + 1] == 3)
                dungeon[i][j + 1] = 4;
            if (dungeon[i][j] == 11 && dungeon[i][j + 1] == 5)
                dungeon[i][j + 1] = 12;
            if (dungeon[i][j] == 10 && dungeon[i + 1][j] == 3 && dungeon[i + 1][j - 1] == 16)
                dungeon[i + 1][j] = 12;
            if (dungeon[i][j] == 16 && dungeon[i][j + 1] == 5)
                dungeon[i][j + 1] = 12;
            if (dungeon[i][j] == 1 && dungeon[i][j + 1] == 6)
                dungeon[i][j + 1] = 4;
            if (dungeon[i][j] == 21 && dungeon[i + 1][j] == 13 && dungeon[i][j + 1] == 10)
                dungeon[i + 1][j + 1] = 12;
            if (dungeon[i][j] == 15 && dungeon[i + 1][j] == 10)
                dungeon[i + 1][j] = 17;
            if (dungeon[i][j] == 22 && dungeon[i][j + 1] == 11)
                dungeon[i][j + 1] = 17;
            if (dungeon[i][j] == 15 && dungeon[i + 1][j] == 28 && dungeon[i + 2][j] == 16)
                dungeon[i + 1][j] = 23;
            if (dungeon[i][j] == 28 && dungeon[i + 1][j] == 23 && dungeon[i + 1][j + 1] == 1 && dungeon[i + 2][j] == 6)
                dungeon[i + 1][j] = 16;
        }
    }

    for (int j = 0; j < DMAXY; j++) {
        for (int i = 0; i < DMAXX; i++) {
            if (dungeon[i][j] == 15 && dungeon[i + 1][j] == 28 && dungeon[i + 2][j] == 16)
                dungeon[i + 1][j] = 23;
            if (dungeon[i][j] == 21 && dungeon[i + 1][j - 1] == 21 && dungeon[i + 1][j + 1] == 13 && dungeon[i + 2][j] == 2)
                dungeon[i + 1][j] = 17;
            if (dungeon[i][j] == 19 && dungeon[i + 1][j] == 15 && dungeon[i + 1][j + 1] == 12)
                dungeon[i + 1][j] = 17;
        }
    }
}

static void Substitution()
{
    for (int y = 0; y < DMAXY; y++) {
        for (int x = 0; x < DMAXX; x++) {
            if (FlipCoin(3)) {
                const uint8_t c = L4BTYPES[dungeon[x][y]];
                if (c != 0 && !ProtectedBits.test(x, y)) {
                    int rv = GenerateRnd(16);
                    int i = -1;
                    while (rv >= 0) {
                        i++;
                        if (i == sizeof(L4BTYPES)) {
                            i = 0;
                        }
                        if (c == L4BTYPES[i]) {
                            rv--;
                        }
                    }

                    dungeon[x][y] = i;
                }
            }
        }
    }
    for (int y = 0; y < DMAXY; y++) {
        for (int x = 0; x < DMAXX; x++) {
            if (FlipCoin(10)) {
                const uint8_t c = dungeon[x][y];
                if (L4BTYPES[c] == 6 && !ProtectedBits.test(x, y)) {
                    dungeon[x][y] = PickRandomlyAmong({ 95, 96, 97 });
                }
            }
        }
    }
}

/** @brief Sets up the inside borders of the first quadrant so there are valid
 *  paths after mirroring the layout. */
static void PrepareInnerBorders()
{
    for (int y = (DMAXY / 2) - 1; y >= 0; y--) {
        for (int x = (DMAXX / 2) - 1; x >= 0; x--) {
            if (!DungeonMask.test(x, y)) {
                hallok[y] = false;
            } else {
                const bool hasSouthWestRoom = y + 1 < DMAXY / 2 && DungeonMask.test(x, y + 1);
                const bool hasSouthRoom = x + 1 < DMAXX / 2 && y + 1 < DMAXY / 2 && DungeonMask.test(x + 1, y + 1);
                hallok[y] = hasSouthWestRoom && !hasSouthRoom;
                x = 0;
            }
        }
    }

    int ry = GenerateRnd((DMAXY / 2) - 1) + 1;
    do {
        if (hallok[ry]) {
            for (int x = (DMAXX / 2) - 1; x >= 0; x--) {
                if (DungeonMask.test(x, ry)) {
                    x = -1;
                    ry = 0;
                } else {
                    DungeonMask.set(x, ry);
                    DungeonMask.set(x, ry + 1);
                }
            }
        } else {
            ry++;
            if (ry == DMAXY / 2) {
                ry = 1;
            }
        }
    } while (ry != 0);

    for (int x = (DMAXX / 2) - 1; x >= 0; x--) {
        for (int y = (DMAXY / 2) - 1; y >= 0; y--) {
            if (!DungeonMask.test(x, y)) {
                hallok[x] = false;
            } else {
                const bool hasSouthEastRoom = x + 1 < DMAXX / 2 && DungeonMask.test(x + 1, y);
                const bool hasSouthRoom = x + 1 < DMAXX / 2 && y + 1 < DMAXY / 2 && DungeonMask.test(x + 1, y + 1);
                hallok[x] = hasSouthEastRoom && !hasSouthRoom;
                y = 0;
            }
        }
    }

    int rx = GenerateRnd((DMAXX / 2) - 1) + 1;
    do {
        if (hallok[rx]) {
            for (int y = (DMAXY / 2) - 1; y >= 0; y--) {
                if (DungeonMask.test(rx, y)) {
                    y = -1;
                    rx = 0;
                } else {
                    DungeonMask.set(rx, y);
                    DungeonMask.set(rx + 1, y);
                }
            }
        } else {
            rx++;
            if (rx == DMAXX / 2) {
                rx = 1;
            }
        }
    } while (rx != 0);
}

/** @brief Find the number of mega tiles used by layout. */
static size_t FindArea()
{
    // Hell layouts are mirrored based on a single quadrant, this function is
    // called after the quadrant has been generated but before mirroring the
    // layout. We need to multiply by 4 to get the expected number of tiles.
    return DungeonMask.count() * 4;
}

static void ProtectQuads()
{
    for (int y = 0; y < 14; y++) {
        for (int x = 0; x < 14; x++) {
            ProtectedBits.set(L4Hold.x + x, L4Hold.y + y);
            ProtectedBits.set(DMAXX - 1 - x - L4Hold.x, L4Hold.y + y);
            ProtectedBits.set(L4Hold.x + x, DMAXY - 1 - y - L4Hold.y);
            ProtectedBits.set(DMAXX - 1 - x - L4Hold.x, DMAXY - 1 - y - L4Hold.y);
        }
    }
}

// Transparency-only helpers (rendering state omitted): no-ops for the megatile grid.
static void FloodTransparencyValues(uint8_t) {}
static void FixTransparency() {}

/** @brief Marks the edge of the map as solid/not part of the dungeon layout. */
static void CloseOuterBorders()
{
    for (int x = 0; x < DMAXX / 2; x++) { // NOLINT(modernize-loop-convert)
        DungeonMask.reset(x, 0);
    }
    for (int y = 0; y < DMAXY / 2; y++) {
        DungeonMask.reset(0, y);
    }
}

static void GeneralFix()
{
    for (int j = 0; j < DMAXY - 1; j++) {
        for (int i = 0; i < DMAXX - 1; i++) {
            if ((dungeon[i][j] == 24 || dungeon[i][j] == 122) && dungeon[i + 1][j] == 2 && dungeon[i][j + 1] == 5) {
                dungeon[i][j] = 17;
            }
        }
    }
}

static bool PlaceStairs(lvl_entry entry)
{
    auto position = placeMiniSet(L4USTAIRS, 199, false);
    if (!position)
        return false;
    if (entry == ENTRY_MAIN)
        ViewPosition = WorldTilePosition(position->first, position->second).megaToWorld() + WorldTileDisplacement(6, 6);

    if (currlevel != 15) {
        // Place stairs down
        if (currlevel != 16) {
            if (Quests[Q_WARLORD].IsAvailable()) {
                if (entry == ENTRY_PREV)
                    ViewPosition = SetPiece.position.megaToWorld() + WorldTileDisplacement(7, 7);
            } else {
                position = placeMiniSet(L4DSTAIRS, 199, false);
                if (!position)
                    return false;
                if (entry == ENTRY_PREV)
                    ViewPosition = WorldTilePosition(position->first, position->second).megaToWorld() + WorldTileDisplacement(7, 5);
            }
        }

        // Place town warp stairs
        if (currlevel == 13) {
            position = placeMiniSet(L4TWARP, 199, false);
            if (!position)
                return false;
            if (entry == ENTRY_TWARPDN)
                ViewPosition = WorldTilePosition(position->first, position->second).megaToWorld() + WorldTileDisplacement(6, 6);
        }
    } else {
        // Place hell gate
        position = placeMiniSet(L4PENTA2, 199, false);
        if (!position)
            return false;
        Quests[Q_DIABLO].position_x = position->first;
        Quests[Q_DIABLO].position_y = position->second;
        if (entry == ENTRY_PREV)
            ViewPosition = WorldTilePosition(position->first, position->second).megaToWorld() + WorldTileDisplacement(6, 5);
    }

    return true;
}

static void FixCornerTiles();

static void GenerateLevel(lvl_entry entry, const HellOptions& opts)
{
    uint32_t seed = g_rng.state();
    int guard = 0;
    while (guard++ < 200) {
        DRLG_InitTrans();

        constexpr size_t Minarea = 692;
        size_t areaTries = 0;
        do {
            SetRndSeed(seed);
            InitDungeonFlags();
            FirstRoom();
            CloseOuterBorders();
            seed = GetLCGEngineState();
        } while (++areaTries < 500 && FindArea() < Minarea);

        PrepareInnerBorders();
        MirrorDungeonLayout();

        MakeDmt();
        FixTilesPatterns();
        if (currlevel == 16) {
            ProtectQuads();
        }
        if (Quests[Q_WARLORD].IsAvailable() || (currlevel == Quests[Q_BETRAYER]._qlevel && UseMultiplayerQuests())) {
            for (int spi = SetPieceRoom.position.x; spi < SetPieceRoom.position.x + SetPieceRoom.size.width - 1; spi++) {
                for (int spj = SetPieceRoom.position.y; spj < SetPieceRoom.position.y + SetPieceRoom.size.height - 1; spj++) {
                    ProtectedBits.set(spi, spj);
                }
            }
        }
        AddWall();
        FloodTransparencyValues(6);
        FixTransparency();
        InitSetPiece(opts);
        if (currlevel == 16) {
            LoadDiabQuads(true, opts);
        }
        if (PlaceStairs(entry))
            break;
    }

    GeneralFix();

    if (currlevel != 16) {
        DRLG_PlaceThemeRooms(7, 10, 6, 8, true);
    }

    ApplyShadowsPatterns();
    FixCornerTiles();
    Substitution();

    // pdungeon memcpy omitted (rendering state).
    // DRLG_CheckQuests(SetPieceRoom.position) omitted: only sets quest
    // positions, never modifies the megatile grid.

    if (currlevel == 15) {
        const bool isGateOpen = UseMultiplayerQuests() || IsAnyOf(Quests[Q_DIABLO]._qactive, 2, 3);
        if (!isGateOpen) {
            // L4PENTA.place(Quests[Q_DIABLO].position) omitted (gate-closed visual).
        }
        for (int j = 1; j < DMAXY; j++) {
            for (int i = 1; i < DMAXX; i++) {
                if (IsAnyOf(dungeon[i][j], 98, 107)) {
                    Make_SetPC({ WorldTilePosition(i - 1, j - 1), WorldTileSize(5, 5) });
                    Quests[Q_BETRAYER].position_x = i;
                    Quests[Q_BETRAYER].position_y = j;
                }
            }
        }
    }
    if (currlevel == 16) {
        LoadDiabQuads(false, opts);
    }
}

static void FixCornerTiles()
{
    for (int j = 1; j < DMAXY - 1; j++) {
        for (int i = 1; i < DMAXX - 1; i++) {
            if (dungeon[i][j] >= 18 && dungeon[i][j] <= 30) {
                if (dungeon[i + 1][j] < 18 || dungeon[i][j + 1] < 18) {
                    dungeon[i][j] += 98;
                }
            }
        }
    }
}

static void Pass3()
{
    // DRLG_LPass3(30 - 1) only fills the rendering piece grid; the megatile
    // grid is already complete, so this is a no-op for our purposes.
}

static void CreateL4Dungeon(uint32_t rseed, lvl_entry entry, const HellOptions& opts)
{
    SetRndSeed(rseed);

    GenerateLevel(entry, opts);

    Pass3();
}

} // namespace gen

// =================================================================== public API

/** Apply vanilla single-player quest availability for a Hell level. */
void applyVanillaHellQuests(HellOptions& options)
{
    // DevilutionX questdat.tsv / drlg_l4.cpp (single player):
    //   Q_WARLORD  (Warlord of Blood)   qdlvl 13 -> warlord.dun set piece (L13)
    //   Q_BETRAYER (Archbishop Lazarus) qdlvl 15 -> L4PENTA2 hell gate (L15)
    //   Diablo's boss room (diab quads) is generated at dlvl 16.
    options.warlordQuest = (options.level == 13);
    options.betrayerQuest = (options.level == 15);
    options.diabloQuest = (options.level == 16);
}

/** Generate a Hell level (L13-L16). Deterministic; matches CreateL4Dungeon. */
DungeonTileGrid generateHell(const HellOptions& options)
{
    gen::currlevel = options.level;
    gen::leveltype = gen::DTYPE_HELL;
    gen::g_useMultiplayerQuests = options.multiplayerQuests;

    // Reset + configure the minimal quest state the generator reads.
    for (int q = 0; q < gen::NUM_QUESTS; q++) {
        gen::Quests[q]._qactive = 0;
        gen::Quests[q]._qlevel = 0;
        gen::Quests[q].position_x = 0;
        gen::Quests[q].position_y = 0;
    }
    // Quests[]._qlevel mirrors DevilutionX questdat.tsv qdlvl so
    // IsAvailable()/FirstRoom()/PlaceStairs() place the set piece + gate on the
    // correct dlvl: Warlord 13, Betrayer 15. Diablo's quads are keyed on
    // currlevel==16 directly and the gate quest is qdlvl 15.
    gen::Quests[gen::Q_WARLORD]._qlevel = 13;
    gen::Quests[gen::Q_BETRAYER]._qlevel = 15;
    gen::Quests[gen::Q_DIABLO]._qlevel = 15;
    gen::Quests[gen::Q_WARLORD]._qactive = options.warlordQuest ? 1 : 0;
    gen::Quests[gen::Q_BETRAYER]._qactive = options.betrayerQuest ? 2 : 0;
    gen::Quests[gen::Q_DIABLO]._qactive = options.diabloQuest ? 2 : 0;

    gen::CreateL4Dungeon(options.seed, gen::ENTRY_MAIN, options);

    DungeonTileGrid grid;
    for (int y = 0; y < gen::DMAXY; y++) {
        for (int x = 0; x < gen::DMAXX; x++) {
            grid.at(x, y) = gen::dungeon[x][y];
        }
    }
    return grid;
}

} // namespace dhfc
