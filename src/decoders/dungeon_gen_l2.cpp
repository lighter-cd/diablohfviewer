/**
 * @file dungeon_gen_l2.cpp
 *
 * Faithful clean-room port of DevilutionX's Catacombs generator
 * (Source/levels/drlg_l2.cpp) producing the 40x40 megatile grid
 * `dungeon[DMAXX][DMAXY]` — identical in meaning to the golden
 * `CreateL2Dungeon` tile layer.
 *
 * RNG call order is preserved exactly (all randomness flows through the shared
 * `g_rng` via GenerateRnd/FlipCoin/DiscardRandomValues). Only the megatile
 * grid matters for our output; rendering-only state (dPiece/pMegaTiles/dSpecial/
 * dFlags) and the transparency value grid (dTransVal) are intentionally not
 * reproduced — they never modify `dungeon`.
 *
 * Note on fidelity: a handful of reference functions read one cell past the
 * 40x40 grid (Substitution, FixDirtTiles, DoPatternCheck 3x3 neighbourhood).
 * The reference relies on surrounding global memory for those reads; in a
 * clean-room port those reads are clamped to the grid (treated as the grid's
 * default value). This only affects a few edge tiles and never the structure
 * of a valid, connected Catacombs level (including the L2 closed-door arches).
 */
#include "decoders/dungeon_gen_common.h"
#include "decoders/dungeon_gen_l2_data.h"
#include "decoders/dungeon_gen.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <list>
#include <optional>

