// Where a unit is DRAWN, between two ticks.
//
// §7 P7.2's claim is that alpha 0 and 1 reproduce the ENDPOINTS EXACTLY — not to a tolerance.
// That is what makes `--no-interpolate` and a golden screenshot mean anything, and it is why the
// arithmetic is `(1 - t) * a + t * b` rather than `a + (b - a) * t`. The rest of these cases are
// the ways interpolation goes wrong that a still image never shows: a turn taking the long way
// round, and a newborn unit sliding out of the grave of whatever used to hold its slot.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "core/scene/UnitDraw.hpp"
#include "core/sim/Snapshot.hpp"

#include "support/FxMatchers.hpp"

#include <numbers>
#include <vector>

using Catch::Approx;
using rm::DrawUnit;
using rm::sim::Snapshot;
using rm::sim::UnitId;
using rm::sim::UnitView;

namespace {

/// A snapshot built by hand, so a case can put a unit exactly where it wants it.
[[nodiscard]] Snapshot made(rm::TickIndex tick, std::vector<UnitView> units) {
    Snapshot taken;
    taken.tick = tick;
    taken.units = std::move(units);
    return taken;
}

[[nodiscard]] UnitView unitAt(UnitId id, float x, float z, float yawRadians = 0.0f) {
    UnitView view;
    view.id = id;
    view.transform.x = rm::test::fx(x);
    view.transform.z = rm::test::fx(z);
    view.transform.heading = rm::sim::bradFromRadians(yawRadians);
    view.health = rm::sim::Mag::fromInt(100);
    view.maxHealth = rm::sim::Mag::fromInt(100);
    return view;
}

} // namespace

TEST_CASE("alpha 0 and 1 reproduce the endpoints exactly") {
    // §7 P7.2's stated test. EXACTLY, not to a tolerance — the reason `(1 - t) * a + t * b` is
    // used rather than `a + (b - a) * t`, which re-rounds twice and misses `b` by a bit or two.
    const UnitId id{0, 1};
    const Snapshot from = made(10, {unitAt(id, 100.0f, 200.0f, 0.5f)});
    const Snapshot to = made(11, {unitAt(id, 180.0f, 260.0f, 1.25f)});

    std::vector<DrawUnit> out;

    rm::interpolate(from, to, 0.0f, out);
    REQUIRE(out.size() == 1);
    CHECK(out[0].position[0] == rm::sim::fxToFloat(from.units[0].transform.x));
    CHECK(out[0].position[2] == rm::sim::fxToFloat(from.units[0].transform.z));
    CHECK(out[0].rotationY == rm::sim::radiansFromBrad(from.units[0].transform.heading));

    rm::interpolate(from, to, 1.0f, out);
    REQUIRE(out.size() == 1);
    CHECK(out[0].position[0] == rm::sim::fxToFloat(to.units[0].transform.x));
    CHECK(out[0].position[2] == rm::sim::fxToFloat(to.units[0].transform.z));
    CHECK(out[0].rotationY == rm::sim::radiansFromBrad(to.units[0].transform.heading));
}

TEST_CASE("halfway is halfway") {
    const UnitId id{0, 1};
    const Snapshot from = made(10, {unitAt(id, 100.0f, 200.0f)});
    const Snapshot to = made(11, {unitAt(id, 200.0f, 400.0f)});

    std::vector<DrawUnit> out;
    rm::interpolate(from, to, 0.5f, out);
    REQUIRE(out.size() == 1);
    CHECK(out[0].position[0] == Approx(150.0f));
    CHECK(out[0].position[2] == Approx(300.0f));
}

TEST_CASE("alpha is clamped rather than extrapolated") {
    // A frame that arrives late would otherwise extrapolate, and extrapolating a unit that has
    // stopped walks it through whatever is in front of it.
    const UnitId id{0, 1};
    const Snapshot from = made(10, {unitAt(id, 0.0f, 0.0f)});
    const Snapshot to = made(11, {unitAt(id, 100.0f, 0.0f)});

    std::vector<DrawUnit> out;
    rm::interpolate(from, to, 2.5f, out);
    CHECK(out[0].position[0] == Approx(100.0f));
    rm::interpolate(from, to, -1.0f, out);
    CHECK(out[0].position[0] == Approx(0.0f));
}

TEST_CASE("a turn through north takes the short way round") {
    // The classic angle bug. A unit rotating from just-west-of-north to just-east-of-north goes
    // from ~65,000 to ~500 in `Brad`; lerping those as numbers spins it 350 degrees the wrong
    // way at high speed. `Brad`'s own difference is already the shortest arc.
    const UnitId id{0, 1};
    const float nearlyFull = 2.0f * std::numbers::pi_v<float> - 0.1f;
    const Snapshot from = made(10, {unitAt(id, 0.0f, 0.0f, nearlyFull)});
    const Snapshot to = made(11, {unitAt(id, 0.0f, 0.0f, 0.1f)});

    std::vector<DrawUnit> out;
    rm::interpolate(from, to, 0.5f, out);
    REQUIRE(out.size() == 1);

    // Halfway through a 0.2-radian turn across zero is within a hair of zero — NOT near pi,
    // which is where the long way round would put it.
    const float halfway = out[0].rotationY;
    const bool nearZero = halfway < 0.05f
                          || halfway > 2.0f * std::numbers::pi_v<float> - 0.05f;
    CHECK(nearZero);
}

