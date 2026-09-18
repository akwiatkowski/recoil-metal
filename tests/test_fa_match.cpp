// Player-perspective coverage for the FA-MATCH claims (WP-10/11; see
// docs/fa-exe-analysis-plan.md and build/re-fa/coverage/FA-MATCH.md). Each
// TEST_CASE cites its claim IDs.
//
// C-210 is the whole skirmish victory rule — 101 lines of retail Lua. The
// picks here are the parts a player can feel: defeat is a three-second poll
// on a unit COUNT (a replaced commander is never missed), the fifteen-second
// winner confirmation restarts when the verdict changes (a pending win that
// becomes a draw waits its own fifteen seconds), and Annihilation counts
// everything but walls rather than commanders.
#include <catch2/catch_test_macros.hpp>

#include "core/map/HeightField.hpp"
#include "core/sim/Skirmish.hpp"

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
