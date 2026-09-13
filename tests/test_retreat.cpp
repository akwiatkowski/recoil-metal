// Retreat-at-HP automation (#15785): a per-unit threshold setting — off, or break
// off below 30/65/99% hull — that sends a damaged unit to the nearest friendly
// builder and walks it back to where it broke off once it is whole again.
//
// This is a modern-RTS automation, not a retail mechanism: Supreme Commander has
// no retreat stance, so the semantic here is the Zero-K one the ticket names —
// the unit's own hull decides, the queue is replaced rather than appended, and
// "repaired" means full hull, not merely above the threshold that sent it home.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "core/map/HeightField.hpp"
#include "core/sim/Command.hpp"
#include "core/sim/SaveState.hpp"
#include "core/sim/Skirmish.hpp"
#include "core/sim/StateHash.hpp"
#include "core/unit/UnitDef.hpp"

#include "support/FxMatchers.hpp"
#include "support/TestRoster.hpp"

#include <vector>

namespace {

using rm::sim::Army;
using rm::sim::CommandIssue;
using rm::sim::CommandKind;
using rm::sim::CommandPhase;
using rm::sim::Match;
using rm::sim::Player;
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

[[nodiscard]] UnitDef tankDef() {
    UnitDef def;
    def.name = "test_tank";
    def.categories = {"LAND"};
    def.speedElmosPerSecond = 4.0f;
    def.buildTime = rm::sim::Mag::fromInt(100);  // a Repair order needs a buildable target
    return def;
}

[[nodiscard]] UnitDef mechanicDef() {
    UnitDef def;
    def.name = "test_mechanic";
    def.categories = {"STRUCTURE"};
    def.buildRate = 10.0f;
    def.buildDistanceElmos = 20.0f;  // a parked patient must sit inside the beam's reach
    def.buildableCategory = {{"PRODUCT"}};
    return def;
}

/// A one-army match the retreat automation can tick inside: no projectiles, no
/// construction — just units, their orders, and the pass that watches hulls.
/// `passability` is the per-type grid table `Match` carries; an empty span means
/// queued orders never route, so the walking tests must hand theirs over.
[[nodiscard]] Match loneMatch(std::vector<Army>& armies,
                              std::vector<rm::sim::Economy>& economies,
                              std::vector<int>& commandersEver,
                              std::span<const rm::sim::PassabilityGrid* const> grids = {}) {
    return Match{.armies = armies,
                 .economies = economies,
                 .passability = grids,
                 .commandersEver = commandersEver};
}

[[nodiscard]] CommandIssue retreatIssue(const UnitId unit, const std::uint32_t serial) {
    return CommandIssue{.source = 0,
                        .id = rm::commandId(0, serial),
                        .player = 0,
                        .kind = CommandKind::CycleRetreatThreshold,
                        .units = {unit}};
}

} // namespace

TEST_CASE("the retreat threshold cycles on a mobile unit and refuses a building",
          "[retreat]") {
    const rm::HeightField field = flatField();
    const rm::sim::Terrain terrain{field};
    const rm::sim::PassabilityGrid grid =
        rm::sim::buildPassability(field, 0.0f, 60.0f, 0.0f);

    rm::test::Roster roster;
    const rm::UnitTypeIndex tank = roster.addType(tankDef());
    const rm::UnitTypeIndex mechanic = roster.addType(mechanicDef());
    const UnitId mover = roster.add(tank, 40.0f, 40.0f, 0, 500.0f);
    const UnitId building = roster.add(mechanic, 60.0f, 60.0f, 0, 500.0f);

    const std::vector<Player> players{Player{.index = 0, .army = 0}};
    const std::vector<Army> armies = rm::sim::freeForAll(1);

    CHECK(roster.store.retreatThreshold(mover) == rm::RetreatThreshold::Off);
    std::uint32_t serial = 1;
    for (const rm::RetreatThreshold expected :
         {rm::RetreatThreshold::Low, rm::RetreatThreshold::Medium,
          rm::RetreatThreshold::High, rm::RetreatThreshold::Off}) {
        const auto result = rm::sim::applyCommand(
            retreatIssue(mover, serial++), roster.store, roster.catalog, players, armies,
            terrain, [&grid](UnitId) { return &grid; }, roster.rate);
        REQUIRE(result.accepted.size() == 1);
        CHECK(roster.store.retreatThreshold(mover) == expected);
    }

    // A structure cannot walk home, so the setting refuses it rather than sit armed
    // on a unit that could never act on it.
    const auto refused = rm::sim::applyCommand(
        retreatIssue(building, serial), roster.store, roster.catalog, players, armies,
        terrain, [&grid](UnitId) { return &grid; }, roster.rate);
    CHECK(refused.accepted.empty());
    CHECK(roster.store.retreatThreshold(building) == rm::RetreatThreshold::Off);
}

