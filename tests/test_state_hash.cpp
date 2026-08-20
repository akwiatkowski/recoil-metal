#include "core/sim/StateHash.hpp"

#include "core/map/HeightField.hpp"
#include "core/sim/Skirmish.hpp"
#include "core/sim/UnitCatalog.hpp"
#include "core/sim/UnitStore.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include "support/FxMatchers.hpp"

using rm::sim::hashMatch;

namespace {

/// The ordinary default motion for an army. `sim::defaultMotion` plus an owner.
[[nodiscard]] rm::sim::MoveState defaultMotionFor(int army) {
    rm::sim::MoveState motion = rm::sim::defaultMotion(rm::sim::TickRate{});
    motion.armyIndex = army;
    return motion;
}

// A flat field, which is all any of this needs: the hash reads state, and none of these
// cases care what the ground looks like.
[[nodiscard]] rm::HeightField flatField() {
    rm::HeightField field;
    field.squaresX = 100;
    field.squaresZ = 100;
    field.baseHeight = 0.0f;
    field.heightScale = 1.0f;
    field.raw.assign(field.sampleCount(), std::uint16_t{0});
    return field;
}

// The smallest thing that is a match: one unit, one army, one economy.
//
// The unit lives in a `UnitStore` rather than in three parallel vectors, which is what the
// sim now takes. The three accessors below are spans INTO the store, so a test can still
// reach in and change one field — the point of most of these cases.
struct Fixture {
    rm::sim::UnitStore store;
    rm::sim::UnitCatalog catalog;
    std::vector<rm::sim::Army> armies;
    std::vector<rm::sim::Economy> economies;
    std::vector<rm::sim::Projectile> projectiles;
    std::vector<rm::sim::Construction> building;
    std::vector<int> commandersEver;

    Fixture() {
        // One type, no definition: none of these cases fire a weapon or earn anything, and
        // "registered without a def" is a case the catalog has to carry anyway.
        const rm::UnitTypeIndex type = catalog.add(nullptr);
        (void)store.spawn({
            .type = type,
            .transform = {.x = rm::test::fx(100.0f), .z = rm::test::fx(100.0f)},
            // Given the ordinary default speed, so a case that orders it somewhere and
            // expects movement gets it. A bare `MoveState{}` has a per-tick speed of zero
            // now — see `MoveState::speedPerTick` on why the default cannot name a rate.
            .motion = defaultMotionFor(0),
            .health = {.current = rm::test::mag(500.0f), .maximum = rm::test::mag(500.0f)},
        });
        armies = rm::sim::freeForAll(1);
        economies.assign(1, rm::sim::Economy{});
        commandersEver.assign(1, 1);
    }

    [[nodiscard]] std::span<rm::sim::Transform> transforms() { return store.transforms(); }
    [[nodiscard]] std::span<rm::sim::MoveState> motion() { return store.motion(); }
    [[nodiscard]] std::span<rm::sim::Health> health() { return store.health(); }

    [[nodiscard]] rm::sim::Match match() {
        return rm::sim::Match{
            .armies = armies,
            .economies = economies,
            .projectiles = &projectiles,
            .building = &building,
            .commandersEver = commandersEver,
        };
    }

    [[nodiscard]] rm::StateHash hash() {
        rm::sim::Match m = match();
        return hashMatch(store, m);
    }
};

} // namespace

TEST_CASE("the same state hashes the same, twice running") {
    Fixture a;
    Fixture b;
    REQUIRE(a.hash() == b.hash());
    // And stable across repeated calls on one fixture — a hash that consumed state or
    // touched a counter would fail here and nowhere else.
    const rm::StateHash first = a.hash();
    REQUIRE(a.hash() == first);
    REQUIRE(a.hash() == first);
}

TEST_CASE("one unit moving one step changes the hash") {
    Fixture a;
    const rm::StateHash before = a.hash();
    a.transforms()[0].x += rm::test::fx(1.0f);
    REQUIRE(a.hash() != before);
}

TEST_CASE("a change too small to see is still a divergence") {
    // The point of hashing bit patterns rather than comparing with a tolerance. A sim that
    // drifts by one bit per tick has diverged, and finding that on tick 1 rather than tick
    // 100,000 is the entire value of the exercise.
    Fixture a;
    const rm::StateHash before = a.hash();
    // ONE STEP of the type, which is now the smallest change that exists. `std::nextafter`
    // is what this used to say, and it has no fixed-point counterpart: there is no gap
    // between representable values to step over, so "the smallest possible difference" is
    // literally a raw unit.
    a.transforms()[0].z = rm::sim::Fx::fromRaw(a.transforms()[0].z.raw() + 1);
    REQUIRE(a.hash() != before);
}

