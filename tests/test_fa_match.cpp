// Player-perspective coverage for the FA-MATCH claims (WP-10/11; see
// docs/fa-exe-analysis-plan.md and build/re-fa/coverage/FA-MATCH.md). Each
// TEST_CASE cites its claim IDs.
// C-210 is the whole skirmish victory rule — 101 lines of retail Lua. The
// picks here are the parts a player can feel: defeat is a three-second poll
// on a unit COUNT (a replaced commander is never missed), the fifteen-second
// winner confirmation restarts when the verdict changes (a pending win that
// becomes a draw waits its own fifteen seconds), and Annihilation counts
// everything but walls rather than commanders.
//
// C-227 is `CArmyStats`: the serialized per-army stat store with one-shot
// threshold triggers — the thing scenario objectives, economy warnings and
// the score screen all read. C-345 is the self-destruct command: a
// five-second countdown, then the ordinary death path.
#include <catch2/catch_test_macros.hpp>

#include "core/map/HeightField.hpp"
#include "core/sim/Skirmish.hpp"
#include "core/sim/ArmyStats.hpp"
#include "core/sim/SaveState.hpp"

#include "support/FxMatchers.hpp"
#include "support/TestRoster.hpp"

#include <vector>

using rm::sim::Army;
using rm::sim::Match;
using rm::sim::TickReport;
using rm::sim::UnitId;
using rm::unitdef::UnitDef;

namespace {

[[nodiscard]] rm::HeightField flatField() {
    rm::HeightField field;
    field.squaresX = 100;
    field.squaresZ = 100;
    field.baseHeight = 0.0f;
    field.heightScale = 1.0f;
    field.raw.assign(field.sampleCount(), std::uint16_t{0});
    return field;
}

/// A real commander id, spelled the way the blueprints spell it:
/// `isCommanderId` is an exact match against the four.
[[nodiscard]] UnitDef commanderDef() {
    UnitDef def;
    def.name = "UEL0001";
    return def;
}

[[nodiscard]] UnitDef tankDef() {
    UnitDef def;
    def.name = "test_tank";
    return def;
}

} // namespace

TEST_CASE("C-210: a commander replaced inside the poll window is never missed",
          "[fa-match]") {
    // Defeat is a three-second poll on the COMMANDER COUNT, not a death
    // event: "commander death" is really "commander count reached zero". A
    // player who loses the ACU and fields a replacement before the next poll
    // is never defeated — the count never reads zero when it is sampled.
    const rm::HeightField field = flatField();
    const rm::sim::Terrain terrain{field};

    rm::test::Roster roster;
    const rm::UnitTypeIndex commander = roster.addType(commanderDef());
    (void)roster.addType(tankDef());
    const UnitId first = roster.add(commander, 0.0f, 0.0f, 0, 12000.0f);
    const UnitId theirs = roster.add(commander, 400.0f, 0.0f, 1, 12000.0f);
    (void)theirs;

    std::vector<Army> armies = rm::sim::freeForAll(2);
    std::vector<rm::sim::Economy> economies(2);
    std::vector<rm::sim::Projectile> projectiles;
    const std::vector<int> commandersEver{1, 1};
    Match match{.armies = armies,
                .economies = economies,
                .projectiles = &projectiles,
                .commandersEver = commandersEver};
    const rm::sim::TickRate rate{};
    const rm::TickCount pollTicks = rate.ticks(rm::sim::seconds(3.0f));

    // The commander dies mid-window; a replacement stands before the poll.
    roster.health(first).current = rm::sim::Mag{};
    (void)rm::sim::tickSkirmish(roster.store, roster.catalog, match, terrain, rate);
    const UnitId second = roster.add(commander, 20.0f, 0.0f, 0, 12000.0f);
    (void)second;

    for (rm::TickCount tick = 0; tick < pollTicks * 2; ++tick) {
        (void)rm::sim::tickSkirmish(roster.store, roster.catalog, match, terrain, rate);
    }

    // Two polls have run; the count never read zero for army 0.
    CHECK_FALSE(armies[0].defeated);
    CHECK_FALSE(armies[1].defeated);
    CHECK_FALSE(match.over);
}

