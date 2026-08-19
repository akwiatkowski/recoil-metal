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
#include "core/sim/Movement.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <vector>

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

/// One unit at a position, facing +Z (yaw 0).
[[nodiscard]] UnitInstance unitAt(float x, float z, float yaw = 0.0f) {
    UnitInstance instance{};
    instance.position = {{x, 0.0f, z}};
    instance.rotationY = yaw;
    instance.scale = 1.0f;
    return instance;
}

/// Runs `count` fixed ticks over a one-unit world.
void run(std::vector<UnitInstance>& instances, std::vector<MoveState>& motion,
         const HeightField& field, int count) {
    for (int i = 0; i < count; ++i) {
        rm::sim::tick(instances, motion, field);
    }
}

/// Distance between a unit and its destination, on the ground plane.
[[nodiscard]] float distanceToOrder(const UnitInstance& instance, const MoveState& state) {
    const float dx = state.destinationX - instance.position[0];
    const float dz = state.destinationZ - instance.position[2];
    return std::hypot(dx, dz);
}

} // namespace

TEST_CASE("a unit with no order does not move") {
    const HeightField field = flatField();
    std::vector<UnitInstance> instances{unitAt(100.0f, 100.0f, 0.7f)};
    std::vector<MoveState> motion{MoveState{}};

    run(instances, motion, field, 60);

    CHECK(instances[0].position[0] == Approx(100.0f));
    CHECK(instances[0].position[2] == Approx(100.0f));
    // Yaw too: an idle unit that slowly rotates is a bug that is easy to miss.
    CHECK(instances[0].rotationY == Approx(0.7f));
}

TEST_CASE("an ordered unit closes on its destination") {
    const HeightField field = flatField();
    std::vector<UnitInstance> instances{unitAt(100.0f, 100.0f)};
    std::vector<MoveState> motion{MoveState{}};
    rm::sim::orderTo(motion[0], field, 100.0f, 400.0f);

    const float before = distanceToOrder(instances[0], motion[0]);
    // A whole second, expressed as one, so the expectation below does not have to
    // know the tick rate — an earlier version ran a flat 10 ticks and called it a
    // third of a second, which stopped being true at 10 Hz.
    run(instances, motion, field, rm::sim::kTicksPerSecond);
    const float after = distanceToOrder(instances[0], motion[0]);

    CHECK(after < before);
    // Already facing +Z, so it travels at very nearly full speed from the first
    // tick: one second of it.
    CHECK(before - after == Approx(rm::sim::kDefaultSpeedElmosPerSecond).margin(1.0f));
}

TEST_CASE("a unit arrives and then stops") {
    const HeightField field = flatField();
    std::vector<UnitInstance> instances{unitAt(100.0f, 100.0f)};
    std::vector<MoveState> motion{MoveState{}};
    rm::sim::orderTo(motion[0], field, 100.0f, 300.0f);

    // 200 elmos at 87 elmos/s is ~2.3 seconds, so four seconds is ample.
    run(instances, motion, field, 4 * rm::sim::kTicksPerSecond);

    CHECK_FALSE(motion[0].moving);
    CHECK(distanceToOrder(instances[0], motion[0]) < rm::sim::kArrivalRadiusElmos);

    // And it stays stopped rather than creeping or oscillating around the goal.
    const std::array<float, 3> resting = instances[0].position;
    run(instances, motion, field, 60);
    CHECK(instances[0].position[0] == Approx(resting[0]));
    CHECK(instances[0].position[2] == Approx(resting[2]));
}

TEST_CASE("a unit does not overshoot a destination closer than one tick of travel") {
    const HeightField field = flatField();
    std::vector<UnitInstance> instances{unitAt(100.0f, 100.0f)};
    std::vector<MoveState> motion{MoveState{}};

    // Well inside one tick's travel (87/30 = 2.9 elmos), and inside the arrival
    // radius, so this must resolve immediately rather than stepping past.
    rm::sim::orderTo(motion[0], field, 100.0f, 101.0f);
    run(instances, motion, field, 1);

    CHECK(instances[0].position[2] <= Approx(101.0f));
    CHECK_FALSE(motion[0].moving);
}

