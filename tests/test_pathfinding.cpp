// Passability and A* tests. Pure grid arithmetic, so a route around a cliff is
// something to assert rather than to watch.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "core/map/HeightField.hpp"
#include "core/sim/Pathfinding.hpp"

#include <cmath>
#include <cstdint>
#include <numbers>
#include <vector>

using Catch::Approx;
using rm::HeightField;
using rm::sim::PassabilityGrid;

namespace {

/// Elmos across one pathfinding cell.
constexpr float kCell =
    static_cast<float>(rm::sim::kPathCellSquares) * static_cast<float>(rm::kSquareSize);

/// A flat field of `squares` squares, every corner at `height`.
[[nodiscard]] HeightField flatField(int squares, float height = 50.0f) {
    HeightField field;
    field.squaresX = squares;
    field.squaresZ = squares;
    field.baseHeight = height;
    field.heightScale = 1.0f;  // raw is all zero, so every corner reads `height`
    field.raw.assign(field.sampleCount(), std::uint16_t{0});
    return field;
}

/// Sets a rectangle of heightmap CORNERS to a raw value.
void setCorners(HeightField& field, int x0, int z0, int x1, int z1, std::uint16_t raw) {
    for (int z = z0; z <= z1 && z < field.verticesZ(); ++z) {
        for (int x = x0; x <= x1 && x < field.verticesX(); ++x) {
            field.raw[static_cast<std::size_t>(z) * static_cast<std::size_t>(field.verticesX())
                      + static_cast<std::size_t>(x)] = raw;
        }
    }
}

} // namespace

TEST_CASE("flat ground above water is passable everywhere") {
    const HeightField field = flatField(64);
    const PassabilityGrid grid = rm::sim::buildPassability(field, 0.0f);

    REQUIRE(grid.cellsX == 64 / rm::sim::kPathCellSquares);
    REQUIRE(grid.cellsZ == grid.cellsX);

    for (int z = 0; z < grid.cellsZ; ++z) {
        for (int x = 0; x < grid.cellsX; ++x) {
            REQUIRE(grid.passableAt(x, z));
        }
    }
}

TEST_CASE("ground under deep water is impassable") {
    // Recoil's rule is a depth limit, not a water line: a unit fords shallows.
    // BAR's Pawn allows 12 elmos (armpw.lua:18).
    HeightField field = flatField(64, -5.0f);  // 5 elmos deep: fordable
    const PassabilityGrid shallow = rm::sim::buildPassability(field, 0.0f);
    REQUIRE(shallow.passableAt(0, 0));

    field = flatField(64, -40.0f);  // well past any unit's depth limit
    const PassabilityGrid deep = rm::sim::buildPassability(field, 0.0f);
    REQUIRE_FALSE(deep.passableAt(0, 0));
}

TEST_CASE("a slope steeper than the limit is impassable") {
    const HeightField flat = flatField(64);
    REQUIRE(rm::sim::buildPassability(flat, -1000.0f).passableAt(2, 2));

    // A wall: one column of corners raised far above its neighbours, which puts
    // a near-vertical face inside the cells that contain it.
    HeightField wall = flatField(64);
    setCorners(wall, 20, 0, 20, wall.verticesZ() - 1, 4000);

    const PassabilityGrid grid = rm::sim::buildPassability(wall, -1000.0f);
    const int wallCell = 20 / rm::sim::kPathCellSquares;

    REQUIRE_FALSE(grid.passableAt(wallCell, 3));
    // And the rest of the map is untouched, so this is a wall rather than a
    // grid that failed wholesale.
    REQUIRE(grid.passableAt(0, 3));
    REQUIRE(grid.passableAt(grid.cellsX - 1, 3));
}

