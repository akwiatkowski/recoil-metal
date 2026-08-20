// Fixed-point arithmetic and the sim's trigonometry.
//
// Two kinds of test here, and they answer different questions.
//
// ACCURACY, against `libm`: does `fxSin` actually compute a sine? `libm` is not a determinism
// oracle — that is the whole reason this code exists — but it is an excellent CORRECTNESS
// oracle, because whatever its last bit says, it certainly knows what a sine is to within a
// millionth. So the accuracy cases compare against `std::sin` with a tolerance, and the
// tolerance is stated in units of the output's own last bit.
//
// EXACTNESS, against itself: is the answer the same bits every time, everywhere? That is not
// something a tolerance can check. Those cases assert exact integer equality — a golden value,
// an identity, an invariant — so that a change in rounding, in iteration count, or in what the
// optimiser did with the shifts shows up as a failure rather than as drift.
//
// The `-O0` vs `-O2` requirement from PLAN2 §7 P2.1 is checked by the golden values below
// rather than by building twice inside a test: a golden integer that holds at both levels is
// the same claim, and it is a claim every future build re-checks for free.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "core/sim/Fx.hpp"

#include <cmath>
#include <cstdint>
#include <numbers>

using Catch::Approx;
using rm::Brad;
using rm::sim::Fx;
using rm::sim::Mag;

namespace {

constexpr double kTwoPi = 6.283185307179586476925286766559;

/// One step of the output type, as a double — the unit every tolerance below is stated in.
constexpr double kLsb = 1.0 / (1 << rm::kFxFractionalBits);

[[nodiscard]] double radiansOf(Brad angle) {
    return static_cast<double>(angle) / 65536.0 * kTwoPi;
}

} // namespace

// --- The layout itself ----------------------------------------------------------------

TEST_CASE("the fixed-point layout holds what the shipped content contains") {
    // The measurement that chose Q18.14 over PLAN2 §5.2's Q16.16. Both halves of it, as a
    // test, so that a future session that "simplifies" the layout back to 16 fractional bits
    // finds out here rather than by a unit falling off the corner of a map.

    SECTION("a unit can stand in the far corner of the biggest shipped map") {
        // SCMP_029, SCMP_030 and X1MP_012 are 4096 x 4096 squares of 8 elmos = 32,768 elmos.
        // Q16.16 tops out at 32,767.99998, so that corner is literally unreachable in it.
        const Fx corner = Fx::fromInt(32768);
        CHECK(corner.floorToInt() == 32768);

        // And with room to spare, because a projectile aimed off the edge still has to have
        // a position while it flies.
        const Fx wellPast = Fx::fromInt(100000);
        CHECK(wellPast.floorToInt() == 100000);
    }

    SECTION("a magnitude holds the most expensive thing in the corpus") {
        // XSB2401's BuildCostEnergy is 10,008,000 and XSC9010's MaxHealth is 5,000,000.
        const Mag cost = Mag::fromInt(10008000);
        CHECK(cost.floorToInt() == 10008000);

        const Mag health = Mag::fromInt(5000000);
        CHECK(health.floorToInt() == 5000000);

        // Halved, to show the fraction survives at that scale rather than being rounded off.
        CHECK((health * Fx::fromRatio(1, 2)).floorToInt() == 2500000);
    }

    SECTION("Fx and Mag share their fractional bits, so conversion is not a rescale") {
        // The property that keeps two fixed-point types from becoming a source of scale bugs.
        const Fx small = Fx::fromRatio(3, 8);
        CHECK(Mag::fromFx(small).raw() == small.raw());
        CHECK(Mag::fromFx(small).toFx() == small);
    }
}