TEST_CASE("a unit turns to face where it is going") {
    const HeightField field = flatField();
    std::vector<UnitInstance> instances{unitAt(100.0f, 100.0f)};
    std::vector<MoveState> motion{MoveState{}};

    // Due +X. The shader maps local +Z to (sin yaw, cos yaw), so facing +X is
    // yaw = pi/2 — get the convention wrong and every unit walks sideways.
    rm::sim::orderTo(motion[0], field, 400.0f, 100.0f);
    run(instances, motion, field, rm::sim::kTicksPerSecond);

    CHECK(instances[0].rotationY == Approx(kPi / 2.0f).margin(0.05));
}

TEST_CASE("a unit turns no faster than its turn rate") {
    const HeightField field = flatField();
    std::vector<UnitInstance> instances{unitAt(100.0f, 100.0f)};
    std::vector<MoveState> motion{MoveState{}};
    rm::sim::orderTo(motion[0], field, 400.0f, 100.0f);

    const float before = instances[0].rotationY;
    run(instances, motion, field, 1);
    const float delta = std::abs(instances[0].rotationY - before);

    // One tick may not snap the unit round — that is what separates a vehicle
    // from a turret.
    CHECK(delta <= Approx(motion[0].turnRateRadiansPerSecond * rm::sim::kTickSeconds).epsilon(0.01));
    CHECK(delta > 0.0f);
}

TEST_CASE("a unit ordered behind itself turns before it travels") {
    const HeightField field = flatField();
    // Facing +Z, ordered due -Z: the destination is directly behind.
    std::vector<UnitInstance> instances{unitAt(400.0f, 400.0f, 0.0f)};
    std::vector<MoveState> motion{MoveState{}};
    rm::sim::orderTo(motion[0], field, 400.0f, 100.0f);

    // Forward speed scales with how well the unit is aligned, so for the first
    // few ticks it should pivot almost in place rather than drive off the wrong
    // way and arc back.
    run(instances, motion, field, 3);
    CHECK(std::abs(instances[0].position[2] - 400.0f) < 2.0f);

    // Given time it still gets there.
    run(instances, motion, field, 6 * rm::sim::kTicksPerSecond);
    CHECK_FALSE(motion[0].moving);
}

TEST_CASE("a moving unit follows the terrain") {
    const HeightField field = rampField();
    std::vector<UnitInstance> instances{unitAt(0.0f, 400.0f)};
    std::vector<MoveState> motion{MoveState{}};
    rm::sim::orderTo(motion[0], field, 700.0f, 400.0f);

    // Every tick, not just at the end: the Y must track the ground continuously
    // or the unit submarines through hills between waypoints.
    for (int i = 0; i < 4 * rm::sim::kTicksPerSecond; ++i) {
        rm::sim::tick(instances, motion, field);
        const UnitInstance& unit = instances[0];
        REQUIRE(unit.position[1]
                == Approx(field.heightAtWorld(unit.position[0], unit.position[2])));
    }

    // And it did actually climb, so the assertion above was not vacuous.
    CHECK(instances[0].position[1] > 100.0f);
}

TEST_CASE("an order outside the map is clamped onto it") {
    const HeightField field = flatField();
    std::vector<UnitInstance> instances{unitAt(400.0f, 400.0f)};
    std::vector<MoveState> motion{MoveState{}};

    rm::sim::orderTo(motion[0], field, 99999.0f, -500.0f);

    CHECK(motion[0].destinationX == Approx(field.widthElmos()));
    CHECK(motion[0].destinationZ == Approx(0.0f));

    // And it is reachable, so the unit stops rather than pressing at the border.
    run(instances, motion, field, 20 * rm::sim::kTicksPerSecond);
    CHECK_FALSE(motion[0].moving);
}

