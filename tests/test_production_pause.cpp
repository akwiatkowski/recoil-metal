// Pausable production (#15590): one authoritative per-unit flag, checked by
// every economy consumer — construction, silo ammunition, enhancements,
// repairs, captures, reclaim and the unit's own income.
//
// Retail gates the pause button on `RULEUTC_ProductionToggle`, which only
// fabricators, generators and engineering stations declare. FAF extends it to
// everything that produces, and that is the semantic here: eligibility is a
// capability (builds, produces, holds a silo), not a cap.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "core/map/HeightField.hpp"
#include "core/sim/Command.hpp"
#include "core/sim/Economy.hpp"
#include "core/sim/Pathfinding.hpp"
#include "core/sim/Reclaim.hpp"
#include "core/sim/SaveState.hpp"
#include "core/sim/Skirmish.hpp"
#include "core/unit/UnitDef.hpp"

#include "support/EconomyTick.hpp"
#include "support/TestRoster.hpp"

#include <vector>

namespace {

using rm::sim::Army;
using rm::sim::Command;
using rm::sim::CommandIssue;
using rm::sim::CommandKind;
using rm::sim::CommandPhase;
using rm::sim::Construction;
using rm::sim::Economy;
using rm::sim::Match;
using rm::sim::Player;
using rm::sim::SiloAmmo;
using rm::sim::TickReport;
using rm::sim::UnitId;
using rm::unitdef::UnitDef;

[[nodiscard]] rm::HeightField flatField() {
    rm::HeightField field;
    field.squaresX = 100;
    field.squaresZ = 100;
    field.baseHeight = 0.0f;
    field.heightScale = 1.0f;
    field.raw.assign(field.sampleCount(), std::uint16_t{0});
    return field;
}

[[nodiscard]] UnitDef builderDef() {
    UnitDef def;
    def.name = "test_engineer";
    def.categories = {"LAND"};
    def.buildRate = 10.0f;
    def.buildableCategory = {{"PRODUCT"}};
    return def;
}

/// A producer that builds nothing — the mass-fab shape: income with no
//  build capability.
[[nodiscard]] UnitDef fabricatorDef() {
    UnitDef def;
    def.name = "test_fab";
    def.categories = {"STRUCTURE"};
    def.producesMassPerSecond = 1.0f;
    def.upkeepEnergyPerSecond = 150.0f;
    return def;
}

[[nodiscard]] UnitDef productDef() {
    UnitDef def;
    def.name = "test_product";
    def.categories = {"PRODUCT"};
    // A mobile product is assembled on the factory's own pad — no build-site placement.
    def.speedElmosPerSecond = 1.0f;
    def.buildCostMass = rm::sim::magFromFloat(100.0f);
    def.buildCostEnergy = rm::sim::magFromFloat(1000.0f);
    def.buildTime = rm::sim::magFromFloat(100.0f);
    return def;
}

[[nodiscard]] CommandIssue toggleIssue(const UnitId unit, const std::uint32_t serial) {
    return CommandIssue{.source = 0,
                        .id = rm::commandId(0, serial),
                        .player = 0,
                        .kind = CommandKind::ToggleProduction,
                        .units = {unit}};
}

} // namespace

TEST_CASE("the production toggle flips a producer and refuses a pure combat unit",
          "[production][pause]") {
    const rm::HeightField field = flatField();
    const rm::sim::Terrain terrain{field};
    const rm::sim::PassabilityGrid grid =
        rm::sim::buildPassability(field, 0.0f, 60.0f, 0.0f);

    rm::test::Roster roster;
    const rm::UnitTypeIndex engineer = roster.addType(builderDef());
    UnitDef tank;
    tank.name = "test_tank";
    tank.categories = {"LAND"};
    const rm::UnitTypeIndex combat = roster.addType(tank);

    const UnitId builder = roster.add(engineer, 40.0f, 40.0f, 0, 500.0f);
    const UnitId fighter = roster.add(combat, 60.0f, 60.0f, 0, 500.0f);

    const std::vector<Player> players{Player{.index = 0, .army = 0}};
    const std::vector<Army> armies = rm::sim::freeForAll(1);

    const auto result = rm::sim::applyCommand(
        toggleIssue(builder, 1), roster.store, roster.catalog, players, armies, terrain,
        [&grid](UnitId) { return &grid; }, roster.rate);
    CHECK(result.accepted.size() == 1);
    CHECK(roster.store.productionPaused(builder));

    // A unit that neither builds nor produces has no production to pause.
    const auto refused = rm::sim::applyCommand(
        toggleIssue(fighter, 2), roster.store, roster.catalog, players, armies, terrain,
        [&grid](UnitId) { return &grid; }, roster.rate);
    CHECK(refused.accepted.empty());
    CHECK_FALSE(roster.store.productionPaused(fighter));

    // A second toggle on the producer releases it.
    const auto resume = rm::sim::applyCommand(
        toggleIssue(builder, 3), roster.store, roster.catalog, players, armies, terrain,
        [&grid](UnitId) { return &grid; }, roster.rate);
    CHECK(resume.accepted.size() == 1);
    CHECK_FALSE(roster.store.productionPaused(builder));
}