TEST_CASE("C-210: a pending win that becomes a draw waits its own fifteen seconds",
          "[fa-match]") {
    // The winner must remain stable for fifteen seconds — and the verdict
    // CHANGING restarts the confirmation. Here alliance 0 becomes the sole
    // survivor, then its armies fall too: the pending win is replaced by a
    // pending draw, which needs its own fifteen seconds rather than
    // inheriting the win's elapsed time.
    const rm::HeightField field = flatField();
    const rm::sim::Terrain terrain{field};

    rm::test::Roster roster;
    const rm::UnitTypeIndex commander = roster.addType(commanderDef());
    const UnitId oursA = roster.add(commander, 0.0f, 0.0f, 0, 12000.0f);
    const UnitId oursB = roster.add(commander, 50.0f, 0.0f, 1, 12000.0f);
    const UnitId enemyA = roster.add(commander, 400.0f, 0.0f, 2, 12000.0f);
    const UnitId enemyB = roster.add(commander, 500.0f, 0.0f, 3, 12000.0f);

    std::vector<Army> armies = rm::sim::freeForAll(4);
    armies[0].alliance = 0;
    armies[1].alliance = 0;
    armies[2].alliance = 1;
    armies[3].alliance = 1;
    std::vector<rm::sim::Economy> economies(4);
    std::vector<rm::sim::Projectile> projectiles;
    const std::vector<int> commandersEver(4, 1);
    Match match{.armies = armies,
                .economies = economies,
                .projectiles = &projectiles,
                .commandersEver = commandersEver};
    const rm::sim::TickRate rate{};
    const rm::TickCount pollTicks = rate.ticks(rm::sim::seconds(3.0f));
    const rm::TickCount confirmTicks = rate.ticks(rm::sim::seconds(15.0f));

    // Alliance 1 falls at the first poll: alliance 0 becomes the pending winner.
    roster.health(enemyA).current = rm::sim::Mag{};
    roster.health(enemyB).current = rm::sim::Mag{};
    for (rm::TickCount tick = 0; tick < pollTicks; ++tick) {
        (void)rm::sim::tickSkirmish(roster.store, roster.catalog, match, terrain, rate);
    }
    REQUIRE(armies[2].defeated);
    REQUIRE(armies[3].defeated);
    REQUIRE(match.pendingWinner.has_value());
    REQUIRE(*match.pendingWinner == 0);

    // Let the win almost confirm, then kill alliance 0 inside the next poll
    // window: the verdict flips from "alliance 0 wins" to "draw".
    for (rm::TickCount tick = 0; tick < confirmTicks - pollTicks - 5; ++tick) {
        (void)rm::sim::tickSkirmish(roster.store, roster.catalog, match, terrain, rate);
    }
    REQUIRE_FALSE(match.over);
    roster.health(oursA).current = rm::sim::Mag{};
    roster.health(oursB).current = rm::sim::Mag{};
    for (rm::TickCount tick = 0; tick < pollTicks; ++tick) {
        (void)rm::sim::tickSkirmish(roster.store, roster.catalog, match, terrain, rate);
    }
    REQUIRE(armies[0].defeated);
    REQUIRE(armies[1].defeated);
    // The draw is now the pending verdict — and it must NOT inherit the
    // fourteen seconds the win had already banked: the stability counter
    // restarted when the verdict flipped.
    REQUIRE_FALSE(match.over);
    CHECK(match.pendingWinner == std::nullopt);
    CHECK(match.winnerStableTicks < confirmTicks / 2);

    TickReport report{};
    // However many stable ticks the draw has already banked, the match ends
    // exactly when the counter reaches fifteen seconds — not earlier.
    const rm::TickCount banked = match.winnerStableTicks;
    for (rm::TickCount tick = 0; tick < confirmTicks - banked - 1; ++tick) {
        report = rm::sim::tickSkirmish(roster.store, roster.catalog, match, terrain, rate);
    }
    CHECK_FALSE(match.over);

    report = rm::sim::tickSkirmish(roster.store, roster.catalog, match, terrain, rate);
    CHECK(match.over);
    CHECK(report.matchEnded);
    CHECK_FALSE(report.winner.has_value());  // a draw, not a win
}