TEST_CASE("a unit below its threshold breaks off for the nearest friendly builder",
          "[retreat]") {
    const rm::HeightField field = flatField();
    const rm::sim::Terrain terrain{field};
    const rm::sim::PassabilityGrid grid =
        rm::sim::buildPassability(field, 0.0f, 60.0f, 0.0f);

    rm::test::Roster roster;
    const rm::UnitTypeIndex tank = roster.addType(tankDef());
    const rm::UnitTypeIndex mechanic = roster.addType(mechanicDef());
    const UnitId mover = roster.add(tank, 50.0f, 50.0f, 0, 500.0f);
    (void)roster.add(mechanic, 10.0f, 50.0f, 0, 500.0f);
    // A second, nearer mechanic proves "nearest", not "any".
    (void)roster.add(mechanic, 40.0f, 50.0f, 0, 500.0f);

    const std::vector<Player> players{Player{.index = 0, .army = 0}};
    std::vector<Army> armies = rm::sim::freeForAll(1);
    std::vector<rm::sim::Economy> economies(1);
    std::vector<int> commandersEver(1, 0);

    const auto set = rm::sim::applyCommand(
        retreatIssue(mover, 1), roster.store, roster.catalog, players, armies, terrain,
        [&grid](UnitId) { return &grid; }, roster.rate);
    REQUIRE(set.accepted.size() == 1);  // Low: break off below 30%

    // Sent somewhere else first, then hurt past the line: 100 of 500 is 20%.
    rm::sim::orderTo(roster.motion(mover), terrain, rm::test::fx(90.0f),
                     rm::test::fx(50.0f));
    roster.store.health()[mover.index].current = rm::sim::Mag::fromInt(100);

    Match match = loneMatch(armies, economies, commandersEver);
    (void)rm::sim::tickSkirmish(roster.store, roster.catalog, match, terrain);

    // The queue is the retreat move now — the earlier destination is gone — and it
    // points at the nearer mechanic.
    const rm::sim::QueuedCommand* head = roster.store.orders()[mover.index].current();
    REQUIRE(head != nullptr);
    CHECK(head->kind() == CommandKind::Move);
    CHECK(rm::sim::fxToFloat(head->targetX()) == Catch::Approx(40.0f).margin(0.5f));
    CHECK(rm::sim::fxToFloat(head->targetZ()) == Catch::Approx(50.0f).margin(0.5f));
}

TEST_CASE("a parked retreater hands the idle mechanic a repair order", "[retreat]") {
    const rm::HeightField field = flatField();
    const rm::sim::Terrain terrain{field};
    const rm::sim::PassabilityGrid grid =
        rm::sim::buildPassability(field, 0.0f, 60.0f, 0.0f);

    rm::test::Roster roster;
    const rm::UnitTypeIndex tank = roster.addType(tankDef());
    const rm::UnitTypeIndex mechanic = roster.addType(mechanicDef());
    const UnitId mover = roster.add(tank, 50.0f, 50.0f, 0, 500.0f);
    const UnitId fixer = roster.add(mechanic, 40.0f, 50.0f, 0, 500.0f);

    const std::vector<Player> players{Player{.index = 0, .army = 0}};
    std::vector<Army> armies = rm::sim::freeForAll(1);
    std::vector<rm::sim::Economy> economies(1);
    std::vector<int> commandersEver(1, 0);

    REQUIRE(rm::sim::applyCommand(
                retreatIssue(mover, 1), roster.store, roster.catalog, players, armies,
                terrain, [&grid](UnitId) { return &grid; }, roster.rate)
                .accepted.size() == 1);
    roster.store.health()[mover.index].current = rm::sim::Mag::fromInt(100);

    const std::array<const rm::sim::PassabilityGrid*, 2> grids{&grid, &grid};
    Match match = loneMatch(armies, economies, commandersEver, grids);
    // Two elmos to the parking ring at four a second: a dozen ticks covers the walk,
    // the request, and the dispatch beat — while staying far short of the forty a
    // 400-point heal at this build rate would take (the queue empties when it ends).
    for (int tick = 0; tick < 12; ++tick) {
        (void)rm::sim::tickSkirmish(roster.store, roster.catalog, match, terrain);
    }

    const rm::sim::QueuedCommand* repair = roster.store.orders()[fixer.index].current();
    REQUIRE(repair != nullptr);
    CHECK(repair->kind() == CommandKind::Repair);
    CHECK(repair->payload().target == mover);
}

