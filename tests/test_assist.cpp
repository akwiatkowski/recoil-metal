// Lending a build arm: the Assist order, the combined rate, and the bill that grows with
// it. A rate-10 founder helped by a rate-10 assister advances a build at 2 units a tick at
// 10 Hz — and drains twice as fast, which is the decision assist puts in the player's
// hands: time bought with bank.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "core/sim/Assist.hpp"
#include "core/sim/Command.hpp"
#include "core/sim/Skirmish.hpp"
#include "core/sim/UnitStore.hpp"

#include "support/EconomyTick.hpp"
#include "support/FxMatchers.hpp"
#include "support/TestRoster.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <vector>

using Catch::Approx;
using rm::sim::Command;
using rm::sim::CommandKind;
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

struct Fixture {
    rm::HeightField field = flatField();
    // Free placement: these cases hand-place structures at exact coordinates to test
    // assistance, not siting; the grid rule has its own tests ([placement]).
    rm::sim::Terrain terrain{field, false, 0.0f, nullptr, {}, rm::sim::PlacementMode::Free};
    rm::sim::PassabilityGrid grid = rm::sim::buildPassability(field, 0.0f);
    rm::test::Roster roster;
    std::vector<rm::sim::Army> armies = rm::sim::freeForAll(2);
    std::vector<Player> players = rm::sim::onePlayerPerArmy(2, /*humanArmy=*/0);
    std::vector<rm::sim::Economy> economies{2};
    std::vector<rm::sim::Projectile> shots;
    std::vector<rm::sim::Construction> building;
    std::vector<int> commandersEver{0, 0};

    rm::UnitTypeIndex engineerType{};
    rm::UnitTypeIndex factoryType{};
    rm::UnitTypeIndex tankType{};
    rm::UnitTypeIndex hutType{};
    /// An engineering station: FA's Kennel shape — immobile, no orders of its own, twice an
    /// engineer's rate. It has no build tree because it never founds anything.
    rm::UnitTypeIndex stationType{};
    /// A structure that upgrades in place (an extractor's tech path) and what it becomes.
    rm::UnitTypeIndex mexType{};
    rm::UnitTypeIndex mex2Type{};
    /// A unit repair can price: it has a build time and a cost, unlike the bare `tankType`.
    rm::UnitTypeIndex repairableType{};

    Fixture() {
        rm::unitdef::UnitDef engineer;
        engineer.name = "test_engineer";
        engineer.buildRate = 10.0f;  // 1 build unit per tick at 10 Hz
        engineer.buildableCategory = {{"TESTHUT"}};
        engineerType = roster.addType(engineer);

        rm::unitdef::UnitDef factory = engineer;
        factory.name = "test_factory";
        factory.categories = {"FACTORY"};
        factoryType = roster.addType(factory);

        rm::unitdef::UnitDef tank;
        tank.name = "test_tank";
        tankType = roster.addType(tank);

        rm::unitdef::UnitDef hut;
        hut.name = "test_hut";
        hut.categories = {"TESTHUT"};
        hut.buildCostMass = rm::sim::magFromFloat(100.0f);
        hut.buildCostEnergy = rm::sim::magFromFloat(100.0f);
        hut.buildTime = rm::sim::magFromFloat(100.0f);  // 100 ticks alone, 50 helped
        hutType = roster.addType(hut);

        rm::unitdef::UnitDef station;
        station.name = "test_station";
        station.categories = {"ENGINEERSTATION"};
        station.buildRate = 20.0f;  // 2 build units per tick at 10 Hz
        stationType = roster.addType(station);

        rm::unitdef::UnitDef mex;
        mex.name = "test_mex";
        mex.buildRate = 10.0f;
        mex.upgradesTo = "test_mex2";
        mex.buildableCategory = {{"TESTMEX2"}};
        mexType = roster.addType(mex);
        rm::unitdef::UnitDef mex2 = hut;
        mex2.name = "test_mex2";
        mex2.categories = {"TESTMEX2"};
        mex2Type = roster.addType(mex2);

        rm::unitdef::UnitDef repairable = tank;
        repairable.name = "test_repairable";
        repairable.buildCostMass = rm::sim::magFromFloat(100.0f);
        repairable.buildCostEnergy = rm::sim::magFromFloat(200.0f);
        repairable.buildTime = rm::sim::magFromFloat(100.0f);  // one build unit heals 1%
        repairableType = roster.addType(repairable);
    }

    [[nodiscard]] bool apply(const Command& command) {
        return rm::sim::applyCommand(command, roster.store, roster.catalog, players, armies,
                                     terrain, grid, roster.rate, &building);
    }

    [[nodiscard]] bool build(UnitId who, float x, float z, bool queued = false) {
        return apply(Command{.kind = CommandKind::Build,
                             .queued = queued,
                             .unit = who,
                             .targetX = rm::sim::fxFromFloat(x),
                             .targetZ = rm::sim::fxFromFloat(z),
                             .buildType = hutType});
    }

    /// The in-place upgrade order: a structure builds what its blueprint says it becomes,
    /// on its own pad.
    [[nodiscard]] bool upgrade(UnitId who) {
        const rm::sim::Transform& at = roster.store.transforms()[who.index];
        return apply(Command{.kind = CommandKind::Build,
                             .unit = who,
                             .targetX = at.x,
                             .targetZ = at.z,
                             .buildType = mex2Type});
    }