TEST_CASE("C-210: annihilation counts everything but walls, not commanders",
          "[fa-match]") {
    // The four modes are one category test. Under Annihilation an army that
    // loses its commander but still fields a tank is NOT defeated — the mode
    // counts ALLUNITS − WALL, so the commander is just another unit. Under
    // Assassination the same loss is fatal. Same battlefield, two verdicts.
    const rm::HeightField field = flatField();
    const rm::sim::Terrain terrain{field};
    const rm::sim::TickRate rate{};
    const rm::TickCount pollTicks = rate.ticks(rm::sim::seconds(3.0f));

    const auto run = [&](rm::sim::VictoryMode mode) {
        rm::test::Roster roster;
        const rm::UnitTypeIndex commander = roster.addType(commanderDef());
        const rm::UnitTypeIndex tank = roster.addType(tankDef());
        const UnitId dead = roster.add(commander, 0.0f, 0.0f, 0, 12000.0f);
        (void)roster.add(tank, 20.0f, 0.0f, 0, 500.0f);   // army 0's survivor
        (void)roster.add(commander, 400.0f, 0.0f, 1, 12000.0f);

        std::vector<Army> armies = rm::sim::freeForAll(2);
        std::vector<rm::sim::Economy> economies(2);
        std::vector<rm::sim::Projectile> projectiles;
        const std::vector<int> commandersEver{1, 1};
        Match match{.armies = armies,
                    .economies = economies,
                    .projectiles = &projectiles,
                    .commandersEver = commandersEver,
                    .victoryMode = mode};

        roster.health(dead).current = rm::sim::Mag{};
        for (rm::TickCount tick = 0; tick < pollTicks; ++tick) {
            (void)rm::sim::tickSkirmish(roster.store, roster.catalog, match, terrain, rate);
        }
        return armies[0].defeated;
    };

    CHECK(run(rm::sim::VictoryMode::Assassination));    // the commander was the army
    CHECK_FALSE(run(rm::sim::VictoryMode::Annihilation));  // the tank still counts
}

TEST_CASE("C-227: a stat trigger fires once when its threshold is crossed",
          "[fa-match]") {
    // `CArmyStats` is a serialized per-army store plus one-shot triggers: the
    // economy warning that says "your mass is running out" is an
    // `Economy_Ratio_Mass < 0.1` trigger, and it fires ONCE — the qualifying
    // trigger is removed, not re-armed. The evaluator is gated on the
    // per-army beat AFTER TICK 10, retail's own delay.
    const rm::HeightField field = flatField();
    const rm::sim::Terrain terrain{field};
    const rm::sim::TickRate rate{};

    rm::test::Roster roster;
    const rm::UnitTypeIndex tank = roster.addType(tankDef());
    (void)roster.add(tank, 0.0f, 0.0f, 0, 500.0f);

    std::vector<Army> armies = rm::sim::freeForAll(2);
    std::vector<rm::sim::Economy> economies(2);
    // Storage exists but nothing is banked: the ratio reads zero, under the
    // warning threshold.
    economies[0].storage = {.mass = rm::sim::magFromFloat(1000.0f),
                            .energy = rm::sim::magFromFloat(1000.0f)};
    std::vector<rm::sim::ArmyStats> stats(2);
    rm::sim::setArmyStatsTrigger(stats[0], rm::sim::ArmyStatTrigger{
        .name = "EconLowMassStore",
        .conditions = {{.stat = "Economy_Ratio_Mass",
                        .op = rm::sim::StatCompare::LessThan,
                        .value = rm::sim::magFromFloat(0.1f)}}});

    std::vector<rm::sim::Projectile> projectiles;
    const std::vector<int> commandersEver{0, 0};
    Match match{.armies = armies,
                .economies = economies,
                .projectiles = &projectiles,
                .commandersEver = commandersEver,
                .baseStorage = {.mass = rm::sim::magFromFloat(1000.0f),
                                .energy = rm::sim::magFromFloat(1000.0f)},
                .armyStats = &stats};

    // Ticks 0..10 feed the stats but the evaluator has not run yet.
    for (rm::TickCount tick = 0; tick <= 10; ++tick) {
        const TickReport report =
            rm::sim::tickSkirmish(roster.store, roster.catalog, match, terrain, rate,
                                  tick);
        CHECK(report.armyStatsFired.empty());
    }
    CHECK(stats[0].triggers.size() == 1);

    const TickReport fired =
        rm::sim::tickSkirmish(roster.store, roster.catalog, match, terrain, rate, 11);
    REQUIRE(fired.armyStatsFired.size() == 1);
    CHECK(fired.armyStatsFired[0].army == 0);
    CHECK(fired.armyStatsFired[0].name == "EconLowMassStore");
    CHECK(stats[0].triggers.empty());  // one-shot: the qualifying trigger is removed

    // A second crossing does nothing — there is no trigger left to fire.
    const TickReport again =
        rm::sim::tickSkirmish(roster.store, roster.catalog, match, terrain, rate, 12);
    CHECK(again.armyStatsFired.empty());
}

