// Wrecks back into mass: the reclaim order, the harvest, and the economy they feed.
//
// The numbers are the game's own, cited on `UnitDef` and `core/sim/Reclaim.hpp`: a wreck
// holds `BuildCost × 0.9` and a reclaimer drains `10 × BuildRate` value a second — so a
// rate-10 engineer takes 10 mass a tick at 10 Hz, and the arithmetic below is exact in
// fixed point, which is what lets these tests use == rather than margins.
#include <catch2/catch_test_macros.hpp>

#include "core/sim/Command.hpp"
#include "core/sim/FeatureStore.hpp"
#include "core/sim/Reclaim.hpp"
#include "core/sim/Skirmish.hpp"
#include "core/sim/UnitStore.hpp"

#include "support/FxMatchers.hpp"
#include "support/TestRoster.hpp"

#include <cstdint>
#include <filesystem>
#include <vector>

using rm::sim::Command;
using rm::sim::CommandKind;
using rm::sim::Feature;
using rm::sim::FeatureId;
using rm::sim::FeatureStore;
using rm::sim::Player;
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

/// One army, one player, an engineer, and somewhere for wrecks to be.
struct Fixture {
    rm::HeightField field = flatField();
    rm::sim::Terrain terrain{field};
    rm::sim::PassabilityGrid grid = rm::sim::buildPassability(field, 0.0f);
    rm::test::Roster roster;
    std::vector<rm::sim::Army> armies = rm::sim::freeForAll(2);
    std::vector<Player> players = rm::sim::onePlayerPerArmy(2, /*humanArmy=*/0);
    std::vector<rm::sim::Economy> economies{2};
    std::vector<rm::sim::Projectile> shots;
    std::vector<rm::sim::Construction> building;
    std::vector<int> commandersEver{0, 0};
    FeatureStore features;

    rm::UnitTypeIndex engineerType{};
    rm::UnitTypeIndex tankType{};

    Fixture() {
        rm::unitdef::UnitDef engineer;
        engineer.name = "test_engineer";
        engineer.buildRate = 10.0f;  // 1 build unit per tick at 10 Hz
        engineerType = roster.addType(engineer);

        rm::unitdef::UnitDef tank;
        tank.name = "test_tank";
        tank.buildTime = rm::sim::magFromFloat(100.0f);
        tankType = roster.addType(tank);
    }

    /// A standard 90-mass wreck — what a 100-mass unit leaves at the corpus's 0.9.
    [[nodiscard]] FeatureId wreckAt(float x, float z, float mass = 90.0f) {
        return features.add(Feature{.at = {rm::sim::fxFromFloat(x), rm::sim::Fx{},
                                           rm::sim::fxFromFloat(z)},
                                    .radiusElmos = rm::sim::Fx::fromInt(4),
                                    .fromType = tankType,
                                    .armyIndex = 1,
                                    .massRemaining = rm::sim::magFromFloat(mass),
                                    .reclaimPerBuildRate = rm::sim::fxFromFloat(10.0f)});
    }

    [[nodiscard]] bool reclaim(UnitId who, FeatureId what) {
        const Feature* wreck = features.find(what);
        Command command{.kind = CommandKind::Reclaim, .unit = who, .target = what};
        if (wreck != nullptr) {
            command.targetX = wreck->at[0];
            command.targetZ = wreck->at[2];
        }
        return rm::sim::applyCommand(command, roster.store, roster.catalog, players, armies,
                                     terrain, grid, roster.rate, &building, nullptr,
                                     &features);
    }

    void tick(int times = 1, float storageMass = 1000.0f) {
        const std::vector<const rm::sim::PassabilityGrid*> grids(roster.catalog.size(), &grid);
        rm::sim::Match match{.armies = armies,
                             .economies = economies,
                             .projectiles = &shots,
                             .building = &building,
                             .features = &features,
                             .passability = grids,
                             .commandersEver = commandersEver,
                             .baseStorage = {.mass = rm::sim::magFromFloat(storageMass),
                                             .energy = rm::sim::magFromFloat(1000.0f)}};
        for (int i = 0; i < times; ++i) {
            (void)rm::sim::tickSkirmish(roster.store, roster.catalog, match, terrain,
                                        roster.rate);
        }
    }
};

} // namespace