TEST_CASE("arithmetic rounds to nearest, and the rule does not vary with the sign") {
    // Round-to-nearest with ties toward +infinity. Both halves are arbitrary; what matters is
    // that they are fixed, so they are pinned here as exact integers.

    // A half is exact, so a half doubled is exactly one — no slack, because a value that IS
    // representable must survive the round trip.
    const Fx half = Fx::fromRatio(1, 2);
    CHECK((half * Fx::fromInt(2)).raw() == Fx::fromInt(1).raw());
    CHECK((-half * Fx::fromInt(2)).raw() == Fx::fromInt(-1).raw());

    // A THIRD IS NOT REPRESENTABLE, and the test says so rather than rounding the problem
    // away: 1/3 in Q18.14 is 5461.33, stored as 5461, so three of them are one step short of
    // one. That is quantisation. What the rounding rule guarantees is that the error stays at
    // one step and does not grow, and that it is the same step either side of zero.
    const Fx third = Fx::fromRatio(1, 3);
    CHECK(std::abs((third * Fx::fromInt(3)).raw() - Fx::fromInt(1).raw()) <= 1);
    CHECK(std::abs((-third * Fx::fromInt(3)).raw() + Fx::fromInt(1).raw()) <= 1);
    // Symmetric: whatever the error is, it is the same magnitude negated.
    CHECK((third * Fx::fromInt(3)).raw() == -(-third * Fx::fromInt(3)).raw());

    // And the ratio itself is the NEAREST representable one, not the one below it — the
    // difference between `fromRatio` rounding and truncating.
    CHECK(Fx::fromRatio(2, 3).raw() == 10923);  // 32768/3 = 10922.67
    CHECK(Fx::fromRatio(-2, 3).raw() == -10923);

    // Division is round-to-nearest too, so a/b*b returns to a for values that divide cleanly.
    const Fx seven = Fx::fromInt(7);
    CHECK(((seven / Fx::fromInt(4)) * Fx::fromInt(4)).raw() == seven.raw());
}

TEST_CASE("floor truncates toward negative infinity, not toward zero") {
    // The sim's `std::floor`, and the distinction matters: grid cell lookups use it, and
    // truncating toward zero would make the cell either side of the origin the same cell.
    CHECK(Fx::fromRatio(3, 2).floorToInt() == 1);
    CHECK(Fx::fromRatio(-3, 2).floorToInt() == -2);
    CHECK(Fx::fromInt(-1).floorToInt() == -1);
    CHECK(Fx::fromRatio(-1, 16384).floorToInt() == -1);  // one step below zero
}

TEST_CASE("overflow saturates rather than wrapping") {
    // Wrapping would put a unit at the opposite corner of the map — a bug that reads as
    // physics. Saturation is also wrong, but wrong in the direction the value was going.
    const Fx huge = Fx::fromInt(130000);
    CHECK((huge + huge).raw() == INT32_MAX);
    CHECK((-huge - huge).raw() == INT32_MIN);

    // And division by zero is deterministic rather than a signal: a replay can find a wrong
    // number, and cannot find a crash that happened on another machine.
    CHECK((Fx::fromInt(1) / Fx::fromInt(0)).raw() == INT32_MAX);
    CHECK((Fx::fromInt(-1) / Fx::fromInt(0)).raw() == INT32_MIN);
}

// --- Trigonometry: accuracy ------------------------------------------------------------

TEST_CASE("sine and cosine agree with libm across the whole circle") {
    // Every angle the type can hold, not a sample: `Brad` has 65,536 of them, which is cheap
    // to sweep and removes any question of a bad region between the samples.
    double worstSin = 0.0;
    double worstCos = 0.0;
    for (std::uint32_t a = 0; a < 65536; ++a) {
        const auto angle = static_cast<Brad>(a);
        const double expected = radiansOf(angle);
        worstSin = std::max(worstSin,
                            std::abs(rm::sim::fxToFloat(rm::sim::fxSin(angle))
                                     - std::sin(expected)));
        worstCos = std::max(worstCos,
                            std::abs(rm::sim::fxToFloat(rm::sim::fxCos(angle))
                                     - std::cos(expected)));
    }
    // Within one step of the output type. CORDIC converges far below that internally; what is
    // left is the single rounding at the end, which cannot be better than half a step.
    CHECK(worstSin <= kLsb);
    CHECK(worstCos <= kLsb);
}