    [[nodiscard]] bool move(UnitId who, float x, float z) {
        return apply(Command{.kind = CommandKind::Move,
                             .unit = who,
                             .targetX = rm::sim::fxFromFloat(x),
                             .targetZ = rm::sim::fxFromFloat(z)});
    }

    [[nodiscard]] bool assist(UnitId who, UnitId target, bool queued = false) {
        const rm::sim::Transform& at = roster.store.transforms()[target.index];
        return apply(Command{.kind = CommandKind::Assist,
                             .unit = who,
                             .targetX = at.x,
                             .targetZ = at.z,
                             .target = target,
                             .queued = queued});
    }

    void tick(int times = 1) {
        const std::vector<const rm::sim::PassabilityGrid*> grids(roster.catalog.size(), &grid);
        rm::sim::Match match{.armies = armies,
                             .economies = economies,
                             .projectiles = &shots,
                             .building = &building,
                             .passability = grids,
                             .commandersEver = commandersEver,
                             .baseStorage = {.mass = rm::sim::magFromFloat(10000.0f),
                                             .energy = rm::sim::magFromFloat(10000.0f)}};
        for (int i = 0; i < times; ++i) {
            (void)rm::sim::tickSkirmish(roster.store, roster.catalog, match, terrain,
                                        roster.rate);
        }
    }
};

} // namespace

TEST_CASE("an assisted build advances at the combined rate, and drains for it") {
    Fixture f;
    const UnitId founder = f.roster.add(f.engineerType, 200.0f, 200.0f, 0, 100.0f);
    const UnitId helper = f.roster.add(f.engineerType, 210.0f, 200.0f, 0, 100.0f);
    f.economies[0].stored = {.mass = rm::sim::magFromFloat(1000.0f),
                             .energy = rm::sim::magFromFloat(1000.0f)};

    REQUIRE(f.build(founder, 205.0f, 200.0f));
    SECTION("explicit Assist") {
        REQUIRE(f.assist(helper, founder));
    }
    SECTION("FAF manager assistance uses Guard") {
        REQUIRE(f.apply(Command{.kind = CommandKind::Guard, .unit = helper, .target = founder}));
    }

    // Ten ticks helped: 2 build units a tick, 20 of 100 done — where alone it would be 10.
    f.tick(10);
    REQUIRE(f.building.size() == 1);
    CHECK(rm::test::asFloat(f.building[0].buildTimeRemaining) == Approx(80.0f).margin(0.01));
    CHECK(rm::test::asFloat(f.building[0].assistPerTick) == Approx(1.0f).margin(0.001));
    // Q18 floors the per-tick cost share, leaving roughly 0.04 after ten drains; what matters
    // here is that the bank paid for twenty build units, not the founder's ten.
    CHECK(rm::test::asFloat(f.economies[0].stored.mass) == Approx(980.0f).margin(0.05));
    CHECK(rm::test::asFloat(f.economies[0].stored.energy) == Approx(980.0f).margin(0.05));

    // And the whole hut lands in ~50 ticks instead of 100.
    f.tick(41);
    CHECK(f.building[0].finished());
}

TEST_CASE("Guard attack priority suppresses construction assistance in the same tick",
          "[guard][guard-work-regression]") {
    Fixture f;
    auto armed = *f.roster.catalog.def(f.engineerType);
    armed.name = "armed_guard";
    armed.speedElmosPerSecond = 20.0f;
    armed.guardScanRadiusElmos = rm::sim::Fx::fromInt(80);
    rm::unitdef::Weapon gun;
    gun.role = rm::unitdef::WeaponRole::DirectFire;
    gun.targetPriorities = {{"ALLUNITS"}};
    gun.damage = rm::sim::Mag::fromInt(10);
    gun.maxRange = rm::sim::Fx::fromInt(20);
    gun.rateOfFire = 1.0f;
    armed.weapons.push_back(gun);
    const auto armedType = f.roster.addType(armed);
    const auto founder = f.roster.add(f.engineerType, 200, 200, 0, 100);
    const auto guard = f.roster.add(armedType, 220, 200, 0, 100);
    (void)f.roster.add(f.tankType, 270, 200, 1, 100);
    f.economies[0].stored = {rm::sim::Mag::fromInt(1000), rm::sim::Mag::fromInt(1000)};
    REQUIRE(f.build(founder, 205, 200));
    REQUIRE(f.apply(Command{.kind = CommandKind::Guard, .unit = guard, .target = founder}));

    f.tick();

    REQUIRE(f.building.size() == 1);
    CHECK(f.building.front().assistPerTick == rm::sim::Mag{});
    CHECK(f.building.front().buildTimeRemaining == rm::sim::Mag::fromInt(99));
    REQUIRE(f.roster.store.orders()[guard.index].active());
    CHECK(f.roster.store.orders()[guard.index].active()->kind() == CommandKind::Guard);
    // An in-build-reach helper would stand still. Pursuit proves ATTACK won the ladder.
    CHECK(f.roster.motion(guard).moving);
    CHECK(f.roster.motion(guard).destinationX > rm::sim::Fx::fromInt(220));
}

