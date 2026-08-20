// Movement sim tests. The whole point of keeping the sim in core/ is that
// "does a unit walk to where it was told" is arithmetic, not something to
// judge by watching a screen.
//
// Everything here runs the fixed tick directly. Wall-clock pacing is the
// TickClock's job and is tested separately at the bottom.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "core/map/HeightField.hpp"
#include "core/scene/UnitPlacement.hpp"
#include "core/sim/Combat.hpp"  // headingError, for the angle assertions
#include "core/sim/Movement.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <vector>

#include "support/FxMatchers.hpp"

using Catch::Approx;
using rm::HeightField;
using rm::UnitInstance;
using rm::sim::MoveState;
using rm::sim::TickClock;

namespace {

constexpr float kPi = std::numbers::pi_v<float>;

/// A flat field 800 elmos square (100 squares), all at y = 0. Flat so that a
/// test about *movement* is not also a test about terrain sampling — the two
/// that care about height build their own.
[[nodiscard]] HeightField flatField() {
    HeightField field;
    field.squaresX = 100;
    field.squaresZ = 100;
    field.baseHeight = 0.0f;
    field.heightScale = 1.0f;
    field.raw.assign(field.sampleCount(), std::uint16_t{0});
    return field;
}

/// A field that ramps from y = 0 to y = 1000 along +X, so a unit walking it
/// must climb.
[[nodiscard]] HeightField rampField() {
    HeightField field;
    field.squaresX = 100;
    field.squaresZ = 100;
    field.baseHeight = 0.0f;
    field.heightScale = 1.0f;
    field.raw.resize(field.sampleCount());
    for (int z = 0; z < field.verticesZ(); ++z) {
        for (int x = 0; x < field.verticesX(); ++x) {
            const auto index = static_cast<std::size_t>(z) * static_cast<std::size_t>(field.verticesX())
                             + static_cast<std::size_t>(x);
            field.raw[index] = static_cast<std::uint16_t>(x * 10);
        }
    }
    return field;
}

/// The clock these cases are written against. A member of the file rather than a default,
/// because motion is per-tick now and a test that mixed two rates would be checking nothing.
const rm::sim::TickRate kRate{10};

/// One unit at a position, facing +Z (yaw 0). Authored in decimals; stored in fixed point.
[[nodiscard]] rm::sim::Transform unitAt(float x, float z, float yaw = 0.0f) {
    return rm::sim::Transform{.x = rm::test::fx(x),
                              .z = rm::test::fx(z),
                              .heading = rm::sim::bradFromRadians(yaw)};
}

/// A `MoveState` with the authored default speed and turn rate for this clock. What
/// `MoveState{}` used to give for free, before per-tick rates made a bare default meaningless.
[[nodiscard]] MoveState ordinary() { return rm::sim::defaultMotion(kRate); }

/// Runs `count` fixed ticks over a one-unit world.
void run(std::vector<rm::sim::Transform>& instances, std::vector<MoveState>& motion,
         const HeightField& field, int count) {
    for (int i = 0; i < count; ++i) {
        rm::sim::tick(instances, motion, rm::sim::Terrain{field});
    }
}

/// Distance between a unit and its destination, on the ground plane.
[[nodiscard]] rm::sim::Fx distanceToOrder(const rm::sim::Transform& instance,
                                          const MoveState& state) {
    return rm::sim::fxHypot(state.destinationX - instance.x, state.destinationZ - instance.z);
}

} // namespace

TEST_CASE("a unit with no order does not move") {
    const HeightField field = flatField();
    std::vector<rm::sim::Transform> instances{unitAt(100.0f, 100.0f, 0.7f)};
    std::vector<MoveState> motion{ordinary()};

    run(instances, motion, field, 60);

    CHECK(rm::test::asFloat(instances[0].x) == Approx(100.0f));
    CHECK(rm::test::asFloat(instances[0].z) == Approx(100.0f));
    // Yaw too: an idle unit that slowly rotates is a bug that is easy to miss.
    CHECK(instances[0].heading == rm::sim::bradFromRadians(0.7f));
}

TEST_CASE("an ordered unit closes on its destination") {
    const HeightField field = flatField();
    std::vector<rm::sim::Transform> instances{unitAt(100.0f, 100.0f)};
    std::vector<MoveState> motion{ordinary()};
    rm::sim::orderTo(motion[0], rm::sim::Terrain{field}, rm::test::fx(100.0f),
                     rm::test::fx(400.0f));

    const rm::sim::Fx before = distanceToOrder(instances[0], motion[0]);
    // A whole second, expressed as one, so the expectation below does not have to
    // know the tick rate — an earlier version ran a flat 10 ticks and called it a
    // third of a second, which stopped being true at 10 Hz.
    run(instances, motion, field, static_cast<int>(kRate.ticksPerSecond()));
    const rm::sim::Fx after = distanceToOrder(instances[0], motion[0]);

    CHECK(after < before);
    // Already facing +Z, so it travels at very nearly full speed from the first
    // tick: one second of it.
    CHECK(rm::test::asFloat(before - after)
          == Approx(rm::sim::kDefaultSpeedElmosPerSecond).margin(1.0f));
}