TEST_CASE("presentation is not reachable from the store, let alone hashed") {
    // THIS CASE CHANGED SHAPE, and the change is the point. It used to set `animationPhase`
    // and `teamColour` on a stored unit and require the hash not to move — a real risk, since
    // the store held the GPU's struct and a careless `feed` would have fingerprinted the
    // palette.
    //
    // P2.2 removed the risk rather than guarding it: the store holds `Transform`, which has no
    // presentation fields, and `UnitInstance` is built at draw time from the transform plus
    // the type's scale and the army's colour. There is nothing left to accidentally hash, and
    // a future `feed` cannot reintroduce the bug because the data is not there to feed.
    //
    // So what is asserted is the structure. If someone puts a colour or an animation phase
    // back into sim state, this stops compiling — which is a better guard than a runtime check.
    static_assert(sizeof(rm::sim::Transform) == 3 * sizeof(rm::sim::Fx) + 3 * sizeof(rm::Brad)
                      + 2,
                  "Transform should hold exactly a position and three angles; anything else "
                  "has crept in, and if it is presentation it does not belong in the store");
    SUCCEED();
}

TEST_CASE("every field the sim owns reaches the hash") {
    // One case per field group rather than one per field: what this guards against is a
    // field being added to the sim and forgotten here, which would make the hash agree
    // about a match that differs. If you added a field and this test still passes, the
    // field is not being hashed.
    SECTION("orientation") {
        Fixture a;
        const rm::StateHash before = a.hash();
        a.transforms()[0].heading = static_cast<rm::Brad>(a.transforms()[0].heading + 100);
        REQUIRE(a.hash() != before);
    }
    SECTION("slope alignment") {
        Fixture a;
        const rm::StateHash before = a.hash();
        a.transforms()[0].pitch = static_cast<rm::Brad>(a.transforms()[0].pitch + 100);
        REQUIRE(a.hash() != before);
    }
    SECTION("health") {
        Fixture a;
        const rm::StateHash before = a.hash();
        a.health()[0].current -= rm::test::mag(1.0f);
        REQUIRE(a.hash() != before);
    }
    SECTION("reload") {
        Fixture a;
        const rm::StateHash before = a.hash();
        a.health()[0].reloadRemaining.push_back(3);
        REQUIRE(a.hash() != before);
    }
    SECTION("motion order") {
        Fixture a;
        const rm::StateHash before = a.hash();
        a.motion()[0].moving = true;
        REQUIRE(a.hash() != before);
    }
    SECTION("route") {
        Fixture a;
        const rm::StateHash before = a.hash();
        a.motion()[0].path.push_back({rm::test::fx(10.0f), rm::test::fx(20.0f)});
        REQUIRE(a.hash() != before);
    }
    SECTION("how far along the route") {
        Fixture a;
        a.motion()[0].path.push_back({rm::test::fx(10.0f), rm::test::fx(20.0f)});
        a.motion()[0].path.push_back({rm::test::fx(30.0f), rm::test::fx(40.0f)});
        const rm::StateHash before = a.hash();
        a.motion()[0].pathIndex = 1;
        REQUIRE(a.hash() != before);
    }
    SECTION("defeat") {
        Fixture a;
        const rm::StateHash before = a.hash();
        a.armies[0].defeated = true;
        REQUIRE(a.hash() != before);
    }
    SECTION("banked resources") {
        Fixture a;
        const rm::StateHash before = a.hash();
        a.economies[0].stored.mass += rm::test::mag(1.0f);
        REQUIRE(a.hash() != before);
    }
    SECTION("the stall ratio") {
        Fixture a;
        const rm::StateHash before = a.hash();
        a.economies[0].fundedFraction = rm::sim::Fx::fromRatio(1, 2);
        REQUIRE(a.hash() != before);
    }
    SECTION("a shot in flight") {
        Fixture a;
        const rm::StateHash before = a.hash();
        a.projectiles.push_back(rm::sim::Projectile{.damage = rm::test::mag(10.0f), .ticksRemaining = 30});
        REQUIRE(a.hash() != before);
    }
    SECTION("work under construction") {
        Fixture a;
        const rm::StateHash before = a.hash();
        a.building.push_back(
            rm::sim::Construction{.armyIndex = 0, .buildTimeRemaining = rm::test::mag(5.0f)});
        REQUIRE(a.hash() != before);
    }
}