TEST_CASE("a paused factory holds its construction, pays nothing, and resumes",
          "[production][pause]") {
    const rm::HeightField field = flatField();
    const rm::sim::Terrain terrain{field};
    const rm::sim::PassabilityGrid grid =
        rm::sim::buildPassability(field, 0.0f, 60.0f, 0.0f);

    rm::test::Roster roster;
    UnitDef factoryDef;
    factoryDef.name = "test_factory";
    factoryDef.categories = {"FACTORY"};
    factoryDef.buildRate = 10.0f;
    factoryDef.buildableCategory = {{"PRODUCT"}};
    const rm::UnitTypeIndex factoryType = roster.addType(factoryDef);
    const rm::UnitTypeIndex productType = roster.addType(productDef());
    const UnitId factory = roster.add(factoryType, 40.0f, 40.0f, 0, 500.0f);

    const std::vector<Player> players{Player{.index = 0, .army = 0}};
    const std::vector<Army> armies = rm::sim::freeForAll(1);
    std::vector<Construction> building;
    const std::vector<const rm::sim::PassabilityGrid*> grids{&grid, &grid};

    REQUIRE(rm::sim::applyCommand(
        CommandIssue{.source = 0,
                     .id = rm::commandId(0, 1),
                     .player = 0,
                     .kind = CommandKind::Build,
                     .units = {factory},
                     .buildType = productType},
        roster.store, roster.catalog, players, armies, terrain,
        [&grid](UnitId) { return &grid; }, roster.rate, &building)
                .accepted.size() == 1);

    Economy economy;
    economy.storage = {rm::sim::Mag::fromInt(100000), rm::sim::Mag::fromInt(100000)};
    economy.stored = {rm::sim::Mag::fromInt(50000), rm::sim::Mag::fromInt(50000)};

    // One beat, driven the way tickSkirmish does: dispatch advances the work on last
    // beat's ratio, then the economy bills and writes the next one.
    const auto beat = [&] {
        (void)rm::sim::advanceOrders(roster.store, roster.catalog, terrain, grids,
                                     roster.rate, &building);
        rm::sim::tickEconomy(economy, building);
    };

    // Beat one starts the order and funds it; beat two spends the grant as progress.
    beat();
    REQUIRE(building.size() == 1);
    beat();
    const rm::sim::Mag progressed = building.front().buildTimeRemaining;
    REQUIRE(progressed < building.front().totalBuildTime);

    // Pause it: the queue stays, the work holds still, and nothing is charged.
    REQUIRE(rm::sim::applyCommand(toggleIssue(factory, 2), roster.store, roster.catalog,
                                  players, armies, terrain,
                                  [&grid](UnitId) { return &grid; }, roster.rate)
                .accepted.size() == 1);
    const rm::sim::Resources before = economy.stored;
    beat();
    REQUIRE(building.size() == 1);
    CHECK(building.front().buildTimeRemaining == progressed);
    CHECK_FALSE(building.front().workedThisTick);
    CHECK(economy.stored.mass == before.mass);
    CHECK(economy.stored.energy == before.energy);
    // A paused build carries no stale funding ratio into its resume.
    CHECK(building.front().fundedLastTick == rm::sim::Fx{});

    // Unpause: the resume beat has no ratio to spend — the pause zeroed it — so it earns
    // one, and progress returns on the beat after.
    REQUIRE(rm::sim::applyCommand(toggleIssue(factory, 3), roster.store, roster.catalog,
                                  players, armies, terrain,
                                  [&grid](UnitId) { return &grid; }, roster.rate)
                .accepted.size() == 1);
    beat();
    CHECK(building.front().buildTimeRemaining == progressed);
    beat();
    CHECK(building.front().buildTimeRemaining < progressed);
}

