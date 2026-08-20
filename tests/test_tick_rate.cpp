// The tick rate, and the rule that no duration is ever written in ticks.
//
// PLAN2.md §5.1's mechanism. What is checked here is not arithmetic — it is that the
// conversion is the ONLY way to get a tick count, that it rounds the way the contract says,
// and that an out-of-range rate is refused rather than clamped. The rule itself — that no
// tick-denominated literal survives anywhere — is checked by the multi-rate match test (P2.3)
// and by `tools/check_no_tick_literals.sh`, because a unit test cannot see a constant it does
// not reference.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "core/sim/TickRate.hpp"

#include <stdexcept>
#include <type_traits>

using Catch::Approx;

/// One step of the fixed-point type. Every tolerance below is stated in these, because the
/// conversions quantise by design — 0.2 is not representable in Q18.14, and a test that
/// demanded it exactly would be asserting that quantisation does not happen.
constexpr float kLsb = 1.0f / (1 << rm::kFxFractionalBits);
using rm::sim::seconds;
using rm::sim::TickRate;

TEST_CASE("the permitted range is a hard rule, not a clamp") {
    // A clamp would let a config ask for 100 Hz, silently get 50, and produce a match nobody
    // can explain. Both ends, and both just outside them.
    CHECK_NOTHROW(TickRate{5});
    CHECK_NOTHROW(TickRate{50});
    CHECK_THROWS_AS(TickRate{4}, std::invalid_argument);
    CHECK_THROWS_AS(TickRate{51}, std::invalid_argument);
    CHECK_THROWS_AS(TickRate{0}, std::invalid_argument);

    // And the default is inside it, which is the sort of thing that is obvious until someone
    // changes the default.
    CHECK_NOTHROW(TickRate{});
    CHECK(TickRate{}.ticksPerSecond() == rm::sim::kDefaultTicksPerSecond);
}

TEST_CASE("a duration in seconds becomes the same wall-clock time at any rate") {
    // The property the whole rule exists for: thirty seconds is thirty seconds, whatever the
    // rate. Checked as a duration in SECONDS rather than in ticks, because the number of ticks
    // is supposed to differ — that is the point.
    for (const std::uint32_t hz : {5u, 10u, 20u, 50u}) {
        const TickRate rate{hz};
        const rm::TickCount ticks = rate.ticks(seconds(30.0f));
        const double asSeconds = static_cast<double>(ticks) / static_cast<double>(hz);
        REQUIRE(asSeconds == Approx(30.0).margin(1.0 / static_cast<double>(hz)));
    }
}

TEST_CASE("a positive duration never rounds down to zero ticks") {
    // The one rounding rule that is not a matter of taste. Zero ticks means "every tick",
    // which is not a slightly-fast weapon — it is a different mechanic, and an unbounded one
    // for anything counting down.
    const TickRate rate{10};

    // Forty milliseconds is 0.4 ticks at 10 Hz. Nearest is zero; the floor makes it one.
    CHECK(rate.ticks(seconds(0.04f)) == 1);
    CHECK(rate.ticks(seconds(0.0001f)) == 1);

    // A zero duration is genuinely no ticks, though — the floor applies to positive
    // durations, not to the absence of one.
    CHECK(rate.ticks(seconds(0.0f)) == 0);
    CHECK(rate.ticks(seconds(-1.0f)) == 0);
}

TEST_CASE("durations round to nearest, not down") {
    const TickRate rate{10};
    CHECK(rate.ticks(seconds(1.0f)) == 10);
    CHECK(rate.ticks(seconds(0.14f)) == 1);   // 1.4 ticks
    CHECK(rate.ticks(seconds(0.16f)) == 2);   // 1.6 ticks
    CHECK(rate.ticks(seconds(0.15f)) == 2);   // exactly 1.5: to nearest even is 2 here
    CHECK(rate.ticks(seconds(30.0f)) == 300); // the projectile lifetime, derived
}

TEST_CASE("a per-second rate converts to a per-tick amount once") {
    // The conversion that must happen at content load and nowhere else. Ten elmos a second is
    // one elmo a tick at 10 Hz and a fifth of one at 50 Hz.
    CHECK(rm::sim::fxToFloat(TickRate{10}.perTick(10.0f)) == Approx(1.0f));
    CHECK(rm::sim::fxToFloat(TickRate{50}.perTick(10.0f)) == Approx(0.2f).margin(kLsb));
    CHECK(rm::sim::fxToFloat(TickRate{5}.perTick(10.0f)) == Approx(2.0f));

    // The slowest content speed in the corpus against the fastest rate, which is the case
    // PLAN2 §5.1 flags as the precision worry. A few elmos a second at 50 Hz is ~0.1
    // elmo/tick, four orders of magnitude above the type's resolution.
    const rm::sim::Fx slow = TickRate{50}.perTick(2.0f);
    CHECK(rm::sim::fxToFloat(slow) == Approx(0.04f).margin(0.001));
    CHECK(slow.raw() > 100);  // hundreds of raw units, not a handful
}

TEST_CASE("a magnitude rate converts the same way, at magnitude range") {
    // Income and build rates go through this, and they reach thousands per second.
    CHECK(rm::sim::magToFloat(TickRate{10}.magPerTick(6000.0f)) == Approx(600.0f));
    CHECK(rm::sim::magToFloat(TickRate{10}.magPerTick(2.0f)) == Approx(0.2f).margin(kLsb));
}

TEST_CASE("an angular rate converts to binary radians a tick, and stays signed") {
    // One radian a second at 10 Hz is a tenth of a radian a tick. A full turn is 65,536 brad,
    // so a radian is 65536/(2*pi) = 10,430.4 brad and a tenth of one is 1043.
    const std::int32_t tenth = TickRate{10}.bradPerTick(1.0f);
    CHECK(tenth == 1043);

    // SIGNED, and not a `Brad`. A rate fast enough to wrap a full turn in one tick would be
    // indistinguishable from standing still in the wrapping type; keeping it signed and wide
    // means a clamp against the remaining error still behaves.
    CHECK(TickRate{10}.bradPerTick(-1.0f) == -tenth);
    CHECK(TickRate{5}.bradPerTick(100.0f) > 65536);  // more than a full turn per tick
}

TEST_CASE("seconds per tick is available for display, and is the reciprocal") {
    // The renderer and the HUD clock want wall-clock seconds; the sim must not.
    CHECK(TickRate{10}.secondsPerTick() == Approx(0.1f));
    CHECK(TickRate{50}.secondsPerTick() == Approx(0.02f));
}

TEST_CASE("a duration cannot be confused with a tick count") {
    // Not a runtime check — a compile-time one, stated as a test so the intent is recorded.
    // `Seconds` is a struct with an explicit constructor, so `rate.ticks(300)` does not
    // compile: the only way in is `seconds(30.0f)`, which says what it means.
    static_assert(!std::is_convertible_v<float, rm::sim::Seconds>,
                  "a bare float must not become a duration implicitly");
    static_assert(!std::is_convertible_v<rm::sim::Seconds, float>,
                  "a duration must not decay to a bare float implicitly");
    static_assert(!std::is_convertible_v<int, rm::sim::Seconds>,
                  "a tick count must not become a duration implicitly");
    SUCCEED();
}