TEST_CASE("a unit arrives and then stops") {
    const HeightField field = flatField();
    std::vector<rm::sim::Transform> instances{unitAt(100.0f, 100.0f)};
    std::vector<MoveState> motion{ordinary()};
    rm::sim::orderTo(motion[0], rm::sim::Terrain{field}, rm::test::fx(100.0f),
                     rm::test::fx(300.0f));

    // 200 elmos at 87 elmos/s is ~2.3 seconds, so four seconds is ample.
    run(instances, motion, field, 4 * static_cast<int>(kRate.ticksPerSecond()));

    CHECK_FALSE(motion[0].moving);
    CHECK(distanceToOrder(instances[0], motion[0]) < rm::sim::arrivalRadius(kRate));

    // And it stays stopped rather than creeping or oscillating around the goal.
    const rm::sim::Transform resting = instances[0];
    run(instances, motion, field, 60);
    CHECK(rm::test::asFloat(instances[0].x) == Approx(rm::test::asFloat(resting.x)));
    CHECK(rm::test::asFloat(instances[0].z) == Approx(rm::test::asFloat(resting.z)));
}

TEST_CASE("a unit does not overshoot a destination closer than one tick of travel") {
    const HeightField field = flatField();
    std::vector<rm::sim::Transform> instances{unitAt(100.0f, 100.0f)};
    std::vector<MoveState> motion{ordinary()};

    // Well inside one tick's travel (87/30 = 2.9 elmos), and inside the arrival
    // radius, so this must resolve immediately rather than stepping past.
    rm::sim::orderTo(motion[0], rm::sim::Terrain{field}, rm::test::fx(100.0f),
                     rm::test::fx(101.0f));
    run(instances, motion, field, 1);

    CHECK(rm::test::asFloat(instances[0].z) <= Approx(101.0f));
    CHECK_FALSE(motion[0].moving);
}

TEST_CASE("a unit turns to face where it is going") {
    const HeightField field = flatField();
    std::vector<rm::sim::Transform> instances{unitAt(100.0f, 100.0f)};
    std::vector<MoveState> motion{ordinary()};

    // Due +X. The shader maps local +Z to (sin yaw, cos yaw), so facing +X is
    // yaw = pi/2 — get the convention wrong and every unit walks sideways.
    rm::sim::orderTo(motion[0], rm::sim::Terrain{field}, rm::test::fx(400.0f),
                     rm::test::fx(100.0f));
    run(instances, motion, field, static_cast<int>(kRate.ticksPerSecond()));

    CHECK(rm::sim::headingError(instances[0].heading, rm::sim::kBradQuarterTurn)
          < rm::sim::bradFromRadians(0.05f));
}

TEST_CASE("a unit turns no faster than its turn rate") {
    const HeightField field = flatField();
    std::vector<rm::sim::Transform> instances{unitAt(100.0f, 100.0f)};
    std::vector<MoveState> motion{ordinary()};
    rm::sim::orderTo(motion[0], rm::sim::Terrain{field}, rm::test::fx(400.0f),
                     rm::test::fx(100.0f));

    const rm::Brad before = instances[0].heading;
    run(instances, motion, field, 1);
    // In binary radians, and the comparison is EXACT rather than within a percent: the turn
    // per tick is an integer number of brad, and the pass may not exceed it by one.
    const std::uint32_t delta = rm::sim::headingError(before, instances[0].heading);

    // One tick may not snap the unit round — that is what separates a vehicle
    // from a turret.
    CHECK(delta <= static_cast<std::uint32_t>(motion[0].turnPerTick));
    CHECK(delta > 0);
}

TEST_CASE("a unit ordered behind itself turns before it travels") {
    const HeightField field = flatField();
    // Facing +Z, ordered due -Z: the destination is directly behind.
    std::vector<rm::sim::Transform> instances{unitAt(400.0f, 400.0f, 0.0f)};
    std::vector<MoveState> motion{ordinary()};
    rm::sim::orderTo(motion[0], rm::sim::Terrain{field}, rm::test::fx(400.0f),
                     rm::test::fx(100.0f));

    // Forward speed scales with how well the unit is aligned, so for the first
    // few ticks it should pivot almost in place rather than drive off the wrong
    // way and arc back.
    run(instances, motion, field, 3);
    CHECK(std::abs(rm::test::asFloat(instances[0].z) - 400.0f) < 2.0f);

    // Given time it still gets there.
    run(instances, motion, field, 6 * static_cast<int>(kRate.ticksPerSecond()));
    CHECK_FALSE(motion[0].moving);
}