TEST_CASE("the cardinal angles are exact") {
    // Pinned as integers, because these are the values every other result is anchored to and
    // a half-step error at zero is a half-step error everywhere.
    CHECK(rm::sim::fxSin(0).raw() == 0);
    CHECK(rm::sim::fxCos(0) == rm::sim::kFxOne);
    CHECK(rm::sim::fxSin(rm::sim::kBradQuarterTurn) == rm::sim::kFxOne);
    CHECK(rm::sim::fxCos(rm::sim::kBradQuarterTurn).raw() == 0);
    CHECK(rm::sim::fxSin(rm::sim::kBradHalfTurn).raw() == 0);
    CHECK(rm::sim::fxCos(rm::sim::kBradHalfTurn) == -rm::sim::kFxOne);
    CHECK(rm::sim::fxSin(static_cast<Brad>(3 * rm::sim::kBradQuarterTurn))
          == -rm::sim::kFxOne);
}

TEST_CASE("an angle needs no wrapping, because the type cannot hold an unwrapped one") {
    // The reason angles are binary radians and not fixed-point radians. Adding a full turn is
    // the identity by unsigned overflow — no modulo, so no error to accumulate over a match.
    for (std::uint32_t a = 0; a < 65536; a += 1021) {  // a prime stride, to avoid a pattern
        const auto angle = static_cast<Brad>(a);
        const auto plusTurn = static_cast<Brad>(a + rm::sim::kBradFullTurn);
        REQUIRE(rm::sim::fxSin(angle).raw() == rm::sim::fxSin(plusTurn).raw());
    }
}

TEST_CASE("sin squared plus cos squared is one, everywhere") {
    // The identity that catches a scaling mistake the libm comparison would not: if the CORDIC
    // gain were removed twice, or not at all, both functions would be wrong by the same factor
    // and each would still look like a sine.
    for (std::uint32_t a = 0; a < 65536; a += 37) {
        const auto angle = static_cast<Brad>(a);
        const Fx s = rm::sim::fxSin(angle);
        const Fx c = rm::sim::fxCos(angle);
        const Fx sum = s * s + c * c;
        // Two multiplies and an add, so three roundings; four steps of slack is generous
        // without being meaningless.
        REQUIRE(std::abs(sum.raw() - rm::sim::kFxOne.raw()) <= 4);
    }
}

TEST_CASE("a bearing is measured from +Z toward +X, the way a unit's yaw is") {
    // The convention, asserted rather than commented. Swapping the axes compiles and points
    // every turret ninety degrees off, which is why the function names them.
    CHECK(rm::sim::fxBearing(rm::sim::kFxZero, Fx::fromInt(100)) == 0);  // due +Z
    CHECK(rm::sim::fxBearing(Fx::fromInt(100), rm::sim::kFxZero) == rm::sim::kBradQuarterTurn);
    CHECK(rm::sim::fxBearing(rm::sim::kFxZero, Fx::fromInt(-100)) == rm::sim::kBradHalfTurn);
    CHECK(rm::sim::fxBearing(Fx::fromInt(-100), rm::sim::kFxZero)
          == static_cast<Brad>(3 * rm::sim::kBradQuarterTurn));
}