TEST_CASE("an engineer empties a wreck into its army's store, and the wreck disappears") {
    Fixture f;
    const UnitId engineer = f.roster.add(f.engineerType, 200.0f, 200.0f, 0, 100.0f);
    const FeatureId wreck = f.wreckAt(210.0f, 200.0f);  // well inside the 40-elmo reach

    REQUIRE(f.reclaim(engineer, wreck));

    // 90 mass at 10 a tick: eight ticks in, the wreck still stands with 10 left...
    f.tick(8);
    CHECK(rm::test::asFloat(f.economies[0].stored.mass) == 80.0f);
    REQUIRE(f.features.find(wreck) != nullptr);
    CHECK(rm::test::asFloat(f.features.find(wreck)->massRemaining) == 10.0f);

    // ...and the ninth empties it. The wreck is gone — reclaimed ground is clean
    // ground — and one more tick retires the order, leaving the engineer idle.
    f.tick(1);
    CHECK(rm::test::asFloat(f.economies[0].stored.mass) == 90.0f);
    CHECK(f.features.find(wreck) == nullptr);
    f.tick(1);
    CHECK(f.roster.store.orders()[engineer.index].empty());
}

TEST_CASE("reclaiming over a full mass bar overflows and is lost, like any other income") {
    Fixture f;
    const UnitId engineer = f.roster.add(f.engineerType, 200.0f, 200.0f, 0, 100.0f);
    const FeatureId wreck = f.wreckAt(210.0f, 200.0f);

    REQUIRE(f.reclaim(engineer, wreck));
    f.tick(9, /*storageMass=*/10.0f);

    // The wreck gave all 90; the store kept its cap's worth. `tickEconomy` clamps the
    // same tick each grant lands, so the loss is per tick, not a one-off at the end.
    CHECK(f.features.find(wreck) == nullptr);
    CHECK(rm::test::asFloat(f.economies[0].stored.mass) == 10.0f);
}

TEST_CASE("a tank cannot reclaim, and a bare scorch offers nothing to anyone") {
    Fixture f;
    const UnitId tank = f.roster.add(f.tankType, 200.0f, 200.0f, 0, 100.0f);
    const UnitId engineer = f.roster.add(f.engineerType, 220.0f, 200.0f, 0, 100.0f);
    const FeatureId wreck = f.wreckAt(210.0f, 200.0f);

    // A non-builder is refused outright — the same rule as a tank founding a factory.
    CHECK_FALSE(f.reclaim(tank, wreck));

    // An ACU's wreck is a scorch record with nothing in it (`Unit.lua:1762-1765`), and
    // ordering an engineer onto one is refused rather than an order that never completes.
    const FeatureId scorch = f.wreckAt(230.0f, 200.0f, /*mass=*/0.0f);
    CHECK_FALSE(f.reclaim(engineer, scorch));
}

TEST_CASE("two engineers empty one wreck faster, and the total never exceeds what it held") {
    Fixture f;
    const UnitId first = f.roster.add(f.engineerType, 200.0f, 200.0f, 0, 100.0f);
    const UnitId second = f.roster.add(f.engineerType, 220.0f, 200.0f, 0, 100.0f);
    const FeatureId wreck = f.wreckAt(210.0f, 200.0f);

    REQUIRE(f.reclaim(first, wreck));
    REQUIRE(f.reclaim(second, wreck));

    // 90 mass at 20 a tick between them: five ticks, and not a point more than the wreck
    // held — the last tick's grant is whatever is left, in slot order.
    f.tick(5);
    CHECK(rm::test::asFloat(f.economies[0].stored.mass) == 90.0f);
    CHECK(f.features.find(wreck) == nullptr);

    // BOTH orders retire: the second engineer's wreck was emptied by the first, which is
    // completion, not failure.
    f.tick(1);
    CHECK(f.roster.store.orders()[first.index].empty());
    CHECK(f.roster.store.orders()[second.index].empty());
}