TEST_CASE("a moving unit follows the terrain") {
    const HeightField field = rampField();
    std::vector<rm::sim::Transform> instances{unitAt(0.0f, 400.0f)};
    std::vector<MoveState> motion{ordinary()};
    rm::sim::orderTo(motion[0], rm::sim::Terrain{field}, rm::test::fx(700.0f),
                     rm::test::fx(400.0f));

    // Every tick, not just at the end: the Y must track the ground continuously
    // or the unit submarines through hills between waypoints.
    for (int i = 0; i < 4 * static_cast<int>(kRate.ticksPerSecond()); ++i) {
        rm::sim::tick(instances, motion, rm::sim::Terrain{field});
        const rm::sim::Transform& unit = instances[0];
        // Against the FLOAT accessor, which is the point: the sim's height and the
        // renderer's must agree, within the fixed-point step. That equivalence is what
        // `sim::Terrain`'s own tests establish; this checks the movement pass uses it.
        REQUIRE(rm::test::asFloat(unit.y)
                == Approx(field.heightAtWorld(rm::test::asFloat(unit.x),
                                              rm::test::asFloat(unit.z)))
                       // Eight steps: the terrain sample rounds, the two axis interpolations
                       // round, and the vertical decode rounds — against a float accessor
                       // that does none of that. Still four orders of magnitude below the
                       // 0.01-elmo vertical resolution of a real heightmap.
                       .margin(8 * rm::test::kFxStep));
    }

    // And it did actually climb, so the assertion above was not vacuous.
    CHECK(rm::test::asFloat(instances[0].y) > 100.0f);
}

TEST_CASE("an order outside the map is clamped onto it") {
    const HeightField field = flatField();
    std::vector<rm::sim::Transform> instances{unitAt(400.0f, 400.0f)};
    std::vector<MoveState> motion{ordinary()};

    rm::sim::orderTo(motion[0], rm::sim::Terrain{field}, rm::test::fx(99999.0f),
                     rm::test::fx(-500.0f));

    CHECK(rm::test::asFloat(motion[0].destinationX) == Approx(field.widthElmos()));
    CHECK(rm::test::asFloat(motion[0].destinationZ) == Approx(0.0f));

    // And it is reachable, so the unit stops rather than pressing at the border.
    run(instances, motion, field, 20 * static_cast<int>(kRate.ticksPerSecond()));
    CHECK_FALSE(motion[0].moving);
}

TEST_CASE("a unit never leaves the map") {
    const HeightField field = flatField();
    std::vector<rm::sim::Transform> instances{unitAt(10.0f, 10.0f)};
    std::vector<MoveState> motion{ordinary()};
    rm::sim::orderTo(motion[0], rm::sim::Terrain{field}, rm::test::fx(0.0f),
                     rm::test::fx(0.0f));

    run(instances, motion, field, 5 * static_cast<int>(kRate.ticksPerSecond()));

    CHECK(rm::test::asFloat(instances[0].x) >= 0.0f);
    CHECK(rm::test::asFloat(instances[0].z) >= 0.0f);
    CHECK(rm::test::asFloat(instances[0].x) <= field.widthElmos());
    CHECK(rm::test::asFloat(instances[0].z) <= field.depthElmos());
}

TEST_CASE("stop cancels an order in place") {
    const HeightField field = flatField();
    std::vector<rm::sim::Transform> instances{unitAt(100.0f, 100.0f)};
    std::vector<MoveState> motion{ordinary()};
    rm::sim::orderTo(motion[0], rm::sim::Terrain{field}, rm::test::fx(100.0f),
                     rm::test::fx(700.0f));
    run(instances, motion, field, 10);

    const rm::sim::Transform halfway = instances[0];
    motion[0].moving = false;
    run(instances, motion, field, 60);

    CHECK(rm::test::asFloat(instances[0].z) == Approx(rm::test::asFloat(halfway.z)));
}

