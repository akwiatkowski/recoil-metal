// Construction priority tiers (#15779): a producer carries a High/Normal/Low
// tier, and the allocator funds tiers in order when the bank cannot pay
// everyone — High first, then Normal, then Low, each out of what the tier
// above left. All-Normal demand is exactly the pre-tier allocator.
//
// Zero-K's construction priority, adapted to the retail two-bucket allocator:
// the tier decides WHEN a consumer's bucket is served, not how the buckets
// split inside it — a High build that cannot afford its energy still stalls.
#include <catch2/catch_test_macros.hpp>

#include "core/map/HeightField.hpp"
#include "core/sim/Command.hpp"
#include "core/sim/Economy.hpp"
#include "core/sim/Pathfinding.hpp"
#include "core/sim/SaveState.hpp"
#include "core/sim/StateHash.hpp"
#include "core/unit/UnitDef.hpp"

#include "support/TestRoster.hpp"

#include <array>
#include <vector>

namespace {

using rm::BuildPriority;
using rm::sim::Army;
using rm::sim::CommandIssue;
using rm::sim::CommandKind;
using rm::sim::Construction;
using rm::sim::Economy;
using rm::sim::Match;
using rm::sim::Player;
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

[[nodiscard]] CommandIssue cycleIssue(const UnitId unit, const std::uint32_t serial,
                                      const rm::PlayerIndex player = 0) {
    return CommandIssue{.source = 0,
                        .id = rm::commandId(0, serial),
                        .player = player,
                        .kind = CommandKind::CycleBuildPriority,
                        .units = {unit}};
}

/// A work record that asks for exactly 250 mass per tick and no energy — a
/// quarter of its 1000-mass cost each beat. The build time is a power of two so
/// the fixed-point share division lands exactly and the arithmetic is checkable.
[[nodiscard]] Construction workFor(const UnitId builder) {
    return Construction{.armyIndex = 0,
                        .cost = {.mass = rm::sim::Mag::fromInt(1000)},
                        .buildTimeRemaining = rm::sim::Mag::fromInt(4),
                        .totalBuildTime = rm::sim::Mag::fromInt(4),
                        .buildPerTick = rm::sim::Mag::fromInt(1),
                        .builder = builder};
}

} // namespace

TEST_CASE("build priority defaults to Normal and cycles on a producer",
          "[priority]") {
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

    CHECK(roster.store.buildPriority(builder) == BuildPriority::Normal);
    CHECK(roster.store.buildPriority(fighter) == BuildPriority::Normal);

    const std::vector<Player> players{Player{.index = 0, .army = 0},
                                      Player{.index = 1, .army = 1}};
    const std::vector<Army> armies = rm::sim::freeForAll(2);
    const auto apply = [&](const CommandIssue& issue) {
        return rm::sim::applyCommand(issue, roster.store, roster.catalog, players,
                                     armies, terrain,
                                     [&grid](UnitId) { return &grid; }, roster.rate);
    };

    // A pure combat unit produces nothing — nothing to tier.
    CHECK(apply(cycleIssue(fighter, 1)).accepted.empty());
    CHECK(roster.store.buildPriority(fighter) == BuildPriority::Normal);

    // A foreign player's click changes nothing either.
    CHECK(apply(cycleIssue(builder, 2, 1)).accepted.empty());
    CHECK(roster.store.buildPriority(builder) == BuildPriority::Normal);

    // The producer cycles Normal → High → Low → Normal.
    REQUIRE(apply(cycleIssue(builder, 3)).accepted.size() == 1);
    CHECK(roster.store.buildPriority(builder) == BuildPriority::High);
    REQUIRE(apply(cycleIssue(builder, 4)).accepted.size() == 1);
    CHECK(roster.store.buildPriority(builder) == BuildPriority::Low);
    REQUIRE(apply(cycleIssue(builder, 5)).accepted.size() == 1);
    CHECK(roster.store.buildPriority(builder) == BuildPriority::Normal);
}