TEST_CASE("the slope limit is the engine's, in degrees") {
    // Recoil stores 1 - cos(clamp(deg, 0, 60) * 1.5) — see MoveDefHandler.cpp:84.
    CHECK(rm::sim::maxSlopeFromDegrees(0.0f) == Approx(0.0f));
    CHECK(rm::sim::maxSlopeFromDegrees(17.0f)
          == Approx(1.0f - std::cos(25.5f * std::numbers::pi_v<float> / 180.0f)));
    // Clamped at 60, which becomes a 90-degree face — everything is passable.
    CHECK(rm::sim::maxSlopeFromDegrees(90.0f) == Approx(1.0f).margin(1e-5));
}

TEST_CASE("world positions map to cells and back") {
    const HeightField field = flatField(64);
    const PassabilityGrid grid = rm::sim::buildPassability(field, 0.0f);

    CHECK(grid.cellAtWorld(0.0f) == 0);
    CHECK(grid.cellAtWorld(kCell * 0.5f) == 0);
    CHECK(grid.cellAtWorld(kCell) == 1);

    // The centre of cell 0 is half a cell in, which is where a path waypoint
    // lands — a unit walking to a cell CORNER would clip whatever is next door.
    CHECK(grid.worldAtCellCentre(0) == Approx(kCell * 0.5f));
    CHECK(grid.worldAtCellCentre(3) == Approx(kCell * 3.5f));

    // Off the map clamps rather than indexing out of range.
    CHECK(grid.cellAtWorld(-500.0f) == 0);
    CHECK(grid.cellAtWorld(1e6f) == grid.cellsX - 1);
}

TEST_CASE("a path across open ground is found and runs end to end") {
    const HeightField field = flatField(64);
    const PassabilityGrid grid = rm::sim::buildPassability(field, 0.0f);

    const auto path = rm::sim::findPath(grid, 20.0f, 20.0f, 460.0f, 460.0f);

    REQUIRE_FALSE(path.empty());
    // Ends at the cell containing the destination.
    CHECK(grid.cellAtWorld(path.back()[0]) == grid.cellAtWorld(460.0f));
    CHECK(grid.cellAtWorld(path.back()[1]) == grid.cellAtWorld(460.0f));

    // Consecutive waypoints are neighbours: a path that teleports would still
    // satisfy every other assertion here.
    for (std::size_t i = 1; i < path.size(); ++i) {
        const float dx = std::abs(path[i][0] - path[i - 1][0]);
        const float dz = std::abs(path[i][1] - path[i - 1][1]);
        REQUIRE(dx <= Approx(kCell));
        REQUIRE(dz <= Approx(kCell));
        REQUIRE(dx + dz > 0.0f);
    }

    // On open ground it should be the diagonal, not a detour.
    CHECK(path.size() <= 8);
}

TEST_CASE("a path goes around a wall rather than through it") {
    HeightField field = flatField(64);
    // A wall spanning most of the map, with a gap at the far +Z edge.
    setCorners(field, 32, 0, 32, 40, 4000);

    const PassabilityGrid grid = rm::sim::buildPassability(field, -1000.0f);
    const auto path = rm::sim::findPath(grid, 60.0f, 60.0f, 440.0f, 60.0f);

    REQUIRE_FALSE(path.empty());

    // Every waypoint is on passable ground — the point of the whole exercise.
    for (const auto& point : path) {
        REQUIRE(grid.passableAt(grid.cellAtWorld(point[0]), grid.cellAtWorld(point[1])));
    }

    // And it detoured: the straight line is ~380 elmos, so a route that had to
    // reach the gap and come back is materially longer.
    float length = 0.0f;
    for (std::size_t i = 1; i < path.size(); ++i) {
        length += std::hypot(path[i][0] - path[i - 1][0], path[i][1] - path[i - 1][1]);
    }
    CHECK(length > 500.0f);
}

