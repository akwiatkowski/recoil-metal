#pragma once

#include "core/map/HeightField.hpp"
#include "core/sim/Fx.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
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

/// C-176/C-177 phase periods for staggered land-path work.
inline constexpr int kPathPhase7Period = 7;
inline constexpr int kPathPhase13Period = 13;

/// The C-176 phase assigned to a path's row-major start cell.
[[nodiscard]] constexpr int pathPhase7(int startX, int startZ, int cellsX) noexcept {
    return (startZ * cellsX + startX) % kPathPhase7Period;
}

/// The C-177 phase assigned to a path's row-major start cell.
[[nodiscard]] constexpr int pathPhase13(int startX, int startZ, int cellsX) noexcept {
    return (startZ * cellsX + startX) % kPathPhase13Period;
}

/// Both staggered passes must select the path before its work is due.
[[nodiscard]] constexpr bool pathPhaseDue(int startX, int startZ, int cellsX,
                                           std::uint64_t tick) noexcept {
    return pathPhase7(startX, startZ, cellsX) == tick % kPathPhase7Period
           && pathPhase13(startX, startZ, cellsX) == tick % kPathPhase13Period;
}

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

// A coarse map of where a ground unit may stand, and at what price.
//
// One byte per cell rather than a bitset: the grid is small (16 KB for a
// 1024-square map), it is read far more than it is built, and a bitset would
// trade that for shifting on every A* neighbour test.
//
// THE BYTE IS A SPEED DIVISOR (ADR-035 layer 1, P10.4), not a flag: 0 is
// impassable, 1 is fully clear, and anything larger divides a mover's speed —
// and multiplies a route's cost — while it crosses the cell. `buildPassability`
// sets it to the cell's square count over its walkable count rounded up, so a
// partly blocked cell is expensive instead of refused and 1 still means every
// square is clean, which is what `sitePlaceable` charges a footprint for.
struct PassabilityGrid {
    int cellsX = 0;
    int cellsZ = 0;
    /// FIXED POINT: the queries below run inside a tick, so the arithmetic that turns a
    /// world position into a cell has to be integer. Set at construction from the float the
    /// map states, which is load time and therefore the legitimate boundary.
    Fx elmosPerCell{};
    std::vector<std::uint8_t> divisor;  ///< row-major, 0 = impassable, else speed divisor

    /// Whether a cell may be stood on AT ALL. Out-of-range cells are impassable rather
    /// than an error, so callers can test a neighbour without checking bounds
    /// first — which is what the A* inner loop wants.
    [[nodiscard]] bool passableAt(int x, int z) const noexcept;

    /// The cell's speed divisor: 0 impassable, 1 clean, larger the more of the
    /// cell is blocked. Same out-of-range contract as `passableAt`.
    [[nodiscard]] std::uint8_t divisorAt(int x, int z) const noexcept;

    /// The cell containing a world coordinate, clamped onto the grid.
    [[nodiscard]] int cellAtWorld(Fx elmos) const noexcept;

    /// The world coordinate of a cell's CENTRE. Waypoints sit at centres: a
    /// path through cell corners would run along the boundary of whatever is
    /// next door, which is exactly where the impassable things are.
    [[nodiscard]] Fx worldAtCellCentre(int cell) const noexcept;
};

/// One open A* frontier entry. The cell index resolves equal-cost ties.
struct PathSearchNode {
    Fx f{};
    int cell = 0;
};

/// Resumable form of `findPath`, retaining its geometry and tie-breaking exactly.
class PathSearch {
public:
    PathSearch(std::shared_ptr<const PassabilityGrid> grid, Fx fromX, Fx fromZ, Fx toX, Fx toZ);

    /// Expands no more than `budget` nodes. A zero budget is deliberately a no-op.
    void step(std::size_t budget);

    [[nodiscard]] bool finished() const noexcept { return finished_; }
    [[nodiscard]] const std::vector<std::array<Fx, 2>>& path() const noexcept { return path_; }
    [[nodiscard]] const std::vector<Fx>& costs() const noexcept { return costs_; }
    [[nodiscard]] const std::vector<int>& parents() const noexcept { return parents_; }
    [[nodiscard]] const std::vector<std::uint8_t>& closed() const noexcept { return closed_; }
    [[nodiscard]] const std::vector<PathSearchNode>& open() const noexcept { return open_; }
    [[nodiscard]] int startX() const noexcept { return startX_; }
    [[nodiscard]] int startZ() const noexcept { return startZ_; }
    [[nodiscard]] int goalX() const noexcept { return goalX_; }
    [[nodiscard]] int goalZ() const noexcept { return goalZ_; }
    [[nodiscard]] Fx targetX() const noexcept { return targetX_; }
    [[nodiscard]] Fx targetZ() const noexcept { return targetZ_; }

private:
    void finish(bool reached);

