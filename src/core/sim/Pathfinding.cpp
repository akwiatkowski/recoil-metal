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

/// Highest corner height of a square — the point a surface hull would strike first.
[[nodiscard]] float squareMaxHeight(const rm::HeightField& field, int x, int z) noexcept {
    return std::max({field.heightAt(x, z), field.heightAt(x + 1, z),
                     field.heightAt(x, z + 1), field.heightAt(x + 1, z + 1)});
}

/// Octile distance in cells: the exact cost of an unobstructed 8-connected walk,
/// which makes it both admissible and tight.
/// The cost of a diagonal step: sqrt(2), in `Fx`.
///
/// A constant rather than a call to `fxSqrt(Fx::fromInt(2))`, so the heuristic costs no
/// arithmetic per node. The value is pinned by `test_fx`'s golden values, which assert
/// `fxSqrt(2).raw() == 23170` — so if the square root ever changed, that test fails rather
/// than this constant silently disagreeing with it.
inline constexpr rm::sim::Fx kDiagonalCost = rm::sim::Fx::fromRaw(23170);

[[nodiscard]] rm::sim::Fx octile(int dx, int dz) noexcept {
    const int a = std::abs(dx);
    const int b = std::abs(dz);
    const int lo = std::min(a, b);
    const int hi = std::max(a, b);
    return rm::sim::Fx::fromInt(hi - lo) + kDiagonalCost * lo;
}