TEST_CASE("a unit never leaves the map") {
    const HeightField field = flatField();
    std::vector<UnitInstance> instances{unitAt(10.0f, 10.0f)};
    std::vector<MoveState> motion{MoveState{}};
    rm::sim::orderTo(motion[0], field, 0.0f, 0.0f);

    run(instances, motion, field, 5 * rm::sim::kTicksPerSecond);

    CHECK(instances[0].position[0] >= 0.0f);
    CHECK(instances[0].position[2] >= 0.0f);
    CHECK(instances[0].position[0] <= field.widthElmos());
    CHECK(instances[0].position[2] <= field.depthElmos());
}

TEST_CASE("stop cancels an order in place") {
    const HeightField field = flatField();
    std::vector<UnitInstance> instances{unitAt(100.0f, 100.0f)};
    std::vector<MoveState> motion{MoveState{}};
    rm::sim::orderTo(motion[0], field, 100.0f, 700.0f);
    run(instances, motion, field, 10);

    const std::array<float, 3> halfway = instances[0].position;
    motion[0].moving = false;
    run(instances, motion, field, 60);

    CHECK(instances[0].position[2] == Approx(halfway[2]));
}

TEST_CASE("the sim moves every ordered unit, and only those") {
    const HeightField field = flatField();
    std::vector<UnitInstance> instances{
        unitAt(100.0f, 100.0f),
        unitAt(200.0f, 100.0f),
        unitAt(300.0f, 100.0f),
    };
    std::vector<MoveState> motion(instances.size());
    rm::sim::orderTo(motion[0], field, 100.0f, 500.0f);
    rm::sim::orderTo(motion[2], field, 300.0f, 500.0f);

    run(instances, motion, field, 30);

    CHECK(instances[0].position[2] > 100.0f);
    CHECK(instances[1].position[2] == Approx(100.0f));
    CHECK(instances[2].position[2] > 100.0f);
}

TEST_CASE("the sim is safe when the two spans disagree in length") {
    // A defensive check rather than a designed case: the caller is expected to
    // keep them parallel, but reading past the shorter one would be a crash in
    // the frame loop rather than a visible bug.
    const HeightField field = flatField();
    std::vector<UnitInstance> instances{unitAt(100.0f, 100.0f), unitAt(200.0f, 100.0f)};
    std::vector<MoveState> motion(1);
    rm::sim::orderTo(motion[0], field, 100.0f, 500.0f);

    REQUIRE_NOTHROW(run(instances, motion, field, 10));
    CHECK(instances[0].position[2] > 100.0f);
    CHECK(instances[1].position[2] == Approx(100.0f));
}

TEST_CASE("the sim is deterministic") {
    const HeightField field = rampField();
    const auto play = [&field]() {
        std::vector<UnitInstance> instances{unitAt(50.0f, 400.0f), unitAt(600.0f, 200.0f, 2.0f)};
        std::vector<MoveState> motion(instances.size());
        rm::sim::orderTo(motion[0], field, 700.0f, 700.0f);
        rm::sim::orderTo(motion[1], field, 100.0f, 100.0f);
        run(instances, motion, field, 90);
        return instances;
    };

    const std::vector<UnitInstance> first = play();
    const std::vector<UnitInstance> second = play();

    REQUIRE(first.size() == second.size());
    for (std::size_t i = 0; i < first.size(); ++i) {
        // Bit-exact, not approximate: the same ticks over the same state must
        // produce the same floats, or a benchmark and a screenshot stop being
        // reproducible.
        CHECK(first[i].position[0] == second[i].position[0]);
        CHECK(first[i].position[1] == second[i].position[1]);
        CHECK(first[i].position[2] == second[i].position[2]);
        CHECK(first[i].rotationY == second[i].rotationY);
    }
}

TEST_CASE("TickClock hands out whole ticks and carries the remainder") {
    TickClock clock;

    // Exactly one tick's worth.
    CHECK(clock.advance(rm::sim::kTickSeconds) == 1);

    // Half a tick twice is one tick, not zero and not two.
    CHECK(clock.advance(rm::sim::kTickSeconds * 0.5f) == 0);
    CHECK(clock.advance(rm::sim::kTickSeconds * 0.5f) == 1);

    // A whole second is the tick rate, however it is chopped up.
    TickClock other;
    int total = 0;
    for (int i = 0; i < 120; ++i) {
        total += other.advance(1.0f / 120.0f);
    }
    CHECK(total == rm::sim::kTicksPerSecond);
}