TEST_CASE("a retreater walks back to where it broke off once its hull is whole",
          "[retreat]") {
    const rm::HeightField field = flatField();
    const rm::sim::Terrain terrain{field};
    const rm::sim::PassabilityGrid grid =
        rm::sim::buildPassability(field, 0.0f, 60.0f, 0.0f);

    rm::test::Roster roster;
    const rm::UnitTypeIndex tank = roster.addType(tankDef());
    const rm::UnitTypeIndex mechanic = roster.addType(mechanicDef());
    const UnitId mover = roster.add(tank, 50.0f, 50.0f, 0, 500.0f);
    (void)roster.add(mechanic, 40.0f, 50.0f, 0, 500.0f);

    const std::vector<Player> players{Player{.index = 0, .army = 0}};
    std::vector<Army> armies = rm::sim::freeForAll(1);
    std::vector<rm::sim::Economy> economies(1);
    std::vector<int> commandersEver(1, 0);

    const auto set = rm::sim::applyCommand(
        retreatIssue(mover, 1), roster.store, roster.catalog, players, armies, terrain,
        [&grid](UnitId) { return &grid; }, roster.rate);
    REQUIRE(set.accepted.size() == 1);
    roster.store.health()[mover.index].current = rm::sim::Mag::fromInt(100);

    Match match = loneMatch(armies, economies, commandersEver);
    (void)rm::sim::tickSkirmish(roster.store, roster.catalog, match, terrain);
    REQUIRE(roster.store.orders()[mover.index].current() != nullptr);

    // Repaired in full — the mechanic's hands are the test's — so the next tick owes
    // the front a unit walking back to (50, 50).
    roster.store.health()[mover.index].current = rm::sim::Mag::fromInt(500);
    (void)rm::sim::tickSkirmish(roster.store, roster.catalog, match, terrain);

    const rm::sim::QueuedCommand* head = roster.store.orders()[mover.index].current();
    REQUIRE(head != nullptr);
    CHECK(head->kind() == CommandKind::Move);
    CHECK(rm::sim::fxToFloat(head->targetX()) == Catch::Approx(50.0f).margin(0.5f));
    CHECK(rm::sim::fxToFloat(head->targetZ()) == Catch::Approx(50.0f).margin(0.5f));
}

