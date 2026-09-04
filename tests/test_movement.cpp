// Movement sim tests. The whole point of keeping the sim in core/ is that
// "does a unit walk to where it was told" is arithmetic, not something to
// judge by watching a screen.
//
// Everything here runs the fixed tick directly. Wall-clock pacing is the
// TickClock's job and is tested separately at the bottom.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "app/SceneBuild.hpp"  // motionFor — the one derivation both spawn paths use
#include "core/map/HeightField.hpp"
#include "core/scene/UnitPlacement.hpp"
#include "core/sim/Combat.hpp"  // headingError, for the angle assertions
#include "core/sim/Movement.hpp"
#include "core/sim/Pathfinding.hpp"

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

/// A store holding units at given places, for the collision cases.
///
/// `resolveCollisions` takes the store now (§7 P5.2) rather than two spans, because it reads
/// the store's spatial index. This keeps the cases reading the way they did — place units, run
/// separation, look at where they ended up — with the reindex in one place instead of at every
/// call.
struct Crowd {
    rm::sim::UnitStore store;

    void add(float x, float z, bool airborne = false, bool surfaceWater = false) {
        MoveState motion = ordinary();
        motion.airborne = airborne;
        motion.surfaceWater = surfaceWater;
        (void)store.spawn(rm::sim::UnitStore::Spawn{
            .transform = unitAt(x, z),
            .motion = motion,
            .health = rm::sim::Health{.current = rm::sim::Mag::fromInt(100),
                                      .maximum = rm::sim::Mag::fromInt(100)},
        });
    }

    [[nodiscard]] rm::sim::Transform& at(std::size_t i) { return store.transforms()[i]; }
    [[nodiscard]] const rm::sim::MoveState& motionAt(std::size_t i) const {
        return store.motion()[i];
    }
    [[nodiscard]] std::size_t size() const { return store.slotCount(); }

    /// One separation pass, with the index rebuilt first — which is what the tick does.
    void separate(const HeightField& field) {
        store.reindex(rm::sim::Fx::fromInt(64));
        rm::sim::resolveCollisions(store, rm::sim::Terrain{field});
    }
};

/// Runs `count` fixed ticks over a one-unit world.
void run(std::vector<rm::sim::Transform>& instances, std::vector<MoveState>& motion,
         const HeightField& field, int count) {
    for (int i = 0; i < count; ++i) {
        rm::sim::tick(instances, motion, rm::sim::Terrain{field});
    }
}

TEST_CASE("aircraft keep fixed terrain clearance and remain level") {
    const HeightField field = rampField();
    const rm::sim::Terrain terrain{field};
    std::vector<rm::sim::Transform> units{unitAt(100.0f, 100.0f)};
    std::vector<MoveState> motion{ordinary()};
    motion[0].airborne = true;
    rm::sim::orderTo(motion[0], terrain, rm::test::fx(400.0f), rm::test::fx(100.0f));

    for (int tick = 0; tick < 20; ++tick) {
        rm::sim::tick(units, motion, terrain);
        CHECK(units[0].y == terrain.heightAt(units[0].x, units[0].z)
                                  + rm::sim::kAirClearanceElmos);
        CHECK(units[0].pitch == rm::Brad{0});
        CHECK(units[0].roll == rm::Brad{0});
    }
}

TEST_CASE("aircraft and ground units do not push each other") {
    const HeightField field = flatField();
    Crowd crowd;
    crowd.add(100.0f, 100.0f, false);
    crowd.add(100.0f, 100.0f, true);
    crowd.separate(field);

    CHECK(crowd.at(0).x == rm::test::fx(100.0f));
    CHECK(crowd.at(0).z == rm::test::fx(100.0f));
    CHECK(crowd.at(1).x == rm::test::fx(100.0f));
    CHECK(crowd.at(1).z == rm::test::fx(100.0f));
    CHECK(crowd.at(1).y == rm::sim::kAirClearanceElmos);
}

TEST_CASE("aircraft sharing an altitude still separate") {
    const HeightField field = flatField();
    Crowd crowd;
    crowd.add(100.0f, 100.0f, true);
    crowd.add(100.0f, 100.0f, true);
    crowd.separate(field);

    CHECK(rm::sim::fxHypot(crowd.at(1).x - crowd.at(0).x,
                           crowd.at(1).z - crowd.at(0).z)
          > rm::sim::Fx{});
    CHECK(crowd.at(0).y == rm::sim::kAirClearanceElmos);
    CHECK(crowd.at(1).y == rm::sim::kAirClearanceElmos);
}