TEST_CASE("an unreachable destination yields no path rather than a wrong one") {
    HeightField field = flatField(64);
    // A wall clean across the map: nothing can cross it.
    setCorners(field, 32, 0, 32, field.verticesZ() - 1, 4000);

    const PassabilityGrid grid = rm::sim::buildPassability(field, -1000.0f);
    const auto path = rm::sim::findPath(grid, 60.0f, 200.0f, 440.0f, 200.0f);

    CHECK(path.empty());
}

TEST_CASE("a destination on impassable ground yields no path") {
    HeightField field = flatField(64);
    setCorners(field, 32, 0, 32, field.verticesZ() - 1, 4000);
    const PassabilityGrid grid = rm::sim::buildPassability(field, -1000.0f);

    const float wallWorld = 32.0f * static_cast<float>(rm::kSquareSize);
    CHECK(rm::sim::findPath(grid, 60.0f, 200.0f, wallWorld, 200.0f).empty());
}

TEST_CASE("a path to where the unit already stands is empty, not a null step") {
    const HeightField field = flatField(64);
    const PassabilityGrid grid = rm::sim::buildPassability(field, 0.0f);

    // Same cell: there is nowhere to walk, and returning a single waypoint would
    // make a unit trundle to the cell centre for no reason.
    CHECK(rm::sim::findPath(grid, 20.0f, 20.0f, 30.0f, 30.0f).empty());
}

TEST_CASE("pathfinding is safe on a field with no samples") {
    const HeightField empty;
    const PassabilityGrid grid = rm::sim::buildPassability(empty, 0.0f);

    CHECK(grid.cellsX == 0);
    CHECK_FALSE(grid.passableAt(0, 0));
    CHECK(rm::sim::findPath(grid, 0.0f, 0.0f, 100.0f, 100.0f).empty());
}

TEST_CASE("pathfinding is deterministic") {
    HeightField field = flatField(64);
    setCorners(field, 32, 0, 32, 40, 4000);
    const PassabilityGrid grid = rm::sim::buildPassability(field, -1000.0f);

    const auto first = rm::sim::findPath(grid, 60.0f, 60.0f, 440.0f, 60.0f);
    const auto second = rm::sim::findPath(grid, 60.0f, 60.0f, 440.0f, 60.0f);

    REQUIRE(first.size() == second.size());
    for (std::size_t i = 0; i < first.size(); ++i) {
        CHECK(first[i][0] == second[i][0]);
        CHECK(first[i][1] == second[i][1]);
    }
}

TEST_CASE("different unit limits give genuinely different passability") {
    // The reason for building a grid per limit pair rather than one per scene:
    // a slope one unit can climb is a wall to another, and a depth one can ford
    // drowns another. If these came out identical the whole exercise would be
    // pointless.
    HeightField field = flatField(64);

    // A moderate ramp: passable to something that climbs 40 degrees, not to
    // something that climbs 5.
    for (int z = 0; z < field.verticesZ(); ++z) {
        for (int x = 0; x < field.verticesX(); ++x) {
            field.raw[static_cast<std::size_t>(z) * static_cast<std::size_t>(field.verticesX())
                      + static_cast<std::size_t>(x)] = static_cast<std::uint16_t>(x * 4);
        }
    }

    const PassabilityGrid nimble = rm::sim::buildPassability(field, -1000.0f, 40.0f);
    const PassabilityGrid clumsy = rm::sim::buildPassability(field, -1000.0f, 5.0f);

    CHECK(nimble.passableAt(3, 3));
    CHECK_FALSE(clumsy.passableAt(3, 3));
}

TEST_CASE("a deeper wader reaches ground a shallower one cannot") {
    const HeightField field = flatField(64, -20.0f);  // 20 elmos under the water line

    const PassabilityGrid amphibious = rm::sim::buildPassability(field, 0.0f, 60.0f, 40.0f);
    const PassabilityGrid landlubber = rm::sim::buildPassability(field, 0.0f, 60.0f, 5.0f);

    CHECK(amphibious.passableAt(0, 0));
    CHECK_FALSE(landlubber.passableAt(0, 0));
}
