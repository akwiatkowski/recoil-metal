#pragma once

#include "core/map/HeightField.hpp"
#include "core/sim/Fx.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace rm::sim {

// Where a unit can go, and how to get there.
//
// Both halves are grid arithmetic over the heightfield, which is why they live
// in core/ with the rest of the sim: "does this route go around the cliff" is a
// test rather than something to judge by watching a unit walk into one.

/// Heightmap squares along each side of one pathfinding cell.
///
/// Eight, so a cell is 64 elmos — about two unit widths, and coarse enough that
/// a 1024-square map searches 128x128 = 16k nodes instead of a million. Recoil's
/// own slope map is finer (one value per 2x2 squares, ReadMap.cpp:765-778), but
/// that is a movement-cost map consulted per step, not a search graph.
inline constexpr int kPathCellSquares = 8;

/// Default slope limit, in the degrees a unit definition is authored in.
///
/// 17 is BAR's Pawn (`units/ArmBots/armpw.lua:17`); its Stumpy tank says 10.
/// Per-unit limits belong to unit defs, which this engine does not read yet.
inline constexpr float kDefaultMaxSlopeDegrees = 17.0f;

/// Default wading depth, in elmos. Also the Pawn's (armpw.lua:18), and the
/// Stumpy's — 12 elmos is what most BAR ground units share.
inline constexpr float kDefaultMaxWaterDepthElmos = 12.0f;

/// Converts a unit definition's `maxslope` into the value a slope compares
/// against, exactly as the engine does (MoveDefHandler.cpp:84-95):
///
///     1 - cos(clamp(degrees, 0, 60) * 1.5)
///
/// The 1.5 and the clamp are the engine's, so the field's nominal 0..60 range
/// really means 0..90 degrees of ground. Worth copying rather than inventing a
/// cleaner scale: every number in the content is authored against this one.
[[nodiscard]] float maxSlopeFromDegrees(float degrees) noexcept;

// A coarse map of where a ground unit may stand.
//
// One byte per cell rather than a bitset: the grid is small (16 KB for a
// 1024-square map), it is read far more than it is built, and a bitset would
// trade that for shifting on every A* neighbour test.
struct PassabilityGrid {
    int cellsX = 0;
    int cellsZ = 0;
    /// FIXED POINT: the queries below run inside a tick, so the arithmetic that turns a
    /// world position into a cell has to be integer. Set at construction from the float the
    /// map states, which is load time and therefore the legitimate boundary.
    Fx elmosPerCell{};
    std::vector<std::uint8_t> passable;  ///< row-major, 1 = a unit may stand here

    /// Whether a cell may be stood on. Out-of-range cells are impassable rather
    /// than an error, so callers can test a neighbour without checking bounds
    /// first — which is what the A* inner loop wants.
    [[nodiscard]] bool passableAt(int x, int z) const noexcept;

    /// The cell containing a world coordinate, clamped onto the grid.
    [[nodiscard]] int cellAtWorld(Fx elmos) const noexcept;

    /// The world coordinate of a cell's CENTRE. Waypoints sit at centres: a
    /// path through cell corners would run along the boundary of whatever is
    /// next door, which is exactly where the impassable things are.
    [[nodiscard]] Fx worldAtCellCentre(int cell) const noexcept;
};

/// Whether a structure of `radiusElmos` may be founded at a world point.
///
/// EVERY CELL ITS FOOTPRINT TOUCHES, not just the centre one. A factory is wider than a cell on
/// this grid and a player aims at the middle of it, so a centre-only test cheerfully puts half a
/// building inside a cliff — and it looks fine until the thing finishes and stands in rock.
///
/// THE TARGET DOMAIN'S GRID for mobile products and naval yards; ordinary immobile structures
/// still use the builder's grid. Passability answers "may a unit STAND here", and buildability
/// is a different question in both reference engines: Recoil has a separate blocking map,
/// Supreme Commander has per-blueprint terrain classes. Ours remains the walkable test until a
/// dedicated build map exists, but the target-domain distinction prevents a commander that can
/// cross water from founding a naval yard on land.
///
/// P10.4 makes this better rather than different: a cost field replaces the binary answer, and
/// `buildPassability`'s "one blocked square blocks the cell" — which at 64 elmos is very coarse
/// for a 4-elmo extractor — stops being the conservative lie it is today.
[[nodiscard]] bool sitePlaceable(const PassabilityGrid& grid, Fx x, Fx z,
                                 Fx radiusElmos) noexcept;

/// Builds the passability grid for a map.
///
/// A cell is passable when every square in it is walkable, which is the
/// conservative reading — one cliff face in a cell blocks the cell. That errs
/// toward routing around things rather than through them, which is the right
/// direction to be wrong in when the cells are this coarse.
///
/// Two rules, both the engine's. **Slope**: the steepest face in the cell must
/// be no steeper than the limit, where a face's slope is `1 - normal.y` (the
/// same quantity Recoil's slope map holds, ReadMap.cpp:778). **Depth**: ground
/// under more than `maxWaterDepth` elmos of water is out, because Recoil's rule
/// is a depth limit and not a water line — a unit fords shallows.
[[nodiscard]] PassabilityGrid buildPassability(
    const HeightField& field, float waterLevelElmos,
    float maxSlopeDegrees = kDefaultMaxSlopeDegrees,
    float maxWaterDepthElmos = kDefaultMaxWaterDepthElmos);

/// Builds the inverse grid used by surface ships.
///
/// Every terrain corner under a cell must be strictly below `waterLevelElmos - minDepthElmos`.
/// The maximum corner binds because one dry/shallow corner is enough for a hull to hit shore.
/// Seabed slope is irrelevant to a unit floating on the plane above it.
[[nodiscard]] PassabilityGrid buildSurfaceWaterPassability(const HeightField& field,
                                                            float waterLevelElmos,
                                                            float minDepthElmos = 0.0f);

/// A route from one world position to another, as cell-centre waypoints.
///
/// Empty when there is no route, when either end is impassable, or when both
/// ends are in the same cell — in that last case there is genuinely nowhere to
/// walk, and returning one waypoint would send a unit trundling to the cell
/// centre for no reason.
///
/// The start cell is NOT included: a unit is already there, and a waypoint
/// behind it would make it turn round before setting off.
///
/// Plain A* over the 8-connected grid with an octile heuristic, which is
/// admissible for these move costs and so returns a shortest path. Diagonal
/// steps require both adjacent orthogonal cells to be passable, or units cut
/// the corners of cliffs. Deterministic: ties in the open set break on cell
/// index, so the same query always returns the same route.
[[nodiscard]] std::vector<std::array<Fx, 2>> findPath(const PassabilityGrid& grid, Fx fromX,
                                                       Fx fromZ, Fx toX, Fx toZ);

/// Nearest cell centre whose complete circular footprint is passable.
/// Ties break by row-major cell index, so site selection is deterministic.
[[nodiscard]] std::optional<std::array<Fx, 2>> nearestPlaceableSite(
    const PassabilityGrid& grid, Fx nearX, Fx nearZ, Fx radiusElmos);

/// The reachable cell in `from`'s connected component nearest `toward`.
///
/// Used when a water-only unit is tactically aimed at a point on land or across a disconnected
/// sea: the order advances as far as its own water permits instead of being rejected wholesale.
[[nodiscard]] std::optional<std::array<Fx, 2>> reachablePointToward(
    const PassabilityGrid& grid, Fx fromX, Fx fromZ, Fx towardX, Fx towardZ);

} // namespace rm::sim