TEST_CASE("a paused silo neither accumulates nor pays for ammunition",
          "[production][pause]") {
    rm::test::Roster roster;
    const rm::UnitTypeIndex siloType = roster.addType(builderDef());
    const UnitId silo = roster.add(siloType, 40.0f, 40.0f, 0, 500.0f);

    std::vector<SiloAmmo> ammo{
        rm::sim::makeSiloAmmo(silo, 0, false, 1,
                              {.mass = rm::sim::Mag::fromInt(120),
                               .energy = rm::sim::Mag::fromInt(1200)},
                              rm::sim::Mag::fromInt(10), rm::sim::Mag::fromInt(1))};
    REQUIRE(ammo.front().totalTicks > 0);

    Economy economy;
    economy.storage = {rm::sim::Mag::fromInt(100000), rm::sim::Mag::fromInt(100000)};
    economy.stored = {rm::sim::Mag::fromInt(50000), rm::sim::Mag::fromInt(50000)};

    // One running beat, then the pause, then two beats that must do nothing.
    rm::sim::tickEconomy(economy, {}, {}, ammo);
    const rm::TickCount before = ammo.front().elapsedTicks;
    ammo.front().paused = true;
    const rm::sim::Resources banked = economy.stored;
    rm::sim::tickEconomy(economy, {}, {}, ammo);
    rm::sim::tickEconomy(economy, {}, {}, ammo);
    CHECK(ammo.front().elapsedTicks == before);
    CHECK(ammo.front().stored == 0);
    CHECK(economy.stored.mass == banked.mass);
    CHECK(economy.stored.energy == banked.energy);
}

TEST_CASE("a paused producer contributes no income and no upkeep",
          "[production][pause]") {
    const rm::HeightField field = flatField();
    rm::test::Roster roster;
    const rm::UnitTypeIndex fab = roster.addType(fabricatorDef());
    const UnitId unit = roster.add(fab, 40.0f, 40.0f, 0, 500.0f);

    std::vector<Army> armies = rm::sim::freeForAll(1);
    std::vector<Economy> economies(1);
    const std::vector<int> commandersEver(1, 0);
    Match match{.armies = armies, .economies = economies, .commandersEver = commandersEver};

    rm::sim::tickSkirmish(roster.store, roster.catalog, match, rm::sim::Terrain{field});
    const rm::sim::Resources running = match.economies[0].incomePerTick;
    REQUIRE(running.mass > rm::sim::Mag{});

    REQUIRE(roster.store.setProductionPaused(unit, true));
    rm::sim::tickSkirmish(roster.store, roster.catalog, match, rm::sim::Terrain{field});
    CHECK(match.economies[0].incomePerTick.mass == rm::sim::Mag{});
    CHECK(match.economies[0].upkeepPerTick.energy == rm::sim::Mag{});
}

TEST_CASE("a paused engineer submits no repair demand", "[production][pause]") {
    const rm::HeightField field = flatField();
    const rm::sim::Terrain terrain{field};
    const rm::sim::PassabilityGrid grid =
        rm::sim::buildPassability(field, 0.0f, 60.0f, 0.0f);

    rm::test::Roster roster;
    const rm::UnitTypeIndex engineer = roster.addType(builderDef());
    UnitDef ally;
    ally.name = "test_ally";
    ally.categories = {"STRUCTURE"};
    ally.buildTime = rm::sim::magFromFloat(100.0f);
    const rm::UnitTypeIndex allyType = roster.addType(ally);

    const UnitId builder = roster.add(engineer, 40.0f, 40.0f, 0, 500.0f);
    const UnitId hurt = roster.add(allyType, 44.0f, 40.0f, 0, 500.0f);
    roster.store.health()[hurt.index].current = rm::sim::Mag::fromInt(100);

    const std::vector<Player> players{Player{.index = 0, .army = 0}};
    const std::vector<Army> armies = rm::sim::freeForAll(1);
    REQUIRE(rm::sim::applyCommand(
                CommandIssue{.source = 0,
                             .id = rm::commandId(0, 1),
                             .player = 0,
                             .kind = CommandKind::Repair,
                             .units = {builder},
                             .target = hurt},
                roster.store, roster.catalog, players, armies, terrain,
                [&grid](UnitId) { return &grid; }, roster.rate)
                .accepted.size() == 1);

    std::vector<rm::sim::RepairWork> repairs;
    rm::sim::collectRepairWork(roster.store, roster.catalog, armies, repairs, {}, {});
    REQUIRE(repairs.size() == 1);

    REQUIRE(roster.store.setProductionPaused(builder, true));
    rm::sim::collectRepairWork(roster.store, roster.catalog, armies, repairs, {}, {});
    CHECK(repairs.empty());
}

TEST_CASE("production pause survives a save-state round trip", "[production][pause]") {
    rm::test::Roster roster;
    const rm::UnitTypeIndex engineer = roster.addType(builderDef());
    const UnitId builder = roster.add(engineer, 40.0f, 40.0f, 0, 500.0f);
    REQUIRE(roster.store.setProductionPaused(builder, true));

    rm::sim::SaveState state{.tick = 42, .units = roster.store.snapshot()};
    const std::vector<std::byte> bytes = rm::sim::SaveState::encode(state);
    const auto restored = rm::sim::SaveState::decode(bytes);
    REQUIRE(restored.has_value());
    REQUIRE(restored->units.productionPaused.size() > builder.index);
    CHECK(restored->units.productionPaused[builder.index]);
}