TEST_CASE("the sim moves every ordered unit, and only those") {
    const HeightField field = flatField();
    std::vector<rm::sim::Transform> instances{
        unitAt(100.0f, 100.0f),
        unitAt(200.0f, 100.0f),
        unitAt(300.0f, 100.0f),
    };
    std::vector<MoveState> motion(instances.size(), ordinary());
    rm::sim::orderTo(motion[0], rm::sim::Terrain{field}, rm::test::fx(100.0f),
                     rm::test::fx(500.0f));
    rm::sim::orderTo(motion[2], rm::sim::Terrain{field}, rm::test::fx(300.0f),
                     rm::test::fx(500.0f));

    run(instances, motion, field, 30);

    CHECK(rm::test::asFloat(instances[0].z) > 100.0f);
    CHECK(rm::test::asFloat(instances[1].z) == Approx(100.0f));
    CHECK(rm::test::asFloat(instances[2].z) > 100.0f);
}

TEST_CASE("the sim is safe when the two spans disagree in length") {
    // A defensive check rather than a designed case: the caller is expected to
    // keep them parallel, but reading past the shorter one would be a crash in
    // the frame loop rather than a visible bug.
    const HeightField field = flatField();
    std::vector<rm::sim::Transform> instances{unitAt(100.0f, 100.0f), unitAt(200.0f, 100.0f)};
    std::vector<MoveState> motion(1, ordinary());
    rm::sim::orderTo(motion[0], rm::sim::Terrain{field}, rm::test::fx(100.0f),
                     rm::test::fx(500.0f));

    REQUIRE_NOTHROW(run(instances, motion, field, 10));
    CHECK(rm::test::asFloat(instances[0].z) > 100.0f);
    CHECK(rm::test::asFloat(instances[1].z) == Approx(100.0f));
}

TEST_CASE("the sim is deterministic") {
    const HeightField field = rampField();
    const auto play = [&field]() {
        std::vector<rm::sim::Transform> instances{unitAt(50.0f, 400.0f), unitAt(600.0f, 200.0f, 2.0f)};
        std::vector<MoveState> motion(instances.size(), ordinary());
        rm::sim::orderTo(motion[0], rm::sim::Terrain{field}, rm::test::fx(700.0f),
                     rm::test::fx(700.0f));
        rm::sim::orderTo(motion[1], rm::sim::Terrain{field}, rm::test::fx(100.0f),
                     rm::test::fx(100.0f));
        run(instances, motion, field, 90);
        return instances;
    };

    const std::vector<rm::sim::Transform> first = play();
    const std::vector<rm::sim::Transform> second = play();

    REQUIRE(first.size() == second.size());
    for (std::size_t i = 0; i < first.size(); ++i) {
        // Bit-exact, not approximate: the same ticks over the same state must
        // produce the same floats, or a benchmark and a screenshot stop being
        // reproducible.
        CHECK(first[i].x == second[i].x);
        CHECK(first[i].y == second[i].y);
        CHECK(first[i].z == second[i].z);
        CHECK(first[i].heading == second[i].heading);
    }
}

TEST_CASE("TickClock hands out whole ticks and carries the remainder") {
    TickClock clock;

    // Exactly one tick's worth.
    CHECK(clock.advance(kRate.secondsPerTick()) == 1);

    // Half a tick twice is one tick, not zero and not two.
    CHECK(clock.advance(kRate.secondsPerTick() * 0.5f) == 0);
    CHECK(clock.advance(kRate.secondsPerTick() * 0.5f) == 1);

    // A whole second is the tick rate, however it is chopped up.
    TickClock other;
    int total = 0;
    for (int i = 0; i < 120; ++i) {
        total += other.advance(1.0f / 120.0f);
    }
    CHECK(total == static_cast<int>(kRate.ticksPerSecond()));
}

TEST_CASE("TickClock refuses to spiral after a long stall") {
    TickClock clock;

    // A breakpoint, a stalled load, or a laptop lid: without a cap the sim would
    // try to catch up thousands of ticks in one frame and hang the app, which is
    // a far worse failure than the clock quietly losing time.
    const int ticks = clock.advance(60.0f);
    CHECK(ticks <= static_cast<int>(clock.maxTicksPerAdvance()));
    CHECK(ticks > 0);
}

TEST_CASE("TickClock ignores time going backwards") {
    TickClock clock;
    CHECK(clock.advance(-1.0f) == 0);
    // And the negative time is not banked against future ticks.
    CHECK(clock.advance(kRate.secondsPerTick()) == 1);
}