TEST_CASE("a death leaves a wreck worth the definition's word, and reclaim empties it") {
    // End to end: the tick makes the wreck from `UnitDef::wreckMass` — the blueprint's
    // `BuildCost × MassMult`, computed at parse time — and the same tick loop reclaims it.
    Fixture f;
    rm::unitdef::UnitDef costly;
    costly.name = "test_costly";
    costly.wreckMass = rm::sim::magFromFloat(180.0f);
    costly.reclaimPerBuildRate = rm::sim::fxFromFloat(10.0f);
    const rm::UnitTypeIndex costlyType = f.roster.addType(costly);

    const UnitId doomed = f.roster.add(costlyType, 210.0f, 200.0f, 1, 100.0f);
    f.roster.health(doomed).current = rm::sim::Mag{};
    f.tick(1);

    REQUIRE(f.features.size() == 1);
    const FeatureId wreck = f.features.idAt(0);
    REQUIRE(f.features.find(wreck) != nullptr);
    CHECK(rm::test::asFloat(f.features.find(wreck)->massRemaining) == 180.0f);

    const UnitId engineer = f.roster.add(f.engineerType, 200.0f, 200.0f, 0, 100.0f);
    REQUIRE(f.reclaim(engineer, wreck));
    f.tick(18);
    CHECK(f.features.find(wreck) == nullptr);
    CHECK(rm::test::asFloat(f.economies[0].stored.mass) == 180.0f);
}

TEST_CASE("a reclaim order survives the log round trip") {
    // The replay contract: the kind serialises by name, the feature handle rides in the
    // target columns the chase already writes, and what comes back is what went in.
    rm::sim::CommandLog log;
    log.record(Command{.tick = 7,
                       .player = 1,
                       .kind = CommandKind::Reclaim,
                       .unit = UnitId{3, 2},
                       .targetX = rm::sim::fxFromFloat(210.0f),
                       .targetZ = rm::sim::fxFromFloat(200.0f),
                       .target = UnitId{5, 1}});

    const auto path = std::filesystem::temp_directory_path() / "rm_reclaim_log_test.txt";
    REQUIRE(rm::sim::writeCommandLog(log, path.string()));
    const auto reread = rm::sim::readCommandLog(path.string());
    std::filesystem::remove(path);

    REQUIRE(reread.has_value());
    REQUIRE(reread->size() == 1);
    CHECK(reread->all()[0] == log.all()[0]);
}

TEST_CASE("a patrolling engineer repairs an allied unit already inside build reach") {
    Fixture f;
    f.armies[1].alliance = f.armies[0].alliance;
    const UnitId engineer = f.roster.add(f.engineerType, 200.0f, 200.0f, 0, 100.0f);
    const UnitId ally = f.roster.add(f.tankType, 210.0f, 200.0f, 1, 100.0f);
    f.roster.health(ally).current = rm::sim::magFromFloat(50.0f);

    Command patrol{.kind = CommandKind::Patrol,
                   .unit = engineer,
                   .targetX = rm::sim::fxFromFloat(500.0f),
                   .targetZ = rm::sim::fxFromFloat(200.0f)};
    REQUIRE(rm::sim::applyCommand(patrol, f.roster.store, f.roster.catalog, f.players,
                                  f.armies, f.terrain, f.grid, f.roster.rate));
    f.tick();

    // BuildRate 10 at 10 Hz is one work unit; the target's BuildTime 100 makes that exactly
    // 1% of maximum health, without first quantising the ratio to geometry's Q18.14 range.
    CHECK(rm::test::asFloat(f.roster.health(ally).current) == 51.0f);
}

