#include "core/sim/StateHash.hpp"

#include "core/map/HeightField.hpp"
#include "core/sim/Skirmish.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

using rm::sim::hashMatch;

namespace {

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
struct Fixture {
    std::vector<rm::UnitInstance> instances;
    std::vector<rm::sim::MoveState> motion;
    std::vector<rm::sim::Health> health;
    std::vector<rm::sim::Army> armies;
    std::vector<rm::sim::Economy> economies;
    std::vector<rm::sim::Projectile> projectiles;
    std::vector<rm::sim::Construction> building;
    std::vector<int> commandersEver;

    Fixture() {
        instances.push_back(rm::UnitInstance{.position = {100.0f, 0.0f, 100.0f},
                                             .rotationY = 0.0f,
                                             .scale = 1.0f});
        motion.push_back(rm::sim::MoveState{.armyIndex = 0});
        health.push_back(rm::sim::Health{.current = 500.0f, .maximum = 500.0f});
        armies = rm::sim::freeForAll(1);
        economies.assign(1, rm::sim::Economy{});
        commandersEver.assign(1, 1);
    }

    [[nodiscard]] std::vector<rm::sim::SkirmishGroup> groups() {
        return {rm::sim::SkirmishGroup{
            .instances = instances, .motion = motion, .health = health, .def = nullptr}};
    }

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
        std::vector<rm::sim::SkirmishGroup> g = groups();
        rm::sim::Match m = match();
        return hashMatch(g, m);
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
    a.instances[0].position[0] += 1.0f;
    REQUIRE(a.hash() != before);
}

TEST_CASE("a change too small to see is still a divergence") {
    // The point of hashing bit patterns rather than comparing with a tolerance. A sim that
    // drifts by one bit per tick has diverged, and finding that on tick 1 rather than tick
    // 100,000 is the entire value of the exercise.
    Fixture a;
    const rm::StateHash before = a.hash();
    a.instances[0].position[2] = std::nextafter(a.instances[0].position[2], 1e9f);
    REQUIRE(a.hash() != before);
}

TEST_CASE("presentation is not state") {
    // `animationPhase` is advanced by the CALLER from distance walked, and `teamColour` is a
    // palette entry fixed at spawn. Two runs that played the identical match and drew it
    // differently must agree — otherwise the divergence harness cries wolf on every run.
    Fixture a;
    const rm::StateHash before = a.hash();

    a.instances[0].animationPhase = 0.75f;
    REQUIRE(a.hash() == before);

    a.instances[0].teamColour = rm::kTeamColours[3];
    REQUIRE(a.hash() == before);
}

TEST_CASE("every field the sim owns reaches the hash") {
    // One case per field group rather than one per field: what this guards against is a
    // field being added to the sim and forgotten here, which would make the hash agree
    // about a match that differs. If you added a field and this test still passes, the
    // field is not being hashed.
    SECTION("orientation") {
        Fixture a;
        const rm::StateHash before = a.hash();
        a.instances[0].rotationY += 0.1f;
        REQUIRE(a.hash() != before);
    }
    SECTION("slope alignment") {
        Fixture a;
        const rm::StateHash before = a.hash();
        a.instances[0].rotationX += 0.1f;
        REQUIRE(a.hash() != before);
    }
    SECTION("health") {
        Fixture a;
        const rm::StateHash before = a.hash();
        a.health[0].current -= 1.0f;
        REQUIRE(a.hash() != before);
    }
    SECTION("reload") {
        Fixture a;
        const rm::StateHash before = a.hash();
        a.health[0].reloadRemaining.push_back(3);
        REQUIRE(a.hash() != before);
    }
    SECTION("motion order") {
        Fixture a;
        const rm::StateHash before = a.hash();
        a.motion[0].moving = true;
        REQUIRE(a.hash() != before);
    }
    SECTION("route") {
        Fixture a;
        const rm::StateHash before = a.hash();
        a.motion[0].path.push_back({10.0f, 20.0f});
        REQUIRE(a.hash() != before);
    }
    SECTION("how far along the route") {
        Fixture a;
        a.motion[0].path.push_back({10.0f, 20.0f});
        a.motion[0].path.push_back({30.0f, 40.0f});
        const rm::StateHash before = a.hash();
        a.motion[0].pathIndex = 1;
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
        a.economies[0].stored.mass += 1.0f;
        REQUIRE(a.hash() != before);
    }
    SECTION("the stall ratio") {
        Fixture a;
        const rm::StateHash before = a.hash();
        a.economies[0].fundedFraction = 0.5f;
        REQUIRE(a.hash() != before);
    }
    SECTION("a shot in flight") {
        Fixture a;
        const rm::StateHash before = a.hash();
        a.projectiles.push_back(rm::sim::Projectile{.damage = 10.0f, .ticksRemaining = 30});
        REQUIRE(a.hash() != before);
    }
    SECTION("work under construction") {
        Fixture a;
        const rm::StateHash before = a.hash();
        a.building.push_back(rm::sim::Construction{.armyIndex = 0,
                                                   .buildTimeRemaining = 5.0f});
        REQUIRE(a.hash() != before);
    }
}

TEST_CASE("losing a unit changes the hash even though the survivors match") {
    // Counts are fed alongside contents for exactly this: a hash built only from the units
    // present would agree about a match that had lost one.
    Fixture a;
    a.instances.push_back(a.instances[0]);
    a.motion.push_back(a.motion[0]);
    a.health.push_back(a.health[0]);
    const rm::StateHash two = a.hash();

    a.instances.pop_back();
    a.motion.pop_back();
    a.health.pop_back();
    REQUIRE(a.hash() != two);
}

TEST_CASE("two units swapping places is a different match") {
    // Order IS state. A sim whose behaviour did not depend on iteration order would be a
    // sim we could not write a replay for.
    Fixture a;
    a.instances.push_back(rm::UnitInstance{.position = {200.0f, 0.0f, 200.0f},
                                           .rotationY = 0.0f,
                                           .scale = 1.0f});
    a.motion.push_back(rm::sim::MoveState{.armyIndex = 0});
    a.health.push_back(rm::sim::Health{.current = 500.0f, .maximum = 500.0f});
    const rm::StateHash before = a.hash();

    std::swap(a.instances[0], a.instances[1]);
    REQUIRE(a.hash() != before);
}

TEST_CASE("no list and an empty list hash differently") {
    // A decorative crowd has no projectile list at all; a match between two ticks of combat
    // has an empty one. Those are different situations and must not collide.
    Fixture a;
    std::vector<rm::sim::SkirmishGroup> g = a.groups();

    rm::sim::Match withList = a.match();
    rm::sim::Match without = a.match();
    without.projectiles = nullptr;

    REQUIRE(hashMatch(g, withList) != hashMatch(g, without));
}

TEST_CASE("a decided match hashes differently from one still being played") {
    Fixture a;
    std::vector<rm::sim::SkirmishGroup> g = a.groups();
    rm::sim::Match running = a.match();
    rm::sim::Match decided = a.match();
    decided.over = true;

    REQUIRE(hashMatch(g, running) != hashMatch(g, decided));
}

TEST_CASE("negative zero is not a divergence") {
    // -0.0f == 0.0f arithmetically but their bit patterns differ. A sim that produced one
    // where it previously produced the other has not diverged, and reporting it as such
    // would be a false positive on a value that compares equal.
    Fixture a;
    const rm::StateHash positive = a.hash();
    a.instances[0].position[1] = -0.0f;
    REQUIRE(a.hash() == positive);
}

TEST_CASE("a real tick moves the hash, and the same tick moves it the same way") {
    // The property the divergence harness is built on, end to end: two matches stepped
    // identically stay identical, tick after tick.
    const rm::HeightField field = flatField();

    Fixture a;
    Fixture b;
    a.motion[0].moving = true;
    a.motion[0].destinationX = 400.0f;
    a.motion[0].destinationZ = 100.0f;
    b.motion[0] = a.motion[0];

    REQUIRE(a.hash() == b.hash());

    for (int tick = 0; tick < 20; ++tick) {
        std::vector<rm::sim::SkirmishGroup> ga = a.groups();
        std::vector<rm::sim::SkirmishGroup> gb = b.groups();
        rm::sim::Match ma = a.match();
        rm::sim::Match mb = b.match();
        (void)rm::sim::tickSkirmish(ga, ma, field);
        (void)rm::sim::tickSkirmish(gb, mb, field);
        REQUIRE(a.hash() == b.hash());
    }

    // ...and that it actually advanced, so the agreement above is not two frozen matches
    // agreeing about nothing.
    Fixture fresh;
    fresh.motion[0] = a.motion[0];
    REQUIRE(a.instances[0].position[0] != fresh.instances[0].position[0]);
}