TEST_CASE("a unit accumulates the ground distance it has covered") {
    // What drives the walk cycle. Time cannot: a unit that is turning on the
    // spot, or stopped, must not keep striding, and one crossing a slope covers
    // less ground per second than one on the flat.
    const HeightField field = flatField();
    std::vector<rm::sim::Transform> instances{unitAt(100.0f, 100.0f)};
    std::vector<MoveState> motion{ordinary()};

    SECTION("an idle unit covers nothing") {
        run(instances, motion, field, 60);
        CHECK(rm::test::asFloat(motion[0].distanceTravelledElmos) == Approx(0.0f));
    }

    SECTION("a unit already facing its destination covers speed x time") {
        rm::sim::orderTo(motion[0], rm::sim::Terrain{field}, rm::test::fx(100.0f),
                     rm::test::fx(700.0f));
        run(instances, motion, field, static_cast<int>(kRate.ticksPerSecond()));
        // One second at 87 elmos/s, give or take the first tick's turn.
        CHECK(rm::test::asFloat(motion[0].distanceTravelledElmos) == Approx(rm::sim::kDefaultSpeedElmosPerSecond).margin(3.0));
    }

    SECTION("a unit turning on the spot covers almost nothing") {
        // Ordered directly behind: alignment is near zero, so it pivots rather
        // than travels, and the legs should barely move with it.
        rm::sim::orderTo(motion[0], rm::sim::Terrain{field}, rm::test::fx(100.0f),
                     rm::test::fx(-400.0f));
        run(instances, motion, field, 3);
        CHECK(rm::test::asFloat(motion[0].distanceTravelledElmos) < 2.0f);
    }

    SECTION("it stops accumulating once it has arrived") {
        rm::sim::orderTo(motion[0], rm::sim::Terrain{field}, rm::test::fx(100.0f),
                     rm::test::fx(300.0f));
        run(instances, motion, field, 4 * static_cast<int>(kRate.ticksPerSecond()));
        REQUIRE_FALSE(motion[0].moving);

        const float onArrival = rm::test::asFloat(motion[0].distanceTravelledElmos);
        REQUIRE(onArrival > 150.0f);
        run(instances, motion, field, 60);
        CHECK(rm::test::asFloat(motion[0].distanceTravelledElmos) == Approx(onArrival));
    }

    SECTION("it never goes backwards") {
        // Monotonic, because the walk cycle is driven straight off it — a
        // decrease would run a unit's legs in reverse.
        rm::sim::orderTo(motion[0], rm::sim::Terrain{field}, rm::test::fx(600.0f),
                     rm::test::fx(600.0f));
        rm::sim::Fx previous{};
        for (int i = 0; i < 3 * static_cast<int>(kRate.ticksPerSecond()); ++i) {
            rm::sim::tick(instances, motion, rm::sim::Terrain{field});
            REQUIRE(motion[0].distanceTravelledElmos >= previous);
            previous = motion[0].distanceTravelledElmos;
        }
    }
}

TEST_CASE("a unit walks a path waypoint by waypoint") {
    const HeightField field = flatField();
    std::vector<rm::sim::Transform> instances{unitAt(50.0f, 50.0f)};
    std::vector<MoveState> motion{ordinary()};

    // An L: due +Z, then due +X. A unit that ignored the corner and cut
    // straight to the end would arrive too, so the corner is what is checked.
    const std::vector<std::array<rm::sim::Fx, 2>> path{
        {{rm::test::fx(50.0f), rm::test::fx(400.0f)}},
        {{rm::test::fx(400.0f), rm::test::fx(400.0f)}},
        };
    rm::sim::orderAlongPath(motion[0], path);

    REQUIRE(motion[0].moving);

    // Partway through it should be near the corner, not on the diagonal.
    run(instances, motion, field, 2 * static_cast<int>(kRate.ticksPerSecond()));
    CHECK(rm::test::asFloat(instances[0].x) < 150.0f);
    CHECK(rm::test::asFloat(instances[0].z) > 150.0f);

    run(instances, motion, field, 10 * static_cast<int>(kRate.ticksPerSecond()));
    CHECK_FALSE(motion[0].moving);
    CHECK(rm::test::asFloat(instances[0].x) == Approx(400.0f).margin(rm::test::asFloat(rm::sim::arrivalRadius(kRate))));
    CHECK(rm::test::asFloat(instances[0].z) == Approx(400.0f).margin(rm::test::asFloat(rm::sim::arrivalRadius(kRate))));
}

TEST_CASE("an empty path leaves a unit where it stands") {
    const HeightField field = flatField();
    std::vector<rm::sim::Transform> instances{unitAt(100.0f, 100.0f)};
    std::vector<MoveState> motion{ordinary()};

    rm::sim::orderAlongPath(motion[0], {});

    CHECK_FALSE(motion[0].moving);
    run(instances, motion, field, 60);
    CHECK(rm::test::asFloat(instances[0].x) == Approx(100.0f));
    CHECK(rm::test::asFloat(instances[0].z) == Approx(100.0f));
}