    std::shared_ptr<const PassabilityGrid> grid_;
    Fx targetX_{};
    Fx targetZ_{};
    int startX_ = 0;
    int startZ_ = 0;
    int goalX_ = 0;
    int goalZ_ = 0;
    std::vector<Fx> costs_;
    std::vector<int> parents_;
    std::vector<std::uint8_t> closed_;
    std::vector<PathSearchNode> open_;
    std::vector<std::array<Fx, 2>> path_;
    bool finished_ = false;
};

/// A content fingerprint distinguishing two grids that happen to share an
/// address across requests.
///
/// Requests snapshot their grid by COPY, so pointer identity cannot key the
/// flow-field cache — two movers on the same terrain arrive holding different
/// shared_ptrs to equal contents. FNV-1a over the dimensions and the divisor
/// bytes: cheap enough to run per request, and a grid mutated in place (the
/// C-177 re-request path edits `divisor` directly) earns a new fingerprint,
/// which is exactly the invalidation that path needs.
[[nodiscard]] std::uint64_t fingerprintOf(const PassabilityGrid& grid) noexcept;

/// A shared field of cost-to-goal over one grid — ADR-035's layer 2.
///
/// A reverse Dijkstra grown lazily from the goal cell: `stepUntil` expands the
/// frontier only until the cells it was asked about are CLOSED, so a field's
/// footprint stays the size of its demand rather than flooding the map. Every
/// requester to the same goal cell shares the object — the second mover to a
/// click point walks a gradient that is already settled instead of paying a
/// search of its own.
///
/// `blocked` is the dynamic overlay (layer 3): cells under standing structures
/// read as impassable WITHOUT being written into the terrain grid, so a placed
/// building reroutes new fields while the grid underneath stays immutable. A
/// field records every cell its frontier ever entered (`touched`); a blocking
/// change inside that region marks it `stale` and the cache drops it, while a
/// change outside leaves the field alone — "dirties only the fields whose
/// region it touches".
class FlowField {
public:
    enum class Reach : std::uint8_t { Pending, Reached, Unreachable };

    FlowField(std::shared_ptr<const PassabilityGrid> grid, int goalX, int goalZ,
              std::vector<std::uint8_t> blocked = {});

    /// Expands at most `budget` nodes, stopping early once every cell in
    /// `until` is settled. `until` is a span rather than a cell because two
    /// armies can share one field and the step owes them all progress.
    void stepUntil(std::span<const int> until, std::size_t budget);

    /// Whether `cell` (row-major index) is settled: reached once closed,
    /// unreachable once the frontier can never arrive — including a cell the
    /// grid or the overlay has always refused, which answers at once rather
    /// than waiting out the expansion.
    [[nodiscard]] Reach reach(int cell) const noexcept;

    /// Whether `cell` ever entered the frontier — the region a blocking change
    /// inside it would invalidate.
    [[nodiscard]] bool touched(int cell) const noexcept;

    /// Waypoints from a world position to the goal's exact target, in the same
    /// shape `findPath` returns: cell centres, start cell excluded, exact
    /// target last. Empty while the start is unsettled — the caller checks
    /// `reach` first.
    [[nodiscard]] std::vector<std::array<Fx, 2>> route(Fx fromX, Fx fromZ, Fx toX,
                                                       Fx toZ) const;

    /// Whether a blocking change inside the explored region has retired this
    /// field. A stale field stops spending budget and its owner re-attaches.
    [[nodiscard]] bool stale() const noexcept { return stale_; }
    void markStale() noexcept { stale_ = true; }