TEST_CASE("a newly hostile Guard contributes no construction work before cancellation",
          "[guard][guard-work-regression]") {
    Fixture f;
    f.armies[1].alliance = f.armies[0].alliance;
    const auto founder = f.roster.add(f.engineerType, 200, 200, 0, 100);
    const auto guard = f.roster.add(f.engineerType, 220, 200, 1, 100);
    f.economies[0].stored = {rm::sim::Mag::fromInt(1000), rm::sim::Mag::fromInt(1000)};
    REQUIRE(f.build(founder, 205, 200));
    REQUIRE(f.apply(Command{.player = 1, .kind = CommandKind::Guard,
                            .unit = guard, .target = founder}));
    f.armies[1].alliance = 1;

    f.tick();

    REQUIRE(f.building.size() == 1);
    CHECK(f.building.front().assistPerTick == rm::sim::Mag{});
    CHECK(f.building.front().buildTimeRemaining == rm::sim::Mag::fromInt(99));
    CHECK(f.roster.store.orders()[guard.index].empty());
}

TEST_CASE("the final assisted tick requests every builder's full offered work") {
    rm::sim::Economy economy;
    economy.storage = {.mass = rm::sim::magFromFloat(1000.0f),
                       .energy = rm::sim::magFromFloat(1000.0f)};
    economy.stored = economy.storage;

    std::vector<rm::sim::Construction> building{
        rm::sim::Construction{
            .cost = {.mass = rm::sim::magFromFloat(100.0f),
                     .energy = rm::sim::magFromFloat(100.0f)},
            .totalBuildTime = rm::sim::magFromFloat(100.0f),
            .buildTimeRemaining = rm::sim::magFromFloat(1.0f),
            .buildPerTick = rm::sim::magFromFloat(1.0f),
            .assistPerTick = rm::sim::magFromFloat(2.0f),
        },
    };

    rm::test::tickBuild(economy, building);

    CHECK(building[0].finished());
    // Materialize clamps progress at completion, but retail discards the applied amount and
    // never reconciles it against the full 1 founder + 2 assister request.
    CHECK(rm::test::asFloat(economy.requestedLastTick.mass) == Approx(3.0f).margin(0.01));
    CHECK(rm::test::asFloat(economy.requestedLastTick.energy) == Approx(3.0f).margin(0.01));
    CHECK(rm::test::asFloat(economy.usageLastTick.mass) == Approx(3.0f).margin(0.01));
    CHECK(rm::test::asFloat(economy.usageLastTick.energy) == Approx(3.0f).margin(0.01));
    CHECK(rm::test::asFloat(economy.stored.mass) == Approx(997.0f).margin(0.01));
    CHECK(rm::test::asFloat(economy.stored.energy) == Approx(997.0f).margin(0.01));
}

TEST_CASE("final build progress scales the full request before clamping") {
    rm::sim::Economy economy;
    economy.storage = {.mass = rm::test::mag(1000.0f),
                       .energy = rm::test::mag(1000.0f)};
    // Half of the roughly three-resource completing request.
    economy.stored = {.mass = rm::test::mag(1.5f), .energy = rm::test::mag(1.5f)};

    std::vector<rm::sim::Construction> building{
        rm::sim::Construction{
            .cost = {.mass = rm::test::mag(100.0f),
                     .energy = rm::test::mag(100.0f)},
            .totalBuildTime = rm::test::mag(100.0f),
            .buildTimeRemaining = rm::test::mag(1.0f),
            .buildPerTick = rm::test::mag(1.0f),
            .assistPerTick = rm::test::mag(2.0f),
        },
    };

    rm::test::tickBuild(economy, building);

    CHECK(rm::test::asFloat(economy.requestedLastTick.mass) == Approx(3.0f).margin(0.01));
    CHECK(rm::test::asFloat(economy.usageLastTick.mass) == Approx(1.5f).margin(0.01));
    CHECK(rm::test::asFloat(economy.usageLastTick.energy) == Approx(1.5f).margin(0.01));
    CHECK(rm::test::asFloat(economy.fundedFraction) == Approx(0.5f).margin(0.01));
    CHECK(building[0].finished());
    CHECK(rm::test::asFloat(economy.stored.mass) == Approx(0.0f).margin(0.01));
    CHECK(rm::test::asFloat(economy.stored.energy) == Approx(0.0f).margin(0.01));
    const rm::sim::Resources afterCompletion = economy.stored;

    rm::test::tickBuild(economy, building);

    CHECK(economy.requestedLastTick.mass == rm::sim::Mag{});
    CHECK(economy.requestedLastTick.energy == rm::sim::Mag{});
    CHECK(economy.usageLastTick.mass == rm::sim::Mag{});
    CHECK(economy.usageLastTick.energy == rm::sim::Mag{});
    CHECK(economy.stored.mass == afterCompletion.mass);
    CHECK(economy.stored.energy == afterCompletion.energy);
}

TEST_CASE("a helper out of reach contributes nothing until it arrives") {
    Fixture f;
    const UnitId founder = f.roster.add(f.engineerType, 200.0f, 200.0f, 0, 100.0f);
    // 400 elmos away — far outside the 40-elmo default build reach.
    const UnitId helper = f.roster.add(f.engineerType, 600.0f, 200.0f, 0, 100.0f);
    f.economies[0].stored = {.mass = rm::sim::magFromFloat(1000.0f),
                             .energy = rm::sim::magFromFloat(1000.0f)};

    REQUIRE(f.build(founder, 205.0f, 200.0f));
    REQUIRE(f.assist(helper, founder));  // accepted: it routes over

    f.tick(5);
    REQUIRE(f.building.size() == 1);
    // No contribution from across the map; the founder works alone at 1 a tick.
    CHECK(rm::test::asFloat(f.building[0].assistPerTick) == 0.0f);
    CHECK(rm::test::asFloat(f.building[0].buildTimeRemaining) == Approx(95.0f).margin(0.01));

    // The standing order is a pursuit, not a proximity-only buff: after crossing the map the
    // helper enters build reach and starts contributing without another command.
    f.tick(45);
    CHECK(f.roster.transform(helper).x < rm::sim::fxFromFloat(600.0f));
    CHECK(rm::test::asFloat(f.building[0].assistPerTick) == Approx(1.0f).margin(0.001));
}