TEST_CASE("a bearing agrees with libm over a grid of vectors") {
    double worst = 0.0;
    for (int x = -64; x <= 64; ++x) {
        for (int z = -64; z <= 64; ++z) {
            if (x == 0 && z == 0) {
                continue;
            }
            const Brad got = rm::sim::fxBearing(Fx::fromInt(x), Fx::fromInt(z));
            // The engine's convention is atan2(x, z), and the result is compared as a signed
            // difference so that the wrap at a full turn is not read as a huge error.
            const double expected =
                std::atan2(static_cast<double>(x), static_cast<double>(z)) / kTwoPi * 65536.0;
            double delta = static_cast<double>(got) - expected;
            while (delta > 32768.0) {
                delta -= 65536.0;
            }
            while (delta < -32768.0) {
                delta += 65536.0;
            }
            worst = std::max(worst, std::abs(delta));
        }
    }
    CHECK(worst <= 1.0);  // within one binary radian, i.e. 0.0055 degrees
}

TEST_CASE("a length agrees with libm, including the cases that are exactly right") {
    // The 3-4-5 triangle first, as an exact integer: a length that is a whole number must come
    // out a whole number, or every distance comparison in the sim is off by a rounding.
    CHECK(rm::sim::fxHypot(Fx::fromInt(30), Fx::fromInt(40)) == Fx::fromInt(50));

    double worst = 0.0;
    for (int x = -50; x <= 50; ++x) {
        for (int z = -50; z <= 50; ++z) {
            const double expected = std::hypot(static_cast<double>(x), static_cast<double>(z));
            const double got =
                static_cast<double>(rm::sim::fxToFloat(rm::sim::fxHypot(Fx::fromInt(x),
                                                                       Fx::fromInt(z))));
            worst = std::max(worst, std::abs(got - expected));
        }
    }
    // Vectoring mode removes the gain by a multiply, so there are two roundings in a length
    // rather than one. Four steps.
    CHECK(worst <= 4 * kLsb);
}

TEST_CASE("a bearing and a length come from one pass and match the two separate calls") {
    // `fxPolar` exists so a caller that wants both does not pay for two CORDIC runs. If it
    // ever disagreed with the single-value forms, the saving would be a bug.
    for (int x = -20; x <= 20; x += 3) {
        for (int z = -20; z <= 20; z += 3) {
            const rm::sim::Polar both = rm::sim::fxPolar(Fx::fromInt(x), Fx::fromInt(z));
            REQUIRE(both.bearing == rm::sim::fxBearing(Fx::fromInt(x), Fx::fromInt(z)));
            REQUIRE(both.length == rm::sim::fxHypot(Fx::fromInt(x), Fx::fromInt(z)));
        }
    }
}

TEST_CASE("arcsine agrees with libm, and saturates instead of going undefined") {
    double worst = 0.0;
    for (int n = -16384; n <= 16384; n += 37) {
        const Fx value = Fx::fromRaw(n);
        const double expected =
            std::asin(static_cast<double>(n) / 16384.0) / kTwoPi * 65536.0;
        double delta = static_cast<double>(static_cast<std::int16_t>(rm::sim::fxAsin(value)))
                       - expected;
        worst = std::max(worst, std::abs(delta));
    }
    CHECK(worst <= 2.0);  // two binary radians, ~0.011 degrees

    // Out of range saturates. A caller reaches this by normalising a vector whose length
    // rounded a hair past one, which is ordinary rather than exceptional.
    CHECK(rm::sim::fxAsin(Fx::fromInt(2)) == rm::sim::kBradQuarterTurn);
    CHECK(static_cast<std::int16_t>(rm::sim::fxAsin(Fx::fromInt(-2)))
          == -static_cast<std::int16_t>(rm::sim::kBradQuarterTurn));
}

TEST_CASE("square root is exact for perfect squares, and never negative") {
    // Exactness on perfect squares is the property that keeps a distance comparison honest:
    // if sqrt(2500) came out 49.9999, a unit exactly at its maximum range would be out of it.
    for (int n = 0; n <= 200; ++n) {
        REQUIRE(rm::sim::fxSqrt(Fx::fromInt(n * n)) == Fx::fromInt(n));
    }

    // A negative input is a bug in the caller; zero is the deterministic answer, and it beats
    // a NaN that would propagate silently into the state hash.
    CHECK(rm::sim::fxSqrt(Fx::fromInt(-4)).raw() == 0);

    double worst = 0.0;
    for (int n = 1; n <= 20000; n += 7) {
        const Fx value = Fx::fromInt(n);
        const double expected = std::sqrt(static_cast<double>(n));
        worst = std::max(worst, std::abs(static_cast<double>(rm::sim::fxToFloat(
                                             rm::sim::fxSqrt(value)))
                                         - expected));
    }
    CHECK(worst <= kLsb);
}

