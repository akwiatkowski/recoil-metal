#include "core/sim/Pathfinding.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <queue>

namespace {

/// Slope of one heightmap square, as `1 - normal.y`.
///
/// The normal comes from the square's own gradient rather than from the two
/// triangles the mesh builder would make of it: averaging the opposite corner
/// pairs gives the same answer for any planar square and does not depend on
/// which diagonal the triangulation happened to choose.
[[nodiscard]] float squareSlope(const rm::HeightField& field, int x, int z) noexcept {
    const float h00 = field.heightAt(x, z);
    const float h10 = field.heightAt(x + 1, z);
    const float h01 = field.heightAt(x, z + 1);
    const float h11 = field.heightAt(x + 1, z + 1);

    const auto side = static_cast<float>(rm::kSquareSize);
    const float dx = ((h10 + h11) - (h00 + h01)) / (2.0f * side);
    const float dz = ((h01 + h11) - (h00 + h10)) / (2.0f * side);

    // For a surface y = f(x, z) the unnormalised normal is (-df/dx, 1, -df/dz),
    // so normal.y is 1 / |n|.
    const float normalY = 1.0f / std::sqrt(dx * dx + dz * dz + 1.0f);
    return 1.0f - normalY;
}

/// Lowest corner height of a square — the deepest point a unit standing on it
/// would have to wade through.
[[nodiscard]] float squareMinHeight(const rm::HeightField& field, int x, int z) noexcept {
    return std::min({field.heightAt(x, z), field.heightAt(x + 1, z),
                     field.heightAt(x, z + 1), field.heightAt(x + 1, z + 1)});
}

/// Octile distance in cells: the exact cost of an unobstructed 8-connected walk,
/// which makes it both admissible and tight.
[[nodiscard]] float octile(int dx, int dz) noexcept {
    static constexpr float kDiagonal = std::numbers::sqrt2_v<float>;
    const int a = std::abs(dx);
    const int b = std::abs(dz);
    const int lo = std::min(a, b);
    const int hi = std::max(a, b);
    return static_cast<float>(hi - lo) + kDiagonal * static_cast<float>(lo);
}

/// One entry in the open set. Ordered by f, then by cell index so that equal-cost
/// frontiers are explored in the same order every run.
struct OpenNode {
    float f = 0.0f;
    int cell = 0;

    [[nodiscard]] friend bool operator>(const OpenNode& a, const OpenNode& b) noexcept {
        if (a.f != b.f) {
            return a.f > b.f;
        }
        return a.cell > b.cell;
    }
};

} // namespace