/// One entry in the open set. Ordered by f, then by cell index so that equal-cost
/// frontiers are explored in the same order every run.
struct OpenNode {
    rm::sim::Fx f{};
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

int PassabilityGrid::cellAtWorld(Fx elmos) const noexcept {
    if (cellsX <= 0 || elmosPerCell <= Fx{}) {
        return 0;
    }
    // cellsX for both axes: the grid is square-celled, and callers pass whichever
    // axis they mean. Clamped, so a position off the map maps to the edge cell.
    const int cell = (elmos / elmosPerCell).floorToInt();
    return std::clamp(cell, 0, std::max(cellsX, cellsZ) - 1);
}

Fx PassabilityGrid::worldAtCellCentre(int cell) const noexcept {
    return (Fx::fromInt(cell) + Fx::fromRatio(1, 2)) * elmosPerCell;
}

bool sitePlaceable(const PassabilityGrid& grid, Fx x, Fx z, Fx radiusElmos) noexcept {
    if (grid.cellsX <= 0 || grid.cellsZ <= 0) {
        return false;  // no grid, no answer — and refusing beats founding a building on nothing
    }

    // OFF THE MAP IS REFUSED BEFORE THE CLAMP. `cellAtWorld` clamps to the edge cell, which is
    // right for pathing — a route to a point past the border should walk to the border — and
    // wrong here: it would silently accept a click beyond the map and build at the edge, which
    // is not where the player pointed.
    const Fx width = Fx::fromInt(grid.cellsX) * grid.elmosPerCell;
    const Fx depth = Fx::fromInt(grid.cellsZ) * grid.elmosPerCell;
    if (radiusElmos < Fx{} || x < Fx{} || z < Fx{} || x >= width || z >= depth
        || x - radiusElmos < Fx{} || z - radiusElmos < Fx{}
        || x + radiusElmos > width || z + radiusElmos > depth) {
        return false;
    }

    // The footprint's bounding square, in cells. A radius rather than a rectangle because that
    // is what the sim stores for every unit — `collisionRadiusElmos`, which both content
    // families state — and a structure's footprint is square in both of them.
    const int minX = grid.cellAtWorld(x - radiusElmos);
    const int maxX = grid.cellAtWorld(x + radiusElmos);
    const int minZ = grid.cellAtWorld(z - radiusElmos);
    const int maxZ = grid.cellAtWorld(z + radiusElmos);

    for (int cz = minZ; cz <= maxZ; ++cz) {
        for (int cx = minX; cx <= maxX; ++cx) {
            if (!grid.passableAt(cx, cz)) {
                return false;  // one blocked cell under the footprint is enough
            }
        }
    }
    return true;
}

PassabilityGrid buildPassability(const HeightField& field, float waterLevelElmos,
                                 float maxSlopeDegrees, float maxWaterDepthElmos) {
    PassabilityGrid grid;
    if (field.squaresX <= 0 || field.squaresZ <= 0) {
        return grid;
    }

    grid.cellsX = field.squaresX / kPathCellSquares;
    grid.cellsZ = field.squaresZ / kPathCellSquares;
    grid.elmosPerCell = Fx::fromInt(kPathCellSquares * kSquareSize);
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

PassabilityGrid buildSurfaceWaterPassability(const HeightField& field, float waterLevelElmos,
                                              float minDepthElmos) {
    PassabilityGrid grid;
    if (field.squaresX <= 0 || field.squaresZ <= 0) {
        return grid;
    }

    grid.cellsX = field.squaresX / kPathCellSquares;
    grid.cellsZ = field.squaresZ / kPathCellSquares;
    grid.elmosPerCell = Fx::fromInt(kPathCellSquares * kSquareSize);
    if (grid.cellsX <= 0 || grid.cellsZ <= 0) {
        grid.cellsX = 0;
        grid.cellsZ = 0;
        return grid;
    }

    const float highestNavigableHeight = waterLevelElmos - std::max(0.0f, minDepthElmos);
    grid.passable.assign(static_cast<std::size_t>(grid.cellsX)
                             * static_cast<std::size_t>(grid.cellsZ),
                         std::uint8_t{1});

    for (int cz = 0; cz < grid.cellsZ; ++cz) {
        for (int cx = 0; cx < grid.cellsX; ++cx) {
            bool navigable = true;
            for (int z = cz * kPathCellSquares;
                 z < (cz + 1) * kPathCellSquares && navigable; ++z) {
                for (int x = cx * kPathCellSquares; x < (cx + 1) * kPathCellSquares; ++x) {
                    if (squareMaxHeight(field, x, z) >= highestNavigableHeight) {
                        navigable = false;
                        break;
                    }
                }
            }
            grid.passable[static_cast<std::size_t>(cz) * static_cast<std::size_t>(grid.cellsX)
                          + static_cast<std::size_t>(cx)] = navigable ? std::uint8_t{1}
                                                                      : std::uint8_t{0};
        }
    }
    return grid;
}

std::vector<std::array<Fx, 2>> findPath(const PassabilityGrid& grid, Fx fromX, Fx fromZ,
                                        Fx toX, Fx toZ) {
    if (grid.cellsX <= 0 || grid.cellsZ <= 0) {
        return {};
    }

    const auto cellAt = [&grid](Fx position, int cells) {
        const int cell = (position / grid.elmosPerCell).floorToInt();
        return std::clamp(cell, 0, cells - 1);
    };
    const Fx targetX = std::clamp(toX, Fx{}, Fx::fromInt(grid.cellsX) * grid.elmosPerCell);
    const Fx targetZ = std::clamp(toZ, Fx{}, Fx::fromInt(grid.cellsZ) * grid.elmosPerCell);
    const int startX = cellAt(fromX, grid.cellsX);
    const int startZ = cellAt(fromZ, grid.cellsZ);
    const int goalX = cellAt(targetX, grid.cellsX);
    const int goalZ = cellAt(targetZ, grid.cellsZ);

    // A unit standing somewhere it could never have walked to is a state this
    // sim cannot produce, but scattering can — refuse rather than search the
    // whole map to fail.
    if (!grid.passableAt(startX, startZ) || !grid.passableAt(goalX, goalZ)) {
        return {};
    }
    if (startX == goalX && startZ == goalZ) {
        return {{{targetX, targetZ}}};
    }

    const auto cellCount = static_cast<std::size_t>(grid.cellsX)
                         * static_cast<std::size_t>(grid.cellsZ);
    const auto index = [&grid](int x, int z) {
        return static_cast<std::size_t>(z) * static_cast<std::size_t>(grid.cellsX)
             + static_cast<std::size_t>(x);
    };

    // "Unreached" is the type's maximum rather than an infinity, because fixed point has no
    // infinity — and the maximum works for the same reason infinity did: every real cost is
    // below it, so the first path found to a cell always wins.
    std::vector<Fx> costToReach(cellCount, Fx::fromRaw(INT32_MAX));
    std::vector<int> cameFrom(cellCount, -1);
    std::vector<std::uint8_t> closed(cellCount, 0);

    std::priority_queue<OpenNode, std::vector<OpenNode>, std::greater<>> open;

    costToReach[index(startX, startZ)] = Fx{};
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

            const Fx stepCost = diagonal ? kDiagonalCost : kFxOne;
            const Fx candidate = costToReach[current] + stepCost;
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
    std::vector<std::array<Fx, 2>> path;
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
    // Keep the goal cell's centre: the A* edge into it was checked for corner cutting.
    // The exact endpoint is then a safe final segment wholly inside that passable cell.
    if (path.back()[0] != targetX || path.back()[1] != targetZ) {
        path.push_back({{targetX, targetZ}});
    }
    return path;
}

std::optional<std::array<Fx, 2>> nearestPlaceableSite(const PassabilityGrid& grid, Fx nearX,
                                                       Fx nearZ, Fx radiusElmos) {
    if (grid.cellsX <= 0 || grid.cellsZ <= 0) {
        return std::nullopt;
    }
    const int nearCellX = grid.cellAtWorld(nearX);
    const int nearCellZ = grid.cellAtWorld(nearZ);
    std::optional<std::array<Fx, 2>> best;
    std::int64_t bestDistance = std::numeric_limits<std::int64_t>::max();

    for (int z = 0; z < grid.cellsZ; ++z) {
        for (int x = 0; x < grid.cellsX; ++x) {
            const Fx worldX = grid.worldAtCellCentre(x);
            const Fx worldZ = grid.worldAtCellCentre(z);
            if (!sitePlaceable(grid, worldX, worldZ, radiusElmos)) {
                continue;
            }
            const std::int64_t dx = static_cast<std::int64_t>(x - nearCellX);
            const std::int64_t dz = static_cast<std::int64_t>(z - nearCellZ);
            const std::int64_t distance = dx * dx + dz * dz;
            if (distance < bestDistance) {
                bestDistance = distance;
                best = std::array<Fx, 2>{worldX, worldZ};
            }
        }
    }
    return best;
}

std::optional<std::array<Fx, 2>> reachablePointToward(const PassabilityGrid& grid, Fx fromX,
                                                       Fx fromZ, Fx towardX, Fx towardZ) {
    if (grid.cellsX <= 0 || grid.cellsZ <= 0) {
        return std::nullopt;
    }
    const int startX = grid.cellAtWorld(fromX);
    const int startZ = grid.cellAtWorld(fromZ);
    if (!grid.passableAt(startX, startZ)) {
        return std::nullopt;
    }
    const int targetX = grid.cellAtWorld(towardX);
    const int targetZ = grid.cellAtWorld(towardZ);
    const auto index = [&grid](int x, int z) {
        return z * grid.cellsX + x;
    };
    static constexpr std::array<std::array<int, 2>, 8> kSteps{{
        {{1, 0}}, {{-1, 0}}, {{0, 1}}, {{0, -1}},
        {{1, 1}}, {{1, -1}}, {{-1, 1}}, {{-1, -1}},
    }};

    std::vector<std::uint8_t> seen(
        static_cast<std::size_t>(grid.cellsX) * static_cast<std::size_t>(grid.cellsZ), 0);
    std::queue<int> open;
    open.push(index(startX, startZ));
    seen[static_cast<std::size_t>(index(startX, startZ))] = 1;

    int bestX = startX;
    int bestZ = startZ;
    std::int64_t bestDistance = std::numeric_limits<std::int64_t>::max();
    while (!open.empty()) {
        const int current = open.front();
        open.pop();
        const int x = current % grid.cellsX;
        const int z = current / grid.cellsX;
        const std::int64_t dx = static_cast<std::int64_t>(x - targetX);
        const std::int64_t dz = static_cast<std::int64_t>(z - targetZ);
        const std::int64_t distance = dx * dx + dz * dz;
        if (distance < bestDistance) {
            bestDistance = distance;
            bestX = x;
            bestZ = z;
        }

        for (const auto& step : kSteps) {
            const int nx = x + step[0];
            const int nz = z + step[1];
            if (!grid.passableAt(nx, nz)) {
                continue;
            }
            const bool diagonal = step[0] != 0 && step[1] != 0;
            if (diagonal
                && (!grid.passableAt(x + step[0], z) || !grid.passableAt(x, z + step[1]))) {
                continue;
            }
            const int next = index(nx, nz);
            if (seen[static_cast<std::size_t>(next)] != 0) {
                continue;
            }
            seen[static_cast<std::size_t>(next)] = 1;
            open.push(next);
        }
    }
    return std::array<Fx, 2>{grid.worldAtCellCentre(bestX),
                              grid.worldAtCellCentre(bestZ)};
}

} // namespace rm::sim