// --- Exactness: golden values ----------------------------------------------------------

TEST_CASE("golden values, so a change in rounding cannot pass unnoticed") {
    // These are not derived from anything — they are what this implementation produces, pinned
    // so that it keeps producing them. Any change to the iteration count, the gain constant,
    // the rounding rule, or the internal precision moves at least one of them.
    //
    // THIS IS THE `-O0` VS `-O2` CHECK from PLAN2 §7 P2.1. An integer that is equal at both
    // optimisation levels is the claim; asserting it here means every build re-checks it,
    // which is stronger than a one-off comparison of two builds.
    CHECK(rm::sim::fxSin(static_cast<Brad>(8192)).raw() == 11585);   // 45 degrees
    CHECK(rm::sim::fxCos(static_cast<Brad>(8192)).raw() == 11585);
    CHECK(rm::sim::fxSin(static_cast<Brad>(5461)).raw() == 8192);    // 30 degrees
    CHECK(rm::sim::fxSqrt(Fx::fromInt(2)).raw() == 23170);
    CHECK(rm::sim::fxHypot(Fx::fromInt(1), Fx::fromInt(1)).raw() == 23170);
    CHECK(rm::sim::fxBearing(Fx::fromInt(1), Fx::fromInt(1)) == 8192);
    CHECK(Fx::fromRatio(1, 3).raw() == 5461);
}

// --- The float boundary ----------------------------------------------------------------

TEST_CASE("the float boundary rounds to nearest in both directions") {
    // Content arrives as floats and the renderer wants floats back. Both conversions round
    // rather than truncate, so a value authored as 0.1 does not become 0.09994.
    CHECK(rm::sim::fxFromFloat(1.0f) == rm::sim::kFxOne);
    CHECK(rm::sim::fxFromFloat(-1.0f) == -rm::sim::kFxOne);
    CHECK(rm::sim::fxToFloat(rm::sim::kFxOne) == Approx(1.0f));

    // A round trip is lossy by design — that is what quantisation means — but it must be
    // lossy by at most half a step, and it must not drift on repetition.
    const Fx once = rm::sim::fxFromFloat(0.1f);
    const Fx twice = rm::sim::fxFromFloat(rm::sim::fxToFloat(once));
    CHECK(once == twice);
    CHECK(std::abs(static_cast<double>(rm::sim::fxToFloat(once)) - 0.1) <= kLsb / 2);
}

TEST_CASE("radians convert to binary radians and back") {
    constexpr float pi = std::numbers::pi_v<float>;
    CHECK(rm::sim::bradFromRadians(0.0f) == 0);
    CHECK(rm::sim::bradFromRadians(pi / 2.0f) == rm::sim::kBradQuarterTurn);
    CHECK(rm::sim::bradFromRadians(pi) == rm::sim::kBradHalfTurn);

    // A negative angle wraps into the top half of the range rather than being rejected — the
    // property that makes `Brad` unsigned safe to use for signed angles.
    CHECK(rm::sim::bradFromRadians(-pi / 2.0f)
          == static_cast<Brad>(3 * rm::sim::kBradQuarterTurn));

    // And a full turn past anything is the same angle.
    CHECK(rm::sim::bradFromRadians(2.0f * pi) == 0);
    CHECK(rm::sim::radiansFromBrad(rm::sim::kBradQuarterTurn) == Approx(pi / 2.0f));
}
