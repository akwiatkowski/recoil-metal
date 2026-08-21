// The intel grid: who can see which square, and how removal undoes exactly what
// addition did (ADR-037).

#include "core/sim/Intel.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <vector>

using rm::sim::Fx;
using rm::sim::IntelGrid;

namespace {

/// A square index from grid coordinates, for tests that want to name one.
[[nodiscard]] std::int32_t squareOf(const IntelGrid& grid, int x, int z) {
    return z * grid.squaresX() + x;
}

/// The world centre of a square, which is where a coverage test is answered.
[[nodiscard]] Fx centreOf(const IntelGrid& grid, int square) {
    const int x = square % grid.squaresX();
    return Fx::fromInt(x) * grid.squareElmos() + grid.squareElmos() / Fx::fromInt(2);
}

} // namespace

TEST_CASE("a grid covers the map at its own resolution") {
    // 512 elmos across at mip 0 is 8 elmos a square — SQUARE_SIZE, the resolution
    // Recoil's own heightmap is stated in.
    const IntelGrid grid{Fx::fromInt(512), Fx::fromInt(256), 0};

    CHECK(grid.squaresX() == 64);
    CHECK(grid.squaresZ() == 32);
    CHECK(grid.squareElmos() == Fx::fromInt(8));

    // Each mip level halves it, exactly as `mipDiv = SQUARE_SIZE * (1 << mipLevel)`.
    const IntelGrid coarse{Fx::fromInt(512), Fx::fromInt(256), 2};
    CHECK(coarse.squaresX() == 16);
    CHECK(coarse.squareElmos() == Fx::fromInt(32));
}

TEST_CASE("a position off the map has no square rather than the nearest one") {
    // Clamping would put a unit standing past the border into the border square and
    // give it sight there. Off the map is off the grid.
    const IntelGrid grid{Fx::fromInt(512), Fx::fromInt(256), 0};

    CHECK(grid.squareAt(Fx::fromInt(0), Fx::fromInt(0)) == 0);
    CHECK(grid.squareAt(Fx::fromInt(-1), Fx::fromInt(0)) == IntelGrid::kNoSquare);
    CHECK(grid.squareAt(Fx::fromInt(0), Fx::fromInt(-1)) == IntelGrid::kNoSquare);
    CHECK(grid.squareAt(Fx::fromInt(512), Fx::fromInt(0)) == IntelGrid::kNoSquare);
    CHECK(grid.squareAt(Fx::fromInt(0), Fx::fromInt(256)) == IntelGrid::kNoSquare);
}

TEST_CASE("a square is covered while something counts it and not after") {
    IntelGrid grid{Fx::fromInt(512), Fx::fromInt(256), 0};
    const std::int32_t square = squareOf(grid, 10, 5);
    const std::vector<std::int32_t> shape{square};

    CHECK_FALSE(grid.covered(square));

    grid.add(shape);
    CHECK(grid.covered(square));
    CHECK(grid.count(square) == 1);

    grid.remove(shape);
    CHECK_FALSE(grid.covered(square));
    CHECK(grid.count(square) == 0);
}

TEST_CASE("two emitters over one square keep it lit until BOTH leave") {
    // The reason the grid holds a count and not a flag, and the reason it is worth a
    // test of its own: with a boolean, the first unit to walk away takes the second
    // unit's sight with it, and the symptom — vision flickering off where two units
    // overlap — looks like anything but a type error.
    IntelGrid grid{Fx::fromInt(512), Fx::fromInt(256), 0};
    const std::vector<std::int32_t> shape{squareOf(grid, 3, 3)};

    grid.add(shape);
    grid.add(shape);
    CHECK(grid.count(shape[0]) == 2);

    grid.remove(shape);
    CHECK(grid.covered(shape[0]));

    grid.remove(shape);
    CHECK_FALSE(grid.covered(shape[0]));
}