TEST_CASE("a new order abandons the path it was following") {
    const HeightField field = flatField();
    std::vector<rm::sim::Transform> instances{unitAt(50.0f, 50.0f)};
    std::vector<MoveState> motion{ordinary()};

    const std::vector<std::array<rm::sim::Fx, 2>> path{
        {{rm::test::fx(50.0f), rm::test::fx(700.0f)}},
        {{rm::test::fx(700.0f), rm::test::fx(700.0f)}}};
    rm::sim::orderAlongPath(motion[0], path);
    run(instances, motion, field, 10);

    // A plain order must clear the route, or the unit resumes the old one after
    // reaching the new destination.
    rm::sim::orderTo(motion[0], rm::sim::Terrain{field}, rm::test::fx(50.0f),
                     rm::test::fx(60.0f));
    run(instances, motion, field, 4 * static_cast<int>(kRate.ticksPerSecond()));

    CHECK_FALSE(motion[0].moving);
    CHECK(rm::test::asFloat(instances[0].z) < 120.0f);
}

TEST_CASE("units pushed together are separated") {
    const HeightField field = flatField();

    SECTION("two units at the same spot end up a radius apart") {
        std::vector<rm::sim::Transform> instances{unitAt(400.0f, 400.0f), unitAt(400.0f, 400.0f)};
        std::vector<MoveState> motion(2, ordinary());

        // Exactly coincident is the degenerate case: there is no direction to
        // push along, and a naive normalise divides by zero.
        rm::sim::resolveCollisions(instances, motion, rm::sim::Terrain{field});

        const rm::sim::Fx apart = rm::sim::fxHypot(instances[0].x - instances[1].x,
                                                   instances[0].z - instances[1].z);
        CHECK(apart > rm::sim::Fx{});
        // `isfinite` is gone, and cannot come back: a fixed-point value has no infinity and
        // no NaN to check for. The degenerate case that produced them — dividing by a zero
        // separation — now saturates instead, which is a wrong number a test can see rather
        // than a poison one that spreads.
        CHECK(instances[0].x.raw() != INT32_MAX);
        CHECK(instances[1].x.raw() != INT32_MAX);

        // A few passes should reach the full separation.
        for (int i = 0; i < 20; ++i) {
            rm::sim::resolveCollisions(instances, motion, rm::sim::Terrain{field});
        }
        const float settled = rm::test::asFloat(rm::sim::fxHypot(instances[0].x - instances[1].x,
                                                            instances[0].z - instances[1].z));
        CHECK(settled >= Approx(rm::test::asFloat(motion[0].radiusElmos + motion[1].radiusElmos)).epsilon(0.05));
    }

    SECTION("units already clear of each other do not move") {
        std::vector<rm::sim::Transform> instances{unitAt(100.0f, 100.0f), unitAt(500.0f, 500.0f)};
        std::vector<MoveState> motion(2, ordinary());

        rm::sim::resolveCollisions(instances, motion, rm::sim::Terrain{field});

        CHECK(rm::test::asFloat(instances[0].x) == Approx(100.0f));
        CHECK(rm::test::asFloat(instances[1].x) == Approx(500.0f));
    }

    SECTION("a crowd spreads out instead of stacking") {
        // Thirty units dumped on one point, which is exactly what a rally order
        // produces once pathfinding works.
        std::vector<rm::sim::Transform> instances;
        for (int i = 0; i < 30; ++i) {
            instances.push_back(unitAt(400.0f, 400.0f));
        }
        std::vector<MoveState> motion(instances.size(), ordinary());

        for (int i = 0; i < 120; ++i) {
            rm::sim::resolveCollisions(instances, motion, rm::sim::Terrain{field});
        }

        std::size_t overlapping = 0;
        for (std::size_t a = 0; a < instances.size(); ++a) {
            for (std::size_t b = a + 1; b < instances.size(); ++b) {
                const rm::sim::Fx d = rm::sim::fxHypot(instances[a].x - instances[b].x,
                                                       instances[a].z - instances[b].z);
                if (d < (motion[a].radiusElmos + motion[b].radiusElmos)
                            * rm::sim::Fx::fromRatio(4, 5)) {
                    ++overlapping;
                }
            }
        }
        CHECK(overlapping == 0);
    }

    SECTION("separation keeps units on the map and on the ground") {
        const HeightField ramp = rampField();
        std::vector<rm::sim::Transform> instances{unitAt(0.0f, 0.0f), unitAt(0.0f, 0.0f),
                                            unitAt(0.0f, 0.0f)};
        std::vector<MoveState> motion(3, ordinary());

        for (int i = 0; i < 30; ++i) {
            rm::sim::resolveCollisions(instances, motion, rm::sim::Terrain{ramp});
        }

        for (const rm::sim::Transform& unit : instances) {
            CHECK(rm::test::asFloat(unit.x) >= 0.0f);
            CHECK(rm::test::asFloat(unit.z) >= 0.0f);
            CHECK(rm::test::asFloat(unit.x) <= ramp.widthElmos());
            CHECK(rm::test::asFloat(unit.z) <= ramp.depthElmos());
            CHECK(rm::test::asFloat(unit.y)
                  == Approx(ramp.heightAtWorld(rm::test::asFloat(unit.x),
                                               rm::test::asFloat(unit.z)))
                         // Eight steps: the terrain sample rounds, the two axis interpolations
                       // round, and the vertical decode rounds — against a float accessor
                       // that does none of that. Still four orders of magnitude below the
                       // 0.01-elmo vertical resolution of a real heightmap.
                       .margin(8 * rm::test::kFxStep));
        }
    }

    SECTION("it is deterministic") {
        const auto play = [&field]() {
            std::vector<rm::sim::Transform> instances;
            for (int i = 0; i < 12; ++i) {
                instances.push_back(unitAt(400.0f + static_cast<float>(i % 3),
                                           400.0f + static_cast<float>(i % 2)));
            }
            std::vector<MoveState> motion(instances.size(), ordinary());
            for (int i = 0; i < 40; ++i) {
                rm::sim::resolveCollisions(instances, motion, rm::sim::Terrain{field});
            }
            return instances;
        };

        const auto first = play();
        const auto second = play();
        for (std::size_t i = 0; i < first.size(); ++i) {
            CHECK(first[i].x == second[i].x);
            CHECK(first[i].z == second[i].z);
        }
    }

    SECTION("a unit with no radius is left alone") {
        std::vector<rm::sim::Transform> instances{unitAt(400.0f, 400.0f), unitAt(400.0f, 400.0f)};
        std::vector<MoveState> motion(2, ordinary());
        motion[0].radiusElmos = rm::sim::Fx{};
        motion[1].radiusElmos = rm::sim::Fx{};

        rm::sim::resolveCollisions(instances, motion, rm::sim::Terrain{field});

        CHECK(rm::test::asFloat(instances[0].x) == Approx(400.0f));
        CHECK(rm::test::asFloat(instances[1].x) == Approx(400.0f));
    }
}