TEST_CASE("surface ships stay level at the waterline over an uneven seabed") {
    const HeightField field = rampField();
    const rm::sim::Terrain terrain{field, true, 500.0f};
    std::vector<rm::sim::Transform> units{unitAt(100.0f, 100.0f)};
    std::vector<MoveState> motion{ordinary()};
    motion[0].surfaceWater = true;
    rm::sim::orderTo(motion[0], terrain, rm::test::fx(400.0f), rm::test::fx(100.0f));

    for (int tick = 0; tick < 20; ++tick) {
        rm::sim::tick(units, motion, terrain);
        CHECK(units[0].y == rm::test::fx(500.0f));
        CHECK(units[0].pitch == rm::Brad{0});
        CHECK(units[0].roll == rm::Brad{0});
    }
}

TEST_CASE("surface ships and ground units do not push each other") {
    const HeightField field = flatField();
    Crowd crowd;
    crowd.add(100.0f, 100.0f, false, false);
    crowd.add(100.0f, 100.0f, false, true);
    crowd.separate(field);

    CHECK(crowd.at(0).x == rm::test::fx(100.0f));
    CHECK(crowd.at(0).z == rm::test::fx(100.0f));
    CHECK(crowd.at(1).x == rm::test::fx(100.0f));
    CHECK(crowd.at(1).z == rm::test::fx(100.0f));
}

TEST_CASE("surface ships sharing the waterline still separate") {
    const HeightField field = flatField();
    Crowd crowd;
    crowd.add(100.0f, 100.0f, false, true);
    crowd.add(100.0f, 100.0f, false, true);
    crowd.separate(field);

    CHECK(rm::sim::fxHypot(crowd.at(1).x - crowd.at(0).x,
                           crowd.at(1).z - crowd.at(0).z)
          > rm::sim::Fx{});
}

TEST_CASE("collision separation does not push a surface ship into a blocked water cell") {
    const HeightField field = flatField();
    const rm::sim::Terrain terrain{field, true, 10.0f};
    Crowd crowd;
    crowd.add(110.0f, 32.0f, false, true);
    crowd.add(111.0f, 32.0f, false, true);
    crowd.store.reindex(rm::sim::Fx::fromInt(64));

    rm::sim::PassabilityGrid water;
    water.cellsX = 3;
    water.cellsZ = 1;
    water.elmosPerCell = rm::sim::Fx::fromInt(64);
    water.passable = {1, 1, 0};
    const std::array<const rm::sim::PassabilityGrid*, 1> grids{{&water}};

    rm::sim::resolveCollisions(crowd.store, terrain, grids);

    for (std::size_t i = 0; i < crowd.size(); ++i) {
        CHECK(rm::sim::sitePlaceable(water, crowd.at(i).x, crowd.at(i).z,
                                     crowd.motionAt(i).radiusElmos));
    }
}

