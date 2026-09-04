// The ground, sampled in fixed point.
//
// The claim under test is that `sim::Terrain` and `HeightField::heightAtWorld` agree about
// where the ground is — one in fixed point for the sim, one in float for the renderer. If they
// drifted, a unit would stand at a height the terrain mesh does not draw it at, which reads as
// a rendering bug and is not one.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "core/sim/Terrain.hpp"

#include "support/FxMatchers.hpp"

#include <cstdint>

using Catch::Approx;
using rm::sim::Fx;
using rm::sim::Terrain;

namespace {

/// A field with a real vertical scale and a ramp in it, so interpolation has something to do.
[[nodiscard]] rm::HeightField rampField() {
    rm::HeightField field;
    field.squaresX = 16;
    field.squaresZ = 16;
    field.baseHeight = 12.5f;
    field.heightScale = 0.01f;  // the order of magnitude a real .smf states
    field.raw.assign(field.sampleCount(), std::uint16_t{0});
    for (int z = 0; z <= field.squaresZ; ++z) {
        for (int x = 0; x <= field.squaresX; ++x) {
            const auto index = static_cast<std::size_t>(z)
                                   * static_cast<std::size_t>(field.verticesX())
                               + static_cast<std::size_t>(x);
            field.raw[index] = static_cast<std::uint16_t>(x * 800 + z * 300);
        }
    }
    return field;
}

} // namespace

TEST_CASE("a corner height matches the float accessor exactly where it can") {
    const rm::HeightField field = rampField();
    const Terrain terrain{field};

    for (int z = 0; z <= field.squaresZ; ++z) {
        for (int x = 0; x <= field.squaresX; ++x) {
            REQUIRE(rm::test::asFloat(terrain.cornerHeight(x, z))
                    == Approx(field.heightAt(x, z)).margin(rm::test::kFxStep));
        }
    }
}

TEST_CASE("an interpolated height matches the float accessor across the map") {
    // Every eighth of a square, so the fractional part takes every value that matters — the
    // corners, the midpoints, and the places in between where a rounding difference would
    // show up.
    const rm::HeightField field = rampField();
    const Terrain terrain{field};

    double worst = 0.0;
    for (int i = 0; i <= 16 * 8; ++i) {
        for (int j = 0; j <= 16 * 8; ++j) {
            const float x = static_cast<float>(i) * (8.0f / 8.0f);
            const float z = static_cast<float>(j) * (8.0f / 8.0f);
            const double got =
                static_cast<double>(rm::test::asFloat(terrain.heightAt(rm::test::fx(x),
                                                                      rm::test::fx(z))));
            worst = std::max(worst, std::abs(got - static_cast<double>(
                                                       field.heightAtWorld(x, z))));
        }
    }
    // Four steps: the sample position, the two axis interpolations and the decode each round
    // once. Well below the vertical resolution of the source data, which is 0.01 elmos.
    CHECK(worst <= 4.0 * static_cast<double>(rm::test::kFxStep));
}

TEST_CASE("a square is eight elmos, so the sample position is exact") {
    // Small piece of luck worth pinning: 8 is a power of two, so dividing a world coordinate
    // by the square size introduces no rounding at all. On a grid of any other pitch every
    // height would carry an error from this one division.
    const rm::HeightField field = rampField();
    const Terrain terrain{field};

    // A position exactly on a corner must give exactly that corner's height, with no slack.
    for (int c = 0; c <= field.squaresX; ++c) {
        const Fx at = Fx::fromInt(c * rm::kSquareSize);
        REQUIRE(terrain.heightAt(at, Fx{}) == terrain.cornerHeight(c, 0));
    }
}