TEST_CASE("patrol repair has priority over reclaim, then a clear route harvests the wreck") {
    Fixture f;
    const UnitId engineer = f.roster.add(f.engineerType, 200.0f, 200.0f, 0, 100.0f);
    const UnitId ally = f.roster.add(f.tankType, 210.0f, 200.0f, 0, 100.0f);
    f.roster.health(ally).current = rm::sim::magFromFloat(99.5f);
    const FeatureId wreck = f.wreckAt(205.0f, 200.0f);

    Command patrol{.kind = CommandKind::Patrol,
                   .unit = engineer,
                   .targetX = rm::sim::fxFromFloat(500.0f),
                   .targetZ = rm::sim::fxFromFloat(200.0f)};
    REQUIRE(rm::sim::applyCommand(patrol, f.roster.store, f.roster.catalog, f.players,
                                  f.armies, f.terrain, f.grid, f.roster.rate));
    f.tick();
    REQUIRE(f.features.find(wreck) != nullptr);
    CHECK(rm::test::asFloat(f.features.find(wreck)->massRemaining) == 90.0f);

    f.tick();
    REQUIRE(f.features.find(wreck) != nullptr);
    CHECK(rm::test::asFloat(f.features.find(wreck)->massRemaining) == 80.0f);
    CHECK(rm::test::asFloat(f.economies[0].stored.mass) == 10.0f);
}

TEST_CASE("a patrolling engineer does not detour for work outside build reach") {
    Fixture f;
    const UnitId engineer = f.roster.add(f.engineerType, 200.0f, 200.0f, 0, 100.0f);
    const UnitId ally = f.roster.add(f.tankType, 280.0f, 200.0f, 0, 100.0f);
    f.roster.health(ally).current = rm::sim::magFromFloat(50.0f);
    const FeatureId wreck = f.wreckAt(290.0f, 200.0f);

    Command patrol{.kind = CommandKind::Patrol,
                   .unit = engineer,
                   .targetX = rm::sim::fxFromFloat(500.0f),
                   .targetZ = rm::sim::fxFromFloat(200.0f)};
    REQUIRE(rm::sim::applyCommand(patrol, f.roster.store, f.roster.catalog, f.players,
                                  f.armies, f.terrain, f.grid, f.roster.rate));
    f.tick();

    CHECK(rm::test::asFloat(f.roster.health(ally).current) == 50.0f);
    REQUIRE(f.features.find(wreck) != nullptr);
    CHECK(rm::test::asFloat(f.features.find(wreck)->massRemaining) == 90.0f);
    CHECK(f.roster.store.motion()[engineer.index].moving);
}

TEST_CASE("patrol repair keeps sub-Q18 work and safely clamps oversized work") {
    Fixture f;
    rm::unitdef::UnitDef slowTarget;
    slowTarget.name = "slow_target";
    slowTarget.buildTime = rm::sim::magFromFloat(100000.0f);
    const rm::UnitTypeIndex slowType = f.roster.addType(slowTarget);
    const UnitId engineer = f.roster.add(f.engineerType, 200.0f, 200.0f, 0, 100.0f);
    const UnitId slow = f.roster.add(slowType, 210.0f, 200.0f, 0, 100.0f);
    f.roster.health(slow).current = rm::sim::magFromFloat(50.0f);

    Command patrol{.kind = CommandKind::Patrol,
                   .unit = engineer,
                   .targetX = rm::sim::fxFromFloat(500.0f),
                   .targetZ = rm::sim::fxFromFloat(200.0f)};
    REQUIRE(rm::sim::applyCommand(patrol, f.roster.store, f.roster.catalog, f.players,
                                  f.armies, f.terrain, f.grid, f.roster.rate));
    f.tick();
    CHECK(f.roster.health(slow).current > rm::sim::magFromFloat(50.0f));

    Fixture fast;
    rm::unitdef::UnitDef fastEngineer;
    fastEngineer.name = "fast_engineer";
    fastEngineer.buildRate = 100000.0f;
    const rm::UnitTypeIndex fastType = fast.roster.addType(fastEngineer);
    const UnitId builder = fast.roster.add(fastType, 200.0f, 200.0f, 0, 100.0f);
    const UnitId huge = fast.roster.add(fast.tankType, 210.0f, 200.0f, 0, 5000000.0f);
    fast.roster.health(huge).current = rm::sim::magFromFloat(1.0f);
    patrol.unit = builder;
    REQUIRE(rm::sim::applyCommand(patrol, fast.roster.store, fast.roster.catalog, fast.players,
                                  fast.armies, fast.terrain, fast.grid, fast.roster.rate));
    fast.tick();
    CHECK(fast.roster.health(huge).current == fast.roster.health(huge).maximum);
}