TEST_CASE("an engineer ordered to assist a distant structure walks over and joins its work") {
    Fixture f;
    const UnitId factory = f.roster.add(f.factoryType, 200.0f, 200.0f, 0, 100.0f);
    // 400 elmos away, far outside build reach — the case a player sees when they right-click
    // a factory across the base: the engineer has to arrive before it can lend anything.
    const UnitId helper = f.roster.add(f.engineerType, 600.0f, 200.0f, 0, 100.0f);
    f.economies[0].stored = {.mass = rm::sim::magFromFloat(1000.0f),
                             .energy = rm::sim::magFromFloat(1000.0f)};

    REQUIRE(f.build(factory, 205.0f, 200.0f));
    REQUIRE(f.assist(helper, factory));

    f.tick(5);
    REQUIRE(f.building.size() == 1);
    CHECK(rm::test::asFloat(f.building[0].assistPerTick) == 0.0f);

    f.tick(45);
    CHECK(f.roster.transform(helper).x < rm::sim::fxFromFloat(600.0f));
    CHECK(rm::test::asFloat(f.building[0].assistPerTick) == Approx(1.0f).margin(0.001));
}

TEST_CASE("assisting an upgrading structure speeds the upgrade") {
    Fixture f;
    const UnitId mex = f.roster.add(f.mexType, 200.0f, 200.0f, 0, 100.0f);
    const UnitId helper = f.roster.add(f.engineerType, 210.0f, 200.0f, 0, 100.0f);
    f.economies[0].stored = {.mass = rm::sim::magFromFloat(1000.0f),
                             .energy = rm::sim::magFromFloat(1000.0f)};

    REQUIRE(f.upgrade(mex));
    REQUIRE(f.assist(helper, mex));
    f.tick(1);

    // The upgrade is a construction founded by the structure itself, which is the link an
    // Assist resolves through — so the helper's 1 a tick lands on it beside the mex's own 1.
    REQUIRE(f.building.size() == 1);
    CHECK(f.building[0].upgradeOf == mex);
    CHECK(rm::test::asFloat(f.building[0].assistPerTick) == Approx(1.0f).margin(0.001));
    CHECK(rm::test::asFloat(f.building[0].buildTimeRemaining) == Approx(98.0f).margin(0.01));
}

TEST_CASE("an idle engineering station lends its rate to the nearest construction in reach") {
    Fixture f;
    const UnitId station = f.roster.add(f.stationType, 200.0f, 200.0f, 0, 100.0f);
    const UnitId near = f.roster.add(f.engineerType, 210.0f, 200.0f, 0, 100.0f);
    const UnitId far = f.roster.add(f.engineerType, 600.0f, 200.0f, 0, 100.0f);
    f.economies[0].stored = {.mass = rm::sim::magFromFloat(1000.0f),
                             .energy = rm::sim::magFromFloat(1000.0f)};

    // Two builds start the same tick: one 15 elmos from the station, one across the map.
    REQUIRE(f.build(near, 215.0f, 200.0f));
    REQUIRE(f.build(far, 605.0f, 200.0f));
    f.tick(1);
    REQUIRE(f.building.size() == 2);
    CHECK(f.roster.store.orders()[station.index].empty());  // nobody told it anything

    // The station's 2 a tick joins the engineer's 1 on the build in reach; the far one is
    // out of reach and gets nothing.
    CHECK(rm::test::asFloat(f.building[0].assistPerTick) == Approx(2.0f).margin(0.001));
    CHECK(rm::test::asFloat(f.building[0].buildTimeRemaining) == Approx(97.0f).margin(0.01));
    CHECK(rm::test::asFloat(f.building[1].assistPerTick) == 0.0f);

    // Help is recomputed each tick from the facts: once the near build is done the station
    // has nothing in reach and stops.
    f.building[0].buildTimeRemaining = rm::sim::magFromFloat(1.0f);
    f.tick(2);
    CHECK(f.building[0].finished());
    CHECK(rm::test::asFloat(f.building[1].assistPerTick) == 0.0f);
}

TEST_CASE("an idle engineering station repairs the nearest damaged ally, but builds first") {
    Fixture f;
    (void)f.roster.add(f.stationType, 200.0f, 200.0f, 0, 100.0f);
    const UnitId hurt = f.roster.add(f.repairableType, 210.0f, 200.0f, 0, 100.0f);
    const UnitId farHurt = f.roster.add(f.repairableType, 600.0f, 200.0f, 0, 100.0f);
    f.roster.health(hurt).current = rm::sim::magFromFloat(50.0f);
    f.roster.health(farHurt).current = rm::sim::magFromFloat(50.0f);
    f.economies[0].stored = {.mass = rm::sim::magFromFloat(1000.0f),
                             .energy = rm::sim::magFromFloat(1000.0f)};

    // Two build units a tick heal 2% of a 100-build-time unit: 50 -> 52, paid for from the
    // bank like any repair. The unit across the map is out of reach.
    f.tick(1);
    CHECK(rm::test::asFloat(f.roster.health(hurt).current) == Approx(52.0f).margin(0.01));
    CHECK(rm::test::asFloat(f.roster.health(farHurt).current) == Approx(50.0f).margin(0.01));
    CHECK(rm::test::asFloat(f.economies[0].stored.mass) < 1000.0f);

    // A construction in reach takes precedence over the repair (the C-183 ladder ranks
    // build-assist above repair): the station's rate moves to the build and the wounded
    // unit waits.
    const UnitId engineer = f.roster.add(f.engineerType, 190.0f, 200.0f, 0, 100.0f);
    REQUIRE(f.build(engineer, 185.0f, 200.0f));
    f.tick(1);
    REQUIRE(f.building.size() == 1);
    CHECK(rm::test::asFloat(f.building[0].assistPerTick) == Approx(2.0f).margin(0.001));
    CHECK(rm::test::asFloat(f.roster.health(hurt).current) == Approx(52.0f).margin(0.01));
}