TEST_CASE("slope alignment tilts a unit onto the ground beneath it") {
    SECTION("flat ground needs no tilt") {
        const HeightField field = flatField();
        const auto align = rm::sim::slopeAlignment(rm::sim::Terrain{field}, rm::test::fx(400.0f),
                                              rm::test::fx(400.0f), rm::sim::bradFromRadians(0.0f));
        CHECK(rm::test::signedRadians(align[0]) == Approx(0.0f).margin(1e-5));
        CHECK(rm::test::signedRadians(align[1]) == Approx(0.0f).margin(1e-5));
    }

    SECTION("a ramp along +X rolls a unit facing +Z") {
        // Facing +Z (yaw 0) with the slope rising to the right, the unit banks
        // sideways: that is roll, and pitch stays level.
        const HeightField field = rampField();  // climbs along +X
        const auto align = rm::sim::slopeAlignment(rm::sim::Terrain{field}, rm::test::fx(400.0f),
                                              rm::test::fx(400.0f), rm::sim::bradFromRadians(0.0f));
        CHECK(rm::test::signedRadians(align[0]) == Approx(0.0f).margin(1e-4));  // pitch
        CHECK(std::abs(rm::test::signedRadians(align[1])) > 0.1f);              // roll
    }

    SECTION("the same slope pitches a unit facing up it") {
        // Turned to face +X — straight up the ramp — the same ground becomes
        // pitch instead of roll. Getting this backwards is the bug that makes
        // units lean sideways going uphill, and no flat-ground test finds it.
        const HeightField field = rampField();
        const auto align = rm::sim::slopeAlignment(rm::sim::Terrain{field}, rm::test::fx(400.0f),
                                              rm::test::fx(400.0f), rm::sim::bradFromRadians(kPi / 2.0f));
        CHECK(std::abs(rm::test::signedRadians(align[0])) > 0.1f);              // pitch
        CHECK(rm::test::signedRadians(align[1]) == Approx(0.0f).margin(1e-4));  // roll
    }

    SECTION("the tilt magnitude matches the slope's angle") {
        // rampField climbs 10 raw units per square at 1 elmo per raw unit, so
        // 10 elmos of rise over 8 of run — about 51 degrees.
        const HeightField field = rampField();
        const auto align = rm::sim::slopeAlignment(rm::sim::Terrain{field}, rm::test::fx(400.0f),
                                              rm::test::fx(400.0f), rm::sim::bradFromRadians(kPi / 2.0f));
        const float expected = std::atan2(10.0f, 8.0f);
        CHECK(std::abs(rm::test::signedRadians(align[0])) == Approx(expected).margin(0.05));
    }

    SECTION("it stays finite on a vertical wall") {
        // A cliff face makes the gradient enormous; asin must not be handed
        // something outside [-1, 1] and the result must not be a NaN that
        // spreads into every vertex of the model.
        HeightField field = flatField();
        for (int z = 0; z < field.verticesZ(); ++z) {
            field.raw[static_cast<std::size_t>(z) * static_cast<std::size_t>(field.verticesX())
                      + 50] = 60000;
        }
        for (float yaw : {0.0f, kPi / 2.0f, kPi}) {
            const auto align = rm::sim::slopeAlignment(rm::sim::Terrain{field}, rm::test::fx(400.0f),
                                              rm::test::fx(400.0f), rm::sim::bradFromRadians(yaw));
            // No `isfinite` to check: a `Brad` is 16 bits of angle and every value of it is
            // a valid angle. The vertical-wall case the float version could turn into a NaN
            // now yields a saturated arcsine, which is a real angle.
            CHECK(align[0] <= 65535);
            CHECK(align[1] <= 65535);
        }
    }
}