TEST_CASE("a stalled bank funds High before Normal before Low", "[priority]") {
    rm::test::Roster roster;
    const rm::UnitTypeIndex engineer = roster.addType(builderDef());
    const UnitId high = roster.add(engineer, 10.0f, 10.0f, 0, 500.0f);
    const UnitId normal = roster.add(engineer, 20.0f, 20.0f, 0, 500.0f);
    const UnitId low = roster.add(engineer, 30.0f, 30.0f, 0, 500.0f);

    // 750 mass wanted, 375 in the bank: High is paid in full, Normal halves,
    // Low starves.
    Economy economy;
    economy.storage = {rm::sim::Mag::fromInt(100000), rm::sim::Mag::fromInt(100000)};
    economy.stored = {rm::sim::Mag::fromInt(375), rm::sim::Mag::fromInt(0)};
    std::vector<Construction> building{workFor(high), workFor(normal), workFor(low)};
    const std::array<BuildPriority, 3> priorities{BuildPriority::High,
                                                  BuildPriority::Normal,
                                                  BuildPriority::Low};
    rm::sim::tickEconomy(economy, building, {}, {}, false, {}, rm::sim::kNoArmy, {},
                         {}, priorities);

    CHECK(building[0].fundedLastTick == rm::sim::kFxOne);
    CHECK(building[1].fundedLastTick == rm::sim::Fx::fromRaw(1 << 13));
    CHECK(building[2].fundedLastTick == rm::sim::Fx{});
    CHECK(economy.stored.mass == rm::sim::Mag{});
}

TEST_CASE("all-Normal demand allocates exactly like the pre-tier allocator",
          "[priority]") {
    rm::test::Roster roster;
    const rm::UnitTypeIndex engineer = roster.addType(builderDef());
    const UnitId a = roster.add(engineer, 10.0f, 10.0f, 0, 500.0f);
    const UnitId b = roster.add(engineer, 20.0f, 20.0f, 0, 500.0f);
    const UnitId c = roster.add(engineer, 30.0f, 30.0f, 0, 500.0f);

    const auto run = [&](std::span<const BuildPriority> priorities) {
        Economy economy;
        economy.storage = {rm::sim::Mag::fromInt(100000), rm::sim::Mag::fromInt(100000)};
        economy.stored = {rm::sim::Mag::fromInt(375), rm::sim::Mag::fromInt(0)};
        economy.upkeepPerTick = {.mass = rm::sim::Mag::fromInt(30)};
        std::vector<Construction> building{workFor(a), workFor(b), workFor(c)};
        rm::sim::tickEconomy(economy, building, {}, {}, false, {},
                             rm::sim::kNoArmy, {}, {}, priorities);
        return std::pair{economy, building};
    };

    const std::array<BuildPriority, 3> allNormal{BuildPriority::Normal,
                                                 BuildPriority::Normal,
                                                 BuildPriority::Normal};
    const auto [baselineEco, baselineWork] = run({});
    const auto [tieredEco, tieredWork] = run(allNormal);

    CHECK(tieredEco.stored.mass == baselineEco.stored.mass);
    CHECK(tieredEco.fundedFraction == baselineEco.fundedFraction);
    CHECK(tieredEco.multiResourceFunded == baselineEco.multiResourceFunded);
    CHECK(tieredEco.singleResourceFunded == baselineEco.singleResourceFunded);
    for (std::size_t i = 0; i < 3; ++i) {
        CHECK(tieredWork[i].fundedLastTick == baselineWork[i].fundedLastTick);
        CHECK(tieredWork[i].allocated.mass == baselineWork[i].allocated.mass);
    }
}

TEST_CASE("build priority survives a save-state round trip and feeds the hash",
          "[priority]") {
    rm::test::Roster roster;
    const rm::UnitTypeIndex engineer = roster.addType(builderDef());
    const UnitId builder = roster.add(engineer, 40.0f, 40.0f, 0, 500.0f);

    std::vector<Army> armies = rm::sim::freeForAll(1);
    std::vector<Economy> economies(1);
    std::vector<int> commandersEver(1, 0);
    Match match{.armies = armies, .economies = economies,
                .commandersEver = commandersEver};
    const rm::StateHash baseline = rm::sim::hashMatch(roster.store, match);
    REQUIRE(roster.store.setBuildPriority(builder, BuildPriority::Low));
    CHECK(rm::sim::hashMatch(roster.store, match) != baseline);

    rm::sim::SaveState state{.tick = 42, .units = roster.store.snapshot()};
    const std::vector<std::byte> bytes = rm::sim::SaveState::encode(state);
    const auto restored = rm::sim::SaveState::decode(bytes);
    REQUIRE(restored.has_value());
    REQUIRE(restored->units.buildPriority.size() > builder.index);
    CHECK(restored->units.buildPriority[builder.index] == BuildPriority::Low);
}