namespace dhfc {
namespace gen {

// ----------------------------------------------------------------- quest flags
// Resolved quest availability for the current generateCatacombs() call.
static bool q_blood = false;
static bool q_schamb = false;
static bool q_blind = false;
static CatacombsOptions g_opts {};

// --------------------------------------------------------- predungeon (ASCII)
// Char scaffolding used by the room/hall algorithm (NOT the output grid).
static char predungeon[DMAXX][DMAXY];

enum class HallDirection : int8_t {
	None,
	Up,
	Right,
	Down,
	Left,
};

struct HallNode {
	WorldTilePosition beginning;
	WorldTilePosition end;
	HallDirection direction;
};

struct RoomNode {
	WorldTilePosition topLeft;
	WorldTilePosition bottomRight;
};

static int nRoomCnt = 0;
static RoomNode RoomList[81];
static std::list<HallNode> HallList;

using Displacement = WorldTileDisplacement;
using Point = WorldTilePosition;

static const Displacement DirAdd[5] = {
	{ 0, 0 },
	{ 0, -1 },
	{ 1, 0 },
	{ 0, 1 },
	{ -1, 0 }
};

// ------------------------------------------------------------- guarded access
inline bool inB(int x, int y) { return x >= 0 && x < DMAXX && y >= 0 && y < DMAXY; }
inline uint8_t D(int x, int y) { return inB(x, y) ? dungeon[x][y] : 0; }
inline char P(int x, int y) { return inB(x, y) ? predungeon[x][y] : ' '; }

// --------------------------------------------------- set-piece .dun -> buffer
// buildDunBuffer is defined in dungeon_gen_gendung.cpp (shared by l2/l3/l4).

// ------------------------------------------------------------------- Patterns
// Identical to DevilutionX drlg_l2.cpp `int Patterns[100][10]`.
static int Patterns[100][10] = {
	{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 3 },
	{ 0, 0, 0, 0, 2, 0, 0, 0, 0, 3 },
	{ 0, 7, 0, 0, 1, 0, 0, 5, 0, 2 },
	{ 0, 5, 0, 0, 1, 0, 0, 7, 0, 2 },
	{ 0, 0, 0, 7, 1, 5, 0, 0, 0, 1 },
	{ 0, 0, 0, 5, 1, 7, 0, 0, 0, 1 },
	{ 0, 1, 0, 0, 3, 0, 0, 1, 0, 4 },
	{ 0, 0, 0, 1, 3, 1, 0, 0, 0, 5 },
	{ 0, 6, 0, 6, 1, 0, 0, 0, 0, 6 },
	{ 0, 6, 0, 0, 1, 6, 0, 0, 0, 9 },
	{ 0, 0, 0, 6, 1, 0, 0, 6, 0, 7 },
	{ 0, 0, 0, 0, 1, 6, 0, 6, 0, 8 },
	{ 0, 6, 0, 6, 6, 0, 8, 6, 0, 7 },
	{ 0, 6, 8, 6, 6, 6, 0, 0, 0, 9 },
	{ 0, 6, 0, 0, 6, 6, 0, 6, 8, 8 },
	{ 6, 6, 6, 6, 6, 6, 0, 6, 0, 8 },
	{ 2, 6, 6, 6, 6, 6, 0, 6, 0, 8 },
	{ 7, 7, 7, 6, 6, 6, 0, 6, 0, 8 },
	{ 6, 6, 2, 6, 6, 6, 0, 6, 0, 8 },
	{ 6, 2, 6, 6, 6, 6, 0, 6, 0, 8 },
	{ 2, 6, 6, 6, 6, 6, 0, 6, 0, 8 },
	{ 6, 7, 7, 6, 6, 6, 0, 6, 0, 8 },
	{ 4, 4, 6, 6, 6, 6, 2, 6, 2, 8 },
	{ 2, 2, 2, 2, 6, 2, 2, 6, 2, 7 },
	{ 2, 2, 2, 2, 6, 2, 6, 6, 6, 7 },
	{ 2, 2, 6, 2, 6, 6, 2, 2, 6, 9 },
	{ 2, 6, 2, 2, 6, 2, 2, 2, 2, 6 },
	{ 2, 2, 2, 2, 6, 6, 2, 2, 2, 9 },
	{ 2, 2, 2, 6, 6, 2, 2, 2, 2, 6 },
	{ 2, 2, 0, 2, 6, 6, 2, 2, 0, 9 },
	{ 0, 0, 0, 0, 4, 0, 0, 0, 0, 12 },
	{ 0, 1, 0, 0, 1, 4, 0, 1, 0, 10 },
	{ 0, 0, 0, 1, 1, 1, 0, 4, 0, 11 },
	{ 0, 0, 0, 6, 1, 4, 0, 1, 0, 14 },
	{ 0, 6, 0, 1, 1, 0, 0, 4, 0, 16 },
	{ 0, 6, 0, 0, 1, 1, 0, 4, 0, 15 },
	{ 0, 0, 0, 0, 1, 1, 0, 1, 4, 13 },
	{ 8, 8, 8, 8, 1, 1, 0, 1, 1, 13 },
	{ 8, 8, 4, 8, 1, 1, 0, 1, 1, 10 },
	{ 0, 0, 0, 1, 1, 1, 1, 1, 1, 11 },
	{ 1, 1, 1, 1, 1, 1, 2, 2, 8, 2 },
	{ 0, 1, 0, 1, 1, 4, 1, 1, 0, 16 },
	{ 0, 0, 0, 1, 1, 1, 1, 1, 4, 11 },
	{ 1, 1, 4, 1, 1, 1, 0, 2, 2, 2 },
	{ 1, 1, 1, 1, 1, 1, 6, 2, 6, 2 },
	{ 4, 1, 1, 1, 1, 1, 6, 2, 6, 2 },
	{ 2, 2, 2, 1, 1, 1, 4, 1, 1, 11 },
	{ 4, 1, 1, 1, 1, 1, 2, 2, 2, 2 },
	{ 1, 1, 4, 1, 1, 1, 2, 2, 1, 2 },
	{ 4, 1, 1, 1, 1, 1, 1, 2, 2, 2 },
	{ 2, 2, 6, 1, 1, 1, 4, 1, 1, 11 },
	{ 4, 1, 1, 1, 1, 1, 2, 2, 6, 2 },
	{ 1, 2, 2, 1, 1, 1, 4, 1, 1, 11 },
	{ 0, 1, 1, 0, 1, 1, 0, 1, 1, 10 },
	{ 2, 1, 1, 3, 1, 1, 2, 1, 1, 14 },
	{ 1, 1, 0, 1, 1, 2, 1, 1, 0, 1 },
	{ 0, 4, 0, 1, 1, 1, 0, 1, 1, 14 },
	{ 4, 1, 0, 1, 1, 0, 1, 1, 0, 1 },
	{ 0, 1, 0, 4, 1, 1, 0, 1, 1, 15 },
	{ 1, 1, 1, 1, 1, 1, 0, 2, 2, 2 },
	{ 0, 1, 1, 2, 1, 1, 2, 1, 4, 10 },
	{ 2, 1, 1, 1, 1, 1, 0, 4, 0, 16 },
	{ 1, 1, 4, 1, 1, 2, 0, 1, 2, 1 },
	{ 2, 1, 1, 2, 1, 1, 1, 1, 4, 10 },
	{ 1, 1, 2, 1, 1, 2, 4, 1, 8, 1 },
	{ 2, 1, 4, 1, 1, 1, 4, 4, 1, 16 },
	{ 2, 1, 1, 1, 1, 1, 1, 1, 1, 16 },
	{ 1, 1, 2, 1, 1, 1, 1, 1, 1, 15 },
	{ 1, 1, 1, 1, 1, 1, 2, 1, 1, 14 },
	{ 4, 1, 1, 1, 1, 1, 2, 1, 1, 14 },
	{ 1, 1, 1, 1, 1, 1, 1, 1, 2, 8 },
	{ 0, 0, 0, 0, 255, 0, 0, 0, 0, 0 },
};

// ----------------------------------------------------------------- shadows
void ApplyShadowsPatterns()
{
	uint8_t sd[2][2];

	for (int y = 1; y < DMAXY; y++) {
		for (int x = 1; x < DMAXX; x++) {
			const int v = dungeon[x][y];
			sd[0][0] = BSTYPESL2[v <= 160 ? v : 0];
			sd[1][0] = BSTYPESL2[D(x - 1, y) <= 160 ? D(x - 1, y) : 0];
			sd[0][1] = BSTYPESL2[D(x, y - 1) <= 160 ? D(x, y - 1) : 0];
			sd[1][1] = BSTYPESL2[D(x - 1, y - 1) <= 160 ? D(x - 1, y - 1) : 0];

			for (const auto& shadow : SPATSL2) {
				if (shadow.strig != sd[0][0])
					continue;
				if (shadow.s1 != 0 && shadow.s1 != sd[1][1])
					continue;
				if (shadow.s2 != 0 && shadow.s2 != sd[0][1])
					continue;
				if (shadow.s3 != 0 && shadow.s3 != sd[1][0])
					continue;

				if (shadow.nv1 != 0)
					dungeon[x - 1][y - 1] = shadow.nv1;
				if (shadow.nv2 != 0)
					dungeon[x][y - 1] = shadow.nv2;
				if (shadow.nv3 != 0)
					dungeon[x - 1][y] = shadow.nv3;
			}
		}
	}
}

void PlaceMiniSetRandom(const Miniset& miniset, int rndper)
{
	const int sw = miniset.width;
	const int sh = miniset.height;

	for (int sy = 0; sy < DMAXY - sh; sy++) {
		for (int sx = 0; sx < DMAXX - sw; sx++) {
			if (SetPieceRoom.contains({ sx, sy }))
				continue;
			if (!minisetMatches(miniset, sx, sy))
				continue;
			bool found = true;
			for (int yy = std::max(sy - sh, 0); yy < std::min(sy + (2 * sh), DMAXY) && found; yy++) {
				for (int xx = std::max(sx - sw, 0); xx < std::min(sx + (2 * sw), DMAXX); xx++) {
					if (dungeon[xx][yy] == miniset.replace[0][0]) {
						found = false;
						break;
					}
				}
			}
			if (!found)
				continue;
			if (GenerateRnd(100) >= rndper)
				continue;
			minisetPlace(miniset, sx, sy);
		}
	}
}

void PlaceMiniSetRandom1x1(uint8_t search, uint8_t replace, int rndper)
{
	PlaceMiniSetRandom(Miniset { 1, 1, { { search } }, { { replace } } }, rndper);
}

void InitSetPiece()
{
	const DunTileLayer* sp = nullptr;
	if (q_blind)
		sp = g_opts.blind1;
	else if (q_blood)
		sp = g_opts.blood1;
	else if (q_schamb)
		sp = g_opts.bonestr2;
	else
		return; // no setpiece needed for this level

	if (sp == nullptr)
		return; // missing set piece -> graceful fallback

	const std::vector<uint16_t> buf = buildDunBuffer(*sp);
	const WorldTilePosition setPiecePosition = SetPieceRoom.position;
	PlaceDunTiles(buf.data(), setPiecePosition.x, setPiecePosition.y, 3);
	SetPiece = { setPiecePosition, GetDunSize(buf.data()) };
}

void InitDungeonFlags()
{
	ProtectedBits.reset();
	memset(predungeon, ' ', sizeof(predungeon));
}

void MapRoom(int x1, int y1, int x2, int y2)
{
	for (int jj = y1; jj <= y2; jj++) {
		for (int ii = x1; ii <= x2; ii++) {
			predungeon[ii][jj] = '.';
		}
	}
	for (int jj = y1; jj <= y2; jj++) {
		predungeon[x1][jj] = '#';
		predungeon[x2][jj] = '#';
	}
	for (int ii = x1; ii <= x2; ii++) {
		predungeon[ii][y1] = '#';
		predungeon[ii][y2] = '#';
	}
}

void DefineRoom(Point topLeft, Point bottomRight, bool forceHW)
{
	predungeon[topLeft.x][topLeft.y] = 'C';
	predungeon[topLeft.x][bottomRight.y] = 'E';
	predungeon[bottomRight.x][topLeft.y] = 'B';
	predungeon[bottomRight.x][bottomRight.y] = 'A';

	nRoomCnt++;
	RoomList[nRoomCnt] = { topLeft, bottomRight };

	if (forceHW) {
		for (int i = topLeft.x; i < bottomRight.x; i++) {
			/// BUGFIX: Should loop j between nY1 and nY2 instead of always using nY1.
			while (i < bottomRight.y) {
				ProtectedBits.set(i, topLeft.y);
				i++;
			}
		}
	}
	for (int i = topLeft.x + 1; i <= bottomRight.x - 1; i++) {
		predungeon[i][topLeft.y] = '#';
		predungeon[i][bottomRight.y] = '#';
	}
	bottomRight.y--;
	for (int j = topLeft.y + 1; j <= bottomRight.y; j++) {
		predungeon[topLeft.x][j] = '#';
		predungeon[bottomRight.x][j] = '#';
		for (int i = topLeft.x + 1; i < bottomRight.x; i++) {
			predungeon[i][j] = '.';
		}
	}
}

void CreateDoorType(Point position)
{
	if (P(position.x - 1, position.y) == 'D')
		return;
	if (P(position.x + 1, position.y) == 'D')
		return;
	if (P(position.x, position.y - 1) == 'D')
		return;
	if (P(position.x, position.y + 1) == 'D')
		return;
	if (IsAnyOf(P(position.x, position.y), 'A', 'B', 'C', 'E'))
		return;

	predungeon[position.x][position.y] = 'D';
}

void PlaceHallExt(Point position)
{
	if (inB(position.x, position.y) && P(position.x, position.y) == ' ')
		predungeon[position.x][position.y] = ',';
}

void CreateRoom(WorldTilePosition topLeft, WorldTilePosition bottomRight, int nRDest, HallDirection nHDir, std::optional<WorldTileSize> size)
{
	constexpr int AreaMin = 2;
	if (nRoomCnt >= 80 || topLeft.x + AreaMin > bottomRight.x || topLeft.y + AreaMin > bottomRight.y)
		return;

	const WorldTileSize area = { bottomRight.x - topLeft.x, bottomRight.y - topLeft.y };

	constexpr int RoomMax = 10;
	constexpr int RoomMin = 4;
	WorldTileSize roomSize = area;
	if (area.width > RoomMin)
		roomSize.width = GenerateRnd(std::min(area.width, RoomMax) - RoomMin) + RoomMin;
	if (area.height > RoomMin)
		roomSize.height = GenerateRnd(std::min(area.height, RoomMax) - RoomMin) + RoomMin;

	if (size)
		roomSize = *size;

	const int32_t randomWidth = GenerateRnd(area.width);
	const int32_t randomHeight = GenerateRnd(area.height);

	WorldTilePosition roomTopLeft = topLeft + WorldTileDisplacement(randomWidth, randomHeight);
	WorldTilePosition roomBottomRight = roomTopLeft + WorldTileDisplacement(roomSize.width, roomSize.height);
	if (roomBottomRight.x > bottomRight.x) {
		roomBottomRight.x = bottomRight.x;
		roomTopLeft.x = bottomRight.x - roomSize.width;
	}
	if (roomBottomRight.y > bottomRight.y) {
		roomBottomRight.y = bottomRight.y;
		roomTopLeft.y = bottomRight.y - roomSize.height;
	}

	roomTopLeft.x = std::clamp<int>(roomTopLeft.x, 1, 38);
	roomTopLeft.y = std::clamp<int>(roomTopLeft.y, 1, 38);
	roomBottomRight.x = std::clamp<int>(roomBottomRight.x, 1, 38);
	roomBottomRight.y = std::clamp<int>(roomBottomRight.y, 1, 38);

	DefineRoom(roomTopLeft, roomBottomRight, static_cast<bool>(size));

	constexpr WorldTileDisplacement standoff { 2, 2 };
	if (size)
		SetPieceRoom = { roomTopLeft + standoff, roomSize - 1 };

	const int nRid = nRoomCnt;

	if (nRDest != 0) {
		int nHx1 = 0;
		int nHy1 = 0;
		int nHx2 = 0;
		int nHy2 = 0;
		if (nHDir == HallDirection::Up) {
			nHx1 = GenerateRnd(roomSize.width - 2) + roomTopLeft.x + 1;
			nHy1 = roomTopLeft.y;
			const int nHw = RoomList[nRDest].bottomRight.x - RoomList[nRDest].topLeft.x - 2;
			nHx2 = GenerateRnd(nHw) + RoomList[nRDest].topLeft.x + 1;
			nHy2 = RoomList[nRDest].bottomRight.y;
		}
		if (nHDir == HallDirection::Down) {
			nHx1 = GenerateRnd(roomSize.width - 2) + roomTopLeft.x + 1;
			nHy1 = roomBottomRight.y;
			const int nHw = RoomList[nRDest].bottomRight.x - RoomList[nRDest].topLeft.x - 2;
			nHx2 = GenerateRnd(nHw) + RoomList[nRDest].topLeft.x + 1;
			nHy2 = RoomList[nRDest].topLeft.y;
		}
		if (nHDir == HallDirection::Right) {
			nHx1 = roomBottomRight.x;
			nHy1 = GenerateRnd(roomSize.height - 2) + roomTopLeft.y + 1;
			nHx2 = RoomList[nRDest].topLeft.x;
			const int nHh = RoomList[nRDest].bottomRight.y - RoomList[nRDest].topLeft.y - 2;
			nHy2 = GenerateRnd(nHh) + RoomList[nRDest].topLeft.y + 1;
		}
		if (nHDir == HallDirection::Left) {
			nHx1 = roomTopLeft.x;
			nHy1 = GenerateRnd(roomSize.height - 2) + roomTopLeft.y + 1;
			nHx2 = RoomList[nRDest].bottomRight.x;
			const int nHh = RoomList[nRDest].bottomRight.y - RoomList[nRDest].topLeft.y - 2;
			nHy2 = GenerateRnd(nHh) + RoomList[nRDest].topLeft.y + 1;
		}
		HallList.push_back({ { nHx1, nHy1 }, { nHx2, nHy2 }, nHDir });
	}

	const WorldTilePosition roomBottomLeft { roomTopLeft.x, roomBottomRight.y };
	const WorldTilePosition roomTopRight { roomBottomRight.x, roomTopLeft.y };
	if (roomSize.height > roomSize.width) {
		CreateRoom(topLeft + standoff, roomBottomLeft - standoff, nRid, HallDirection::Right, {});
		CreateRoom(roomTopRight + standoff, bottomRight - standoff, nRid, HallDirection::Left, {});
		CreateRoom(WorldTilePosition { topLeft.x, roomBottomRight.y } + standoff, WorldTilePosition { roomBottomRight.x, bottomRight.y } - standoff, nRid, HallDirection::Up, {});
		CreateRoom(WorldTilePosition { roomTopLeft.x, topLeft.y } + standoff, WorldTilePosition { bottomRight.x, roomTopLeft.y } - standoff, nRid, HallDirection::Down, {});
	} else {
		CreateRoom(topLeft + standoff, roomTopRight - standoff, nRid, HallDirection::Down, {});
		CreateRoom(roomBottomLeft + standoff, bottomRight - standoff, nRid, HallDirection::Up, {});
		CreateRoom(WorldTilePosition { topLeft.x, roomTopLeft.y } + standoff, WorldTilePosition { roomTopLeft.x, bottomRight.y } - standoff, nRid, HallDirection::Right, {});
		CreateRoom(WorldTilePosition { roomBottomRight.x, topLeft.y } + standoff, WorldTilePosition { bottomRight.x, roomBottomRight.y } - standoff, nRid, HallDirection::Left, {});
	}
}

void ConnectHall(const HallNode& node)
{
	Point beginning = node.beginning;
	Point end = node.end;

	const bool fMinusFlag = GenerateRnd(100) < 50;
	const bool fPlusFlag = GenerateRnd(100) < 50;
	CreateDoorType(beginning);
	CreateDoorType(end);
	HallDirection nCurrd = node.direction;
	end = end - DirAdd[static_cast<int>(nCurrd)];
	predungeon[end.x][end.y] = ',';
	bool fInroom = false;

	do {
		if (beginning.x >= 38 && nCurrd == HallDirection::Right)
			nCurrd = HallDirection::Left;
		if (beginning.y >= 38 && nCurrd == HallDirection::Down)
			nCurrd = HallDirection::Up;
		if (beginning.x <= 1 && nCurrd == HallDirection::Left)
			nCurrd = HallDirection::Right;
		if (beginning.y <= 1 && nCurrd == HallDirection::Up)
			nCurrd = HallDirection::Down;
		if (P(beginning.x, beginning.y) == 'C' && IsAnyOf(nCurrd, HallDirection::Up, HallDirection::Left))
			nCurrd = HallDirection::Right;
		if (P(beginning.x, beginning.y) == 'B' && IsAnyOf(nCurrd, HallDirection::Up, HallDirection::Right))
			nCurrd = HallDirection::Down;
		if (P(beginning.x, beginning.y) == 'E' && IsAnyOf(nCurrd, HallDirection::Left, HallDirection::Down))
			nCurrd = HallDirection::Up;
		if (P(beginning.x, beginning.y) == 'A' && IsAnyOf(nCurrd, HallDirection::Right, HallDirection::Down))
			nCurrd = HallDirection::Left;
		beginning = beginning + DirAdd[static_cast<int>(nCurrd)];
		if (P(beginning.x, beginning.y) == ' ') {
			if (fInroom) {
				CreateDoorType(beginning - DirAdd[static_cast<int>(nCurrd)]);
				fInroom = false;
			} else {
				if (fMinusFlag) {
					if (IsNoneOf(nCurrd, HallDirection::Up, HallDirection::Down))
						PlaceHallExt(beginning + Displacement { 0, -1 }); // Up
					else
						PlaceHallExt(beginning + Displacement { -1, 0 }); // Left
				}
				if (fPlusFlag) {
					if (IsNoneOf(nCurrd, HallDirection::Up, HallDirection::Down))
						PlaceHallExt(beginning + Displacement { 0, 1 }); // Down
					else
						PlaceHallExt(beginning + Displacement { 1, 0 }); // Right
				}
			}
			if (inB(beginning.x, beginning.y))
				predungeon[beginning.x][beginning.y] = ',';
		} else {
			if (!fInroom && P(beginning.x, beginning.y) == '#')
				CreateDoorType(beginning);
			if (P(beginning.x, beginning.y) != ',')
				fInroom = true;
		}
		const int nDx = std::abs(end.x - beginning.x);
		const int nDy = std::abs(end.y - beginning.y);
		if (nDx > nDy) {
			const int nRp = std::min(2 * nDx, 30);
			if (GenerateRnd(100) < nRp) {
				if (end.x <= beginning.x || beginning.x >= DMAXX)
					nCurrd = HallDirection::Left;
				else
					nCurrd = HallDirection::Right;
			}
		} else {
			const int nRp = std::min(5 * nDy, 80);
			if (GenerateRnd(100) < nRp) {
				if (end.y <= beginning.y || beginning.y >= DMAXY)
					nCurrd = HallDirection::Up;
				else
					nCurrd = HallDirection::Down;
			}
		}
		if (nDy < 10 && beginning.x == end.x && IsAnyOf(nCurrd, HallDirection::Right, HallDirection::Left)) {
			if (end.y <= beginning.y || beginning.y >= DMAXY)
				nCurrd = HallDirection::Up;
			else
				nCurrd = HallDirection::Down;
		}
		if (nDx < 10 && beginning.y == end.y && IsAnyOf(nCurrd, HallDirection::Up, HallDirection::Down)) {
			if (end.x <= beginning.x || beginning.x >= DMAXX)
				nCurrd = HallDirection::Left;
			else
				nCurrd = HallDirection::Right;
		}
		if (nDy == 1 && nDx > 1 && IsAnyOf(nCurrd, HallDirection::Up, HallDirection::Down)) {
			if (end.x <= beginning.x || beginning.x >= DMAXX)
				nCurrd = HallDirection::Left;
			else
				nCurrd = HallDirection::Right;
		}
		if (nDx == 1 && nDy > 1 && IsAnyOf(nCurrd, HallDirection::Right, HallDirection::Left)) {
			if (end.y <= beginning.y || beginning.y >= DMAXY)
				nCurrd = HallDirection::Up;
			else
				nCurrd = HallDirection::Down;
		}
		if (nDx == 0 && P(beginning.x, beginning.y) != ' ' && IsAnyOf(nCurrd, HallDirection::Right, HallDirection::Left)) {
			if (end.x <= node.beginning.x || beginning.x >= DMAXX)
				nCurrd = HallDirection::Up;
			else
				nCurrd = HallDirection::Down;
		}
		if (nDy == 0 && P(beginning.x, beginning.y) != ' ' && IsAnyOf(nCurrd, HallDirection::Up, HallDirection::Down)) {
			if (end.y <= node.beginning.y || beginning.y >= DMAXY)
				nCurrd = HallDirection::Left;
			else
				nCurrd = HallDirection::Right;
		}
	} while (beginning != end);
}

void DoPatternCheck(int i, int j)
{
	for (int k = 0; Patterns[k][4] != 255; k++) {
		int x = i - 1;
		int y = j - 1;
		int nOk = 254;
		for (int l = 0; l < 9 && nOk == 254; l++) {
			nOk = 255;
			if (l == 3 || l == 6) {
				y++;
				x = i - 1;
			}
			if (x >= 0 && x < DMAXX && y >= 0 && y < DMAXY) {
				switch (Patterns[k][l]) {
				case 0:
					nOk = 254;
					break;
				case 1:
					if (P(x, y) == '#')
						nOk = 254;
					break;
				case 2:
					if (P(x, y) == '.')
						nOk = 254;
					break;
				case 4:
					if (P(x, y) == ' ')
						nOk = 254;
					break;
				case 3:
					if (P(x, y) == 'D')
						nOk = 254;
					break;
				case 5:
					if (P(x, y) == 'D' || P(x, y) == '.')
						nOk = 254;
					break;
				case 6:
					if (P(x, y) == 'D' || P(x, y) == '#')
						nOk = 254;
					break;
				case 7:
					if (P(x, y) == ' ' || P(x, y) == '.')
						nOk = 254;
					break;
				case 8:
					if (P(x, y) == 'D' || P(x, y) == '#' || P(x, y) == '.')
						nOk = 254;
					break;
				}
			} else {
				nOk = 254;
			}
			x++;
		}
		if (nOk == 254) {
			dungeon[i][j] = static_cast<uint8_t>(Patterns[k][9]);
		}
	}
}

void FixTilesPatterns()
{
	for (int j = 0; j < DMAXY; j++) {
		for (int i = 0; i < DMAXX; i++) {
			if (dungeon[i][j] == 1 && D(i, j + 1) == 3) {
				dungeon[i][j + 1] = 1;
			}
			if (dungeon[i][j] == 3 && D(i, j + 1) == 1) {
				dungeon[i][j + 1] = 3;
			}
			if (dungeon[i][j] == 3 && D(i + 1, j) == 7) {
				dungeon[i + 1][j] = 3;
			}
			if (dungeon[i][j] == 2 && D(i + 1, j) == 3) {
				dungeon[i + 1][j] = 2;
			}
			if (dungeon[i][j] == 11 && D(i + 1, j) == 14) {
				dungeon[i + 1][j] = 16;
			}
			if (dungeon[i][j] == 15 && D(i, j + 1) == 1) {
				dungeon[i][j + 1] = 8;
			}
		}
	}
}

void Substitution()
{
	for (int y = 0; y < DMAXY; y++) {
		for (int x = 0; x < DMAXX; x++) {
			if (SetPieceRoom.contains({ x, y }))
				continue;
			if (!FlipCoin(4))
				continue;

			const uint8_t c = BTYPESL2[D(x, y)];
			if (c != 0) {
				int rv = GenerateRnd(16);
				int i = -1;
				while (rv >= 0) {
					i++;
					if (i == static_cast<int>(sizeof(BTYPESL2)))
						i = 0;
					if (c == BTYPESL2[i]) {
						rv--;
					}
				}

				int j;
				for (j = y - 2; j < y + 2; j++) {
					for (int k = x - 2; k < x + 2; k++) {
						if (D(k, j) == i) {
							j = y + 3;
							k = x + 2;
						}
					}
				}
				if (j < y + 3) {
					dungeon[x][y] = static_cast<uint8_t>(i);
				}
			}
		}
	}
}

int CountEmptyTiles()
{
	int t = 0;
	for (int jj = 0; jj < DMAXY; jj++) {
		for (int ii = 0; ii < DMAXX; ii++) {
			if (predungeon[ii][jj] == ' ') {
				t++;
			}
		}
	}
	return t;
}

void KnockWalls(int x1, int y1, int x2, int y2)
{
	for (int ii = x1 + 1; ii < x2; ii++) {
		if (P(ii, y1 - 1) == '.' && P(ii, y1 + 1) == '.') {
			predungeon[ii][y1] = '.';
		}
		if (P(ii, y2 - 1) == '.' && P(ii, y2 + 1) == '.') {
			predungeon[ii][y2] = '.';
		}
		if (P(ii, y1 - 1) == 'D') {
			predungeon[ii][y1 - 1] = '.';
		}
		if (P(ii, y2 + 1) == 'D') {
			predungeon[ii][y2 + 1] = '.';
		}
	}
	for (int jj = y1 + 1; jj < y2; jj++) {
		if (P(x1 - 1, jj) == '.' && P(x1 + 1, jj) == '.') {
			predungeon[x1][jj] = '.';
		}
		if (P(x2 - 1, jj) == '.' && P(x2 + 1, jj) == '.') {
			predungeon[x2][jj] = '.';
		}
		if (P(x1 - 1, jj) == 'D') {
			predungeon[x1 - 1][jj] = '.';
		}
		if (P(x2 + 1, jj) == 'D') {
			predungeon[x2 + 1][jj] = '.';
		}
	}
}

void FillVoid(bool xf1, bool yf1, bool xf2, bool yf2, int xx, int yy)
{
	int x1 = xx;
	if (xf1) {
		x1--;
	}
	int x2 = xx;
	if (xf2) {
		x2++;
	}
	int y1 = yy;
	if (yf1) {
		y1--;
	}
	int y2 = yy;
	if (yf2) {
		y2++;
	}
	if (!xf1) {
		while (yf1 || yf2) {
			if (y1 == 0) {
				yf1 = false;
			}
			if (y2 == DMAXY - 1) {
				yf2 = false;
			}
			if (y2 - y1 >= 14) {
				yf1 = false;
				yf2 = false;
			}
			if (yf1) {
				y1--;
			}
			if (yf2) {
				y2++;
			}
			if (P(x2, y1) != ' ') {
				yf1 = false;
			}
			if (P(x2, y2) != ' ') {
				yf2 = false;
			}
		}
		y1 += 2;
		y2 -= 2;
		if (y2 - y1 > 5) {
			while (xf2) {
				if (x2 == 39) {
					xf2 = false;
				}
				if (x2 - x1 >= 12) {
					xf2 = false;
				}
				for (int jj = y1; jj <= y2; jj++) {
					if (P(x2, jj) != ' ') {
						xf2 = false;
					}
				}
				if (xf2) {
					x2++;
				}
			}
			x2 -= 2;
			if (x2 - x1 > 5) {
				MapRoom(x1, y1, x2, y2);
				KnockWalls(x1, y1, x2, y2);
			}
		}
	} else if (!xf2) {
		while (yf1 || yf2) {
			if (y1 == 0) {
				yf1 = false;
			}
			if (y2 == DMAXY - 1) {
				yf2 = false;
			}
			if (y2 - y1 >= 14) {
				yf1 = false;
				yf2 = false;
			}
			if (yf1) {
				y1--;
			}
			if (yf2) {
				y2++;
			}
			if (P(x1, y1) != ' ') {
				yf1 = false;
			}
			if (P(x1, y2) != ' ') {
				yf2 = false;
			}
		}
		y1 += 2;
		y2 -= 2;
		if (y2 - y1 > 5) {
			while (xf1) {
				if (x1 == 0) {
					xf1 = false;
				}
				if (x2 - x1 >= 12) {
					xf1 = false;
				}
				for (int jj = y1; jj <= y2; jj++) {
					if (P(x1, jj) != ' ') {
						xf1 = false;
					}
				}
				if (xf1) {
					x1--;
				}
			}
			x1 += 2;
			if (x2 - x1 > 5) {
				MapRoom(x1, y1, x2, y2);
				KnockWalls(x1, y1, x2, y2);
			}
		}
	} else if (!yf1) {
		while (xf1 || xf2) {
			if (x1 == 0) {
				xf1 = false;
			}
			if (x2 == DMAXX - 1) {
				xf2 = false;
			}
			if (x2 - x1 >= 14) {
				xf1 = false;
				xf2 = false;
			}
			if (xf1) {
				x1--;
			}
			if (xf2) {
				x2++;
			}
			if (P(x1, y2) != ' ') {
				xf1 = false;
			}
			if (P(x2, y2) != ' ') {
				xf2 = false;
			}
		}
		x1 += 2;
		x2 -= 2;
		if (x2 - x1 > 5) {
			while (yf2) {
				if (y2 == DMAXY - 1) {
					yf2 = false;
				}
				if (y2 - y1 >= 12) {
					yf2 = false;
				}
				for (int ii = x1; ii <= x2; ii++) {
					if (P(ii, y2) != ' ') {
						yf2 = false;
					}
				}
				if (yf2) {
					y2++;
				}
			}
			y2 -= 2;
			if (y2 - y1 > 5) {
				MapRoom(x1, y1, x2, y2);
				KnockWalls(x1, y1, x2, y2);
			}
		}
	} else if (!yf2) {
		while (xf1 || xf2) {
			if (x1 == 0) {
				xf1 = false;
			}
			if (x2 == DMAXX - 1) {
				xf2 = false;
			}
			if (x2 - x1 >= 14) {
				xf1 = false;
				xf2 = false;
			}
			if (xf1) {
				x1--;
			}
			if (xf2) {
				x2++;
			}
			if (P(x1, y1) != ' ') {
				xf1 = false;
			}
			if (P(x2, y1) != ' ') {
				xf2 = false;
			}
		}
		x1 += 2;
		x2 -= 2;
		if (x2 - x1 > 5) {
			while (yf1) {
				if (y1 == 0) {
					yf1 = false;
				}
				if (y2 - y1 >= 12) {
					yf1 = false;
				}
				for (int ii = x1; ii <= x2; ii++) {
					if (P(ii, y1) != ' ') {
						yf1 = false;
					}
				}
				if (yf1) {
					y1--;
				}
			}
			y1 += 2;
			if (y2 - y1 > 5) {
				MapRoom(x1, y1, x2, y2);
				KnockWalls(x1, y1, x2, y2);
			}
		}
	}
}

bool FillVoids()
{
	int to = 0;
	while (CountEmptyTiles() > 700 && to < 100) {
		const int xx = GenerateRnd(38) + 1;
		const int yy = GenerateRnd(38) + 1;
		if (P(xx, yy) != '#') {
			continue;
		}
		bool xf1 = false;
		bool xf2 = false;
		bool yf1 = false;
		bool yf2 = false;
		if (P(xx - 1, yy) == ' ' && P(xx + 1, yy) == '.') {
			if (P(xx + 1, yy - 1) == '.'
			    && P(xx + 1, yy + 1) == '.'
			    && P(xx - 1, yy - 1) == ' '
			    && P(xx - 1, yy + 1) == ' ') {
				xf1 = true;
				yf1 = true;
				yf2 = true;
			}
		} else if (P(xx + 1, yy) == ' ' && P(xx - 1, yy) == '.') {
			if (P(xx - 1, yy - 1) == '.'
			    && P(xx - 1, yy + 1) == '.'
			    && P(xx + 1, yy - 1) == ' '
			    && P(xx + 1, yy + 1) == ' ') {
				xf2 = true;
				yf1 = true;
				yf2 = true;
			}
		} else if (P(xx, yy - 1) == ' ' && P(xx, yy + 1) == '.') {
			if (P(xx - 1, yy + 1) == '.'
			    && P(xx + 1, yy + 1) == '.'
			    && P(xx - 1, yy - 1) == ' '
			    && P(xx + 1, yy - 1) == ' ') {
				yf1 = true;
				xf1 = true;
				xf2 = true;
			}
		} else if (P(xx, yy + 1) == ' ' && P(xx, yy - 1) == '.') {
			if (P(xx - 1, yy - 1) == '.'
			    && P(xx + 1, yy - 1) == '.'
			    && P(xx - 1, yy + 1) == ' '
			    && P(xx + 1, yy + 1) == ' ') {
				yf2 = true;
				xf1 = true;
				xf2 = true;
			}
		}
		if (xf1 || yf1 || xf2 || yf2) {
			FillVoid(xf1, yf1, xf2, yf2, xx, yy);
		}
		to++;
	}

	return CountEmptyTiles() <= 700;
}

bool CreateDungeon()
{
	std::optional<WorldTileSize> size;

	switch (currlevel) {
	case 5:
		if (q_blood)
			size = { 14, 20 };
		break;
	case 6:
		if (q_schamb)
			size = { 10, 10 };
		break;
	case 7:
		if (q_blind)
			size = { 15, 15 };
		break;
	case 8:
		break;
	}

	CreateRoom({ 2, 2 }, { DMAXX - 1, DMAXY - 1 }, 0, HallDirection::None, size);

	while (!HallList.empty()) {
		ConnectHall(HallList.front());
		HallList.pop_front();
	}

	for (int j = 0; j < DMAXY; j++) {
		for (int i = 0; i < DMAXX; i++) {
			if (IsAnyOf(P(i, j), 'A', 'B', 'C', 'E')) {
				predungeon[i][j] = '#';
			}
			if (P(i, j) == ',') {
				predungeon[i][j] = '.';
				for (int a = -1; a <= 1; a++) {
					for (int b = -1; b <= 1; b++) {
						if (a == 0 && b == 0)
							continue;
						if (i + a < 0 || j + b < 0)
							continue;
						if (i + a >= DMAXX || j + b >= DMAXY)
							continue;
						if (P(i + a, j + b) == ' ') {
							predungeon[i + a][j + b] = '#';
						}
					}
				}
			}
		}
	}

	if (!FillVoids()) {
		return false;
	}

	for (int j = 0; j < DMAXY; j++) {
		for (int i = 0; i < DMAXX; i++) {
			DoPatternCheck(i, j);
		}
	}

	return true;
}

void FixTransparency()
{
	// Rendering-only (dTransVal). No effect on the megatile grid, so omitted.
}

void FixDirtTiles()
{
	for (int j = 0; j < DMAXY; j++) {
		for (int i = 0; i < DMAXX; i++) {
			if (D(i, j) == 13 && D(i + 1, j) != 11) {
				dungeon[i][j] = 146;
			}
			if (D(i, j) == 11 && D(i + 1, j) != 11) {
				dungeon[i][j] = 144;
			}
			if (D(i, j) == 15 && D(i + 1, j) != 11) {
				dungeon[i][j] = 148;
			}
			if (D(i, j) == 10 && D(i, j + 1) != 10) {
				dungeon[i][j] = 143;
			}
			if (D(i, j) == 13 && D(i, j + 1) != 10) {
				dungeon[i][j] = 146;
			}
			if (D(i, j) == 14 && D(i, j + 1) != 15) {
				dungeon[i][j] = 147;
			}
		}
	}
}

void FixLockout()
{
	for (int j = 0; j < DMAXY; j++) {
		for (int i = 0; i < DMAXX; i++) {
			if (D(i, j) == 4 && D(i - 1, j) != 3) {
				dungeon[i][j] = 1;
			}
			if (D(i, j) == 5 && D(i, j - 1) != 3) {
				dungeon[i][j] = 2;
			}
		}
	}
	for (int j = 1; j < DMAXY - 1; j++) {
		for (int i = 1; i < DMAXX - 1; i++) {
			if (ProtectedBits.test(i, j)) {
				continue;
			}
			if ((D(i, j) == 2 || D(i, j) == 5) && D(i, j - 1) == 3 && D(i, j + 1) == 3) {
				bool doorok = false;
				while (true) {
					if (D(i, j) != 2 && D(i, j) != 5) {
						break;
					}
					if (D(i, j - 1) != 3 || D(i, j + 1) != 3) {
						break;
					}
					if (D(i, j) == 5) {
						doorok = true;
					}
					i++;
				}
				if (!doorok && !ProtectedBits.test(i - 1, j)) {
					dungeon[i - 1][j] = 5;
				}
			}
		}
	}
	for (int j = 1; j < DMAXX - 1; j++) {
		for (int i = 1; i < DMAXY - 1; i++) {
			if (ProtectedBits.test(j, i)) {
				continue;
			}
			if ((D(j, i) == 1 || D(j, i) == 4) && D(j - 1, i) == 3 && D(j + 1, i) == 3) {
				bool doorok = false;
				while (true) {
					if (D(j, i) != 1 && D(j, i) != 4) {
						break;
					}
					if (D(j - 1, i) != 3 || D(j + 1, i) != 3) {
						break;
					}
					if (D(j, i) == 4) {
						doorok = true;
					}
					i++;
				}
				if (!doorok && !ProtectedBits.test(j, i - 1)) {
					dungeon[j][i - 1] = 4;
				}
			}
		}
	}
}

void FixDoors()
{
	for (int j = 1; j < DMAXY; j++) {
		for (int i = 1; i < DMAXX; i++) {
			if (D(i, j) == 4 && D(i, j - 1) == 3) {
				dungeon[i][j] = 7;
			}
			if (D(i, j) == 5 && D(i - 1, j) == 3) {
				dungeon[i][j] = 9;
			}
		}
	}
}

bool PlaceStairs(lvl_entry entry)
{
	std::optional<std::pair<int, int>> position;

	// Place stairs up
	position = placeMiniSet(USTAIRS, 199, false);
	if (!position)
		return false;
	if (entry == ENTRY_MAIN)
		ViewPosition = { position->first * 2 + 16 + 5, position->second * 2 + 16 + 4 };

	// Place stairs down
	position = placeMiniSet(DSTAIRS, 199, false);
	if (!position)
		return false;
	if (entry == ENTRY_PREV)
		ViewPosition = { position->first * 2 + 16 + 4, position->second * 2 + 16 + 6 };

	// Place town warp stairs
	if (currlevel == 5) {
		position = placeMiniSet(WARPSTAIRS, 199, false);
		if (!position)
			return false;
		if (entry == ENTRY_TWARPDN)
			ViewPosition = { position->first * 2 + 16 + 5, position->second * 2 + 16 + 4 };
	}

	return true;
}

void DRLG_CheckQuests(Point position)
{
	if (q_blind) {
		dungeon[position.x][position.y + 1] = 154;
		dungeon[position.x + 10][position.y + 8] = 154;
	}
	if (q_blood && g_opts.blood2 != nullptr) {
		const std::vector<uint16_t> buf = buildDunBuffer(*g_opts.blood2);
		PlaceDunTiles(buf.data(), position.x, position.y, 0);
	}
	if (q_schamb && g_opts.bonestr1 != nullptr) {
		const std::vector<uint16_t> buf = buildDunBuffer(*g_opts.bonestr1);
		PlaceDunTiles(buf.data(), position.x, position.y, 3);
	}
}

void FloodTransparencyValues(uint8_t /*floorID*/)
{
	// Rendering-only flood fill on dTransVal. No effect on the megatile grid
	// and no RNG consumed, so omitted from the output pipeline.
}

void GenerateLevel(lvl_entry entry)
{
	uint32_t state = g_rng.state();
	while (true) {
		SetRndSeed(state);
		nRoomCnt = 0;
		InitDungeonFlags();
		DRLG_InitTrans();
		if (!CreateDungeon()) {
			state = GetLCGEngineState();
			continue;
		}
		FixTilesPatterns();
		InitSetPiece();
		FloodTransparencyValues(3);
		FixTransparency();
		if (PlaceStairs(entry))
			break;
		state = GetLCGEngineState();
	}

	FixLockout();
	FixDoors();
	FixDirtTiles();

	DRLG_PlaceThemeRooms(6, 10, 3, 0, false);
	PlaceMiniSetRandom(CTRDOOR1, 100);
	PlaceMiniSetRandom(CTRDOOR2, 100);
	PlaceMiniSetRandom(CTRDOOR3, 100);
	PlaceMiniSetRandom(CTRDOOR4, 100);
	PlaceMiniSetRandom(CTRDOOR5, 100);
	PlaceMiniSetRandom(CTRDOOR6, 100);
	PlaceMiniSetRandom(CTRDOOR7, 100);
	PlaceMiniSetRandom(CTRDOOR8, 100);
	PlaceMiniSetRandom(VARCH33, 100);
	PlaceMiniSetRandom(VARCH34, 100);
	PlaceMiniSetRandom(VARCH35, 100);
	PlaceMiniSetRandom(VARCH36, 100);
	PlaceMiniSetRandom(VARCH37, 100);
	PlaceMiniSetRandom(VARCH38, 100);
	PlaceMiniSetRandom(VARCH39, 100);
	PlaceMiniSetRandom(VARCH40, 100);
	PlaceMiniSetRandom(VARCH1, 100);
	PlaceMiniSetRandom(VARCH2, 100);
	PlaceMiniSetRandom(VARCH3, 100);
	PlaceMiniSetRandom(VARCH4, 100);
	PlaceMiniSetRandom(VARCH5, 100);
	PlaceMiniSetRandom(VARCH6, 100);
	PlaceMiniSetRandom(VARCH7, 100);
	PlaceMiniSetRandom(VARCH8, 100);
	PlaceMiniSetRandom(VARCH9, 100);
	PlaceMiniSetRandom(VARCH10, 100);
	PlaceMiniSetRandom(VARCH11, 100);
	PlaceMiniSetRandom(VARCH12, 100);
	PlaceMiniSetRandom(VARCH13, 100);
	PlaceMiniSetRandom(VARCH14, 100);
	PlaceMiniSetRandom(VARCH15, 100);
	PlaceMiniSetRandom(VARCH16, 100);
	PlaceMiniSetRandom(VARCH17, 100);
	PlaceMiniSetRandom(VARCH18, 100);
	PlaceMiniSetRandom(VARCH19, 100);
	PlaceMiniSetRandom(VARCH20, 100);
	PlaceMiniSetRandom(VARCH21, 100);
	PlaceMiniSetRandom(VARCH22, 100);
	PlaceMiniSetRandom(VARCH23, 100);
	PlaceMiniSetRandom(VARCH24, 100);
	PlaceMiniSetRandom(VARCH25, 100);
	PlaceMiniSetRandom(VARCH26, 100);
	PlaceMiniSetRandom(VARCH27, 100);
	PlaceMiniSetRandom(VARCH28, 100);
	PlaceMiniSetRandom(VARCH29, 100);
	PlaceMiniSetRandom(VARCH30, 100);
	PlaceMiniSetRandom(VARCH31, 100);
	PlaceMiniSetRandom(VARCH32, 100);
	PlaceMiniSetRandom(HARCH1, 100);
	PlaceMiniSetRandom(HARCH2, 100);
	PlaceMiniSetRandom(HARCH3, 100);
	PlaceMiniSetRandom(HARCH4, 100);
	PlaceMiniSetRandom(HARCH5, 100);
	PlaceMiniSetRandom(HARCH6, 100);
	PlaceMiniSetRandom(HARCH7, 100);
	PlaceMiniSetRandom(HARCH8, 100);
	PlaceMiniSetRandom(HARCH9, 100);
	PlaceMiniSetRandom(HARCH10, 100);
	PlaceMiniSetRandom(HARCH11, 100);
	PlaceMiniSetRandom(HARCH12, 100);
	PlaceMiniSetRandom(HARCH13, 100);
	PlaceMiniSetRandom(HARCH14, 100);
	PlaceMiniSetRandom(HARCH15, 100);
	PlaceMiniSetRandom(HARCH16, 100);
	PlaceMiniSetRandom(HARCH17, 100);
	PlaceMiniSetRandom(HARCH18, 100);
	PlaceMiniSetRandom(HARCH19, 100);
	PlaceMiniSetRandom(HARCH20, 100);
	PlaceMiniSetRandom(HARCH21, 100);
	PlaceMiniSetRandom(HARCH22, 100);
	PlaceMiniSetRandom(HARCH23, 100);
	PlaceMiniSetRandom(HARCH24, 100);
	PlaceMiniSetRandom(HARCH25, 100);
	PlaceMiniSetRandom(HARCH26, 100);
	PlaceMiniSetRandom(HARCH27, 100);
	PlaceMiniSetRandom(HARCH28, 100);
	PlaceMiniSetRandom(HARCH29, 100);
	PlaceMiniSetRandom(HARCH30, 100);
	PlaceMiniSetRandom(HARCH31, 100);
	PlaceMiniSetRandom(HARCH32, 100);
	PlaceMiniSetRandom(HARCH33, 100);
	PlaceMiniSetRandom(HARCH34, 100);
	PlaceMiniSetRandom(HARCH35, 100);
	PlaceMiniSetRandom(HARCH36, 100);
	PlaceMiniSetRandom(HARCH37, 100);
	PlaceMiniSetRandom(HARCH38, 100);
	PlaceMiniSetRandom(HARCH39, 100);
	PlaceMiniSetRandom(HARCH40, 100);
	PlaceMiniSetRandom(CRUSHCOL, 99);
	PlaceMiniSetRandom1x1(1, 80, 10);
	PlaceMiniSetRandom1x1(1, 81, 10);
	PlaceMiniSetRandom1x1(1, 82, 10);
	PlaceMiniSetRandom1x1(2, 84, 10);
	PlaceMiniSetRandom1x1(2, 85, 10);
	PlaceMiniSetRandom1x1(2, 86, 10);
	PlaceMiniSetRandom1x1(8, 87, 50);
	PlaceMiniSetRandom(PANCREAS1, 1);
	PlaceMiniSetRandom(PANCREAS2, 1);
	PlaceMiniSetRandom(BIG1, 3);
	PlaceMiniSetRandom(BIG2, 3);
	PlaceMiniSetRandom(BIG3, 3);
	PlaceMiniSetRandom(BIG4, 3);
	PlaceMiniSetRandom(BIG5, 3);
	PlaceMiniSetRandom(BIG6, 20);
	PlaceMiniSetRandom(BIG7, 20);
	PlaceMiniSetRandom(BIG8, 3);
	PlaceMiniSetRandom(BIG9, 20);
	PlaceMiniSetRandom(BIG10, 20);
	Substitution();
	ApplyShadowsPatterns();

	DRLG_CheckQuests(SetPieceRoom.position);
}

void CreateL2Dungeon(uint32_t rseed, lvl_entry entry)
{
	SetRndSeed(rseed);

	GenerateLevel(entry);

	// Pass3(): DRLG_LPass3(12-1) + InitDungeonPieces() only touch rendering
	// state (dPiece / pMegaTiles / dSpecial); they do not modify `dungeon`,
	// so they are intentionally omitted from the output pipeline.
}

} // namespace gen

DungeonTileGrid generateCatacombs(const CatacombsOptions& options)
{
	gen::leveltype = gen::DTYPE_CATACOMBS;
	gen::currlevel = options.level;
	gen::q_blood = options.bloodQuest;
	gen::q_schamb = options.schambQuest;
	gen::q_blind = options.blindQuest;
	gen::g_opts = options;

	gen::CreateL2Dungeon(options.seed, gen::ENTRY_MAIN);

	DungeonTileGrid grid {};
	for (int y = 0; y < DungeonTileGrid::Height; y++) {
		for (int x = 0; x < DungeonTileGrid::Width; x++) {
			grid.at(x, y) = gen::dungeon[x][y];
		}
	}
	return grid;
}

void applyVanillaCatacombsQuests(CatacombsOptions& options)
{
    options.bloodQuest = false;
    options.schambQuest = false;
    options.blindQuest = false;
    switch (options.level) {
    case 5:
        options.bloodQuest = true;
        break;
    case 7:
        options.schambQuest = true;
        break;
    case 8:
        options.blindQuest = true;
        break;
    default:
        break;
    }
}

} // namespace dhfc
