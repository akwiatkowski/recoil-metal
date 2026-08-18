// Heightfield sampling tests.
//
// heightAt (by grid corner) is exercised throughout the SMF and mesh tests;
// what is pinned here is heightAtWorld, the *interpolating* sampler that
// exists for things that move. The property that matters is continuity: a
// nearest-corner sampler is perfectly accurate at every corner and wrong
// everywhere else, and the error shows up as a moving unit jumping the full
// height difference of a square the instant it crosses the midpoint.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "core/map/HeightField.hpp"

#include <cmath>
#include <cstdint>
#include <vector>

using Catch::Approx;
using rm::HeightField;
using rm::kSquareSize;

namespace {

/// One raw unit = 1 elmo here, so expected heights are the raw values verbatim
/// and a failure reads as arithmetic rather than as a decode.
constexpr float kElmosPerRawUnit = 1.0f;

[[nodiscard]] HeightField makeField(int squaresX, int squaresZ,
                                    std::vector<std::uint16_t> raw) {
    HeightField field;
    field.squaresX = squaresX;
    field.squaresZ = squaresZ;
    field.baseHeight = 0.0f;
    field.heightScale = kElmosPerRawUnit;
    field.raw = std::move(raw);
    return field;
}

/// A 1x1-square field — four corners, one quad, the whole interpolation domain.
///
///   raw layout (row-major, +Z down):  a b
///                                     c d
[[nodiscard]] HeightField oneSquare(std::uint16_t a, std::uint16_t b, std::uint16_t c,
                                    std::uint16_t d) {
    return makeField(1, 1, {a, b, c, d});
}

constexpr float kSquare = static_cast<float>(kSquareSize);

} // namespace

TEST_CASE("heightAtWorld agrees with heightAt at every grid corner") {
    const HeightField field = oneSquare(0, 100, 200, 300);

    // The corners are where the two samplers must not disagree: anything else
    // means the world-to-grid mapping is off by a square or half a square.
    CHECK(field.heightAtWorld(0.0f, 0.0f) == Approx(field.heightAt(0, 0)));
    CHECK(field.heightAtWorld(kSquare, 0.0f) == Approx(field.heightAt(1, 0)));
    CHECK(field.heightAtWorld(0.0f, kSquare) == Approx(field.heightAt(0, 1)));
    CHECK(field.heightAtWorld(kSquare, kSquare) == Approx(field.heightAt(1, 1)));
}

TEST_CASE("heightAtWorld interpolates along each axis") {
    SECTION("along +X") {
        const HeightField field = oneSquare(0, 100, 0, 100);
        CHECK(field.heightAtWorld(kSquare * 0.5f, 0.0f) == Approx(50.0f));
        CHECK(field.heightAtWorld(kSquare * 0.25f, 0.0f) == Approx(25.0f));
    }

    SECTION("along +Z") {
        const HeightField field = oneSquare(0, 0, 100, 100);
        CHECK(field.heightAtWorld(0.0f, kSquare * 0.5f) == Approx(50.0f));
        CHECK(field.heightAtWorld(0.0f, kSquare * 0.75f) == Approx(75.0f));
    }

    SECTION("the centre of a square is the mean of its four corners") {
        const HeightField field = oneSquare(0, 100, 200, 300);
        CHECK(field.heightAtWorld(kSquare * 0.5f, kSquare * 0.5f) == Approx(150.0f));
    }
}

TEST_CASE("heightAtWorld is continuous across a square boundary") {
    // The bug this sampler exists to fix. Two squares whose shared edge is a
    // cliff: nearest-corner sampling steps the full 1000 units at the midpoint
    // of each square, so a unit walking +X teleports vertically twice. An
    // interpolating sampler must move by an amount proportional to the step.
    const HeightField field = makeField(2, 1, {0, 1000, 1000,   // z = 0
                                               0, 1000, 1000}); // z = 1

    constexpr float kStep = 0.01f;  // elmos — far smaller than a square
    const float before = field.heightAtWorld(kSquare - kStep, 0.0f);
    const float after = field.heightAtWorld(kSquare + kStep, 0.0f);

    // Across the boundary at x = 8 the terrain is genuinely flat on the far
    // side, so a step of 1/800th of a square must not move the height by
    // anything like the 1000-unit relief. (A nearest-corner sampler passes this
    // one too — it is the ramp below that separates them.)
    CHECK(std::abs(after - before) < 5.0f);

    // Walking the ramp, every step of 1/8th of a square must climb about an
    // eighth of the height difference. Nearest-corner climbs zero for four
    // steps and then 1000 at once.
    float previous = field.heightAtWorld(0.0f, 0.0f);
    for (int i = 1; i <= 8; ++i) {
        const float x = kSquare * static_cast<float>(i) / 8.0f;
        const float here = field.heightAtWorld(x, 0.0f);
        CHECK(here - previous == Approx(125.0f));
        previous = here;
    }
}

TEST_CASE("heightAtWorld clamps outside the map rather than reading out of bounds") {
    const HeightField field = oneSquare(50, 50, 50, 50);

    // Off every edge, and far off — a unit ordered past the border must get a
    // defined height, not a crash and not a garbage sample.
    CHECK(field.heightAtWorld(-1000.0f, 0.0f) == Approx(50.0f));
    CHECK(field.heightAtWorld(0.0f, -1000.0f) == Approx(50.0f));
    CHECK(field.heightAtWorld(1e6f, 0.0f) == Approx(50.0f));
    CHECK(field.heightAtWorld(0.0f, 1e6f) == Approx(50.0f));
    CHECK(field.heightAtWorld(1e6f, 1e6f) == Approx(50.0f));
}

TEST_CASE("heightAtWorld honours the vertical decode") {
    // baseHeight and heightScale are not the sampler's business, but it must
    // apply them — sampling raw units and forgetting the affine decode renders
    // as a map that is flat-ish and in the wrong place.
    HeightField field = oneSquare(0, 65535, 0, 65535);
    field.setVerticalRange(-100.0f, 100.0f);

    CHECK(field.heightAtWorld(0.0f, 0.0f) == Approx(-100.0f));
    // 65535/65536 of the way up the range, not quite the declared maximum —
    // the quantisation divisor is 2^16 (see HeightField.hpp).
    CHECK(field.heightAtWorld(kSquare, 0.0f) == Approx(99.99694f).margin(1e-3));
    CHECK(field.heightAtWorld(kSquare * 0.5f, 0.0f) == Approx(-0.00153f).margin(1e-3));
}

TEST_CASE("heightAtWorld is safe on a field that holds no samples") {
    const HeightField empty;
    CHECK(empty.heightAtWorld(0.0f, 0.0f) == Approx(0.0f));
    CHECK(empty.heightAtWorld(100.0f, 100.0f) == Approx(0.0f));
}
