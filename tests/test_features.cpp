// What the dead leave behind.
//
// §7 P6.2 names two assertions and they are different in kind. "A death creates one feature at
// the right position" is behaviour, and is checked here. "The sim links no GPU type" is a
// STRUCTURAL claim about the include graph, which no unit test can make — a test that compiles
// has already lost the argument — so it is `tools/check_sim_is_headless.sh`, registered in CTest
// beside the other boundary checks.
#include <catch2/catch_test_macros.hpp>

#include "core/sim/FeatureStore.hpp"
#include "core/sim/Skirmish.hpp"

#include "support/FxMatchers.hpp"
#include "support/TestRoster.hpp"

#include <cstdint>
#include <vector>

using rm::sim::Feature;
using rm::sim::FeatureId;
using rm::sim::FeatureStore;
using rm::sim::UnitId;

namespace {

[[nodiscard]] rm::HeightField flatField() {
    rm::HeightField field;
    field.squaresX = 128;
    field.squaresZ = 128;
    field.baseHeight = 0.0f;
    field.heightScale = 1.0f;
    field.raw.assign(field.sampleCount(), std::uint16_t{0});
    return field;
}

} // namespace

TEST_CASE("a feature keeps what was put in it, and a stale handle finds nothing") {
    FeatureStore features;
    CHECK(features.size() == 0);

    const FeatureId wreck = features.add(Feature{.at = {rm::test::fx(100.0f),
                                                       rm::test::fx(0.0f),
                                                       rm::test::fx(250.0f)},
                                                .radiusElmos = rm::test::fx(6.0f),
                                                .fromType = 3,
                                                .armyIndex = 1});
    REQUIRE(features.size() == 1);

    const Feature* found = features.find(wreck);
    REQUIRE(found != nullptr);
    CHECK(rm::test::asFloat(found->at[0]) == 100.0f);
    CHECK(rm::test::asFloat(found->at[2]) == 250.0f);
    CHECK(rm::test::asFloat(found->radiusElmos) == 6.0f);
    CHECK(found->fromType == 3);
    CHECK(found->armyIndex == 1);

    // A handle from nowhere resolves to nothing rather than to whatever is in that slot —
    // the same generational guarantee a unit handle gives, from the same pool.
    CHECK(features.find(FeatureId{0, 999}) == nullptr);
    CHECK(features.find(FeatureId{99, 1}) == nullptr);
}

TEST_CASE("a death creates one feature, where the unit was and its size") {
    // §7 P6.2's stated test, through the tick — because the interesting part is WHEN: the
    // position and radius have to be read before `retireDead` zeroes the radius, which is the
    // same ordering trap `Death` carries its own copies for.
    const rm::HeightField field = flatField();
    const rm::sim::Terrain terrain{field};

    rm::test::Roster roster;
    rm::unitdef::UnitDef def;
    def.name = "test_thing";
    const rm::UnitTypeIndex type = roster.addType(def);
    const UnitId doomed = roster.add(type, 300.0f, 400.0f, 1, 100.0f);

    std::vector<rm::sim::Army> armies = rm::sim::freeForAll(2);
    std::vector<rm::sim::Projectile> shots;
    std::vector<rm::sim::Economy> economies(2);
    const std::vector<int> commandersEver{0, 0};
    FeatureStore features;

    const rm::sim::Fx radius = roster.motion(doomed).radiusElmos;
    REQUIRE(radius > rm::sim::Fx{});
    roster.health(doomed).current = rm::sim::Mag{};  // killed outright

    rm::sim::Match match{.armies = armies,
                         .economies = economies,
                         .projectiles = &shots,
                         .features = &features,
                         .commandersEver = commandersEver};
    const rm::sim::TickReport report =
        rm::sim::tickSkirmish(roster.store, roster.catalog, match, terrain, roster.rate);

    REQUIRE(report.died.size() == 1);
    REQUIRE(features.size() == 1);

    const Feature& wreck = features.all()[0];
    CHECK(rm::test::asFloat(wreck.at[0]) == 300.0f);
    CHECK(rm::test::asFloat(wreck.at[2]) == 400.0f);
    CHECK(wreck.radiusElmos == radius);  // BEFORE retirement zeroed it
    CHECK(wreck.fromType == type);
    CHECK(wreck.armyIndex == 1);

    // ONE, however many more ticks run — the same guard that makes `UnitDestroyed` fire once.
    for (int tick = 0; tick < 20; ++tick) {
        (void)rm::sim::tickSkirmish(roster.store, roster.catalog, match, terrain, roster.rate);
    }
    CHECK(features.size() == 1);
}

TEST_CASE("a scene with nowhere to put wrecks still runs the tick") {
    // Null is the ordinary case for a `--units` crowd and for most tests, so it is a branch
    // rather than a special path — and a death with no feature store must not be a crash.
    const rm::HeightField field = flatField();
    const rm::sim::Terrain terrain{field};

    rm::test::Roster roster;
    rm::unitdef::UnitDef def;
    const rm::UnitTypeIndex type = roster.addType(def);
    const UnitId doomed = roster.add(type, 100.0f, 100.0f, 0, 100.0f);
    roster.health(doomed).current = rm::sim::Mag{};

    std::vector<rm::sim::Army> armies = rm::sim::freeForAll(1);
    std::vector<rm::sim::Projectile> shots;
    std::vector<rm::sim::Economy> economies(1);
    const std::vector<int> commandersEver{0};

    rm::sim::Match match{.armies = armies,
                         .economies = economies,
                         .projectiles = &shots,
                         .features = nullptr,
                         .commandersEver = commandersEver};
    rm::sim::TickReport report;
    CHECK_NOTHROW(report = rm::sim::tickSkirmish(roster.store, roster.catalog, match, terrain,
                                                 roster.rate));
    CHECK(report.died.size() == 1);
}

TEST_CASE("features are append-only, and clearing resets the handles") {
    // Append-only is the decision (a wreck is permanent). `clear` exists for a caller starting
    // a fresh match in the same process — and it has to reset the id pool too, or the first
    // handle of the new match would collide with a live handle from the old one.
    FeatureStore features;
    const FeatureId first = features.add(Feature{});
    (void)features.add(Feature{});
    REQUIRE(features.size() == 2);

    features.clear();
    CHECK(features.size() == 0);
    CHECK(features.find(first) == nullptr);

    const FeatureId reborn = features.add(Feature{});
    CHECK(features.find(reborn) != nullptr);
}