TEST_CASE("TickClock refuses to spiral after a long stall") {
    TickClock clock;

    // A breakpoint, a stalled load, or a laptop lid: without a cap the sim would
    // try to catch up thousands of ticks in one frame and hang the app, which is
    // a far worse failure than the clock quietly losing time.
    const int ticks = clock.advance(60.0f);
    CHECK(ticks <= TickClock::kMaxTicksPerAdvance);
    CHECK(ticks > 0);
}

TEST_CASE("TickClock ignores time going backwards") {
    TickClock clock;
    CHECK(clock.advance(-1.0f) == 0);
    // And the negative time is not banked against future ticks.
    CHECK(clock.advance(rm::sim::kTickSeconds) == 1);
}

TEST_CASE("a unit accumulates the ground distance it has covered") {
    // What drives the walk cycle. Time cannot: a unit that is turning on the
    // spot, or stopped, must not keep striding, and one crossing a slope covers
    // less ground per second than one on the flat.
    const HeightField field = flatField();
    std::vector<UnitInstance> instances{unitAt(100.0f, 100.0f)};
    std::vector<MoveState> motion{MoveState{}};

    SECTION("an idle unit covers nothing") {
        run(instances, motion, field, 60);
        CHECK(motion[0].distanceTravelledElmos == Approx(0.0f));
    }

    SECTION("a unit already facing its destination covers speed x time") {
        rm::sim::orderTo(motion[0], field, 100.0f, 700.0f);
        run(instances, motion, field, rm::sim::kTicksPerSecond);
        // One second at 87 elmos/s, give or take the first tick's turn.
        CHECK(motion[0].distanceTravelledElmos
              == Approx(rm::sim::kDefaultSpeedElmosPerSecond).margin(3.0));
    }

    SECTION("a unit turning on the spot covers almost nothing") {
        // Ordered directly behind: alignment is near zero, so it pivots rather
        // than travels, and the legs should barely move with it.
        rm::sim::orderTo(motion[0], field, 100.0f, -400.0f);
        run(instances, motion, field, 3);
        CHECK(motion[0].distanceTravelledElmos < 2.0f);
    }

    SECTION("it stops accumulating once it has arrived") {
        rm::sim::orderTo(motion[0], field, 100.0f, 300.0f);
        run(instances, motion, field, 4 * rm::sim::kTicksPerSecond);
        REQUIRE_FALSE(motion[0].moving);

        const float onArrival = motion[0].distanceTravelledElmos;
        REQUIRE(onArrival > 150.0f);
        run(instances, motion, field, 60);
        CHECK(motion[0].distanceTravelledElmos == Approx(onArrival));
    }

    SECTION("it never goes backwards") {
        // Monotonic, because the walk cycle is driven straight off it — a
        // decrease would run a unit's legs in reverse.
        rm::sim::orderTo(motion[0], field, 600.0f, 600.0f);
        float previous = 0.0f;
        for (int i = 0; i < 3 * rm::sim::kTicksPerSecond; ++i) {
            rm::sim::tick(instances, motion, field);
            REQUIRE(motion[0].distanceTravelledElmos >= previous);
            previous = motion[0].distanceTravelledElmos;
        }
    }
}

TEST_CASE("a unit walks a path waypoint by waypoint") {
    const HeightField field = flatField();
    std::vector<UnitInstance> instances{unitAt(50.0f, 50.0f)};
    std::vector<MoveState> motion{MoveState{}};

    // An L: due +Z, then due +X. A unit that ignored the corner and cut
    // straight to the end would arrive too, so the corner is what is checked.
    const std::vector<std::array<float, 2>> path{
        {{50.0f, 400.0f}}, {{400.0f, 400.0f}},
    };
    rm::sim::orderAlongPath(motion[0], path);

    REQUIRE(motion[0].moving);

    // Partway through it should be near the corner, not on the diagonal.
    run(instances, motion, field, 2 * rm::sim::kTicksPerSecond);
    CHECK(instances[0].position[0] < 150.0f);
    CHECK(instances[0].position[2] > 150.0f);

    run(instances, motion, field, 10 * rm::sim::kTicksPerSecond);
    CHECK_FALSE(motion[0].moving);
    CHECK(instances[0].position[0] == Approx(400.0f).margin(rm::sim::kArrivalRadiusElmos));
    CHECK(instances[0].position[2] == Approx(400.0f).margin(rm::sim::kArrivalRadiusElmos));
}