TEST_CASE("C-227: every condition must pass and the stats survive a save",
          "[fa-match]") {
    // ALL of a trigger's conditions must hold — a two-condition trigger with
    // one failing stays armed. And the store is serialized: a save/load round
    // trip keeps both the counters and the pending triggers, which is what
    // makes a mid-mission objective survive a save.
    const rm::HeightField field = flatField();
    const rm::sim::Terrain terrain{field};
    const rm::sim::TickRate rate{};
    rm::test::Roster roster;
    const rm::UnitTypeIndex tank = roster.addType(tankDef());
    (void)roster.add(tank, 0.0f, 0.0f, 0, 500.0f);
    std::vector<Army> armies = rm::sim::freeForAll(2);
    std::vector<rm::sim::Economy> economies(2);
    economies[0].storage = {.mass = rm::sim::magFromFloat(1000.0f),
                            .energy = rm::sim::magFromFloat(1000.0f)};
    std::vector<rm::sim::ArmyStats> allStats(2);
    // The kill condition holds (three kills banked); the ratio condition fails
    // (nothing stored) — the trigger stays armed.
    rm::sim::setArmyStat(allStats[0], "Units_Killed", rm::sim::Mag::fromInt(3));
    rm::sim::setArmyStatsTrigger(allStats[0], rm::sim::ArmyStatTrigger{
        .name = "objective",
        .conditions = {
            {.stat = "Units_Killed",
             .op = rm::sim::StatCompare::GreaterThanOrEqual,
             .value = rm::sim::Mag::fromInt(1)},
            {.stat = "Economy_Ratio_Mass",
             .op = rm::sim::StatCompare::GreaterThanOrEqual,
             .value = rm::sim::magFromFloat(0.5f)}}});
    std::vector<rm::sim::Projectile> projectiles;
    const std::vector<int> commandersEver{0, 0};
    Match match{.armies = armies,
                .economies = economies,
                .projectiles = &projectiles,
                .commandersEver = commandersEver,
                .baseStorage = {.mass = rm::sim::magFromFloat(1000.0f),
                                .energy = rm::sim::magFromFloat(1000.0f)},
                .armyStats = &allStats};

    const TickReport report =
        rm::sim::tickSkirmish(roster.store, roster.catalog, match, terrain, rate, 11);
    CHECK(report.armyStatsFired.empty());
    CHECK(allStats[0].triggers.size() == 1);

    // Round-trip: encode the match state, decode it, and the stats — counters
    // and the still-armed trigger — come back byte-identical.
    rm::sim::SaveState state;
    state.tick = 11;
    state.units = roster.store.snapshot();
    state.armyStats = allStats;
    const std::vector<std::byte> bytes = rm::sim::SaveState::encode(state);
    const std::optional<rm::sim::SaveState> decoded = rm::sim::SaveState::decode(bytes);
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->armyStats.has_value());
    CHECK(decoded->armyStats->size() == 2);
    REQUIRE(decoded->armyStats->at(0).triggers.size() == 1);
    CHECK(decoded->armyStats->at(0).triggers[0].conditions.size() == 2);
    CHECK(rm::sim::armyStat(decoded->armyStats->at(0), "Units_Killed")
          == rm::sim::Mag::fromInt(3));
}