TEST_CASE("an idle station ignores an orphaned construction and repairs instead") {
    Fixture f;
    (void)f.roster.add(f.stationType, 200.0f, 200.0f, 0, 100.0f);
    const UnitId engineer = f.roster.add(f.engineerType, 210.0f, 200.0f, 0, 100.0f);
    const UnitId hurt = f.roster.add(f.repairableType, 190.0f, 200.0f, 0, 100.0f);
    f.roster.health(hurt).current = rm::sim::magFromFloat(50.0f);
    f.economies[0].stored = {.mass = rm::sim::magFromFloat(1000.0f),
                             .energy = rm::sim::magFromFloat(1000.0f)};

    REQUIRE(f.build(engineer, 215.0f, 200.0f));
    f.tick(1);
    REQUIRE(f.building.size() == 1);
    CHECK(rm::test::asFloat(f.building[0].assistPerTick) == Approx(2.0f).margin(0.001));

    // The founder dies. Its row stays, unfinished, but nobody advances it: the station must
    // not keep feeding a construction that cannot progress, and its repair arm comes back.
    f.roster.health(engineer).current = rm::sim::Mag{};
    f.tick(2);
    CHECK(rm::test::asFloat(f.building[0].assistPerTick) == 0.0f);
    CHECK(rm::test::asFloat(f.roster.health(hurt).current) > 50.0f);
}

TEST_CASE("a station lends nothing to an enemy's construction or wounds") {
    Fixture f;
    (void)f.roster.add(f.stationType, 200.0f, 200.0f, 0, 100.0f);
    const UnitId enemy = f.roster.add(f.engineerType, 210.0f, 200.0f, 1, 100.0f);
    const UnitId enemyHurt = f.roster.add(f.repairableType, 190.0f, 200.0f, 1, 100.0f);
    f.roster.health(enemyHurt).current = rm::sim::magFromFloat(50.0f);
    f.economies[1].stored = {.mass = rm::sim::magFromFloat(1000.0f),
                             .energy = rm::sim::magFromFloat(1000.0f)};

    // Issued by army 1's own player: a build order names who gave it.
    REQUIRE(f.apply(Command{.kind = CommandKind::Build,
                            .player = 1,
                            .unit = enemy,
                            .targetX = rm::sim::fxFromFloat(215.0f),
                            .targetZ = rm::sim::fxFromFloat(200.0f),
                            .buildType = f.hutType}));
    f.tick(2);
    REQUIRE(f.building.size() == 1);
    CHECK(rm::test::asFloat(f.building[0].assistPerTick) == 0.0f);
    CHECK(rm::test::asFloat(f.roster.health(enemyHurt).current) == Approx(50.0f).margin(0.01));
}

TEST_CASE("a station with an assist order of its own follows the order, not the nearest work") {
    Fixture f;
    const UnitId station = f.roster.add(f.stationType, 200.0f, 200.0f, 0, 100.0f);
    const UnitId ordered = f.roster.add(f.engineerType, 230.0f, 200.0f, 0, 100.0f);
    const UnitId nearer = f.roster.add(f.engineerType, 210.0f, 200.0f, 0, 100.0f);
    f.economies[0].stored = {.mass = rm::sim::magFromFloat(1000.0f),
                             .energy = rm::sim::magFromFloat(1000.0f)};

    // Both sites are within the station's 40-elmo reach; the nearer one is 15 elmos off.
    REQUIRE(f.build(nearer, 215.0f, 200.0f));
    REQUIRE(f.build(ordered, 235.0f, 200.0f));
    REQUIRE(f.assist(station, ordered));
    f.tick(1);

    // The ordered target gets the station's whole rate, once; the nearer build gets none of
    // it — an ordered station is not idle, so the automatic scan skips it.
    REQUIRE(f.building.size() == 2);
    CHECK(rm::test::asFloat(f.building[0].assistPerTick) == 0.0f);
    CHECK(rm::test::asFloat(f.building[1].assistPerTick) == Approx(2.0f).margin(0.001));
}