TEST_CASE("an empty path leaves a unit where it stands") {
    const HeightField field = flatField();
    std::vector<UnitInstance> instances{unitAt(100.0f, 100.0f)};
    std::vector<MoveState> motion{MoveState{}};

    rm::sim::orderAlongPath(motion[0], {});

    CHECK_FALSE(motion[0].moving);
    run(instances, motion, field, 60);
    CHECK(instances[0].position[0] == Approx(100.0f));
    CHECK(instances[0].position[2] == Approx(100.0f));
}

TEST_CASE("a new order abandons the path it was following") {
    const HeightField field = flatField();
    std::vector<UnitInstance> instances{unitAt(50.0f, 50.0f)};
    std::vector<MoveState> motion{MoveState{}};

    const std::vector<std::array<float, 2>> path{{{50.0f, 700.0f}}, {{700.0f, 700.0f}}};
    rm::sim::orderAlongPath(motion[0], path);
    run(instances, motion, field, 10);

    // A plain order must clear the route, or the unit resumes the old one after
    // reaching the new destination.
    rm::sim::orderTo(motion[0], field, 50.0f, 60.0f);
    run(instances, motion, field, 4 * rm::sim::kTicksPerSecond);

    CHECK_FALSE(motion[0].moving);
    CHECK(instances[0].position[2] < 120.0f);
}