TEST_CASE("off the map clamps rather than reading past the grid") {
    const rm::HeightField field = rampField();
    const Terrain terrain{field};

    const Fx farPast = Fx::fromInt(100000);
    const Fx farBefore = Fx::fromInt(-100000);

    CHECK(terrain.heightAt(farBefore, farBefore) == terrain.cornerHeight(0, 0));
    CHECK(terrain.heightAt(farPast, farPast)
          == terrain.cornerHeight(field.squaresX, field.squaresZ));

    // And the clamp happens BEFORE the floor, so a coordinate large enough to overflow the
    // cast is not reached — checked by not crashing and by giving the edge answer.
    CHECK(terrain.heightAt(Fx::fromRaw(INT32_MAX), Fx{})
          == terrain.cornerHeight(field.squaresX, 0));
}

TEST_CASE("an empty field is flat at its base height, not a crash") {
    rm::HeightField empty;
    empty.baseHeight = 7.0f;
    const Terrain terrain{empty};
    CHECK(rm::test::asFloat(terrain.heightAt(Fx::fromInt(500), Fx::fromInt(500)))
          == Approx(7.0f).margin(rm::test::kFxStep));
}

TEST_CASE("the look-ahead reports the highest surface in the containing power-of-two cell") {
    // `C-246`: retail indexes a max-height pyramid at the level whose cell is at least
    // half the reach wide, aligned to the grid — so the answer is a neighbourhood maximum,
    // not a directional scan, and below one square of reach it is the point sample.
    rm::HeightField field;
    field.squaresX = 64;
    field.squaresZ = 64;
    field.baseHeight = 0.0f;
    field.heightScale = 1.0f;
    field.raw.assign(field.sampleCount(), std::uint16_t{0});
    // One spike at corner (20, 20): 500 elmos.
    field.raw[static_cast<std::size_t>(20) * static_cast<std::size_t>(field.verticesX()) + 20] = 500;
    const Terrain dry{field};

    // Standing at square (2, 2), 16 elmos in: a reach of 200 elmos is 25 squares, half is
    // 12, whose highest bit is 3, so level 4 and a 16-square cell [0, 16] — the spike at 20
    // is outside it.
    CHECK(dry.maxSurfaceHeightNear(Fx::fromInt(16), Fx::fromInt(16), Fx::fromInt(200)) == Fx{});
    // A reach of 400 elmos: 50 squares, half 25, bit 4, level 5, cell [0, 32] — inside.
    CHECK(dry.maxSurfaceHeightNear(Fx::fromInt(16), Fx::fromInt(16), Fx::fromInt(400))
          == Fx::fromInt(500));
    // Under a square of reach the point sample is what comes back.
    CHECK(dry.maxSurfaceHeightNear(Fx::fromInt(160), Fx::fromInt(160), Fx::fromInt(4))
          == Fx::fromInt(500));
    CHECK(dry.maxSurfaceHeightNear(Fx::fromInt(16), Fx::fromInt(16), Fx::fromInt(4)) == Fx{});

    // From square 62 the level-5 cell is [32, 64], up against the map edge: its far
    // corners are the border itself and the spike at 20 is not in it. A reach past the
    // whole map caps at the level whose cell is the map, and that cell has the spike.
    CHECK(dry.maxSurfaceHeightNear(Fx::fromInt(500), Fx::fromInt(500), Fx::fromInt(400)) == Fx{});
    CHECK(dry.maxSurfaceHeightNear(Fx::fromInt(16), Fx::fromInt(16), Fx::fromInt(100000))
          == Fx::fromInt(500));

    // Water is surface: a drowned cell reports the water level, and so does the point.
    const Terrain wet{field, true, 30.0f};
    CHECK(wet.maxSurfaceHeightNear(Fx::fromInt(16), Fx::fromInt(16), Fx::fromInt(200))
          == Fx::fromInt(30));
    CHECK(wet.surfaceHeightAt(Fx::fromInt(16), Fx::fromInt(16)) == Fx::fromInt(30));
    CHECK(wet.surfaceHeightAt(Fx::fromInt(160), Fx::fromInt(160)) == Fx::fromInt(500));
}
