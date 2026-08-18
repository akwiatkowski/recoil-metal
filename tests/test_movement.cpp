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
    run(instances, motion, field, 10);
    const float after = distanceToOrder(instances[0], motion[0]);

    CHECK(after < before);
    // Already facing +Z, so it should be travelling at very nearly full speed
    // from the first tick: 10 ticks is a third of a second.
    const float expected = rm::sim::kDefaultSpeedElmosPerSecond / 3.0f;
    CHECK(before - after == Approx(expected).margin(1.0f));
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