TEST_CASE("a unit standing still is still tilted onto its slope") {
    // The whole point of alignment is that a unit placed on a hill does not
    // stick out horizontally — and a scene of scattered units has ordered none
    // of them. Gating this on movement would leave every static scene flat.
    const HeightField field = rampField();
    std::vector<rm::sim::Transform> instances{unitAt(400.0f, 400.0f, kPi / 2.0f)};
    std::vector<MoveState> motion{ordinary()};

    REQUIRE_FALSE(motion[0].moving);
    run(instances, motion, field, 1);

    CHECK(std::abs(rm::sim::radiansFromBrad(instances[0].pitch)) > 0.1f);
}

TEST_CASE("collision sees units of different models") {
    // History: each model's instances lived in their own array, and for three
    // milestones the separation pass ran once per array — so two units of DIFFERENT
    // models could stand in exactly the same spot and neither would notice. Then the
    // pass took a list of arrays. Now there is ONE array for the whole match, so the
    // bug is gone by construction and this case only proves the construction: a tank
    // and a bot are two slots of the same span and cannot be missed by an inner loop
    // that never restarts.
    const HeightField field = flatField();

    std::vector<rm::sim::Transform> units{unitAt(400.0f, 400.0f), unitAt(400.0f, 400.0f)};
    std::vector<MoveState> motion(2, ordinary());

    for (int i = 0; i < 20; ++i) {
        rm::sim::resolveCollisions(units, motion, rm::sim::Terrain{field});
    }

    const float apart = rm::test::asFloat(
        rm::sim::fxHypot(units[0].x - units[1].x, units[0].z - units[1].z));
    CHECK(apart >= Approx(rm::test::asFloat(motion[0].radiusElmos + motion[1].radiusElmos)).epsilon(0.05));
}

TEST_CASE("a crowd of one model still separates from itself") {
    // The flat form must not lose what the grouped one did.
    const HeightField field = flatField();

    std::vector<rm::sim::Transform> crowd;
    for (int i = 0; i < 8; ++i) {
        crowd.push_back(unitAt(400.0f, 400.0f));
    }
    std::vector<MoveState> motion(crowd.size(), ordinary());

    for (int i = 0; i < 80; ++i) {
        rm::sim::resolveCollisions(crowd, motion, rm::sim::Terrain{field});
    }

    for (std::size_t a = 0; a < crowd.size(); ++a) {
        for (std::size_t b = a + 1; b < crowd.size(); ++b) {
            const rm::sim::Fx d =
                rm::sim::fxHypot(crowd[a].x - crowd[b].x, crowd[a].z - crowd[b].z);
            REQUIRE(d > (motion[a].radiusElmos + motion[b].radiusElmos)
                            * rm::sim::Fx::fromRatio(4, 5));
        }
    }
}

TEST_CASE("an empty or single-unit store is harmless") {
    const HeightField field = flatField();

    CHECK_NOTHROW(rm::sim::resolveCollisions(std::span<rm::sim::Transform>{},
                                             std::span<const MoveState>{},
                                             rm::sim::Terrain{field}));

    std::vector<rm::sim::Transform> one{unitAt(100.0f, 100.0f)};
    std::vector<MoveState> motion(1, ordinary());
    rm::sim::resolveCollisions(one, motion, rm::sim::Terrain{field});
    CHECK(rm::test::asFloat(one[0].x) == Approx(100.0f));
}