TEST_CASE("factory assist accelerates the oldest unfinished queue entry") {
    Fixture f;
    const UnitId factory = f.roster.add(f.factoryType, 200.0f, 200.0f, 0, 100.0f);
    const UnitId helper = f.roster.add(f.engineerType, 210.0f, 200.0f, 0, 100.0f);
    f.economies[0].stored = {.mass = rm::sim::magFromFloat(1000.0f),
                             .energy = rm::sim::magFromFloat(1000.0f)};

    // The second product is queued, not constructed in parallel. Assist follows the factory's
    // one active project and transfers when the queue advances.
    REQUIRE(f.build(factory, 205.0f, 200.0f));
    REQUIRE(f.build(factory, 240.0f, 200.0f, true));
    REQUIRE(f.assist(helper, factory));
    f.tick(1);

    REQUIRE(f.building.size() == 1);
    CHECK(rm::test::asFloat(f.building[0].buildTimeRemaining) == Approx(98.0f).margin(0.01));
    CHECK(rm::test::asFloat(f.building[0].assistPerTick) == Approx(1.0f).margin(0.001));

    // Once the first product finishes, the SAME tick starts the second and puts a beat of work
    // into it, because a task that reports done re-enters the dispatcher immediately rather
    // than waiting for the next beat (`C-188`'s status `0`, and the state ladder of
    // `CUnitMobileBuildTask::TaskTick` which returns it after every transition). The standing
    // assist order rolls onto the new work a beat later, when the assist scan next runs — so
    // the cascade tick puts in the factory's 1 and the tick after it puts in 2.
    f.building[0].buildTimeRemaining = rm::sim::magFromFloat(1.0f);
    f.tick(1);
    f.tick(1);
    REQUIRE(f.building.size() == 2);
    CHECK(rm::test::asFloat(f.building[1].buildTimeRemaining) == Approx(97.0f).margin(0.01));
    CHECK(rm::test::asFloat(f.building[1].assistPerTick) == Approx(1.0f).margin(0.001));
}

TEST_CASE("multiple helpers add their build rates to the same project") {
    Fixture f;
    const UnitId founder = f.roster.add(f.engineerType, 200.0f, 200.0f, 0, 100.0f);
    const UnitId first = f.roster.add(f.engineerType, 210.0f, 200.0f, 0, 100.0f);
    const UnitId second = f.roster.add(f.engineerType, 190.0f, 200.0f, 0, 100.0f);
    f.economies[0].stored = {.mass = rm::sim::magFromFloat(1000.0f),
                             .energy = rm::sim::magFromFloat(1000.0f)};

    REQUIRE(f.build(founder, 205.0f, 200.0f));
    REQUIRE(f.assist(first, founder));
    REQUIRE(f.assist(second, founder));
    f.tick(1);

    CHECK(rm::test::asFloat(f.building[0].assistPerTick) == Approx(2.0f).margin(0.001));
    CHECK(rm::test::asFloat(f.building[0].buildTimeRemaining) == Approx(97.0f).margin(0.01));
}

TEST_CASE("assist walks a transitive guard chain to the builder at its end") {
    Fixture f;
    const UnitId founder = f.roster.add(f.engineerType, 200.0f, 200.0f, 0, 100.0f);
    const UnitId relay = f.roster.add(f.engineerType, 210.0f, 200.0f, 0, 100.0f);
    const UnitId helper = f.roster.add(f.engineerType, 220.0f, 200.0f, 0, 100.0f);
    f.economies[0].stored = {.mass = rm::sim::magFromFloat(1000.0f),
                             .energy = rm::sim::magFromFloat(1000.0f)};

    REQUIRE(f.build(founder, 205.0f, 200.0f));
    REQUIRE(f.assist(relay, founder));
    REQUIRE(f.assist(helper, relay));
    f.tick(1);

    REQUIRE(f.building.size() == 1);
    CHECK(rm::test::asFloat(f.building[0].assistPerTick) == Approx(2.0f).margin(0.001));
    CHECK(rm::test::asFloat(f.building[0].buildTimeRemaining) == Approx(97.0f).margin(0.01));
}

TEST_CASE("a cyclic assist chain contributes no work") {
    Fixture f;
    const UnitId first = f.roster.add(f.engineerType, 200.0f, 200.0f, 0, 100.0f);
    const UnitId second = f.roster.add(f.engineerType, 210.0f, 200.0f, 0, 100.0f);
    REQUIRE(f.assist(first, second));
    REQUIRE(f.assist(second, first));
    f.building.push_back(rm::sim::Construction{
        .builder = first,
        .totalBuildTime = rm::sim::magFromFloat(100.0f),
        .buildTimeRemaining = rm::sim::magFromFloat(100.0f),
    });

    CHECK(rm::sim::applyAssistance(f.roster.store, f.roster.catalog, f.building) == 0);
    CHECK(f.building.front().assistPerTick == rm::sim::Mag{});
}

TEST_CASE("a queued assist starts after the order ahead of it finishes") {
    Fixture f;
    const UnitId founder = f.roster.add(f.engineerType, 200.0f, 200.0f, 0, 100.0f);
    const UnitId helper = f.roster.add(f.engineerType, 100.0f, 200.0f, 0, 100.0f);
    f.economies[0].stored = {.mass = rm::sim::magFromFloat(1000.0f),
                             .energy = rm::sim::magFromFloat(1000.0f)};

    REQUIRE(f.build(founder, 205.0f, 200.0f));
    REQUIRE(f.move(helper, 300.0f, 200.0f));
    REQUIRE(f.assist(helper, founder, true));
    REQUIRE(f.roster.store.orders()[helper.index].current() != nullptr);
    CHECK(f.roster.store.orders()[helper.index].current()->kind() == CommandKind::Move);

    f.tick(45);
    REQUIRE(f.roster.store.orders()[helper.index].current() != nullptr);
    CHECK(f.roster.store.orders()[helper.index].current()->kind() == CommandKind::Assist);
    CHECK(rm::test::asFloat(f.building[0].assistPerTick) == Approx(1.0f).margin(0.001));
}