TEST_CASE("collision separation does not push a land unit into a blocked cell") {
    const HeightField field = flatField();
    Crowd crowd;
    crowd.add(110.0f, 32.0f);
    crowd.add(111.0f, 32.0f);
    crowd.store.reindex(rm::sim::Fx::fromInt(64));

    rm::sim::PassabilityGrid land;
    land.cellsX = 3;
    land.cellsZ = 1;
    land.elmosPerCell = rm::sim::Fx::fromInt(64);
    land.passable = {1, 1, 0};
    const std::array<const rm::sim::PassabilityGrid*, 1> grids{{&land}};

    rm::sim::resolveCollisions(crowd.store, rm::sim::Terrain{field}, grids);

    for (std::size_t i = 0; i < crowd.size(); ++i) {
        CHECK(rm::sim::sitePlaceable(land, crowd.at(i).x, crowd.at(i).z,
                                     crowd.motionAt(i).radiusElmos));
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
    instances[0].y = rm::test::fx(123.0f);
    std::vector<MoveState> motion{ordinary()};

    run(instances, motion, field, 60);

    CHECK(rm::test::asFloat(instances[0].x) == Approx(100.0f));
    CHECK(rm::test::asFloat(instances[0].z) == Approx(100.0f));
    // Tick historically aligns an idle ground unit's slope without relocating its height.
    CHECK(rm::test::asFloat(instances[0].y) == Approx(123.0f));
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
        Crowd crowd;
        crowd.add(400.0f, 400.0f);
        crowd.add(400.0f, 400.0f);

        // Exactly coincident is the degenerate case: there is no direction to
        // push along, and a naive normalise divides by zero.
        crowd.separate(field);

        const rm::sim::Fx apart =
            rm::sim::fxHypot(crowd.at(0).x - crowd.at(1).x, crowd.at(0).z - crowd.at(1).z);
        CHECK(apart > rm::sim::Fx{});
        // `isfinite` is gone, and cannot come back: a fixed-point value has no infinity and
        // no NaN to check for. The degenerate case that produced them — dividing by a zero
        // separation — now saturates instead, which is a wrong number a test can see rather
        // than a poison one that spreads.
        CHECK(crowd.at(0).x.raw() != INT32_MAX);
        CHECK(crowd.at(1).x.raw() != INT32_MAX);

        // A few passes should reach the full separation.
        for (int i = 0; i < 20; ++i) {
            crowd.separate(field);
        }
        const float settled = rm::test::asFloat(
            rm::sim::fxHypot(crowd.at(0).x - crowd.at(1).x, crowd.at(0).z - crowd.at(1).z));
        CHECK(settled >= Approx(rm::test::asFloat(crowd.motionAt(0).radiusElmos
                                                  + crowd.motionAt(1).radiusElmos))
                             .epsilon(0.05));
    }

    SECTION("units already clear of each other do not move") {
        Crowd crowd;
        crowd.add(100.0f, 100.0f);
        crowd.add(500.0f, 500.0f);

        crowd.separate(field);

        CHECK(rm::test::asFloat(crowd.at(0).x) == Approx(100.0f));
        CHECK(rm::test::asFloat(crowd.at(1).x) == Approx(500.0f));
    }

    SECTION("a mobile unit moves around an immobile structure") {
        Crowd crowd;
        crowd.add(400.0f, 400.0f);
        crowd.add(401.0f, 400.0f);
        crowd.store.motion()[0].speedPerTick = rm::sim::Fx{};
        const rm::sim::Transform fixed = crowd.at(0);

        crowd.separate(field);

        CHECK(crowd.at(0).x == fixed.x);
        CHECK(crowd.at(0).z == fixed.z);
        CHECK(crowd.at(1).x > rm::test::fx(401.0f));
    }

    SECTION("ground collision placement preserves the established tilt") {
        Crowd crowd;
        crowd.add(400.0f, 400.0f);
        crowd.add(400.0f, 400.0f);
        crowd.at(0).pitch = rm::Brad{123};
        crowd.at(0).roll = rm::Brad{456};

        crowd.separate(field);

        CHECK(crowd.at(0).pitch == rm::Brad{123});
        CHECK(crowd.at(0).roll == rm::Brad{456});
    }

    SECTION("a crowd spreads out instead of stacking") {
        // Thirty units dumped on one point, which is exactly what a rally order
        // produces once pathfinding works.
        Crowd crowd;
        for (int i = 0; i < 30; ++i) {
            crowd.add(400.0f, 400.0f);
        }

        for (int i = 0; i < 120; ++i) {
            crowd.separate(field);
        }

        std::size_t overlapping = 0;
        for (std::size_t a = 0; a < crowd.size(); ++a) {
            for (std::size_t b = a + 1; b < crowd.size(); ++b) {
                const rm::sim::Fx d =
                    rm::sim::fxHypot(crowd.at(a).x - crowd.at(b).x,
                                     crowd.at(a).z - crowd.at(b).z);
                if (d < (crowd.motionAt(a).radiusElmos + crowd.motionAt(b).radiusElmos)
                            * rm::sim::Fx::fromRatio(4, 5)) {
                    ++overlapping;
                }
            }
        }
        CHECK(overlapping == 0);
    }

    SECTION("separation keeps units on the map and on the ground") {
        const HeightField ramp = rampField();
        Crowd crowd;
        for (int i = 0; i < 3; ++i) {
            crowd.add(0.0f, 0.0f);
        }

        for (int i = 0; i < 30; ++i) {
            crowd.separate(ramp);
        }

        for (std::size_t i = 0; i < crowd.size(); ++i) {
            const rm::sim::Transform& unit = crowd.at(i);
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
    // pass took a list of arrays. Now there is ONE store for the whole match, so the
    // bug is gone by construction and this case only proves the construction: a tank
    // and a bot are two slots of the same arrays and cannot be missed by an inner loop
    // that never restarts.
    const HeightField field = flatField();

    Crowd crowd;
    crowd.add(400.0f, 400.0f);
    crowd.add(400.0f, 400.0f);

    for (int i = 0; i < 20; ++i) {
        crowd.separate(field);
    }

    const float apart = rm::test::asFloat(
        rm::sim::fxHypot(crowd.at(0).x - crowd.at(1).x, crowd.at(0).z - crowd.at(1).z));
    CHECK(apart >= Approx(rm::test::asFloat(crowd.motionAt(0).radiusElmos
                                            + crowd.motionAt(1).radiusElmos))
                       .epsilon(0.05));
}

TEST_CASE("a crowd of one model still separates from itself") {
    // The flat form must not lose what the grouped one did.
    const HeightField field = flatField();

    Crowd crowd;
    for (int i = 0; i < 8; ++i) {
        crowd.add(400.0f, 400.0f);
    }

    for (int i = 0; i < 80; ++i) {
        crowd.separate(field);
    }

    for (std::size_t a = 0; a < crowd.size(); ++a) {
        for (std::size_t b = a + 1; b < crowd.size(); ++b) {
            const rm::sim::Fx d =
                rm::sim::fxHypot(crowd.at(a).x - crowd.at(b).x, crowd.at(a).z - crowd.at(b).z);
            REQUIRE(d > (crowd.motionAt(a).radiusElmos + crowd.motionAt(b).radiusElmos)
                            * rm::sim::Fx::fromRatio(4, 5));
        }
    }
}

TEST_CASE("an empty or single-unit store is harmless") {
    const HeightField field = flatField();

    Crowd empty;
    CHECK_NOTHROW(empty.separate(field));

    Crowd one;
    one.add(100.0f, 100.0f);
    one.separate(field);
    CHECK(rm::test::asFloat(one.at(0).x) == Approx(100.0f));
}

TEST_CASE("a stale index answers about where units were") {
    // The price of an index that is not self-maintaining, written down as a test rather than
    // left as a comment. `resolveCollisions` reads the store's spatial index, so a caller that
    // moves units and forgets to reindex gets separation computed against last tick's
    // positions — a wrong answer rather than a crash, which is exactly the kind of failure
    // that hides. `tickSkirmish` is the one place that owns the rebuild points.
    const HeightField field = flatField();

    Crowd crowd;
    crowd.add(100.0f, 100.0f);
    crowd.add(500.0f, 500.0f);
    crowd.separate(field);  // indexes them where they are: far apart, nothing to do

    // Now shove them together WITHOUT reindexing, and separate again. The index still says
    // they are 400 elmos apart, so neither is a candidate for the other and they stay stacked.
    crowd.at(1).x = rm::test::fx(100.0f);
    crowd.at(1).z = rm::test::fx(100.0f);
    rm::sim::resolveCollisions(crowd.store, rm::sim::Terrain{field});
    CHECK(rm::test::asFloat(rm::sim::fxHypot(crowd.at(0).x - crowd.at(1).x,
                                             crowd.at(0).z - crowd.at(1).z))
          == Approx(0.0f));

    // Reindex and they separate, which is what the tick does.
    crowd.separate(field);
    CHECK(rm::sim::fxHypot(crowd.at(0).x - crowd.at(1).x, crowd.at(0).z - crowd.at(1).z)
          > rm::sim::Fx{});
}

TEST_CASE("a mobile unit is born able to move, whichever spawn path made it",
          "[movement][scene]") {
    // THE BUG THIS EXISTS FOR, and it is the one the whole order path is invisible to. There
    // are two spawn paths; `spawnCommanders` set the army index and nothing else, so every
    // commander in the game — the first unit of every match and the one the player drives —
    // started with `speedPerTick` and `turnPerTick` at zero, which are `MoveState`'s deliberate
    // defaults, and with no collision radius either.
    //
    // Nothing an assertion aimed at ordering could see: `findPath` returns a route,
    // `applyCommand` accepts it, `moving` goes true, the queue line is drawn to the
    // destination, and no refusal is printed. `Movement::tick` then multiplies its step by a
    // speed of zero, forever. A headless `--march` reports "2 of 2 units routed" and is telling
    // the truth about routing while saying nothing whatsoever about motion.
    rm::unitdef::UnitDef tank;
    tank.name = "UEL0201";
    tank.motion = rm::unitdef::MotionType::Land;
    tank.speedElmosPerSecond = 14.0f;
    tank.turnRateRadiansPerSecond = 1.57f;
    tank.collisionRadiusElmos = 4.0f;

    const rm::sim::MoveState moving = rm::app::motionFor(tank, 0);
    CHECK(moving.armyIndex == 0);
    CHECK(moving.speedPerTick > rm::sim::Fx{});
    CHECK(moving.turnPerTick > 0);
    CHECK(moving.radiusElmos > rm::sim::Fx{});
    CHECK_FALSE(moving.airborne);

    // A unit whose blueprint states no turn rate still turns — otherwise it would pivot for
    // ever at the destination it is already facing away from.
    rm::unitdef::UnitDef turnless = tank;
    turnless.turnRateRadiansPerSecond = 0.0f;
    CHECK(rm::app::motionFor(turnless, 0).turnPerTick > 0);

    // A STRUCTURE KEEPS THE ZEROES, which is what makes it a structure as far as movement is
    // concerned — the fix must not hand every building a speed.
    rm::unitdef::UnitDef factory;
    factory.name = "UEB0101";
    factory.motion = rm::unitdef::MotionType::None;
    factory.collisionRadiusElmos = 17.6f;
    const rm::sim::MoveState still = rm::app::motionFor(factory, 1);
    CHECK(still.speedPerTick == rm::sim::Fx{});
    CHECK(still.turnPerTick == 0);
    CHECK(still.radiusElmos > rm::sim::Fx{});  // it still occupies ground

    // Air is flagged from the definition, because collision and height both read it.
    rm::unitdef::UnitDef bomber = tank;
    bomber.motion = rm::unitdef::MotionType::Air;
    CHECK(rm::app::motionFor(bomber, 0).airborne);
}

namespace {

/// A flyer with UEA0101's control numbers (`C-221`, `C-244`): KMove 1, KMoveDamping 1,
/// KLift 3, KLiftDamping 2.5, climb authority 7 elmos/s, auto-land after 10 idle ticks,
/// elevation 80. Speed is written directly — 16 elmos/tick is MaxAirspeed 20 at this
/// file's 10 Hz clock.
[[nodiscard]] MoveState flyer() {
    MoveState motion = ordinary();
    motion.canFly = true;
    motion.airState = MoveState::AirState::Top;
    motion.airborne = true;
    motion.speedPerTick = rm::sim::Fx::fromInt(16);
    motion.airMaxSpeedElmosPerSec = rm::sim::Fx::fromInt(160);
    motion.airKMove = rm::sim::Fx::fromInt(1);
    motion.airKMoveDamping = rm::sim::Fx::fromInt(1);
    motion.airKLift = rm::sim::Fx::fromInt(3);
    motion.airKLiftDamping = rm::sim::Fx::fromRatio(5, 2);
    motion.airLiftFactor = rm::sim::Fx::fromInt(7);
    motion.airElevation = rm::sim::Fx::fromInt(80);
    motion.idleLandThreshold = 10;
    motion.fuelDrainPerTick = rm::sim::Fx::fromRatio(1, 5000);
    motion.fuelRatio = rm::sim::Fx::fromInt(1);
    return motion;
}

}  // namespace

TEST_CASE("a flyer integrates velocity trapezoidally") {
    // `C-221`: `pos += (v_old + v_new) x 0.05` with `v_new = v_old + a*dt`, velocity in
    // elmos per SECOND — per-tick velocity would fly every leg at a tenth of its authored
    // speed. Hand-computed (`C-244`): standing start toward +Z at a 160-elmos/s cruise,
    // KMove 1 and nothing to damp gives a = 160, v = 16 after one tick and
    // z += (0 + 16) x 0.05 = 0.8. Level flight: at cruise elevation with no lift
    // authority the lift law returns its zero cap, so nothing climbs.
    const HeightField field = flatField();
    const rm::sim::Terrain terrain{field};
    std::vector<rm::sim::Transform> units{unitAt(100.0f, 100.0f)};
    units[0].y = rm::sim::Fx::fromInt(80);
    std::vector<MoveState> motion{flyer()};
    motion[0].altitudeRef = rm::sim::Fx::fromInt(80);
    motion[0].airLiftFactor = rm::sim::Fx{};
    rm::sim::orderTo(motion[0], terrain, rm::test::fx(100.0f), rm::test::fx(700.0f));

    rm::sim::tick(units, motion, terrain);

    CHECK(rm::sim::fxToFloat(motion[0].velocity[2]) == Approx(16.0f).margin(0.1));
    CHECK(rm::sim::fxToFloat(motion[0].velocity[0]) == Approx(0.0f).margin(0.01));
    CHECK(rm::sim::fxToFloat(units[0].z) == Approx(100.8f).margin(0.01));
    CHECK(rm::sim::fxToFloat(units[0].y) == Approx(80.0f).margin(0.01));
}

TEST_CASE("the horizontal damping is KMove unless the controller is faster than its desire") {
    // `C-244`: `s = max(1, min(|desired|, KMove))`; `KMove` when `KMove <= s`, otherwise
    // `min(KMove / s, KMoveDamping)`. At cruise every shipped aircraft sits in the first
    // branch; the fast controllers (KMove 1.5 and 4) reach the second on final approach.
    using rm::sim::Fx;
    const Fx one = Fx::fromInt(1);
    // URA0102: KMove 1 against a cruise-speed desire.
    CHECK(rm::sim::airDampingFactor(one, one, Fx::fromInt(120)) == one);
    // A KMove of 4 against a desire of 2: s = 2, KMove > s, 4 / 2 = 2 under a damping of 10.
    CHECK(rm::sim::airDampingFactor(Fx::fromInt(4), Fx::fromInt(10), Fx::fromInt(2))
          == Fx::fromInt(2));
    // The same controller nearly at rest: s floors at 1, and KMoveDamping caps the ratio.
    CHECK(rm::sim::airDampingFactor(Fx::fromInt(4), Fx::fromInt(3), Fx::fromRatio(1, 2))
          == Fx::fromInt(3));
}

TEST_CASE("the lift law caps a fast climb and lifts a slow flyer to half elevation") {
    // `C-245`: `cap = (speedRatio - 0.5) x LiftFactor`. With lift, the need is capped by
    // it; without, a flyer under half its elevation gets exactly the climb to that half,
    // and one already there gets the non-positive cap.
    using rm::sim::Fx;
    const Fx lift7 = Fx::fromInt(7);
    const Fx half = Fx::fromRatio(1, 2);
    // Full speed, far reference: 0.5 x 7 = 3.5.
    CHECK(rm::sim::wingedLift(Fx::fromInt(100), Fx::fromInt(1), lift7, Fx::fromInt(80),
                              Fx::fromInt(80)) == Fx::fromRatio(7, 2));
    // Full speed, one elmo short: the need wins.
    CHECK(rm::sim::wingedLift(Fx::fromInt(1), Fx::fromInt(1), lift7, Fx::fromInt(80),
                              Fx::fromInt(80)) == Fx::fromInt(1));
    // On the deck at rest: straight up to half of 80.
    CHECK(rm::sim::wingedLift(Fx::fromInt(80), Fx{}, lift7, Fx{}, Fx::fromInt(80))
          == Fx::fromInt(40));
    // Slow but already above half: the cap, which is now negative.
    CHECK(rm::sim::wingedLift(Fx::fromInt(20), half * half, lift7, Fx::fromInt(60),
                              Fx::fromInt(80)) == (half * half - half) * lift7);
}

TEST_CASE("climb authority above half elevation dies below half max airspeed") {
    // `C-221`/`C-245`: a flyer at 0.04 of max speed already above half its elevation
    // cannot climb, while one at full speed climbs at most 0.5 x 7 = 3.5 elmos/s against
    // a far reference — KLift 3 turns that into 1.05 elmos/s of velocity in one beat.
    const HeightField field = flatField();
    const rm::sim::Terrain terrain{field};

    SECTION("slow flyer above half elevation does not climb") {
        std::vector<rm::sim::Transform> units{unitAt(100.0f, 100.0f)};
        units[0].y = rm::sim::Fx::fromInt(60);
        std::vector<MoveState> motion{flyer()};
        motion[0].velocity = {rm::sim::Fx::fromInt(6), rm::sim::Fx{},
                              rm::sim::Fx{}};
        motion[0].altitudeRef = rm::sim::Fx::fromInt(100);
        motion[0].moving = true;
        rm::sim::tick(units, motion, terrain);
        CHECK(units[0].y <= rm::sim::Fx::fromInt(60));
    }

    SECTION("fast flyer climbs within the cap") {
        std::vector<rm::sim::Transform> units{unitAt(100.0f, 100.0f)};
        units[0].y = rm::sim::Fx::fromInt(60);
        std::vector<MoveState> motion{flyer()};
        motion[0].velocity = {rm::sim::Fx{}, rm::sim::Fx{}, rm::sim::Fx::fromInt(160)};
        motion[0].altitudeRef = rm::sim::Fx::fromInt(100);
        // Straight ahead: no turn couples into the horizontal speed the lift law reads.
        rm::sim::orderTo(motion[0], terrain, rm::test::fx(100.0f), rm::test::fx(700.0f));
        rm::sim::tick(units, motion, terrain);
        CHECK(units[0].y > rm::sim::Fx::fromInt(60));
        CHECK(rm::sim::fxToFloat(motion[0].velocity[1]) == Approx(1.05f).margin(0.01));
    }
}

TEST_CASE("a slow flyer lifts straight up to half its elevation before moving forward") {
    // `C-245`: at rest the lift cap is negative, so the lift law climbs toward half the
    // elevation; below that half and under 0.08 of cruise the horizontal desire is held
    // back by height-over-elevation (`0x006c66e3`), which is zero on the deck. So the
    // first beat rises and does not advance — the retail lift-off, not a runway roll.
    const HeightField field = flatField();
    const rm::sim::Terrain terrain{field};
    std::vector<rm::sim::Transform> units{unitAt(100.0f, 100.0f)};
    std::vector<MoveState> motion{flyer()};
    motion[0].airState = MoveState::AirState::Bottom;
    motion[0].airborne = false;
    motion[0].altitudeRef = rm::sim::Fx::fromInt(80);
    rm::sim::orderTo(motion[0], terrain, rm::test::fx(100.0f), rm::test::fx(700.0f));

    rm::sim::tick(units, motion, terrain);
    CHECK(motion[0].airState == MoveState::AirState::Up);
    CHECK(motion[0].airborne);
    // KLift 3 x (40 - 0) = 120 elmos/s2 -> 12 elmos/s -> 0.6 elmos this beat.
    CHECK(rm::sim::fxToFloat(units[0].y) == Approx(0.6f).margin(0.01));
    CHECK(motion[0].velocity[2] == rm::sim::Fx{});
    CHECK(rm::sim::fxToFloat(units[0].z) == Approx(100.0f).margin(0.001));

    // As it rises the gate opens in proportion, so forward motion begins before half
    // elevation and cruise follows.
    for (int tick = 0; tick < 20; ++tick) {
        rm::sim::tick(units, motion, terrain);
    }
    CHECK(motion[0].velocity[2] > rm::sim::Fx{});
    CHECK(units[0].y > rm::sim::Fx::fromInt(10));
}

TEST_CASE("a hill ahead raises the altitude reference and holds the forward desire back") {
    // `C-246`: the reference chases the highest surface within five seconds of cruise (or
    // the destination, if nearer), and when that climb exceeds a second of lift the nearer
    // half-reach scan decides the hold-back: (halfReach - max(0, 1.5 x nearHeight - y)) /
    // halfReach, floored at 0.2, squared. Here a 600-elmo wall starts 100 elmos ahead: the far cell
    // sees it (reference target 680, slewed 0.7 per beat from 80), the near cell sees it
    // too, so the desire is cut to 0.04 of cruise: v = 160 x 0.04 x 0.1 = 0.64.
    HeightField field = flatField();
    for (int z = 25; z < field.verticesZ(); ++z) {
        for (int x = 0; x < field.verticesX(); ++x) {
            field.raw[static_cast<std::size_t>(z) * static_cast<std::size_t>(field.verticesX())
                      + static_cast<std::size_t>(x)] = 600;
        }
    }
    const rm::sim::Terrain terrain{field};
    std::vector<rm::sim::Transform> units{unitAt(100.0f, 100.0f)};
    units[0].y = rm::sim::Fx::fromInt(80);
    std::vector<MoveState> motion{flyer()};
    motion[0].altitudeRef = rm::sim::Fx::fromInt(80);
    rm::sim::orderTo(motion[0], terrain, rm::test::fx(100.0f), rm::test::fx(700.0f));

    rm::sim::tick(units, motion, terrain);

    CHECK(rm::sim::fxToFloat(motion[0].altitudeRef) == Approx(80.7f).margin(0.01));
    CHECK(rm::sim::fxToFloat(motion[0].velocity[2]) == Approx(0.64f).margin(0.01));

    // The same flyer on a flat field wants the full 16.
    const HeightField flat = flatField();
    const rm::sim::Terrain level{flat};
    std::vector<rm::sim::Transform> units2{unitAt(100.0f, 100.0f)};
    units2[0].y = rm::sim::Fx::fromInt(80);
    std::vector<MoveState> motion2{flyer()};
    motion2[0].altitudeRef = rm::sim::Fx::fromInt(80);
    rm::sim::orderTo(motion2[0], level, rm::test::fx(100.0f), rm::test::fx(700.0f));
    rm::sim::tick(units2, motion2, level);
    CHECK(rm::sim::fxToFloat(motion2[0].velocity[2]) == Approx(16.0f).margin(0.1));
    CHECK(motion2[0].altitudeRef == rm::sim::Fx::fromInt(80));
}

TEST_CASE("a grounded flyer lifts, cruises and lands on arrival") {
    // The full loop with no invented thresholds: an order takes a Bottom flyer Up and off
    // the deck the same beat (`C-245`), speed opens the climb, reaching the reference
    // levels to Top, arrival commits Down, touchdown returns Bottom with the airborne
    // flag cleared. Lift authority is raised for this transition coverage so the climb
    // finishes before arrival on an 800-elmo field; the B-tests above pin the real
    // UEA0101 pacing, at which a short hop lands without ever levelling off (see below).
    const HeightField field = flatField();
    const rm::sim::Terrain terrain{field};
    std::vector<rm::sim::Transform> units{unitAt(100.0f, 100.0f)};
    std::vector<MoveState> motion{flyer()};
    motion[0].airState = MoveState::AirState::Bottom;
    motion[0].airborne = false;
    motion[0].altitudeRef = rm::sim::Fx::fromInt(80);
    motion[0].airLiftFactor = rm::sim::Fx::fromInt(70);
    rm::sim::orderTo(motion[0], terrain, rm::test::fx(700.0f), rm::test::fx(700.0f));

    // First tick: committed to takeoff and already rising.
    rm::sim::tick(units, motion, terrain);
    CHECK(motion[0].airState == MoveState::AirState::Up);
    CHECK(motion[0].airborne);
    CHECK(units[0].y > rm::sim::Fx{});

    bool sawTop = false;
    bool sawDown = false;
    for (int tick = 0; tick < 1000; ++tick) {
        rm::sim::tick(units, motion, terrain);
        sawTop = sawTop || motion[0].airState == MoveState::AirState::Top;
        sawDown = sawDown || motion[0].airState == MoveState::AirState::Down;
        if (motion[0].airState == MoveState::AirState::Bottom) {
            break;
        }
    }
    CHECK(sawTop);
    CHECK(sawDown);
    REQUIRE(motion[0].airState == MoveState::AirState::Bottom);
    CHECK_FALSE(motion[0].airborne);
    CHECK(units[0].y == rm::sim::Fx{});
}

TEST_CASE("a short hop lands without ever levelling off") {
    // The retail-realistic common case at UEA0101 pacing: the destination arrives long
    // before the 3.5-elmos/s climb could level off, so the loop runs Bottom-Up-Down-
    // Bottom with no Top — and, crucially, terminates instead of stranding Down.
    const HeightField field = flatField();
    const rm::sim::Terrain terrain{field};
    std::vector<rm::sim::Transform> units{unitAt(100.0f, 100.0f)};
    std::vector<MoveState> motion{flyer()};
    motion[0].airState = MoveState::AirState::Bottom;
    motion[0].airborne = false;
    motion[0].altitudeRef = rm::sim::Fx::fromInt(80);
    rm::sim::orderTo(motion[0], terrain, rm::test::fx(160.0f), rm::test::fx(100.0f));

    bool sawDown = false;
    for (int tick = 0; tick < 1000; ++tick) {
        rm::sim::tick(units, motion, terrain);
        sawDown = sawDown || motion[0].airState == MoveState::AirState::Down;
        if (motion[0].airState == MoveState::AirState::Bottom && tick > 0) {
            break;
        }
    }
    CHECK(sawDown);
    REQUIRE(motion[0].airState == MoveState::AirState::Bottom);
    CHECK_FALSE(motion[0].airborne);
    CHECK(units[0].y == rm::sim::Fx{});
}

TEST_CASE("an idle flyer auto-lands after its auto-land interval") {
    // `floor(AutoLandTime x 10)` idle ticks (`C-222`): ten here, so nine ticks of
    // loitering and the tenth commits Down.
    const HeightField field = flatField();
    const rm::sim::Terrain terrain{field};
    std::vector<rm::sim::Transform> units{unitAt(100.0f, 100.0f)};
    units[0].y = rm::sim::Fx::fromInt(80);
    std::vector<MoveState> motion{flyer()};
    motion[0].altitudeRef = rm::sim::Fx::fromInt(80);

    for (int tick = 0; tick < 9; ++tick) {
        rm::sim::tick(units, motion, terrain);
        CHECK(motion[0].airState == MoveState::AirState::Top);
    }
    rm::sim::tick(units, motion, terrain);
    CHECK(motion[0].airState == MoveState::AirState::Down);
}

TEST_CASE("a parked flyer recharges fuel while it waits") {
    // `C-223`: recharge runs while the vert event is Bottom. A grounded aircraft with no
    // orders is exactly that, and the idle skip in the tick must not starve it — at a
    // drain of one sixteenth (exact in fixed point), four ticks take a half tank to three
    // quarters, eight fill it, and the clamp holds there.
    const HeightField field = flatField();
    const rm::sim::Terrain terrain{field};
    std::vector<rm::sim::Transform> units{unitAt(100.0f, 100.0f)};
    std::vector<MoveState> motion{flyer()};
    motion[0].airState = MoveState::AirState::Bottom;
    motion[0].airborne = false;
    motion[0].fuelDrainPerTick = rm::sim::Fx::fromRatio(1, 16);
    motion[0].fuelRatio = rm::sim::Fx::fromRatio(1, 2);

    run(units, motion, field, 4);
    CHECK(motion[0].fuelRatio == rm::sim::Fx::fromRatio(3, 4));
    run(units, motion, field, 10);
    CHECK(motion[0].fuelRatio == rm::sim::Fx::fromInt(1));
    CHECK(motion[0].airState == MoveState::AirState::Bottom);
    CHECK(units[0].y == rm::sim::Fx{});
}

TEST_CASE("fuel drains in flight, clamps at zero, and means nothing natively") {
    // `1 / (FuelUseTime x 10)` per tick while off the ground (`C-223`): at drain 0.1,
    // ten ticks empty the tank and it stays empty — no speed penalty, no crash, every
    // consequence Lua, so the assertions stop at the clamp.
    const HeightField field = flatField();
    const rm::sim::Terrain terrain{field};
    std::vector<rm::sim::Transform> units{unitAt(100.0f, 100.0f)};
    std::vector<MoveState> motion{flyer()};
    motion[0].altitudeRef = rm::sim::Fx{};
    motion[0].fuelDrainPerTick = rm::sim::Fx::fromRatio(1, 10);
    motion[0].fuelRatio = rm::sim::Fx::fromInt(1);
    motion[0].moving = true;

    run(units, motion, field, 11);
    CHECK(motion[0].fuelRatio == rm::sim::Fx{});
    run(units, motion, field, 10);
    CHECK(motion[0].fuelRatio == rm::sim::Fx{});
}
