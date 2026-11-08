// src/decoders/level_path.h
//
// Static walkability + path finding over a decoded scene's MICRO-tile grid.
//
// The walkable rule mirrors DevilutionX `IsTileNotSolid` (SOL `Solid` flag).
// Path costs and the diagonal policy mirror `Source/engine/path.cpp`
// (FindPath): 8-way A*, axis-aligned cost 100, diagonal cost 101, and diagonal
// steps are allowed whenever the destination tile is walkable (DevilutionX's
// `CanStep` only restricts axis-aligned steps).
//
// This provides the "pathfinding information" half of stage 4 without pulling
// in runtime actors/objects: it works purely on the map's SOL collision data.

#pragma once

#include <cstdint>
#include <vector>

namespace decoders {

struct PathPoint {
    int x = 0;
    int y = 0;
};

struct PathResult {
    bool found = false;
    std::vector<PathPoint> path; // start .. destination (inclusive)
    int cost = 0;
};

// True when (x,y) is inside the grid and NOT flagged Solid.
bool isWalkableCell(const std::vector<uint8_t>& solid, int gridW, int gridH,
                    int x, int y);

// Count walkable cells (used by scene metadata / tests).
int countWalkableCells(const std::vector<uint8_t>& solid, int gridW, int gridH);

// Find a path between two MICRO-tile coordinates. `solid` is row-major
// (y*gridW+x) and may be shorter than the grid (missing cells are treated as
// walkable). The destination may be solid: like DevilutionX FindPath, the
// destination is still admitted so callers can show "closest reachable" or
// door targets. `maxSteps` bounds the search like MaxPathLengthPlayer.
PathResult findPath(const std::vector<uint8_t>& solid, int gridW, int gridH,
                    int startX, int startY, int destX, int destY,
                    int maxSteps = 1024);

} // namespace decoders