namespace rm::sim {

float maxSlopeFromDegrees(float degrees) noexcept {
    // The engine's own conversion, 1.5 factor and clamp included
    // (MoveDefHandler.cpp:84-95).
    static constexpr float kDegreesToRadians = std::numbers::pi_v<float> / 180.0f;
    const float scaled = std::clamp(degrees, 0.0f, 60.0f) * 1.5f;
    return 1.0f - std::cos(scaled * kDegreesToRadians);
}

bool PassabilityGrid::passableAt(int x, int z) const noexcept {
    if (x < 0 || z < 0 || x >= cellsX || z >= cellsZ) {
        return false;
    }
    const auto index = static_cast<std::size_t>(z) * static_cast<std::size_t>(cellsX)
                     + static_cast<std::size_t>(x);
    return index < passable.size() && passable[index] != 0;
}

int PassabilityGrid::cellAtWorld(float elmos) const noexcept {
    if (cellsX <= 0 || elmosPerCell <= 0.0f) {
        return 0;
    }
    // cellsX for both axes: the grid is square-celled, and callers pass whichever
    // axis they mean. Clamped, so a position off the map maps to the edge cell.
    const auto cell = static_cast<int>(std::floor(elmos / elmosPerCell));
    return std::clamp(cell, 0, std::max(cellsX, cellsZ) - 1);
}

float PassabilityGrid::worldAtCellCentre(int cell) const noexcept {
    return (static_cast<float>(cell) + 0.5f) * elmosPerCell;
}

PassabilityGrid buildPassability(const HeightField& field, float waterLevelElmos,
                                 float maxSlopeDegrees, float maxWaterDepthElmos) {
    PassabilityGrid grid;
    if (field.squaresX <= 0 || field.squaresZ <= 0) {
        return grid;
    }

    grid.cellsX = field.squaresX / kPathCellSquares;
    grid.cellsZ = field.squaresZ / kPathCellSquares;
    grid.elmosPerCell = static_cast<float>(kPathCellSquares) * static_cast<float>(kSquareSize);
    if (grid.cellsX <= 0 || grid.cellsZ <= 0) {
        // A map smaller than one cell. Nothing to path across.
        grid.cellsX = 0;
        grid.cellsZ = 0;
        return grid;
    }

    const float maxSlope = maxSlopeFromDegrees(maxSlopeDegrees);
    const float lowestStandableHeight = waterLevelElmos - maxWaterDepthElmos;

    grid.passable.assign(static_cast<std::size_t>(grid.cellsX)
                             * static_cast<std::size_t>(grid.cellsZ),
                         std::uint8_t{1});

    for (int cz = 0; cz < grid.cellsZ; ++cz) {
        for (int cx = 0; cx < grid.cellsX; ++cx) {
            bool walkable = true;

            for (int z = cz * kPathCellSquares; z < (cz + 1) * kPathCellSquares && walkable; ++z) {
                for (int x = cx * kPathCellSquares; x < (cx + 1) * kPathCellSquares; ++x) {
                    if (squareSlope(field, x, z) > maxSlope
                        || squareMinHeight(field, x, z) < lowestStandableHeight) {
                        walkable = false;
                        break;
                    }
                }
            }

            grid.passable[static_cast<std::size_t>(cz) * static_cast<std::size_t>(grid.cellsX)
                          + static_cast<std::size_t>(cx)] = walkable ? std::uint8_t{1}
                                                                     : std::uint8_t{0};
        }
    }

    return grid;
}

std::vector<std::array<float, 2>> findPath(const PassabilityGrid& grid, float fromX, float fromZ,
                                           float toX, float toZ) {
    if (grid.cellsX <= 0 || grid.cellsZ <= 0) {
        return {};
    }

    const int startX = grid.cellAtWorld(fromX);
    const int startZ = grid.cellAtWorld(fromZ);
    const int goalX = grid.cellAtWorld(toX);
    const int goalZ = grid.cellAtWorld(toZ);

    // A unit standing somewhere it could never have walked to is a state this
    // sim cannot produce, but scattering can — refuse rather than search the
    // whole map to fail.
    if (!grid.passableAt(startX, startZ) || !grid.passableAt(goalX, goalZ)) {
        return {};
    }
    if (startX == goalX && startZ == goalZ) {
        return {};
    }

    const auto cellCount = static_cast<std::size_t>(grid.cellsX)
                         * static_cast<std::size_t>(grid.cellsZ);
    const auto index = [&grid](int x, int z) {
        return static_cast<std::size_t>(z) * static_cast<std::size_t>(grid.cellsX)
             + static_cast<std::size_t>(x);
    };

    std::vector<float> costToReach(cellCount, std::numeric_limits<float>::infinity());
    std::vector<int> cameFrom(cellCount, -1);
    std::vector<std::uint8_t> closed(cellCount, 0);

    std::priority_queue<OpenNode, std::vector<OpenNode>, std::greater<>> open;

    costToReach[index(startX, startZ)] = 0.0f;
    open.push(OpenNode{octile(goalX - startX, goalZ - startZ), static_cast<int>(index(startX, startZ))});

    // The eight neighbours, orthogonals first so that a tie between an
    // orthogonal and a diagonal route resolves the same way every time.
    static constexpr std::array<std::array<int, 2>, 8> kNeighbours{{
        {{1, 0}}, {{-1, 0}}, {{0, 1}}, {{0, -1}},
        {{1, 1}}, {{1, -1}}, {{-1, 1}}, {{-1, -1}},
    }};

    bool reached = false;
    while (!open.empty()) {
        const OpenNode node = open.top();
        open.pop();

        const auto current = static_cast<std::size_t>(node.cell);
        if (closed[current] != 0) {
            continue;  // a stale duplicate; the better entry was already expanded
        }
        closed[current] = 1;

        const int x = node.cell % grid.cellsX;
        const int z = node.cell / grid.cellsX;

        if (x == goalX && z == goalZ) {
            reached = true;
            break;
        }

        for (const auto& step : kNeighbours) {
            const int nx = x + step[0];
            const int nz = z + step[1];
            if (!grid.passableAt(nx, nz)) {
                continue;
            }

            const bool diagonal = step[0] != 0 && step[1] != 0;
            // No squeezing between two blocked cells, and no clipping the corner
            // of one: both orthogonal neighbours of a diagonal step must be open,
            // or a unit walks through the edge of a cliff.
            if (diagonal
                && (!grid.passableAt(x + step[0], z) || !grid.passableAt(x, z + step[1]))) {
                continue;
            }

            const std::size_t next = index(nx, nz);
            if (closed[next] != 0) {
                continue;
            }

            const float stepCost = diagonal ? std::numbers::sqrt2_v<float> : 1.0f;
            const float candidate = costToReach[current] + stepCost;
            if (candidate >= costToReach[next]) {
                continue;
            }

            costToReach[next] = candidate;
            cameFrom[next] = node.cell;
            open.push(OpenNode{candidate + octile(goalX - nx, goalZ - nz),
                               static_cast<int>(next)});
        }
    }

    if (!reached) {
        return {};
    }

    // Walk the parents back, then reverse. The start cell is dropped: the unit
    // is standing in it.
    std::vector<std::array<float, 2>> path;
    for (int cell = static_cast<int>(index(goalX, goalZ)); cell >= 0;
         cell = cameFrom[static_cast<std::size_t>(cell)]) {
        const int x = cell % grid.cellsX;
        const int z = cell / grid.cellsX;
        if (x == startX && z == startZ) {
            break;
        }
        path.push_back({{grid.worldAtCellCentre(x), grid.worldAtCellCentre(z)}});
    }

    std::reverse(path.begin(), path.end());
    return path;
}

} // namespace rm::sim