TEST_CASE("a unit at nine-tenths hull stays put until the threshold says otherwise",
          "[retreat]") {
    const rm::HeightField field = flatField();
    const rm::sim::Terrain terrain{field};
    const rm::sim::PassabilityGrid grid =
        rm::sim::buildPassability(field, 0.0f, 60.0f, 0.0f);

    rm::test::Roster roster;
    const rm::UnitTypeIndex tank = roster.addType(tankDef());
    const rm::UnitTypeIndex mechanic = roster.addType(mechanicDef());
    const UnitId mover = roster.add(tank, 50.0f, 50.0f, 0, 500.0f);
    (void)roster.add(mechanic, 40.0f, 50.0f, 0, 500.0f);

    const std::vector<Player> players{Player{.index = 0, .army = 0}};
    std::vector<Army> armies = rm::sim::freeForAll(1);
    std::vector<rm::sim::Economy> economies(1);
    std::vector<int> commandersEver(1, 0);

    REQUIRE(rm::sim::applyCommand(
                retreatIssue(mover, 1), roster.store, roster.catalog, players, armies,
                terrain, [&grid](UnitId) { return &grid; }, roster.rate)
                .accepted.size() == 1);  // Low — 90% is nowhere near 30%
    roster.store.health()[mover.index].current = rm::sim::Mag::fromInt(450);

    Match match = loneMatch(armies, economies, commandersEver);
    (void)rm::sim::tickSkirmish(roster.store, roster.catalog, match, terrain);
    CHECK(roster.store.orders()[mover.index].current() == nullptr);

    // High — the same scratch now sends it home, because any damage will do. Low is
    // already set, so two more cycles land on High (three would wrap back to Off).
    for (int i = 0; i < 2; ++i) {
        REQUIRE(rm::sim::applyCommand(
                    retreatIssue(mover, static_cast<std::uint32_t>(2 + i)), roster.store,
                roster.catalog, players,
                    armies, terrain, [&grid](UnitId) { return &grid; }, roster.rate)
                    .accepted.size() == 1);
    }
    (void)rm::sim::tickSkirmish(roster.store, roster.catalog, match, terrain);
    const rm::sim::QueuedCommand* head = roster.store.orders()[mover.index].current();
    REQUIRE(head != nullptr);
    CHECK(head->kind() == CommandKind::Move);
}

TEST_CASE("retreat state survives a save-state round trip and feeds the hash",
          "[retreat][savestate]") {
    const rm::HeightField field = flatField();
    const rm::sim::Terrain terrain{field};
    const rm::sim::PassabilityGrid grid =
        rm::sim::buildPassability(field, 0.0f, 60.0f, 0.0f);

    rm::test::Roster roster;
    const rm::UnitTypeIndex tank = roster.addType(tankDef());
    const rm::UnitTypeIndex mechanic = roster.addType(mechanicDef());
    const UnitId mover = roster.add(tank, 50.0f, 50.0f, 0, 500.0f);
    (void)roster.add(mechanic, 40.0f, 50.0f, 0, 500.0f);

    const std::vector<Player> players{Player{.index = 0, .army = 0}};
    std::vector<Army> armies = rm::sim::freeForAll(1);
    std::vector<rm::sim::Economy> economies(1);
    std::vector<int> commandersEver(1, 0);

    REQUIRE(rm::sim::applyCommand(
                retreatIssue(mover, 1), roster.store, roster.catalog, players, armies,
                terrain, [&grid](UnitId) { return &grid; }, roster.rate)
                .accepted.size() == 1);
    roster.store.health()[mover.index].current = rm::sim::Mag::fromInt(100);

    Match match = loneMatch(armies, economies, commandersEver);
    (void)rm::sim::tickSkirmish(roster.store, roster.catalog, match, terrain);
    REQUIRE(roster.store.orders()[mover.index].current() != nullptr);

    const rm::StateHash homeHash = rm::sim::hashMatch(roster.store, match);

    // The setting AND the in-flight bookkeeping both cross the save: a reloaded
    // match must still remember where the retreater ran from and to.
    rm::sim::SaveState state{.tick = 42, .units = roster.store.snapshot()};
    const std::vector<std::byte> bytes = rm::sim::SaveState::encode(state);
    const auto restored = rm::sim::SaveState::decode(bytes);
    REQUIRE(restored.has_value());
    REQUIRE(restored->units.retreatThreshold.size() > mover.index);
    CHECK(restored->units.retreatThreshold[mover.index] == rm::RetreatThreshold::Low);
    REQUIRE(restored->units.retreats.size() > mover.index);
    CHECK(restored->units.retreats[mover.index].active);

    // And a different setting must read differently — the hash is the only thing a
    // replay has to catch two runs diverging on who flinched first.
    REQUIRE(rm::sim::applyCommand(
                retreatIssue(mover, 2), roster.store, roster.catalog, players, armies,
                terrain, [&grid](UnitId) { return &grid; }, roster.rate)
                .accepted.size() == 1);
    CHECK(rm::sim::hashMatch(roster.store, match) != homeHash);
}