TEST_CASE("an assist is a standing order: it waits through an idle queue and ends with its target") {
    Fixture f;
    const UnitId founder = f.roster.add(f.engineerType, 200.0f, 200.0f, 0, 100.0f);
    const UnitId helper = f.roster.add(f.engineerType, 210.0f, 200.0f, 0, 100.0f);
    f.economies[0].stored = {.mass = rm::sim::magFromFloat(1000.0f),
                             .energy = rm::sim::magFromFloat(1000.0f)};

    // Assist BEFORE any build exists: the order holds, nothing breaks, nothing finishes.
    REQUIRE(f.assist(helper, founder));
    f.tick(5);
    CHECK_FALSE(f.roster.store.orders()[helper.index].empty());

    // The founder starts working; the standing helper joins the same tick it can.
    REQUIRE(f.build(founder, 205.0f, 200.0f));
    f.tick(1);
    REQUIRE(f.building.size() == 1);
    CHECK(rm::test::asFloat(f.building[0].assistPerTick) == Approx(1.0f).margin(0.001));

    // The founder dies: help has nothing to attach to and the order retires.
    //
    // ONE MORE BEAT OF HELP IS COUNTED FIRST, and that is retail's shape rather than a lag.
    // The assist scan belongs to the head of the tick, with the command-dispatch stage where
    // retail's assisting builders run their own tasks; a death lands later in the same tick,
    // in combat. So the beat a target dies on was already paid for before it died, exactly as
    // an assister's dispatch-stage tick precedes the damage stage that kills its target.
    f.roster.health(founder).current = rm::sim::Mag{};
    f.tick(1);
    CHECK(rm::test::asFloat(f.building[0].assistPerTick) == Approx(1.0f).margin(0.001));
    CHECK_FALSE(f.roster.store.orders()[helper.index].empty());
    f.tick(1);
    CHECK(rm::test::asFloat(f.building[0].assistPerTick) == 0.0f);
    CHECK(f.roster.store.orders()[helper.index].empty());
}

TEST_CASE("assist requires a compatible friendly assister and a distinct builder target") {
    Fixture f;
    const UnitId engineer = f.roster.add(f.engineerType, 200.0f, 200.0f, 0, 100.0f);
    const UnitId factory = f.roster.add(f.factoryType, 205.0f, 200.0f, 0, 100.0f);
    const UnitId tank = f.roster.add(f.tankType, 210.0f, 200.0f, 0, 100.0f);
    const UnitId theirs = f.roster.add(f.engineerType, 220.0f, 200.0f, 1, 100.0f);

    CHECK(f.assist(factory, engineer));
    CHECK_FALSE(f.assist(tank, engineer));
    CHECK_FALSE(f.assist(engineer, tank));
    CHECK_FALSE(f.assist(engineer, engineer));
    CHECK_FALSE(f.assist(engineer, theirs));

    REQUIRE(f.build(engineer, 240.0f, 200.0f));
    f.tick(1);
    REQUIRE(f.building.size() == 1);
    CHECK(rm::test::asFloat(f.building.front().buildTimeRemaining)
          == Approx(99.0f).margin(0.01));
    CHECK(f.building.front().assistPerTick == rm::sim::Mag{});
}

TEST_CASE("a queued assist rejects a non-builder target before entering authoritative state") {
    Fixture f;
    const UnitId engineer = f.roster.add(f.engineerType, 200.0f, 200.0f, 0, 100.0f);
    const UnitId tank = f.roster.add(f.tankType, 210.0f, 200.0f, 0, 100.0f);

    CHECK_FALSE(f.assist(engineer, tank, true));
    CHECK(f.roster.store.orders()[engineer.index].empty());
}

TEST_CASE("an assist order survives the log round trip") {
    rm::sim::CommandLog log;
    log.record(rm::sim::CommandIssue{
        .tick = 3,
        .source = 0,
        .id = rm::commandId(0, 0),
        .player = 0,
        .kind = CommandKind::Assist,
        .units = {UnitId{1, 1}},
        .targetX = rm::sim::fxFromFloat(200.0f),
        .targetZ = rm::sim::fxFromFloat(200.0f),
        .target = UnitId{0, 1},
    });

    const auto path = std::filesystem::temp_directory_path() / "rm_assist_log_test.txt";
    REQUIRE(rm::sim::writeCommandLog(log, path.string()));
    const auto reread = rm::sim::readCommandLog(path.string());
    std::filesystem::remove(path);

    REQUIRE(reread.has_value());
    REQUIRE(reread->size() == 1);
    CHECK(reread->all()[0] == log.all()[0]);
}

TEST_CASE("shift-clicking the same assist target removes the queued order") {
    rm::sim::CommandQueue queue;
    const Command move{.kind = CommandKind::Move};
    const Command assist{.kind = CommandKind::Assist, .target = UnitId{3, 1}};

    CHECK(queue.give(move, false) == rm::sim::CommandQueue::Result::Replaced);
    CHECK(queue.give(assist, true) == rm::sim::CommandQueue::Result::Appended);
    CHECK(queue.give(assist, true) == rm::sim::CommandQueue::Result::Cancelled);
    REQUIRE(queue.current() != nullptr);
    CHECK(queue.current()->kind() == CommandKind::Move);
}

// --- `C-112`'s two open differences ------------------------------------------------------