TEST_CASE("units pushed together are separated") {
    const HeightField field = flatField();

    SECTION("two units at the same spot end up a radius apart") {
        std::vector<UnitInstance> instances{unitAt(400.0f, 400.0f), unitAt(400.0f, 400.0f)};
        std::vector<MoveState> motion(2);

        // Exactly coincident is the degenerate case: there is no direction to
        // push along, and a naive normalise divides by zero.
        rm::sim::resolveCollisions(instances, motion, field);

        const float apart = std::hypot(instances[0].position[0] - instances[1].position[0],
                                       instances[0].position[2] - instances[1].position[2]);
        CHECK(apart > 0.0f);
        CHECK(std::isfinite(instances[0].position[0]));
        CHECK(std::isfinite(instances[1].position[0]));

        // A few passes should reach the full separation.
        for (int i = 0; i < 20; ++i) {
            rm::sim::resolveCollisions(instances, motion, field);
        }
        const float settled = std::hypot(instances[0].position[0] - instances[1].position[0],
                                         instances[0].position[2] - instances[1].position[2]);
        CHECK(settled >= Approx(motion[0].radiusElmos + motion[1].radiusElmos).epsilon(0.05));
    }

    SECTION("units already clear of each other do not move") {
        std::vector<UnitInstance> instances{unitAt(100.0f, 100.0f), unitAt(500.0f, 500.0f)};
        std::vector<MoveState> motion(2);

        rm::sim::resolveCollisions(instances, motion, field);

        CHECK(instances[0].position[0] == Approx(100.0f));
        CHECK(instances[1].position[0] == Approx(500.0f));
    }

    SECTION("a crowd spreads out instead of stacking") {
        // Thirty units dumped on one point, which is exactly what a rally order
        // produces once pathfinding works.
        std::vector<UnitInstance> instances;
        for (int i = 0; i < 30; ++i) {
            instances.push_back(unitAt(400.0f, 400.0f));
        }
        std::vector<MoveState> motion(instances.size());

        for (int i = 0; i < 120; ++i) {
            rm::sim::resolveCollisions(instances, motion, field);
        }

        std::size_t overlapping = 0;
        for (std::size_t a = 0; a < instances.size(); ++a) {
            for (std::size_t b = a + 1; b < instances.size(); ++b) {
                const float d = std::hypot(instances[a].position[0] - instances[b].position[0],
                                           instances[a].position[2] - instances[b].position[2]);
                if (d < (motion[a].radiusElmos + motion[b].radiusElmos) * 0.8f) {
                    ++overlapping;
                }
            }
        }
        CHECK(overlapping == 0);
    }

    SECTION("separation keeps units on the map and on the ground") {
        const HeightField ramp = rampField();
        std::vector<UnitInstance> instances{unitAt(0.0f, 0.0f), unitAt(0.0f, 0.0f),
                                            unitAt(0.0f, 0.0f)};
        std::vector<MoveState> motion(3);

        for (int i = 0; i < 30; ++i) {
            rm::sim::resolveCollisions(instances, motion, ramp);
        }

        for (const UnitInstance& unit : instances) {
            CHECK(unit.position[0] >= 0.0f);
            CHECK(unit.position[2] >= 0.0f);
            CHECK(unit.position[0] <= ramp.widthElmos());
            CHECK(unit.position[2] <= ramp.depthElmos());
            CHECK(unit.position[1]
                  == Approx(ramp.heightAtWorld(unit.position[0], unit.position[2])));
        }
    }

    SECTION("it is deterministic") {
        const auto play = [&field]() {
            std::vector<UnitInstance> instances;
            for (int i = 0; i < 12; ++i) {
                instances.push_back(unitAt(400.0f + static_cast<float>(i % 3),
                                           400.0f + static_cast<float>(i % 2)));
            }
            std::vector<MoveState> motion(instances.size());
            for (int i = 0; i < 40; ++i) {
                rm::sim::resolveCollisions(instances, motion, field);
            }
            return instances;
        };

        const auto first = play();
        const auto second = play();
        for (std::size_t i = 0; i < first.size(); ++i) {
            CHECK(first[i].position[0] == second[i].position[0]);
            CHECK(first[i].position[2] == second[i].position[2]);
        }
    }

    SECTION("a unit with no radius is left alone") {
        std::vector<UnitInstance> instances{unitAt(400.0f, 400.0f), unitAt(400.0f, 400.0f)};
        std::vector<MoveState> motion(2);
        motion[0].radiusElmos = 0.0f;
        motion[1].radiusElmos = 0.0f;

        rm::sim::resolveCollisions(instances, motion, field);

        CHECK(instances[0].position[0] == Approx(400.0f));
        CHECK(instances[1].position[0] == Approx(400.0f));
    }
}

TEST_CASE("slope alignment tilts a unit onto the ground beneath it") {
    SECTION("flat ground needs no tilt") {
        const HeightField field = flatField();
        const auto align = rm::sim::slopeAlignment(field, 400.0f, 400.0f, 0.0f);
        CHECK(align[0] == Approx(0.0f).margin(1e-5));
        CHECK(align[1] == Approx(0.0f).margin(1e-5));
    }

    SECTION("a ramp along +X rolls a unit facing +Z") {
        // Facing +Z (yaw 0) with the slope rising to the right, the unit banks
        // sideways: that is roll, and pitch stays level.
        const HeightField field = rampField();  // climbs along +X
        const auto align = rm::sim::slopeAlignment(field, 400.0f, 400.0f, 0.0f);
        CHECK(align[0] == Approx(0.0f).margin(1e-4));  // pitch
        CHECK(std::abs(align[1]) > 0.1f);              // roll
    }

    SECTION("the same slope pitches a unit facing up it") {
        // Turned to face +X — straight up the ramp — the same ground becomes
        // pitch instead of roll. Getting this backwards is the bug that makes
        // units lean sideways going uphill, and no flat-ground test finds it.
        const HeightField field = rampField();
        const auto align = rm::sim::slopeAlignment(field, 400.0f, 400.0f, kPi / 2.0f);
        CHECK(std::abs(align[0]) > 0.1f);              // pitch
        CHECK(align[1] == Approx(0.0f).margin(1e-4));  // roll
    }

    SECTION("the tilt magnitude matches the slope's angle") {
        // rampField climbs 10 raw units per square at 1 elmo per raw unit, so
        // 10 elmos of rise over 8 of run — about 51 degrees.
        const HeightField field = rampField();
        const auto align = rm::sim::slopeAlignment(field, 400.0f, 400.0f, kPi / 2.0f);
        const float expected = std::atan2(10.0f, 8.0f);
        CHECK(std::abs(align[0]) == Approx(expected).margin(0.05));
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
            const auto align = rm::sim::slopeAlignment(field, 400.0f, 400.0f, yaw);
            CHECK(std::isfinite(align[0]));
            CHECK(std::isfinite(align[1]));
        }
    }
}

