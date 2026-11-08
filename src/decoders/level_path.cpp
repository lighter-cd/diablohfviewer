// src/decoders/level_path.cpp

#include "decoders/level_path.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <queue>
#include <utility>
#include <vector>

namespace decoders {

namespace {

constexpr int kAxisAlignedStepCost = 100;
constexpr int kDiagonalStepCost = 101;

struct Direction {
    int dx;
    int dy;
};

// Port of DevilutionX PathDirs (engineering/path.cpp) minus the "none" entry.
constexpr Direction kDirections[8] = {
    { 0, -1 }, { 1, -1 }, { 1, 0 }, { 1, 1 },
    { 0, 1 },  { -1, 1 }, { -1, 0 }, { -1, -1 },
};

bool isDiagonal(const Direction& d)
{
    return d.dx != 0 && d.dy != 0;
}

int stepCost(const Direction& d)
{
    return isDiagonal(d) ? kDiagonalStepCost : kAxisAlignedStepCost;
}

int heuristic(int x0, int y0, int x1, int y1)
{
    const int dx = std::abs(x1 - x0);
    const int dy = std::abs(y1 - y0);
    const int diagonalSteps = std::min(dx, dy);
    const int axisAlignedSteps = std::abs(dx - dy);
    return diagonalSteps * kDiagonalStepCost + axisAlignedSteps * kAxisAlignedStepCost;
}

bool solidAt(const std::vector<uint8_t>& solid, int gridW, int gridH, int x, int y)
{
    if (x < 0 || x >= gridW || y < 0 || y >= gridH)
        return true;
    const size_t idx = static_cast<size_t>(y) * static_cast<size_t>(gridW) + x;
    return idx < solid.size() && solid[idx] != 0;
}

} // namespace

bool isWalkableCell(const std::vector<uint8_t>& solid, int gridW, int gridH,
                    int x, int y)
{
    return !solidAt(solid, gridW, gridH, x, y);
}

int countWalkableCells(const std::vector<uint8_t>& solid, int gridW, int gridH)
{
    int count = 0;
    for (int y = 0; y < gridH; ++y) {
        for (int x = 0; x < gridW; ++x) {
            if (!solidAt(solid, gridW, gridH, x, y))
                ++count;
        }
    }
    return count;
}

PathResult findPath(const std::vector<uint8_t>& solid, int gridW, int gridH,
                    int startX, int startY, int destX, int destY,
                    int maxSteps)
{
    PathResult result;
    if (gridW <= 0 || gridH <= 0 || maxSteps <= 0)
        return result;
    if (startX < 0 || startX >= gridW || startY < 0 || startY >= gridH)
        return result;
    if (destX < 0 || destX >= gridW || destY < 0 || destY >= gridH)
        return result;

    const size_t cells = static_cast<size_t>(gridW) * static_cast<size_t>(gridH);
    const int startIdx = startY * gridW + startX;
    const int destIdx = destY * gridW + destX;

    if (startIdx == destIdx) {
        result.found = true;
        result.path.push_back({ startX, startY });
        return result;
    }

    // Early out when the heuristic alone exceeds the step budget (mirrors
    // FindPath's `initialHeuristicCost > PathDiagonalStepCost * maxPathLength`).
    if (heuristic(startX, startY, destX, destY) > kDiagonalStepCost * maxSteps)
        return result;

    const int inf = std::numeric_limits<int>::max();
    std::vector<int> gScore(cells, inf);
    std::vector<int> cameFrom(cells, -1);
    std::vector<uint8_t> closed(cells, 0);
    using QueueItem = std::pair<int, int>; // (f, index)
    std::priority_queue<QueueItem, std::vector<QueueItem>, std::greater<QueueItem>> frontier;

    gScore[static_cast<size_t>(startIdx)] = 0;
    frontier.push({ heuristic(startX, startY, destX, destY), startIdx });

    while (!frontier.empty()) {
        const QueueItem item = frontier.top();
        frontier.pop();
        const int current = item.second;
        const int currentG = gScore[static_cast<size_t>(current)];
        if (item.first != currentG + heuristic(current % gridW, current / gridW, destX, destY))
            continue; // stale entry
        if (closed[static_cast<size_t>(current)] != 0)
            continue;
        if (current == destIdx) {
            result.found = true;
            result.cost = currentG;
            int node = current;
            while (node != -1) {
                result.path.push_back({ node % gridW, node / gridW });
                if (node == startIdx)
                    break;
                node = cameFrom[static_cast<size_t>(node)];
            }
            std::reverse(result.path.begin(), result.path.end());
            return result;
        }
        closed[static_cast<size_t>(current)] = 1;

        if (currentG >= kDiagonalStepCost * maxSteps)
            continue;

        const int cx = current % gridW;
        const int cy = current / gridW;
        for (const Direction& d : kDirections) {
            const int nx = cx + d.dx;
            const int ny = cy + d.dy;
            if (nx < 0 || nx >= gridW || ny < 0 || ny >= gridH)
                continue;
            const int neighbor = ny * gridW + nx;
            const bool walkable = !solidAt(solid, gridW, gridH, nx, ny);
            // Like IfPath: a non-walkable tile is allowed only as the target.
            if (!walkable && neighbor != destIdx)
                continue;
            const int tentative = currentG + stepCost(d);
            if (tentative >= gScore[static_cast<size_t>(neighbor)]
                || tentative > kDiagonalStepCost * maxSteps)
                continue;
            gScore[static_cast<size_t>(neighbor)] = tentative;
            cameFrom[static_cast<size_t>(neighbor)] = current;
            frontier.push({ tentative + heuristic(nx, ny, destX, destY), neighbor });
        }
    }

    return result;
}

} // namespace decoders