TEST_CASE("clamping the summed build rate matches clamping each builder's call in turn") {
    // THE FIRST OF `C-112`'s TWO OPEN DIFFERENCES, and it turns out to be arithmetic rather
    // than architecture. Retail makes N independent `target->Materialize(delta_i)` calls, one
    // per builder, each clamped inside the target (`C-187`); we sum the rates onto one record
    // and clamp once. `C-151` flagged that as an order-dependent rule we implement
    // order-independently — but the clamp only ever saturates at completion and no builder
    // contributes a negative delta, so the two reach the SAME fraction, and per-call summation
    // reaches the same fraction in any order. The order dependence retail really has is in the
    // health it adds while a thing is being built and in whose stack frame the completion
    // cascade runs, and neither is representable here: a construction is a record, not a
    // partially built entity with health of its own.
    //
    // What is NOT identical is quantisation. `(a + b + c) * r` truncates once and
    // `a*r + b*r + c*r` truncates three times, so the two answers may differ by a step or two
    // of `Fx`'s last bit — a rounding residue in the same family as `C-160`, not an ordering
    // difference. The margin below is stated in those steps rather than in decimals so that a
    // real divergence cannot hide inside a generous tolerance.
    const std::array<float, 3> authored{0.7f, 1.3f, 2.5f};  // unequal, so order could show
    const rm::sim::Fx ratio = rm::sim::fxFromFloat(0.5f);   // a half-funded beat

    // Retail's shape: one clamped call per builder, in whatever order the task threads run.
    const auto perCall = [ratio](rm::sim::Mag remaining, const std::array<float, 3>& rates) {
        for (const float rate : rates) {
            remaining -= rm::sim::magFromFloat(rate) * ratio;
            remaining = std::max(rm::sim::Mag{}, remaining);
        }
        return remaining;
    };

    // Ours: one record, the founder's rate plus everyone helping, clamped once.
    const auto summed = [ratio](rm::sim::Mag remaining, const std::array<float, 3>& rates) {
        rm::sim::Construction work;
        work.totalBuildTime = rm::sim::magFromFloat(100.0f);
        work.buildTimeRemaining = remaining;
        work.buildPerTick = rm::sim::magFromFloat(rates[0]);
        work.assistPerTick = rm::sim::magFromFloat(rates[1]) + rm::sim::magFromFloat(rates[2]);
        work.fundedLastTick = ratio;
        rm::sim::advanceConstruction(work);
        return work.buildTimeRemaining;
    };

    // An ordinary beat, and the beat that COMPLETES — the only one where the clamp does
    // anything at all, and therefore the only one where an ordering rule could bite.
    for (const float start : {60.0f, 1.0f}) {
        const rm::sim::Mag remaining = rm::sim::magFromFloat(start);

        std::array<float, 3> order = authored;
        std::ranges::sort(order);
        const rm::sim::Mag first = perCall(remaining, order);
        do {
            CHECK(perCall(remaining, order) == first);  // exactly, not approximately
        } while (std::ranges::next_permutation(order).found);

        // Within two of `Fx`'s last steps of the per-call answer: three truncating multiplies
        // against one.
        CHECK(rm::test::asFloat(summed(remaining, authored))
              == Approx(rm::test::asFloat(first)).margin(2.0f * rm::test::kFxStep));
    }
}

TEST_CASE("a finished build is retired by the dispatch stage, and the next order starts behind it") {
    // THE SECOND OF `C-112`'s TWO OPEN DIFFERENCES. This used to happen in the economy
    // write-back at the foot of the tick, which made construction a SECOND site that mutates a
    // queue head — the thing `C-096` said there was only one of and `C-211` restated as "one
    // site advances a unit's queue as a consequence of that unit's own sub-task finishing".
    // Retail's is the command-dispatch stage, and so is this.
    Fixture f;
    const UnitId founder = f.roster.add(f.engineerType, 200.0f, 200.0f, 0, 100.0f);
    f.economies[0].stored = {.mass = rm::sim::magFromFloat(1000.0f),
                             .energy = rm::sim::magFromFloat(1000.0f)};

    REQUIRE(f.build(founder, 205.0f, 200.0f));
    REQUIRE(f.apply(Command{.kind = CommandKind::Move,
                            .queued = true,
                            .unit = founder,
                            .targetX = rm::sim::fxFromFloat(260.0f),
                            .targetZ = rm::sim::fxFromFloat(200.0f)}));
    REQUIRE(f.building.size() == 1);
    REQUIRE(f.roster.store.orders()[founder.index].size() == 2);

    // One tick's work away from done, so this dispatch beat both completes it and retires it.
    f.building[0].buildTimeRemaining = rm::sim::magFromFloat(0.5f);

    const std::vector<const rm::sim::PassabilityGrid*> grids(f.roster.catalog.size(), &f.grid);
    std::vector<rm::sim::Construction> done;
    (void)rm::sim::advanceOrders(f.roster.store, f.roster.catalog, f.terrain, grids,
                                 f.roster.rate, &f.building, nullptr, nullptr, &done);

    // Completed, reported, and the order gone — all inside the one call.
    REQUIRE(done.size() == 1);
    CHECK(f.building[0].finished());
    REQUIRE(f.roster.store.orders()[founder.index].size() == 1);
    REQUIRE(f.roster.store.orders()[founder.index].current() != nullptr);
    // And the order behind it started in the SAME beat, which is what `C-188`'s status `0`
    // re-entry does: a task that reports done hands the dispatcher straight back to itself.
    CHECK(f.roster.store.orders()[founder.index].current()->kind() == CommandKind::Move);
    CHECK(f.roster.store.motion()[founder.index].moving);
}