    [[nodiscard]] int goalX() const noexcept { return goalX_; }
    [[nodiscard]] int goalZ() const noexcept { return goalZ_; }
    [[nodiscard]] const std::vector<Fx>& costs() const noexcept { return costs_; }
    [[nodiscard]] const std::vector<int>& next() const noexcept { return next_; }
    [[nodiscard]] const std::vector<std::uint8_t>& closed() const noexcept { return closed_; }
    [[nodiscard]] const std::vector<PathSearchNode>& open() const noexcept { return open_; }
    [[nodiscard]] const std::vector<std::uint8_t>& explored() const noexcept {
        return touched_;
    }

private:
    [[nodiscard]] bool standable(int x, int z) const noexcept;
    /// The cheapest settled neighbour a requester standing in a BLOCKED cell can
    /// escape through — its own cell never settles (the overlay refuses it), so
    /// the field answers its route from next door instead. A factory's product
    /// rolls off into cells the factory itself claims; without this the first
    /// order out of every yard reads unreachable. -1 when there is no way out.
    [[nodiscard]] int escapeCell(int cell) const noexcept;

    std::shared_ptr<const PassabilityGrid> grid_;
    /// Snapshot of the overlay the field was built on — a copy, because the
    /// service replaces its layer on change and a cached field outlives it.
    std::vector<std::uint8_t> blocked_;
    int goalX_ = 0;
    int goalZ_ = 0;
    /// Cost-to-goal per closed-or-frontier cell; `next` is the settled step
    /// TOWARD the goal (the cell that relaxed this one), which is what makes a
    /// route a walk and not a search.
    std::vector<Fx> costs_;
    std::vector<int> next_;
    std::vector<std::uint8_t> closed_;
    std::vector<std::uint8_t> touched_;
    std::vector<PathSearchNode> open_;
    /// The goal cell itself is out: nothing stands there, so every reach is
    /// Unreachable — the same early answer `PathSearch` gave an impassable goal.
    bool dead_ = false;
    bool stale_ = false;
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
/// P10.4 sharpened rather than relaxed this: the test is now "every footprint cell PRISTINE"
/// (divisor 1), so a partly blocked cell a unit may now route through still refuses a
/// building — a factory cannot sit with a corner in the cliff it would path around.
[[nodiscard]] bool sitePlaceable(const PassabilityGrid& grid, Fx x, Fx z,
                                 Fx radiusElmos) noexcept;

/// Builds the passability grid for a map.
///
/// A cell's divisor is its square count over its WALKABLE square count, rounded
/// up: fully clear is 1, fully blocked is 0, and a partly blocked cell costs
/// more the more of it is missing — the P10.4 fix for the old all-or-nothing
/// rule that let one cliff face refuse a whole 64-elmo cell.
///
/// Two rules, both the engine's. **Slope**: a square is unwalkable when its
/// steepest face exceeds the limit, where a face's slope is `1 - normal.y` (the
/// same quantity Recoil's slope map holds, ReadMap.cpp:778). **Depth**: a
/// square is unwalkable when its lowest corner sits under more than
/// `maxWaterDepth` elmos of water, because Recoil's rule is a depth limit and
/// not a water line — a unit fords shallows.
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

/// A route from one world position to another, through cell-centre waypoints and ending at the
/// exact requested point (clamped to the grid bounds).
///
/// Empty when there is no route or either end is impassable. Two positions in the same cell
/// produce one direct waypoint; an empty path must never ambiguously mean both "unreachable"
/// and "nearby" to a command caller.
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

/// The grid ONE unit routes and pays on where it stands.
///
/// `gridForType` answers the type's surfaced/ordinary layer; `submergedForType`
/// is the parallel table of second-layer grids for `SurfacingSub` types
/// (nullptr entries read as "no distinct submerged layer" — land units, air,
/// ships that never dive). `submergedLayer` is the caller's
/// `motion.submersible && motion.submerged`, spelled as a bool so the helper
/// does not drag `MoveState` into a header that only knows grids.
///
/// C-219 holds the OLD layer until the dive completes, which is exactly what
/// keying on `submerged` — a flag that flips at the transition's end — gives.
[[nodiscard]] inline const PassabilityGrid* layerGridFor(
    std::span<const PassabilityGrid* const> gridForType,
    std::span<const PassabilityGrid* const> submergedForType, bool submergedLayer,
    std::size_t type) noexcept {
    if (submergedLayer && type < submergedForType.size()
        && submergedForType[type] != nullptr) {
        return submergedForType[type];
    }
    return type < gridForType.size() ? gridForType[type] : nullptr;
}

} // namespace rm::sim