TEST_CASE("C-345: self-destruct counts five seconds down then kills",
          "[fa-match]") {
    // `confirmunitdestroy.lua` -> `ToggleSelfDestruct` -> `selfdestruct.lua`:
    // a five-second `StartCountdown`, then `unit:Kill()`. The kill goes through
    // the ordinary death path — the unit is gone, reported dead, and a second
    // issue of the command cancels the countdown.
    const rm::HeightField field = flatField();
    const rm::sim::Terrain terrain{field};
    const rm::sim::PassabilityGrid grid = rm::sim::buildPassability(field, 0.0f);
    const rm::sim::TickRate rate{};

    rm::test::Roster roster;
    const rm::UnitTypeIndex tank = roster.addType(tankDef());
    const UnitId unit = roster.add(tank, 0.0f, 0.0f, 0, 500.0f);

    std::vector<Army> armies = rm::sim::freeForAll(2);
    std::vector<rm::sim::Economy> economies(2);
    std::vector<rm::sim::Projectile> projectiles;
    std::vector<rm::sim::SelfDestructWork> selfDestructs;
    const std::vector<int> commandersEver{0, 0};
    const std::vector<rm::sim::Player> players = rm::sim::onePlayerPerArmy(2, 0);

    REQUIRE(rm::sim::applyCommand(
        rm::sim::Command{.kind = rm::sim::CommandKind::SelfDestruct, .unit = unit},
        roster.store, roster.catalog, players, armies, terrain, grid, rate,
        nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
        &selfDestructs));
    REQUIRE(selfDestructs.size() == 1);
    CHECK(selfDestructs[0].remainingTicks == rate.ticks(rm::sim::seconds(5.0f)));

    Match match{.armies = armies,
                .economies = economies,
                .projectiles = &projectiles,
                .commandersEver = commandersEver,
                .selfDestructs = &selfDestructs};

    // Four seconds in the unit still stands; the countdown is a warning, not
    // an instant kill.
    for (rm::TickCount tick = 0; tick < rate.ticks(rm::sim::seconds(4.0f)); ++tick) {
        (void)rm::sim::tickSkirmish(roster.store, roster.catalog, match, terrain, rate,
                                  tick);
    }
    CHECK(roster.store.alive(unit));
    CHECK(selfDestructs.size() == 1);

    // The fifth second lands the kill through the ordinary path.
    for (rm::TickCount tick = 0; tick < rate.ticks(rm::sim::seconds(1.0f)); ++tick) {
        (void)rm::sim::tickSkirmish(roster.store, roster.catalog, match, terrain, rate,
                                  tick);
    }
    CHECK_FALSE(roster.store.alive(unit));
    CHECK(selfDestructs.empty());
}

TEST_CASE("C-345: a second self-destruct order cancels the countdown",
          "[fa-match]") {
    // "Toggle" is literal: the same command that starts the countdown stops
    // it, and the unit lives past where the kill would have landed.
    const rm::HeightField field = flatField();
    const rm::sim::Terrain terrain{field};
    const rm::sim::PassabilityGrid grid = rm::sim::buildPassability(field, 0.0f);
    const rm::sim::TickRate rate{};

    rm::test::Roster roster;
    const rm::UnitTypeIndex tank = roster.addType(tankDef());
    const UnitId unit = roster.add(tank, 0.0f, 0.0f, 0, 500.0f);

    std::vector<Army> armies = rm::sim::freeForAll(2);
    std::vector<rm::sim::Economy> economies(2);
    std::vector<rm::sim::Projectile> projectiles;
    std::vector<rm::sim::SelfDestructWork> selfDestructs;
    const std::vector<int> commandersEver{0, 0};
    const std::vector<rm::sim::Player> players = rm::sim::onePlayerPerArmy(2, 0);
    const rm::sim::Command order{.kind = rm::sim::CommandKind::SelfDestruct,
                                 .unit = unit};
    REQUIRE(rm::sim::applyCommand(order, roster.store, roster.catalog, players, armies,
                                  terrain, grid, rate, nullptr, nullptr, nullptr,
                                  nullptr, nullptr, nullptr, nullptr, &selfDestructs));
    REQUIRE(rm::sim::applyCommand(order, roster.store, roster.catalog, players, armies,
                                  terrain, grid, rate, nullptr, nullptr, nullptr,
                                  nullptr, nullptr, nullptr, nullptr, &selfDestructs));
    CHECK(selfDestructs.empty());

    Match match{.armies = armies,
                .economies = economies,
                .projectiles = &projectiles,
                .commandersEver = commandersEver,
                .selfDestructs = &selfDestructs};
    for (rm::TickCount tick = 0; tick < rate.ticks(rm::sim::seconds(6.0f)); ++tick) {
        (void)rm::sim::tickSkirmish(roster.store, roster.catalog, match, terrain, rate,
                                  tick);
    }
    CHECK(roster.store.alive(unit));
}
