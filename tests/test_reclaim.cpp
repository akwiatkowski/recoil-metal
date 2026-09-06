// Wrecks back into mass: the reclaim order, the harvest, and the economy they feed.
//
// The numbers are the game's own, cited on `UnitDef` and `core/sim/Reclaim.hpp`: a wreck
// holds `BuildCost × 0.9` and a reclaimer drains `10 × BuildRate` value a second — so a
// rate-10 engineer takes 10 mass a tick at 10 Hz, and the arithmetic below is exact in
// fixed point, which is what lets these tests use == rather than margins.
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "core/sim/Command.hpp"
#include "core/sim/FeatureStore.hpp"
#include "core/sim/Reclaim.hpp"
#include "core/sim/Skirmish.hpp"
#include "core/sim/UnitStore.hpp"
#include "core/unit/UnitBlueprint.hpp"

#include "support/FxMatchers.hpp"
#include "support/TestRoster.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <vector>

using rm::sim::Command;
using rm::sim::CommandKind;
using rm::sim::Feature;
using rm::sim::FeatureId;
using rm::sim::FeatureStore;
using rm::sim::Player;
using rm::sim::UnitId;
using Catch::Approx;

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
    rm::UnitTypeIndex guardType{};
    rm::UnitTypeIndex tankType{};

    Fixture() {
        rm::unitdef::UnitDef engineer;
        engineer.name = "test_engineer";
        engineer.buildRate = 10.0f;  // 1 build unit per tick at 10 Hz
        engineerType = roster.addType(engineer);

        rm::unitdef::UnitDef guard = engineer;
        guard.name = "test_guard";
        guard.speedElmosPerSecond = 20.0f;
        guard.guardScanRadiusElmos = rm::sim::fxFromFloat(80.0f);
        rm::unitdef::Weapon gun;
        gun.label = "guard gun";
        gun.role = rm::unitdef::WeaponRole::DirectFire;
        gun.targetPriorities = {{"LAND"}};
        gun.damage = rm::sim::magFromFloat(10.0f);
        gun.maxRange = rm::sim::fxFromFloat(30.0f);
        gun.rateOfFire = 1.0f;
        guard.weapons.push_back(gun);
        guardType = roster.addType(guard);

        rm::unitdef::UnitDef tank;
        tank.name = "test_tank";
        tank.buildCostMass = rm::sim::magFromFloat(100.0f);
        tank.buildCostEnergy = rm::sim::magFromFloat(200.0f);
        tank.buildTime = rm::sim::magFromFloat(100.0f);
        tankType = roster.addType(tank);
    }

    /// A standard 90-mass wreck — what a 100-mass unit leaves at the corpus's 0.9.
    [[nodiscard]] FeatureId wreckAt(float x, float z, float mass = 90.0f,
                                    float energy = 0.0f, float health = 100.0f) {
        return features.add(Feature{.at = {rm::sim::fxFromFloat(x), rm::sim::Fx{},
                                           rm::sim::fxFromFloat(z)},
                                    .radiusElmos = rm::sim::Fx::fromInt(4),
                                    .fromType = tankType,
                                    .armyIndex = 1,
                                    .health = rm::sim::magFromFloat(health),
                                    .maximumHealth = rm::sim::magFromFloat(health),
                                    .maximumMassReclaim = rm::sim::magFromFloat(mass),
                                    .maximumEnergyReclaim = rm::sim::magFromFloat(energy),
                                    .massRemaining = rm::sim::magFromFloat(mass),
                                    .energyRemaining = rm::sim::magFromFloat(energy),
                                    .reclaimWorkRemaining = rm::sim::magFromFloat(
                                        std::max(mass, energy)),
                                    .reclaimWorkTotal = rm::sim::magFromFloat(
                                        std::max(mass, energy)),
                                    .reclaimFraction = rm::sim::kFxOne,
                                    .maximumReclaimPerBuildRate = rm::sim::fxFromFloat(10.0f),
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

    [[nodiscard]] bool reclaimUnit(UnitId who, UnitId target) {
        const rm::sim::Transform& at = roster.store.transforms()[target.index];
        return rm::sim::applyCommand(Command{.kind = CommandKind::ReclaimUnit,
                                             .unit = who,
                                             .targetX = at.x,
                                             .targetZ = at.z,
                                             .target = target},
                                     roster.store, roster.catalog, players, armies, terrain,
                                     grid, roster.rate, &building, nullptr, &features);
    }

    [[nodiscard]] bool repair(UnitId who, UnitId target, bool queued = false) {
        return rm::sim::applyCommand(Command{.kind = CommandKind::Repair,
                                             .queued = queued,
                                             .unit = who,
                                             .target = target},
                                     roster.store, roster.catalog, players, armies, terrain,
                                     grid, roster.rate, &building);
    }

    [[nodiscard]] bool assist(UnitId who, UnitId target, CommandKind kind = CommandKind::Assist) {
        const rm::sim::Transform& at = roster.store.transforms()[target.index];
        return rm::sim::applyCommand(Command{.kind = kind,
                                             .unit = who,
                                             .targetX = at.x,
                                             .targetZ = at.z,
                                             .target = target},
                                     roster.store, roster.catalog, players, armies, terrain,
                                     grid, roster.rate, &building, nullptr, &features);
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

TEST_CASE("reclaim progress credits mass and energy by the same applied fraction") {
    Fixture f;
    const UnitId engineer = f.roster.add(f.engineerType, 200.0f, 200.0f, 0, 100.0f);
    const FeatureId wreck = f.wreckAt(210.0f, 200.0f, /*mass=*/90.0f,
                                      /*energy=*/180.0f);

    REQUIRE(f.reclaim(engineer, wreck));
    f.tick(1);

    // Energy is the longer side of GetReclaimCosts, so one eighteenth of BOTH values is
    // earned. Draining each resource by the same absolute pace would incorrectly pay 10 mass.
    CHECK(rm::test::asFloat(f.economies[0].stored.mass) == Approx(5.0f).margin(0.01f));
    CHECK(rm::test::asFloat(f.economies[0].stored.energy) == Approx(10.0f).margin(0.01f));
    REQUIRE(f.features.find(wreck) != nullptr);
    CHECK(rm::sim::fxToFloat(f.features.find(wreck)->reclaimFraction)
          == Approx(17.0f / 18.0f).margin(0.001f));

    f.tick(17);
    CHECK(f.features.find(wreck) == nullptr);
    CHECK(rm::test::asFloat(f.economies[0].stored.mass) == Approx(90.0f).margin(0.01f));
    CHECK(rm::test::asFloat(f.economies[0].stored.energy) == Approx(180.0f).margin(0.01f));
}

TEST_CASE("damage scales a wreck's value and reclaim time from its maximums") {
    Fixture f;
    const FeatureId wreck = f.wreckAt(210.0f, 200.0f, /*mass=*/90.0f,
                                      /*energy=*/180.0f, /*health=*/100.0f);

    CHECK(rm::test::asFloat(rm::sim::damageFeature(f.features, wreck,
                                                   rm::sim::magFromFloat(25.0f)))
          == 25.0f);
    const Feature* damaged = f.features.find(wreck);
    REQUIRE(damaged != nullptr);
    CHECK(rm::test::asFloat(damaged->health) == 75.0f);
    CHECK(rm::test::asFloat(damaged->massRemaining) == Approx(67.5f).margin(0.01f));
    CHECK(rm::test::asFloat(damaged->energyRemaining) == Approx(135.0f).margin(0.01f));
    CHECK(rm::sim::fxToFloat(damaged->reclaimPerBuildRate)
          == Approx(40.0f / 3.0f).margin(0.01f));

    CHECK(rm::test::asFloat(rm::sim::damageFeature(f.features, wreck,
                                                   rm::sim::magFromFloat(100.0f)))
          == 75.0f);
    CHECK(f.features.find(wreck) == nullptr);
}

TEST_CASE("a guard copies its guardee's reclaim target before considering repair") {
    Fixture f;
    const UnitId guardee = f.roster.add(f.engineerType, 200.0f, 200.0f, 0, 100.0f);
    const UnitId guard = f.roster.add(f.guardType, 205.0f, 200.0f, 0, 100.0f);
    const UnitId damaged = f.roster.add(f.tankType, 210.0f, 200.0f, 0, 100.0f);
    f.roster.health(damaged).current = rm::sim::magFromFloat(50.0f);
    f.economies[0].stored = {.mass = rm::sim::magFromFloat(100.0f),
                             .energy = rm::sim::magFromFloat(100.0f)};
    const FeatureId wreck = f.wreckAt(208.0f, 200.0f);

    REQUIRE(f.reclaim(guardee, wreck));
    REQUIRE(f.assist(guard, guardee));
    f.tick(1);

    REQUIRE(f.features.find(wreck) != nullptr);
    CHECK(rm::test::asFloat(f.features.find(wreck)->massRemaining) == 70.0f);
    CHECK(rm::test::asFloat(f.roster.health(damaged).current) == 50.0f);
}

TEST_CASE("a Mantis Guard cannot borrow a reclaim capability from its guardee", "[guard][guard-work-regression]") {
    const char* home = std::getenv("HOME");
    REQUIRE(home != nullptr);
    const auto path = std::filesystem::path{home} / "projects/llm/input/faf/units/URL0107/URL0107_unit.bp";
    if (!std::filesystem::is_regular_file(path)) SKIP("no retail Mantis blueprint");
    const auto mantis = rm::unitbp::loadFile(path);
    REQUIRE(mantis);
    REQUIRE(mantis->buildRate > 0);
    REQUIRE(mantis->hasCommandCap("RULEUCC_Guard"));
    REQUIRE_FALSE(mantis->hasCommandCap("RULEUCC_Reclaim"));
    Fixture f;
    const auto type = f.roster.addType(*mantis);
    const auto founder = f.roster.add(f.engineerType, 200, 200, 0, 100);
    const auto guard = f.roster.add(type, 205, 200, 0, 100);
    const auto wreck = f.wreckAt(208, 200);
    REQUIRE(f.reclaim(founder, wreck));
    REQUIRE(f.assist(guard, founder, CommandKind::Guard));
    f.tick();
    REQUIRE(f.features.find(wreck));
    CHECK(rm::test::asFloat(f.features.find(wreck)->massRemaining) == 80.0f);
}

TEST_CASE("Guard repair respects an explicitly forbidden repair capability", "[guard][guard-work-regression]") {
    Fixture f;
    auto restricted = *f.roster.catalog.def(f.guardType);
    restricted.commandCapsDeclared = true;
    restricted.commandCaps = {"RULEUCC_Guard"};
    const auto type = f.roster.addType(restricted);
    const auto founder = f.roster.add(f.engineerType, 200, 200, 0, 100);
    const auto guard = f.roster.add(type, 205, 200, 0, 100);
    const auto damaged = f.roster.add(f.tankType, 210, 200, 0, 100);
    f.roster.health(damaged).current = rm::sim::Mag::fromInt(50);
    f.economies[0].stored = {rm::sim::Mag::fromInt(100), rm::sim::Mag::fromInt(100)};
    REQUIRE(f.assist(guard, founder, CommandKind::Guard));
    f.tick();
    CHECK(f.roster.health(damaged).current == rm::sim::Mag::fromInt(50));
}

TEST_CASE("guard repair scans around the guardee and chooses the nearest unit to the guard") {
    Fixture f;
    const UnitId guardee = f.roster.add(f.engineerType, 200.0f, 200.0f, 0, 100.0f);
    const UnitId guard = f.roster.add(f.guardType, 240.0f, 200.0f, 0, 100.0f);
    const UnitId nearerGuard = f.roster.add(f.tankType, 245.0f, 200.0f, 0, 100.0f);
    const UnitId nearerGuardee = f.roster.add(f.tankType, 190.0f, 200.0f, 0, 100.0f);
    f.roster.health(nearerGuard).current = rm::sim::magFromFloat(50.0f);
    f.roster.health(nearerGuardee).current = rm::sim::magFromFloat(50.0f);
    f.economies[0].stored = {.mass = rm::sim::magFromFloat(100.0f),
                             .energy = rm::sim::magFromFloat(100.0f)};

    REQUIRE(f.assist(guard, guardee));
    f.tick(1);

    CHECK(rm::test::asFloat(f.roster.health(nearerGuard).current)
          == Approx(51.0f).margin(0.01f));
    CHECK(rm::test::asFloat(f.roster.health(nearerGuardee).current) == 50.0f);
}

TEST_CASE("the guard leash returns to the guardee before engaging a nearby enemy") {
    Fixture f;
    const UnitId guardee = f.roster.add(f.engineerType, 200.0f, 200.0f, 0, 100.0f);
    const UnitId guard = f.roster.add(f.guardType, 300.0f, 200.0f, 0, 100.0f);
    const UnitId enemy = f.roster.add(f.tankType, 310.0f, 200.0f, 1, 100.0f);
    (void)enemy;

    REQUIRE(f.assist(guard, guardee));
    f.tick(1);

    CHECK(f.roster.transform(guard).x < rm::sim::fxFromFloat(300.0f));
    REQUIRE(f.roster.store.orders()[guard.index].active() != nullptr);
    CHECK(f.roster.store.orders()[guard.index].active()->kind() == CommandKind::Assist);
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

TEST_CASE("an engineer un-builds an enemy unit into its store, and the unit leaves no wreck") {
    // FAF GetReclaimCosts: work = max(mass 100, energy 200) = 200 at BuildRate 10 per second,
    // so twenty ticks; each tick pays a twentieth of both costs and takes a twentieth of the
    // health. The end is a Destroy, not a death: no wreck, no kill credit.
    Fixture f;
    const UnitId engineer = f.roster.add(f.engineerType, 200.0f, 200.0f, 0, 100.0f);
    const UnitId victim = f.roster.add(f.tankType, 206.0f, 200.0f, 1, 100.0f);
    REQUIRE(f.reclaimUnit(engineer, victim));
    REQUIRE(f.roster.store.orders()[engineer.index].active() != nullptr);
    CHECK(f.roster.store.orders()[engineer.index].active()->kind() == CommandKind::ReclaimUnit);

    f.tick(10);
    CHECK(f.roster.store.alive(victim));
    CHECK(rm::test::asFloat(f.roster.health(victim).current) == 50.0f);
    CHECK(rm::test::asFloat(f.economies[0].stored.mass) == 50.0f);
    CHECK(rm::test::asFloat(f.economies[0].stored.energy) == 100.0f);

    f.tick(10);
    CHECK_FALSE(f.roster.store.alive(victim));
    CHECK(rm::test::asFloat(f.economies[0].stored.mass) == 100.0f);
    CHECK(rm::test::asFloat(f.economies[0].stored.energy) == 200.0f);
    CHECK(f.features.size() == 0);  // reclaimed away, not killed: nothing left on the ground

    f.tick(1);
    CHECK(f.features.size() == 0);  // and retireDead does not mistake it for a death later
    CHECK(f.roster.store.orders()[engineer.index].empty());  // the order completed with the unit
}

TEST_CASE("only a builder un-builds, and never an ally or itself") {
    Fixture f;
    const UnitId engineer = f.roster.add(f.engineerType, 200.0f, 200.0f, 0, 100.0f);
    const UnitId tank = f.roster.add(f.tankType, 210.0f, 200.0f, 0, 100.0f);
    const UnitId enemy = f.roster.add(f.tankType, 220.0f, 200.0f, 1, 100.0f);
    CHECK_FALSE(f.reclaimUnit(tank, enemy));        // a tank has no build rate
    CHECK_FALSE(f.reclaimUnit(engineer, tank));     // own side: retail permits it, we do not yet
    CHECK_FALSE(f.reclaimUnit(engineer, engineer)); // itself
    CHECK(f.reclaimUnit(engineer, enemy));
}

TEST_CASE("a unit reclaim survives the log round trip") {
    rm::sim::CommandLog log;
    log.record(rm::sim::CommandIssue{
        .tick = 9,
        .source = 1,
        .id = rm::commandId(1, 0),
        .player = 1,
        .kind = CommandKind::ReclaimUnit,
        .units = {UnitId{3, 2}},
        .targetX = rm::sim::fxFromFloat(210.0f),
        .targetZ = rm::sim::fxFromFloat(200.0f),
        .target = UnitId{5, 1},
    });
    const auto path = std::filesystem::temp_directory_path() / "rm_reclaim_unit_log_test.txt";
    REQUIRE(rm::sim::writeCommandLog(log, path.string()));
    const auto reread = rm::sim::readCommandLog(path.string());
    std::filesystem::remove(path);
    REQUIRE(reread.has_value());
    REQUIRE(reread->size() == 1);
    CHECK(reread->all()[0] == log.all()[0]);
    CHECK(reread->all()[0].kind == CommandKind::ReclaimUnit);
}

TEST_CASE("a reclaim order survives the log round trip") {
    // The replay contract: the kind serialises by name, the feature handle rides in the
    // target columns the chase already writes, and what comes back is what went in.
    rm::sim::CommandLog log;
    log.record(rm::sim::CommandIssue{
        .tick = 7,
        .source = 1,
        .id = rm::commandId(1, 0),
        .player = 1,
        .kind = CommandKind::Reclaim,
        .units = {UnitId{3, 2}},
        .targetX = rm::sim::fxFromFloat(210.0f),
        .targetZ = rm::sim::fxFromFloat(200.0f),
        .target = UnitId{5, 1},
    });

    const auto path = std::filesystem::temp_directory_path() / "rm_reclaim_log_test.txt";
    REQUIRE(rm::sim::writeCommandLog(log, path.string()));
    const auto reread = rm::sim::readCommandLog(path.string());
    std::filesystem::remove(path);

    REQUIRE(reread.has_value());
    REQUIRE(reread->size() == 1);
    CHECK(reread->all()[0] == log.all()[0]);
}

TEST_CASE("a repair command records semantic intent and can approach an allied target") {
    Fixture f;
    f.armies[1].alliance = f.armies[0].alliance;
    const UnitId engineer = f.roster.add(f.engineerType, 200.0f, 200.0f, 0, 100.0f);
    const UnitId ally = f.roster.add(f.tankType, 300.0f, 200.0f, 1, 100.0f);
    f.roster.health(ally).current = rm::sim::magFromFloat(50.0f);

    REQUIRE(f.repair(engineer, ally));
    REQUIRE(f.roster.store.orders()[engineer.index].current() != nullptr);
    CHECK(f.roster.store.orders()[engineer.index].current()->kind() == CommandKind::Repair);
    CHECK(f.roster.store.motion()[engineer.index].moving);

    rm::sim::CommandLog log;
    log.record(rm::sim::CommandIssue{.tick = 7,
                                     .source = 0,
                                     .id = rm::commandId(0, 0),
                                     .player = 0,
                                     .kind = CommandKind::Repair,
                                     .units = {engineer},
                                     .target = ally});
    const auto path = std::filesystem::temp_directory_path() / "rm_repair_log_test.txt";
    REQUIRE(rm::sim::writeCommandLog(log, path.string()));
    const auto reread = rm::sim::readCommandLog(path.string());
    std::filesystem::remove(path);

    REQUIRE(reread.has_value());
    REQUIRE(reread->size() == 1);
    CHECK(reread->all()[0] == log.all()[0]);
}

TEST_CASE("repair spends target construction cost and stalls at the available economy ratio") {
    Fixture f;
    f.armies[1].alliance = f.armies[0].alliance;
    const UnitId engineer = f.roster.add(f.engineerType, 200.0f, 200.0f, 0, 100.0f);
    const UnitId ally = f.roster.add(f.tankType, 210.0f, 200.0f, 1, 100.0f);
    f.roster.health(ally).current = rm::sim::magFromFloat(50.0f);
    f.economies[0].stored = {.mass = rm::sim::magFromFloat(100.0f),
                             .energy = rm::sim::magFromFloat(200.0f)};

    REQUIRE(f.repair(engineer, ally));
    f.tick();

    // One build unit restores 1% of health and asks the target's 100/200 cost at the
    // corresponding 1/100 rate: about one mass and two energy per tick. The per-tick
    // share is quantized in the same way as construction, so it is just below the
    // decimal values rather than exactly them.
    CHECK(51.0f == rm::test::near(f.roster.health(ally).current));
    CHECK(rm::test::asFloat(f.economies[0].stored.mass)
          == Catch::Approx(99.0f).margin(0.01f));
    CHECK(rm::test::asFloat(f.economies[0].stored.energy)
          == Catch::Approx(198.0f).margin(0.02f));

    f.economies[0].stored = {.mass = rm::sim::magFromFloat(0.5f),
                              .energy = rm::sim::magFromFloat(1.0f)};
    f.tick();
    CHECK(rm::test::asFloat(f.roster.health(ally).current)
          == Catch::Approx(51.5f).margin(0.01f));
    CHECK(0.0f == rm::test::near(f.economies[0].stored.mass));
    CHECK(0.0f == rm::test::near(f.economies[0].stored.energy));
}

TEST_CASE("repair competes with construction in the shared economy allocation") {
    Fixture f;
    f.armies[1].alliance = f.armies[0].alliance;
    const UnitId engineer = f.roster.add(f.engineerType, 200.0f, 200.0f, 0, 100.0f);
    const UnitId ally = f.roster.add(f.tankType, 210.0f, 200.0f, 1, 100.0f);
    f.roster.health(ally).current = rm::sim::magFromFloat(50.0f);

    // Both requests cost one mass and two energy at this rate. The shared bank can cover only
    // one, so construction and repair must each receive half; repair may not debit first.
    f.building.push_back(rm::sim::Construction{
        .armyIndex = 0,
        .cost = {.mass = rm::sim::magFromFloat(100.0f),
                 .energy = rm::sim::magFromFloat(200.0f)},
        .buildTimeRemaining = rm::sim::magFromFloat(100.0f),
        .totalBuildTime = rm::sim::magFromFloat(100.0f),
        .buildPerTick = rm::sim::magFromFloat(1.0f),
    });
    f.economies[0].stored = {.mass = rm::sim::magFromFloat(1.0f),
                              .energy = rm::sim::magFromFloat(2.0f)};

    REQUIRE(f.repair(engineer, ally));
    f.tick();

    CHECK(rm::test::asFloat(f.roster.health(ally).current)
          == Catch::Approx(50.5f).margin(0.01f));
    CHECK(rm::test::asFloat(f.building[0].fundedLastTick)
          == Catch::Approx(0.5f).margin(0.01f));
    CHECK(rm::test::asFloat(f.economies[0].usageLastTick.mass)
          == Catch::Approx(1.0f).margin(0.01f));
    CHECK(rm::test::asFloat(f.economies[0].usageLastTick.energy)
          == Catch::Approx(2.0f).margin(0.01f));
    CHECK(0.0f == rm::test::near(f.economies[0].stored.mass));
    CHECK(0.0f == rm::test::near(f.economies[0].stored.energy));
}

TEST_CASE("repair finishes on alliance loss and dispatches its queued follower") {
    Fixture f;
    f.armies[1].alliance = f.armies[0].alliance;
    const UnitId engineer = f.roster.add(f.engineerType, 200.0f, 200.0f, 0, 100.0f);
    const UnitId ally = f.roster.add(f.tankType, 210.0f, 200.0f, 1, 100.0f);
    f.roster.health(ally).current = rm::sim::magFromFloat(50.0f);
    REQUIRE(f.repair(engineer, ally));
    REQUIRE(rm::sim::applyCommand(Command{.kind = CommandKind::Move,
                                           .queued = true,
                                           .unit = engineer,
                                           .targetX = rm::sim::fxFromFloat(300.0f),
                                           .targetZ = rm::sim::fxFromFloat(200.0f)},
                                   f.roster.store, f.roster.catalog, f.players, f.armies,
                                   f.terrain, f.grid, f.roster.rate, &f.building));

    f.armies[1].alliance = 1;
    f.tick();

    REQUIRE(f.roster.store.orders()[engineer.index].current() != nullptr);
    CHECK(f.roster.store.orders()[engineer.index].current()->kind() == CommandKind::Move);
    CHECK(f.roster.store.motion()[engineer.index].moving);
}

TEST_CASE("repair rejects enemies, full targets, and non-builders") {
    Fixture f;
    const UnitId engineer = f.roster.add(f.engineerType, 200.0f, 200.0f, 0, 100.0f);
    const UnitId target = f.roster.add(f.tankType, 210.0f, 200.0f, 1, 100.0f);
    const UnitId tank = f.roster.add(f.tankType, 220.0f, 200.0f, 0, 100.0f);
    f.roster.health(target).current = rm::sim::magFromFloat(50.0f);

    CHECK_FALSE(f.repair(engineer, target));
    f.armies[1].alliance = f.armies[0].alliance;
    f.roster.health(target).current = f.roster.health(target).maximum;
    CHECK_FALSE(f.repair(engineer, target));
    f.roster.health(target).current = rm::sim::magFromFloat(50.0f);
    CHECK_FALSE(f.repair(tank, target));
}

TEST_CASE("repair starts at build reach, holds to twice that reach, then finishes") {
    Fixture f;
    f.armies[1].alliance = f.armies[0].alliance;
    const UnitId engineer = f.roster.add(f.engineerType, 200.0f, 200.0f, 0, 100.0f);
    const UnitId ally = f.roster.add(f.tankType, 210.0f, 200.0f, 1, 100.0f);
    f.roster.health(ally).current = rm::sim::magFromFloat(50.0f);
    f.economies[0].stored = {.mass = rm::sim::magFromFloat(1000.0f),
                             .energy = rm::sim::magFromFloat(1000.0f)};
    const rm::sim::Fx reach = rm::sim::repairReach(
        f.roster.catalog, f.engineerType, f.roster.motion(engineer),
        f.roster.motion(ally));
    f.roster.transform(ally).x = f.roster.transform(engineer).x + reach;
    f.roster.reindex();

    REQUIRE(f.repair(engineer, ally));
    f.tick();
    CHECK(rm::test::asFloat(f.roster.health(ally).current) == 51.0f);

    f.roster.transform(ally).x = f.roster.transform(engineer).x + reach * 2;
    f.roster.reindex();
    f.tick();
    CHECK(rm::test::asFloat(f.roster.health(ally).current) == 52.0f);

    f.roster.transform(ally).x = f.roster.transform(engineer).x + reach * 2 + rm::sim::Fx::fromInt(1);
    f.roster.reindex();
    f.tick();
    CHECK(rm::test::asFloat(f.roster.health(ally).current) == 52.0f);
    CHECK(f.roster.store.orders()[engineer.index].empty());
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
    f.roster.store.orders()[engineer.index].currentMutable()->setTarget(enemy);

    CHECK(rm::sim::servicePatrolBuilders(f.roster.store, f.roster.catalog, f.armies,
                                         &f.features, f.economies)
          == 0);
    CHECK(rm::test::asFloat(f.roster.health(ally).current) == 50.0f);
    REQUIRE(f.features.find(wreck) != nullptr);
    CHECK(rm::test::asFloat(f.features.find(wreck)->massRemaining) == 90.0f);
}