TEST_CASE("a unit standing still is still tilted onto its slope") {
    // The whole point of alignment is that a unit placed on a hill does not
    // stick out horizontally — and a scene of scattered units has ordered none
    // of them. Gating this on movement would leave every static scene flat.
    const HeightField field = rampField();
    std::vector<UnitInstance> instances{unitAt(400.0f, 400.0f, kPi / 2.0f)};
    std::vector<MoveState> motion{MoveState{}};

    REQUIRE_FALSE(motion[0].moving);
    run(instances, motion, field, 1);

    CHECK(std::abs(instances[0].rotationX) > 0.1f);
}

TEST_CASE("collision sees units of different models") {
    // Each model's instances live in their own array, and for three milestones
    // the separation pass ran once per array — so two units of DIFFERENT models
    // could stand in exactly the same spot and neither would notice.
    const HeightField field = flatField();

    std::vector<UnitInstance> tanks{unitAt(400.0f, 400.0f)};
    std::vector<UnitInstance> bots{unitAt(400.0f, 400.0f)};
    std::vector<MoveState> tankMotion(1);
    std::vector<MoveState> botMotion(1);

    const std::array<rm::sim::CollisionGroup, 2> groups{{
        {tanks, tankMotion},
        {bots, botMotion},
    }};

    for (int i = 0; i < 20; ++i) {
        rm::sim::resolveCollisions(groups, field);
    }

    const float apart = std::hypot(tanks[0].position[0] - bots[0].position[0],
                                   tanks[0].position[2] - bots[0].position[2]);
    CHECK(apart >= Approx(tankMotion[0].radiusElmos + botMotion[0].radiusElmos).epsilon(0.05));
}

TEST_CASE("a group's own units still separate from each other") {
    // The multi-group form must not lose what the single-group one did.
    const HeightField field = flatField();

    std::vector<UnitInstance> crowd;
    for (int i = 0; i < 8; ++i) {
        crowd.push_back(unitAt(400.0f, 400.0f));
    }
    std::vector<MoveState> motion(crowd.size());
    const std::array<rm::sim::CollisionGroup, 1> groups{{{crowd, motion}}};

    for (int i = 0; i < 80; ++i) {
        rm::sim::resolveCollisions(groups, field);
    }

    for (std::size_t a = 0; a < crowd.size(); ++a) {
        for (std::size_t b = a + 1; b < crowd.size(); ++b) {
            const float d = std::hypot(crowd[a].position[0] - crowd[b].position[0],
                                       crowd[a].position[2] - crowd[b].position[2]);
            REQUIRE(d > (motion[a].radiusElmos + motion[b].radiusElmos) * 0.8f);
        }
    }
}

TEST_CASE("an empty or single-unit group set is harmless") {
    const HeightField field = flatField();

    CHECK_NOTHROW(rm::sim::resolveCollisions(std::span<const rm::sim::CollisionGroup>{}, field));

    std::vector<UnitInstance> one{unitAt(100.0f, 100.0f)};
    std::vector<MoveState> motion(1);
    const std::array<rm::sim::CollisionGroup, 1> groups{{{one, motion}}};
    rm::sim::resolveCollisions(groups, field);
    CHECK(one[0].position[0] == Approx(100.0f));
}