TEST_CASE("a unit that spawned since the last tick is drawn where it is") {
    // Not blended in from wherever the previous occupant of its slot was standing. This is the
    // whole reason the merge matches by id: slot 0's tank dies, slot 0's engineer spawns, and
    // matching by index would slide the engineer out of the tank's grave.
    const UnitId died{0, 1};
    const UnitId born{0, 2};  // the same slot, a later generation
    const Snapshot from = made(10, {unitAt(died, 0.0f, 0.0f)});
    const Snapshot to = made(11, {unitAt(born, 900.0f, 900.0f)});

    std::vector<DrawUnit> out;
    rm::interpolate(from, to, 0.5f, out);
    REQUIRE(out.size() == 1);
    CHECK(out[0].id == born);
    CHECK(out[0].position[0] == Approx(900.0f));  // not 450
    CHECK(out[0].position[2] == Approx(900.0f));
}

TEST_CASE("a unit that died since the last tick is not drawn") {
    const Snapshot from = made(10, {unitAt(UnitId{0, 1}, 10.0f, 10.0f),
                                    unitAt(UnitId{1, 1}, 20.0f, 20.0f)});
    const Snapshot to = made(11, {unitAt(UnitId{1, 1}, 25.0f, 25.0f)});

    std::vector<DrawUnit> out;
    rm::interpolate(from, to, 0.5f, out);
    REQUIRE(out.size() == 1);
    CHECK(out[0].id == UnitId{1, 1});
}

TEST_CASE("health is taken as it is, not eased into") {
    // A bar that eased into a hit would show a unit at 40% when the sim had already killed it.
    // The bar is information rather than motion.
    const UnitId id{0, 1};
    UnitView before = unitAt(id, 0.0f, 0.0f);
    UnitView after = unitAt(id, 0.0f, 0.0f);
    after.health = rm::sim::Mag::fromInt(10);

    std::vector<DrawUnit> out;
    rm::interpolate(made(10, {before}), made(11, {after}), 0.5f, out);
    REQUIRE(out.size() == 1);
    CHECK(out[0].healthFraction == Approx(0.1f));
}

TEST_CASE("shield activity is taken from the current snapshot") {
    const UnitId id{0, 1};
    UnitView before = unitAt(id, 0.0f, 0.0f);
    UnitView after = unitAt(id, 100.0f, 0.0f);
    before.shieldActive = true;
    after.shieldActive = false;

    std::vector<DrawUnit> out;
    rm::interpolate(made(10, {before}), made(11, {after}), 0.5f, out);

    REQUIRE(out.size() == 1);
    CHECK(out[0].position[0] == Approx(50.0f));
    CHECK_FALSE(out[0].shieldActive);
}

TEST_CASE("recently ordered units can draw the current snapshot without moving their neighbours") {
    const UnitId ordered{0, 1};
    const UnitId neighbour{1, 1};
    const Snapshot from = made(10, {unitAt(ordered, 0.0f, 0.0f),
                                    unitAt(neighbour, 10.0f, 0.0f)});
    const Snapshot to = made(11, {unitAt(ordered, 100.0f, 0.0f),
                                  unitAt(neighbour, 110.0f, 0.0f)});

    std::vector<DrawUnit> out;
    rm::interpolate(from, to, 0.25f, out);
    rm::projectCurrentUnits(to, std::array{ordered}, out);

    REQUIRE(out.size() == 2);
    CHECK(out[0].id == ordered);
    CHECK(out[0].position[0] == Approx(100.0f));
    CHECK(out[1].id == neighbour);
    CHECK(out[1].position[0] == Approx(35.0f));
}

TEST_CASE("rapid orders extend current projection without revisiting an older snapshot") {
    const UnitId ordered{0, 1};
    std::vector<rm::CurrentUnitProjection> projections;

    rm::scheduleCurrentUnitProjection(projections, ordered, 10);
    REQUIRE(projections.size() == 1);
    CHECK_FALSE(projections[0].activeAt(10));
    CHECK(projections[0].activeAt(11));

    // A second order while tick 11 is being shown must keep 11 current as well as tick 12.
    rm::scheduleCurrentUnitProjection(projections, ordered, 11);
    CHECK(projections[0].activeAt(11));
    CHECK(projections[0].activeAt(12));
    CHECK_FALSE(projections[0].activeAt(13));

    // Once the old interval is over, a later order starts a fresh interval after its own tick.
    rm::scheduleCurrentUnitProjection(projections, ordered, 13);
    CHECK_FALSE(projections[0].activeAt(13));
    CHECK(projections[0].activeAt(14));
}

TEST_CASE("projecting one snapshot is interpolation with nothing to blend") {
    // `--no-interpolate` and every headless capture take this path, and it has to agree with
    // alpha 1 — a screenshot of tick N must be tick N whichever route it took.
    const UnitId id{0, 1};
    const Snapshot only = made(11, {unitAt(id, 123.0f, 456.0f, 0.75f)});

    std::vector<DrawUnit> projected;
    rm::project(only, projected);

    std::vector<DrawUnit> blended;
    rm::interpolate(only, only, 1.0f, blended);

    REQUIRE(projected.size() == 1);
    REQUIRE(blended.size() == 1);
    CHECK(projected[0].position[0] == blended[0].position[0]);
    CHECK(projected[0].position[2] == blended[0].position[2]);
    CHECK(projected[0].rotationY == blended[0].rotationY);
    CHECK(projected[0].healthFraction == blended[0].healthFraction);
    CHECK(projected[0].shieldActive == blended[0].shieldActive);
}

TEST_CASE("an empty snapshot draws nothing") {
    std::vector<DrawUnit> out;
    out.emplace_back();  // and the previous frame's contents go away
    rm::interpolate(Snapshot{}, Snapshot{}, 0.5f, out);
    CHECK(out.empty());
    rm::project(Snapshot{}, out);
    CHECK(out.empty());
}