TEST_CASE("a dead patrolling builder cannot repair or reclaim before its slot retires") {
    Fixture f;
    const UnitId engineer = f.roster.add(f.engineerType, 200.0f, 200.0f, 0, 100.0f);
    const UnitId ally = f.roster.add(f.tankType, 210.0f, 200.0f, 0, 100.0f);
    f.roster.health(ally).current = rm::sim::magFromFloat(50.0f);
    const FeatureId wreck = f.wreckAt(205.0f, 200.0f);
    Command patrol{.kind = CommandKind::Patrol,
                   .unit = engineer,
                   .targetX = rm::sim::fxFromFloat(500.0f),
                   .targetZ = rm::sim::fxFromFloat(200.0f)};
    REQUIRE(rm::sim::applyCommand(patrol, f.roster.store, f.roster.catalog, f.players,
                                  f.armies, f.terrain, f.grid, f.roster.rate));
    f.roster.health(engineer).current = rm::sim::Mag{};

    CHECK(rm::sim::servicePatrolBuilders(f.roster.store, f.roster.catalog, f.armies,
                                         &f.features, f.economies)
          == 0);
    CHECK(rm::test::asFloat(f.roster.health(ally).current) == 50.0f);
    REQUIRE(f.features.find(wreck) != nullptr);
    CHECK(rm::test::asFloat(f.features.find(wreck)->massRemaining) == 90.0f);
}

TEST_CASE("patrol reclaim preserves a wreck when storage is full") {
    Fixture f;
    const UnitId engineer = f.roster.add(f.engineerType, 200.0f, 200.0f, 0, 100.0f);
    const FeatureId wreck = f.wreckAt(205.0f, 200.0f);
    f.economies[0].stored.mass = rm::sim::magFromFloat(1000.0f);
    Command patrol{.kind = CommandKind::Patrol,
                   .unit = engineer,
                   .targetX = rm::sim::fxFromFloat(500.0f),
                   .targetZ = rm::sim::fxFromFloat(200.0f)};
    REQUIRE(rm::sim::applyCommand(patrol, f.roster.store, f.roster.catalog, f.players,
                                  f.armies, f.terrain, f.grid, f.roster.rate));
    f.tick();

    REQUIRE(f.features.find(wreck) != nullptr);
    CHECK(rm::test::asFloat(f.features.find(wreck)->massRemaining) == 90.0f);
}

TEST_CASE("a patrol helper does no service while its combat target is active") {
    Fixture f;
    const UnitId engineer = f.roster.add(f.engineerType, 200.0f, 200.0f, 0, 100.0f);
    const UnitId ally = f.roster.add(f.tankType, 210.0f, 200.0f, 0, 100.0f);
    const UnitId enemy = f.roster.add(f.tankType, 220.0f, 200.0f, 1, 100.0f);
    f.roster.health(ally).current = rm::sim::magFromFloat(50.0f);
    const FeatureId wreck = f.wreckAt(205.0f, 200.0f);
    Command patrol{.kind = CommandKind::Patrol,
                   .unit = engineer,
                   .targetX = rm::sim::fxFromFloat(500.0f),
                   .targetZ = rm::sim::fxFromFloat(200.0f)};
    REQUIRE(rm::sim::applyCommand(patrol, f.roster.store, f.roster.catalog, f.players,
                                  f.armies, f.terrain, f.grid, f.roster.rate));
    REQUIRE(f.roster.store.orders()[engineer.index].currentMutable() != nullptr);
    f.roster.store.orders()[engineer.index].currentMutable()->target = enemy;

    CHECK(rm::sim::servicePatrolBuilders(f.roster.store, f.roster.catalog, f.armies,
                                         &f.features, f.economies)
          == 0);
    CHECK(rm::test::asFloat(f.roster.health(ally).current) == 50.0f);
    REQUIRE(f.features.find(wreck) != nullptr);
    CHECK(rm::test::asFloat(f.features.find(wreck)->massRemaining) == 90.0f);
}