TEST_CASE("losing a unit changes the hash even though the survivors match") {
    // Death is a TOMBSTONE: the slot stays and keeps its last values, so a hash built from
    // the arrays alone would say nothing happened. What separates the two states is the
    // slot's GENERATION, which `release` bumps — which is why it is hashed.
    Fixture a;
    const rm::sim::UnitId second = a.store.spawn({
        .type = 0,
        .transform = {.x = rm::test::fx(100.0f), .z = rm::test::fx(100.0f)},
        .motion = {.armyIndex = 0},
        .health = {.current = rm::test::mag(500.0f), .maximum = rm::test::mag(500.0f)},
    });
    const rm::StateHash both = a.hash();

    a.store.kill(second);
    REQUIRE(a.store.liveCount() == 1);
    REQUIRE(a.store.slotCount() == 2);  // nothing was erased
    REQUIRE(a.hash() != both);
}

TEST_CASE("two units swapping places is a different match") {
    // Order IS state. A sim whose behaviour did not depend on iteration order would be a
    // sim we could not write a replay for.
    Fixture a;
    (void)a.store.spawn({
        .type = 0,
        .transform = {.x = rm::test::fx(200.0f), .z = rm::test::fx(200.0f)},
        .motion = {.armyIndex = 0},
        .health = {.current = rm::test::mag(500.0f), .maximum = rm::test::mag(500.0f)},
    });
    const rm::StateHash before = a.hash();

    std::swap(a.transforms()[0], a.transforms()[1]);
    REQUIRE(a.hash() != before);
}

TEST_CASE("no list and an empty list hash differently") {
    // A decorative crowd has no projectile list at all; a match between two ticks of combat
    // has an empty one. Those are different situations and must not collide.
    Fixture a;
    rm::sim::Match withList = a.match();
    rm::sim::Match without = a.match();
    without.projectiles = nullptr;

    REQUIRE(hashMatch(a.store, withList) != hashMatch(a.store, without));
}

TEST_CASE("a decided match hashes differently from one still being played") {
    Fixture a;
    rm::sim::Match running = a.match();
    rm::sim::Match decided = a.match();
    decided.over = true;

    REQUIRE(hashMatch(a.store, running) != hashMatch(a.store, decided));
}

TEST_CASE("negative zero is not a divergence") {
    // -0.0f == 0.0f arithmetically but their bit patterns differ. A sim that produced one
    // where it previously produced the other has not diverged, and reporting it as such
    // would be a false positive on a value that compares equal.
    Fixture a;
    const rm::StateHash positive = a.hash();
    // Negative zero has no fixed-point counterpart: `Fx` is an integer, and integers have
    // one zero. The float version needed this case because -0.0f and 0.0f have different bit
    // patterns and the hash fed bit patterns; the whole class of false divergence is gone.
    a.transforms()[0].y = rm::sim::Fx{};
    REQUIRE(a.hash() == positive);
}

TEST_CASE("a real tick moves the hash, and the same tick moves it the same way") {
    // The property the divergence harness is built on, end to end: two matches stepped
    // identically stay identical, tick after tick.
    const rm::HeightField field = flatField();

    Fixture a;
    Fixture b;
    a.motion()[0].moving = true;
    a.motion()[0].destinationX = rm::test::fx(400.0f);
    a.motion()[0].destinationZ = rm::test::fx(100.0f);
    b.motion()[0] = a.motion()[0];

    REQUIRE(a.hash() == b.hash());

    // Two sims stepped side by side in ONE process, which is the strongest determinism test
    // available and the payoff for having no sim globals (PLAN2.md §5.4).
    for (int tick = 0; tick < 20; ++tick) {
        rm::sim::Match ma = a.match();
        rm::sim::Match mb = b.match();
        (void)rm::sim::tickSkirmish(a.store, a.catalog, ma, rm::sim::Terrain{field});
        (void)rm::sim::tickSkirmish(b.store, b.catalog, mb, rm::sim::Terrain{field});
        REQUIRE(a.hash() == b.hash());
    }

    // ...and that it actually advanced, so the agreement above is not two frozen matches
    // agreeing about nothing.
    Fixture fresh;
    fresh.motion()[0] = a.motion()[0];
    REQUIRE(a.transforms()[0].x != fresh.transforms()[0].x);
}