TEST_CASE("a circle of no radius still covers the square it stands on") {
    // 8 of the 568 blueprints state a vision radius under one square. Rounding them
    // to nothing would make a unit unable to see its own feet, which is a worse
    // reading of "sees a little" than "sees one square".
    IntelGrid grid{Fx::fromInt(512), Fx::fromInt(512), 0};
    std::vector<std::int32_t> shape;

    rm::sim::circleSquares(grid, Fx::fromInt(100), Fx::fromInt(100), Fx::fromInt(1), shape);

    REQUIRE(shape.size() == 1);
    CHECK(shape[0] == grid.squareAt(Fx::fromInt(100), Fx::fromInt(100)));
}

TEST_CASE("a circle covers what is inside it and nothing outside") {
    IntelGrid grid{Fx::fromInt(512), Fx::fromInt(512), 0};
    std::vector<std::int32_t> shape;

    // Radius 80 elmos = 10 squares, about a 316-square disc.
    const Fx x = Fx::fromInt(256);
    const Fx z = Fx::fromInt(256);
    rm::sim::circleSquares(grid, x, z, Fx::fromInt(80), shape);
    grid.add(shape);

    // Dead centre and just inside the rim, both lit.
    CHECK(grid.covered(grid.squareAt(x, z)));
    CHECK(grid.covered(grid.squareAt(x + Fx::fromInt(70), z)));

    // Well outside, dark — including the corner of the bounding box, which is what
    // separates a disc from a square.
    CHECK_FALSE(grid.covered(grid.squareAt(x + Fx::fromInt(100), z)));
    CHECK_FALSE(grid.covered(grid.squareAt(x + Fx::fromInt(72), z + Fx::fromInt(72))));

    // No square is counted twice. The midpoint algorithm emits the disc row by row and
    // a duplicated row would double every count in it — harmless for `covered`, since
    // removal doubles too, and wrong for everything that reads the number.
    std::vector<std::int32_t> sorted = shape;
    std::ranges::sort(sorted);
    CHECK(std::ranges::adjacent_find(sorted) == sorted.end());

    // And it is a disc, not a box: 349 squares for a radius of 10.
    //
    // That is pi*(r + 1/2)^2 = 346, NOT pi*r^2 = 314, and the half is real rather than
    // slop — the rasteriser fills up to and including the boundary square on each side,
    // so the disc it draws is half a square wider all round than the radius it was asked
    // for. Recoil's is the same shape. A box of the same radius would be 441.
    CHECK(shape.size() == 349);
}

TEST_CASE("a circle at the map edge is clipped, not wrapped") {
    // Wrapping would light the far side of the map. Worth its own test because the
    // index arithmetic makes it the natural bug: a row that runs past the right edge
    // continues into the start of the next one.
    IntelGrid grid{Fx::fromInt(512), Fx::fromInt(512), 0};
    std::vector<std::int32_t> shape;

    rm::sim::circleSquares(grid, Fx::fromInt(4), Fx::fromInt(256), Fx::fromInt(80), shape);
    grid.add(shape);

    // Nothing on the right-hand edge of any row.
    for (int z = 0; z < grid.squaresZ(); ++z) {
        CHECK(grid.count(squareOf(grid, grid.squaresX() - 1, z)) == 0);
    }
    // Every covered square really is within the radius of the emitter.
    for (const std::int32_t square : shape) {
        CHECK(centreOf(grid, square) < Fx::fromInt(4 + 80 + 8));
    }
}

TEST_CASE("adding a shape and removing it leaves the grid exactly as it was") {
    // The property the whole design rests on: withdrawal is exact, so a unit that
    // moves leaves no residue and a unit that dies takes only its own sight with it.
    IntelGrid grid{Fx::fromInt(512), Fx::fromInt(512), 1};
    std::vector<std::int32_t> a;
    std::vector<std::int32_t> b;

    rm::sim::circleSquares(grid, Fx::fromInt(100), Fx::fromInt(100), Fx::fromInt(64), a);
    rm::sim::circleSquares(grid, Fx::fromInt(130), Fx::fromInt(110), Fx::fromInt(48), b);

    grid.add(a);
    const std::vector<std::uint16_t> before{grid.counts().begin(), grid.counts().end()};

    grid.add(b);
    grid.remove(b);

    const std::vector<std::uint16_t> after{grid.counts().begin(), grid.counts().end()};
    CHECK(before == after);
}
