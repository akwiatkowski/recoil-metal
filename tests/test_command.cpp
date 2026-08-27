// Orders as data, and the single path they take into the sim.
//
// PLAN2.md §7 P2.5's stated test: "a recorded human log and a synthetic script log replay
// through the same path." What makes that checkable is that there IS one path — `applyCommand`
// — so the test builds two logs from two sources and requires them to produce the same match.
//
// Also here, because it only became possible with this file: testing the human order path at
// all. A click used to go through AppKit, so "does an order route around water" could only be
// answered by clicking. A command is a struct.
#include <catch2/catch_test_macros.hpp>

#include "core/sim/Command.hpp"
#include "core/sim/StateHash.hpp"
#include "core/sim/Skirmish.hpp"
#include "core/sim/UnitStore.hpp"

#include "support/FxMatchers.hpp"
#include "support/TestRoster.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <vector>

using rm::sim::Command;
using rm::sim::CommandKind;
using rm::sim::CommandLog;
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

/// A match with two armies, one unit each, and one player driving each.
struct Fixture {
    rm::HeightField field = flatField();
    rm::sim::Terrain terrain{field};
    rm::sim::PassabilityGrid grid = rm::sim::buildPassability(field, 0.0f);
    rm::test::Roster roster;
    std::vector<rm::sim::Army> armies = rm::sim::freeForAll(2);
    std::vector<Player> players = rm::sim::onePlayerPerArmy(2, /*humanArmy=*/0);

    UnitId mine;
    UnitId theirs;

    Fixture() {
        rm::unitdef::UnitDef def;
        def.name = "test_tank";
        const rm::UnitTypeIndex type = roster.addType(def);
        mine = roster.add(type, 200.0f, 200.0f, 0, 500.0f);
        theirs = roster.add(type, 600.0f, 600.0f, 1, 500.0f);
    }

    /// The construction list a `Build` command lands in. A member so a case can inspect it.
    std::vector<rm::sim::Construction> building;

    [[nodiscard]] bool apply(const Command& command) {
        return rm::sim::applyCommand(command, roster.store, roster.catalog, players, armies,
                                     terrain, grid, roster.rate, &building);
    }

    /// Runs the match forward, applying whatever the log says on each tick — which is the
    /// replay loop, and the only loop either a live match or a replay needs.
    void run(const CommandLog& log, rm::TickIndex ticks) {
        std::vector<rm::sim::Projectile> shots;
        std::vector<rm::sim::Economy> economies(2);
        const std::vector<int> commandersEver(2, 0);

        for (rm::TickIndex tick = 0; tick < ticks; ++tick) {
            for (const Command& command : log.at(tick)) {
                (void)apply(command);
            }
            // The routing table for queued orders, one entry per registered type (P4.1). The
            // fixture's units all share a grid, so this is a vector of one pointer repeated —
            // but it goes through the same field the app fills, so the tick exercises the same
            // code either way.
            const std::vector<const rm::sim::PassabilityGrid*> grids(roster.catalog.size(),
                                                                    &grid);
            rm::sim::Match match{.armies = armies,
                                 .economies = economies,
                                 .projectiles = &shots,
                                 .building = &building,
                                 .passability = grids,
                                 .commandersEver = commandersEver};
            (void)rm::sim::tickSkirmish(roster.store, roster.catalog, match, terrain);
        }
    }
};

[[nodiscard]] Command moveOrder(rm::TickIndex tick, rm::PlayerIndex player, UnitId unit,
                                float x, float z) {
    return Command{.tick = tick,
                   .player = player,
                   .kind = CommandKind::Move,
                   .unit = unit,
                   .targetX = rm::test::fx(x),
                   .targetZ = rm::test::fx(z),
                   .buildType = 0};
}

} // namespace

TEST_CASE("a move command routes a unit, and a stop cancels it") {
    Fixture fix;

    CHECK(fix.apply(moveOrder(0, 0, fix.mine, 500.0f, 200.0f)));
    CHECK(fix.roster.motion(fix.mine).moving);
    CHECK_FALSE(fix.roster.motion(fix.mine).path.empty());

    const Command stop{.tick = 1,
                       .player = 0,
                       .kind = CommandKind::Stop,
                       .unit = fix.mine,
                       .targetX = {},
                       .targetZ = {},
                       .buildType = 0};
    CHECK(fix.apply(stop));
    CHECK_FALSE(fix.roster.motion(fix.mine).moving);
    // The route is cleared too, or the unit resumes its old orders the moment something else
    // sets `moving`.
    CHECK(fix.roster.motion(fix.mine).path.empty());
}

TEST_CASE("a short move in one path cell reaches the clicked point") {
    Fixture fix;

    REQUIRE(fix.apply(moveOrder(0, 0, fix.mine, 220.0f, 220.0f)));
    const rm::sim::MoveState& motion = fix.roster.motion(fix.mine);
    REQUIRE(motion.path.size() == 1);
    CHECK(motion.path[0][0] == rm::test::fx(220.0f));
    CHECK(motion.path[0][1] == rm::test::fx(220.0f));
}

TEST_CASE("a player cannot order another army's units") {
    // THE AUTHORISATION CHECK, which was impossible before orders were data: with `orderTo`
    // called at the click site, nothing compared the clicker to the owner. Invisible in a
    // single-player game and the first thing a networked one gets wrong.
    Fixture fix;

    // Player 0 drives army 0. Ordering army 1's unit must be refused, and must change nothing.
    const Command trespass = moveOrder(0, 0, fix.theirs, 100.0f, 100.0f);
    CHECK_FALSE(fix.apply(trespass));
    CHECK_FALSE(fix.roster.motion(fix.theirs).moving);

    // And its own player can.
    CHECK(fix.apply(moveOrder(0, 1, fix.theirs, 100.0f, 100.0f)));
    CHECK(fix.roster.motion(fix.theirs).moving);
}

TEST_CASE("a defeated army takes no more orders") {
    Fixture fix;
    fix.armies[0].defeated = true;
    CHECK_FALSE(fix.apply(moveOrder(0, 0, fix.mine, 500.0f, 200.0f)));
    CHECK_FALSE(fix.roster.motion(fix.mine).moving);
}

TEST_CASE("a stale handle is refused, not resolved to whoever inherited the slot") {
    // The case that makes handles worth having. A player clicks a unit; it dies on the same
    // tick; the slot is reused by a new unit. The order must not arrive at the newcomer.
    Fixture fix;

    const UnitId doomed = fix.mine;
    fix.roster.store.kill(doomed);

    rm::unitdef::UnitDef def;
    def.name = "test_tank";
    const UnitId newcomer = fix.roster.add(fix.roster.addType(def), 200.0f, 200.0f, 0, 500.0f);
    REQUIRE(newcomer.index == doomed.index);       // same slot
    REQUIRE(newcomer.generation != doomed.generation);  // different unit

    CHECK_FALSE(fix.apply(moveOrder(0, 0, doomed, 500.0f, 200.0f)));
    CHECK_FALSE(fix.roster.motion(newcomer).moving);

    // The newcomer takes its own orders perfectly well.
    CHECK(fix.apply(moveOrder(0, 0, newcomer, 500.0f, 200.0f)));
    CHECK(fix.roster.motion(newcomer).moving);
}

TEST_CASE("an unreachable destination is a refused order, not a straight line") {
    // Driving into the water is a worse answer than not moving.
    rm::HeightField sunken = flatField();
    sunken.baseHeight = -500.0f;  // the whole map is under water

    Fixture fix;
    fix.grid = rm::sim::buildPassability(sunken, 0.0f);

    CHECK_FALSE(fix.apply(moveOrder(0, 0, fix.mine, 500.0f, 200.0f)));
    CHECK_FALSE(fix.roster.motion(fix.mine).moving);
}

TEST_CASE("a build command creates a construction, costed from the blueprint") {
    // THE HOLE THIS CLOSED (P3). A `Build` command used to be recorded and refused, because
    // `Construction::blueprintIndex` meant "an index into whatever list the caller is building
    // from" — so the sim had no way to name a blueprint the caller would recognise. One index
    // space later, it can.
    Fixture fix;

    // A builder, and something for it to build.
    rm::unitdef::UnitDef engineerDef;
    engineerDef.name = "engineer";
    engineerDef.buildRate = 10.0f;
    // The build TREE is enforced now: a builder states what it may build, in the same
    // category language the blueprints use, or the order is refused.
    engineerDef.buildableCategory = {{"TESTSTRUCTURE"}};
    const rm::UnitTypeIndex engineerType = fix.roster.addType(engineerDef);

    rm::unitdef::UnitDef mexDef;
    mexDef.name = "mex";
    mexDef.categories = {"TESTSTRUCTURE"};
    mexDef.buildCostMass = rm::test::mag(36.0f);
    mexDef.buildCostEnergy = rm::test::mag(360.0f);
    mexDef.buildTime = rm::test::mag(60.0f);
    const rm::UnitTypeIndex mexType = fix.roster.addType(mexDef);

    const UnitId engineer = fix.roster.add(engineerType, 300.0f, 300.0f, 0, 500.0f);

    const Command build{.tick = 0,
                        .player = 0,
                        .kind = CommandKind::Build,
                        .unit = engineer,
                        .targetX = rm::test::fx(400.0f),
                        .targetZ = rm::test::fx(400.0f),
                        .buildType = mexType};

    REQUIRE(fix.apply(build));
    REQUIRE(fix.building.size() == 1);

    const rm::sim::Construction& work = fix.building.front();
    CHECK(work.armyIndex == 0);
    // Cost and time from the DEFINITION, not from the command — a command says what and where,
    // and the blueprint says what it costs.
    CHECK(rm::test::asFloat(work.cost.mass) == 36.0f);
    CHECK(rm::test::asFloat(work.cost.energy) == 360.0f);
    CHECK(rm::test::asFloat(work.totalBuildTime) == 60.0f);
    // And the rate from the BUILDER, through the clock.
    CHECK(rm::test::asFloat(work.buildPerTick)
          == rm::test::asFloat(fix.roster.rate.magPerTick(10.0f)));
    // The index is the type index, which is the whole point: the caller can resolve it.
    CHECK(work.blueprintIndex == mexType);
}

TEST_CASE("a build command refuses a blocked target footprint") {
    Fixture fix;

    rm::unitdef::UnitDef engineerDef;
    engineerDef.name = "engineer";
    engineerDef.buildRate = 10.0f;
    engineerDef.buildableCategory = {{"TESTSTRUCTURE"}};
    const rm::UnitTypeIndex engineerType = fix.roster.addType(engineerDef);

    rm::unitdef::UnitDef structureDef;
    structureDef.name = "structure";
    structureDef.categories = {"TESTSTRUCTURE"};
    structureDef.collisionRadiusElmos = 8.0f;
    const rm::UnitTypeIndex structureType = fix.roster.addType(structureDef);

    const UnitId engineer = fix.roster.add(engineerType, 300.0f, 300.0f, 0, 500.0f);
    std::fill(fix.grid.passable.begin(), fix.grid.passable.end(), std::uint8_t{0});

    const Command build{.tick = 0,
                        .player = 0,
                        .kind = CommandKind::Build,
                        .unit = engineer,
                        .targetX = rm::test::fx(400.0f),
                        .targetZ = rm::test::fx(400.0f),
                        .buildType = structureType};

    CHECK_FALSE(fix.apply(build));
    CHECK(fix.building.empty());
}

TEST_CASE("only a builder builds") {
    // A tank founding a factory is a caller bug, and refusing it deterministically beats
    // letting it through.
    Fixture fix;
    rm::unitdef::UnitDef mexDef;
    mexDef.name = "mex";
    mexDef.categories = {"TESTSTRUCTURE"};
    mexDef.buildTime = rm::test::mag(60.0f);
    const rm::UnitTypeIndex mexType = fix.roster.addType(mexDef);

    const Command build{.tick = 0,
                        .player = 0,
                        .kind = CommandKind::Build,
                        .unit = fix.mine,  // a plain unit with no build rate
                        .targetX = rm::test::fx(400.0f),
                        .targetZ = rm::test::fx(400.0f),
                        .buildType = mexType};
    CHECK_FALSE(fix.apply(build));
    CHECK(fix.building.empty());
}

TEST_CASE("a build with nowhere to put it, or nothing to build, is refused") {
    Fixture fix;
    rm::unitdef::UnitDef engineerDef;
    engineerDef.name = "engineer";
    engineerDef.buildRate = 10.0f;
    const UnitId engineer =
        fix.roster.add(fix.roster.addType(engineerDef), 300.0f, 300.0f, 0, 500.0f);

    // A type the catalog does not know.
    const Command unknown{.tick = 0,
                          .player = 0,
                          .kind = CommandKind::Build,
                          .unit = engineer,
                          .targetX = rm::test::fx(400.0f),
                          .targetZ = rm::test::fx(400.0f),
                          .buildType = 999};
    CHECK_FALSE(fix.apply(unknown));

    // And a scene with no construction list at all — a decorative crowd. Refused rather than
    // crashing, which is what the null default is for.
    const Command noList{.tick = 0,
                         .player = 0,
                         .kind = CommandKind::Build,
                         .unit = engineer,
                         .targetX = rm::test::fx(400.0f),
                         .targetZ = rm::test::fx(400.0f),
                         .buildType = 0};
    CHECK_FALSE(rm::sim::applyCommand(noList, fix.roster.store, fix.roster.catalog, fix.players,
                                      fix.armies, fix.terrain, fix.grid, fix.roster.rate,
                                      nullptr));
    CHECK(fix.building.empty());
}

// --- The log ---------------------------------------------------------------------------

TEST_CASE("a log returns the commands for a tick, and nothing for an empty one") {
    CommandLog log;
    log.record(moveOrder(0, 0, UnitId{0, 1}, 100.0f, 100.0f));
    log.record(moveOrder(5, 0, UnitId{0, 1}, 200.0f, 100.0f));
    log.record(moveOrder(5, 1, UnitId{1, 1}, 300.0f, 100.0f));
    log.record(moveOrder(9, 0, UnitId{0, 1}, 400.0f, 100.0f));

    CHECK(log.at(0).size() == 1);
    CHECK(log.at(1).empty());  // a tick with no orders is the common case
    CHECK(log.at(5).size() == 2);
    CHECK(log.at(9).size() == 1);
    CHECK(log.at(1000).empty());
    CHECK(log.lastTick() == 9);

    // In the order recorded: two players ordering on one tick must apply in a fixed sequence,
    // or the same log is two different matches.
    CHECK(log.at(5)[0].player == 0);
    CHECK(log.at(5)[1].player == 1);
}

TEST_CASE("a log refuses to go backwards in time") {
    // Checked at RECORD time, because the tick it went backwards on is the only useful thing
    // to know about such a log, and that is knowable here rather than at replay.
    CommandLog log;
    log.record(moveOrder(10, 0, UnitId{0, 1}, 100.0f, 100.0f));
    log.record(moveOrder(3, 0, UnitId{0, 1}, 200.0f, 100.0f));

    CHECK(log.size() == 1);
    CHECK(log.lastTick() == 10);
}

TEST_CASE("a log round-trips through a file exactly") {
    // The fixed-point targets are written as RAW integers, so this is an exact round trip
    // rather than one within a tolerance — which in a determinism artifact is the only
    // acceptable kind.
    CommandLog original;
    original.record(moveOrder(0, 0, UnitId{3, 7}, 123.456f, 789.012f));
    original.record(Command{.tick = 4,
                            .player = 1,
                            .kind = CommandKind::Stop,
                            .unit = UnitId{9, 2},
                            .targetX = {},
                            .targetZ = {},
                            .buildType = 0});
    original.record(Command{.tick = 4,
                            .player = 1,
                            .kind = CommandKind::Attack,
                            .queued = true,
                            .unit = UnitId{9, 2},
                            .targetX = rm::test::fx(-42.5f),
                            .targetZ = rm::test::fx(0.125f),
                            .buildType = 0});
    original.record(Command{.tick = 8,
                            .player = 0,
                            .kind = CommandKind::Build,
                            .unit = UnitId{1, 1},
                            .targetX = rm::test::fx(64.0f),
                            .targetZ = rm::test::fx(64.0f),
                            .buildType = 5});
    original.record(Command{.tick = 9,
                            .player = 0,
                            .kind = CommandKind::AttackMove,
                            .unit = UnitId{3, 7},
                            .targetX = rm::test::fx(400.0f),
                            .targetZ = rm::test::fx(500.0f)});
    original.record(Command{.tick = 10,
                            .player = 0,
                            .kind = CommandKind::Patrol,
                            .unit = UnitId{3, 7},
                            .targetX = rm::test::fx(600.0f),
                            .targetZ = rm::test::fx(700.0f)});

    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "rm-command-log-test.txt";
    REQUIRE(rm::sim::writeCommandLog(original, path.string()));

    const std::optional<CommandLog> read = rm::sim::readCommandLog(path.string());
    REQUIRE(read.has_value());
    REQUIRE(read->size() == original.size());
    for (std::size_t i = 0; i < original.size(); ++i) {
        REQUIRE(read->all()[i] == original.all()[i]);
    }
    CHECK(read->all()[2].queued);

    std::filesystem::remove(path);
}

TEST_CASE("a missing or malformed log is nothing, not a partial one") {
    CHECK_FALSE(rm::sim::readCommandLog("/nonexistent/rm-command-log").has_value());

    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "rm-command-log-bad.txt";
    {
        std::ofstream out{path};
        out << "0 0 move 1 1 100 200 0\n";
        out << "4 0 fly 1 1 100 200 0\n";  // not a kind this engine knows
    }
    // Nothing, rather than the one good line: a partial log replays as a different match.
    CHECK_FALSE(rm::sim::readCommandLog(path.string()).has_value());
    std::filesystem::remove(path);
}

TEST_CASE("a present queue flag must be zero or one") {
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "rm-command-log-bad-queue.txt";
    {
        std::ofstream out{path};
        out << "0 0 move 1 1 100 200 0 0 0 - yes\n";
    }
    CHECK_FALSE(rm::sim::readCommandLog(path.string()).has_value());
    std::filesystem::remove(path);
}

TEST_CASE("a legacy command without a queue column remains a replacement order") {
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "rm-command-log-legacy.txt";
    {
        std::ofstream out{path};
        out << "0 0 move 1 1 100 200 0\n";
    }
    const std::optional<CommandLog> read = rm::sim::readCommandLog(path.string());
    REQUIRE(read.has_value());
    REQUIRE(read->size() == 1);
    CHECK_FALSE(read->all().front().queued);
    std::filesystem::remove(path);
}

TEST_CASE("a queued route survives file round-trip and replay") {
    CommandLog original;
    original.record(moveOrder(0, 0, UnitId{0, 1}, 300.0f, 200.0f));
    Command second = moveOrder(0, 0, UnitId{0, 1}, 300.0f, 500.0f);
    second.queued = true;
    original.record(second);

    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "rm-command-log-queued-route.txt";
    REQUIRE(rm::sim::writeCommandLog(original, path.string()));
    const std::optional<CommandLog> replayLog = rm::sim::readCommandLog(path.string());
    REQUIRE(replayLog.has_value());

    Fixture live;
    Fixture replay;
    live.run(original, 500);
    replay.run(*replayLog, 500);

    REQUIRE(live.roster.store.orders()[live.mine.index].empty());
    REQUIRE(replay.roster.store.orders()[replay.mine.index].empty());
    CHECK(live.roster.store.transforms()[live.mine.index].z
          == replay.roster.store.transforms()[replay.mine.index].z);
    CHECK(live.roster.store.transforms()[live.mine.index].z > rm::test::fx(450.0f));
    std::filesystem::remove(path);
}

// --- The property §7 P2.5 names --------------------------------------------------------

TEST_CASE("a human log and a script log replay through the same path") {
    // THE STATED TEST. Two logs from two sources — one standing in for a player's clicks, one
    // for a script's decisions — both applied by `applyCommand` and both producing a match
    // that a replay reproduces exactly.
    //
    // "The same path" is the claim, and it is structural rather than measured: there is one
    // `applyCommand`, so a human's order and a script's cannot diverge. What is MEASURED here
    // is the consequence: the same log twice is the same match, byte for byte, by state hash.

    // A log as a human would produce it: orders at the moments a person clicked.
    CommandLog human;
    {
        Fixture fix;
        human.record(moveOrder(0, 0, fix.mine, 400.0f, 200.0f));
        human.record(moveOrder(30, 0, fix.mine, 400.0f, 600.0f));
        human.record(Command{.tick = 60,
                             .player = 0,
                             .kind = CommandKind::Stop,
                             .unit = fix.mine,
                             .targetX = {},
                             .targetZ = {},
                             .buildType = 0});
    }

    // A log as a script would produce it: one decision a second, for the other army.
    CommandLog script;
    {
        Fixture fix;
        for (rm::TickIndex tick = 0; tick < 100; tick += 10) {
            script.record(moveOrder(tick, 1, fix.theirs, 300.0f,
                                    300.0f + static_cast<float>(tick)));
        }
    }

    // Both logs, concatenated in tick order — which is what a real match's log is: two sources
    // interleaved. That the two are indistinguishable once recorded IS the property.
    CommandLog both;
    {
        std::vector<Command> merged;
        merged.insert(merged.end(), human.all().begin(), human.all().end());
        merged.insert(merged.end(), script.all().begin(), script.all().end());
        std::stable_sort(merged.begin(), merged.end(),
                         [](const Command& a, const Command& b) { return a.tick < b.tick; });
        for (const Command& command : merged) {
            both.record(command);
        }
    }
    REQUIRE(both.size() == human.size() + script.size());

    // Replayed twice, in two separate sims stepped in one process — the strongest form of the
    // determinism test, and the payoff for having no sim globals (§5.4).
    const auto play = [&both]() {
        Fixture fix;
        fix.run(both, 120);
        std::vector<rm::sim::Projectile> shots;
        std::vector<rm::sim::Economy> economies(2);
        const std::vector<int> commandersEver(2, 0);
        rm::sim::Match match{.armies = fix.armies,
                             .economies = economies,
                             .projectiles = &shots,
                             .commandersEver = commandersEver};
        return rm::sim::hashMatch(fix.roster.store, match);
    };

    const rm::StateHash first = play();
    const rm::StateHash second = play();
    CHECK(first == second);

    // And the log actually did something — otherwise two frozen matches would agree about
    // nothing.
    Fixture moved;
    moved.run(both, 120);
    Fixture still;
    still.run(CommandLog{}, 120);
    CHECK(moved.roster.transform(moved.mine).z != still.roster.transform(still.mine).z);
}

TEST_CASE("a script's order and a click produce identical hashes") {
    // §7 P4.2's stated test, and the sharper half of the one above: not "two replays of one log
    // agree" but "the SOURCE of an order leaves no trace in the match".
    //
    // The two fixtures differ in exactly one way — which army has a human at the keyboard —
    // so in the first the order for army 0 comes from a human player and in the second the
    // identical order comes from a script. Everything else, including the player INDEX, is the
    // same. If the two hashes differ, something about who issued an order has leaked into the
    // world.
    //
    // This is what `feedOrders` deliberately excludes `tick` and `player` for. Those are
    // provenance, they belong in the `CommandLog`, and hashing them would make this test
    // impossible to state rather than merely make it fail.
    const auto playAs = [](int humanArmy) {
        Fixture fix;
        fix.players = rm::sim::onePlayerPerArmy(2, humanArmy);

        CommandLog log;
        log.record(moveOrder(0, 0, fix.mine, 400.0f, 200.0f));
        log.record(moveOrder(30, 0, fix.mine, 400.0f, 600.0f));
        fix.run(log, 120);

        std::vector<rm::sim::Projectile> shots;
        std::vector<rm::sim::Economy> economies(2);
        const std::vector<int> commandersEver(2, 0);
        rm::sim::Match match{.armies = fix.armies,
                             .economies = economies,
                             .projectiles = &shots,
                             .commandersEver = commandersEver};
        return rm::sim::hashMatch(fix.roster.store, match);
    };

    // Army 0 driven by a human, then by a script. Same orders, same result.
    CHECK(playAs(0) == playAs(1));

    // And the orders are not being silently refused in one of the two — a match where nothing
    // happened would agree with another where nothing happened.
    Fixture human;
    human.players = rm::sim::onePlayerPerArmy(2, 0);
    CommandLog log;
    log.record(moveOrder(0, 0, human.mine, 400.0f, 200.0f));
    human.run(log, 120);
    Fixture idle;
    idle.run(CommandLog{}, 120);
    CHECK(human.roster.transform(human.mine).x != idle.roster.transform(idle.mine).x);
}

TEST_CASE("an upgrade builds where the builder stands and remembers who it replaces") {
    // Moho's tech path: a T2 factory is the T1 factory BECOMING one, declared by the
    // blueprint's General.UpgradesTo. Same Build command; the sim recognises the pair and
    // pins the work to the builder's own position, whatever the order said — a factory does
    // not upgrade into a field — and records the unit to replace at completion.
    Fixture fix;

    rm::unitdef::UnitDef t1Def;
    t1Def.name = "FACT1";
    t1Def.buildRate = 20.0f;
    t1Def.buildableCategory = {{"BUILTBYT1"}};
    t1Def.upgradesTo = "FACT2";
    const rm::UnitTypeIndex t1Type = fix.roster.addType(t1Def);

    rm::unitdef::UnitDef t2Def;
    t2Def.name = "FACT2";
    t2Def.categories = {"BUILTBYT1"};
    t2Def.buildTime = rm::test::mag(60.0f);
    const rm::UnitTypeIndex t2Type = fix.roster.addType(t2Def);

    const UnitId factory = fix.roster.add(t1Type, 300.0f, 300.0f, 0, 500.0f);

    // The order names a point far away; the upgrade ignores it.
    const Command upgrade{.tick = 0,
                          .player = 0,
                          .kind = CommandKind::Build,
                          .unit = factory,
                          .targetX = rm::test::fx(700.0f),
                          .targetZ = rm::test::fx(700.0f),
                          .buildType = t2Type};
    REQUIRE(fix.apply(upgrade));
    REQUIRE(fix.building.size() == 1);

    const rm::sim::Construction& work = fix.building.front();
    CHECK(work.isUpgrade());
    CHECK(work.upgradeOf == factory);
    CHECK(rm::test::asFloat(work.position[0]) == 300.0f);
    CHECK(rm::test::asFloat(work.position[2]) == 300.0f);

    // An ordinary build by the same factory type is NOT an upgrade: the target has to be
    // what the blueprint says the builder becomes, not merely something it may build.
    rm::unitdef::UnitDef tankDef;
    tankDef.name = "TANK1";
    tankDef.categories = {"BUILTBYT1"};
    tankDef.motion = rm::unitdef::MotionType::Land;
    tankDef.speedElmosPerSecond = 30.0f;
    tankDef.buildTime = rm::test::mag(10.0f);
    const rm::UnitTypeIndex tankType = fix.roster.addType(tankDef);
    const UnitId secondFactory = fix.roster.add(t1Type, 500.0f, 500.0f, 0, 500.0f);
    const Command train{.tick = 0,
                        .player = 0,
                        .kind = CommandKind::Build,
                        .unit = secondFactory,
                        .targetX = rm::test::fx(500.0f),
                        .targetZ = rm::test::fx(500.0f),
                        .buildType = tankType};
    REQUIRE(fix.apply(train));
    CHECK_FALSE(fix.building.back().isUpgrade());
}

TEST_CASE("a build order names a place on the map, and the ground decides the height") {
    // THE `y` IS ALWAYS ZERO. `spawnUnit` overwrites whatever height a construction carries with
    // `terrain.heightAt(x, z)`, so a height stored here is never read — it is derived data that
    // reaches only the state hash, where it makes two runs of one match look different for a
    // reason no player can observe.
    //
    // This is not hypothetical tidiness. The app's build path carried a mass marker's own `y`
    // through, and routing builds onto `applyCommand` zeroed it — which is exactly why that
    // change moved the golden at the first EXTRACTOR (whose site is a marker) and at neither the
    // power generator nor the factory (whose sites are computed with a zero `y` already).
    Fixture fix;

    rm::unitdef::UnitDef engineerDef;
    engineerDef.name = "engineer";
    engineerDef.buildRate = 10.0f;
    // The build TREE is enforced now: a builder states what it may build, in the same
    // category language the blueprints use, or the order is refused.
    engineerDef.buildableCategory = {{"TESTSTRUCTURE"}};
    const rm::UnitTypeIndex engineerType = fix.roster.addType(engineerDef);

    rm::unitdef::UnitDef mexDef;
    mexDef.name = "mex";
    mexDef.categories = {"TESTSTRUCTURE"};
    mexDef.buildTime = rm::test::mag(60.0f);
    const rm::UnitTypeIndex mexType = fix.roster.addType(mexDef);

    const UnitId engineer = fix.roster.add(engineerType, 300.0f, 300.0f, 0, 500.0f);
    REQUIRE(fix.apply(moveOrder(0, 0, engineer, 600.0f, 300.0f)));
    REQUIRE(fix.roster.motion(engineer).moving);

    REQUIRE(fix.apply(Command{.tick = 0,
                              .player = 0,
                              .kind = CommandKind::Build,
                              .unit = engineer,
                              .targetX = rm::test::fx(400.0f),
                              .targetZ = rm::test::fx(700.0f),
                              .buildType = mexType}));
    REQUIRE(fix.building.size() == 1);

    const rm::sim::Construction& work = fix.building.front();
    CHECK(rm::test::asFloat(work.position[0]) == 400.0f);
    CHECK(work.position[1] == rm::sim::Fx{});
    CHECK(rm::test::asFloat(work.position[2]) == 700.0f);
}

TEST_CASE("a build occupies its builder and refuses parallel work") {
    Fixture fix;

    rm::unitdef::UnitDef engineerDef;
    engineerDef.name = "engineer";
    engineerDef.buildRate = 10.0f;
    // The build TREE is enforced now: a builder states what it may build, in the same
    // category language the blueprints use, or the order is refused.
    engineerDef.buildableCategory = {{"TESTSTRUCTURE"}};
    const rm::UnitTypeIndex engineerType = fix.roster.addType(engineerDef);

    rm::unitdef::UnitDef mexDef;
    mexDef.name = "mex";
    mexDef.categories = {"TESTSTRUCTURE"};
    mexDef.buildTime = rm::test::mag(60.0f);
    const rm::UnitTypeIndex mexType = fix.roster.addType(mexDef);

    const UnitId engineer = fix.roster.add(engineerType, 300.0f, 300.0f, 0, 500.0f);

    REQUIRE(fix.apply(Command{.tick = 0,
                              .player = 0,
                              .kind = CommandKind::Build,
                              .unit = engineer,
                              .targetX = rm::test::fx(400.0f),
                               .targetZ = rm::test::fx(400.0f),
                               .buildType = mexType}));

    CHECK_FALSE(fix.roster.motion(engineer).moving);
    CHECK(fix.roster.motion(engineer).path.empty());
    CHECK_FALSE(fix.roster.store.orders()[engineer.index].empty());
    CHECK_FALSE(fix.apply(Command{.tick = 1,
                                  .player = 0,
                                  .kind = CommandKind::Build,
                                  .unit = engineer,
                                  .targetX = rm::test::fx(500.0f),
                                  .targetZ = rm::test::fx(500.0f),
                                  .buildType = mexType}));
    CHECK(fix.building.size() == 1);

    Command queued{.tick = 1,
                   .player = 0,
                   .kind = CommandKind::Build,
                   .queued = true,
                   .unit = engineer,
                   .targetX = rm::test::fx(500.0f),
                   .targetZ = rm::test::fx(500.0f),
                   .buildType = mexType};
    REQUIRE(fix.apply(queued));
    CHECK(fix.roster.store.orders()[engineer.index].size() == 2);
    CHECK(fix.building.size() == 1);

    fix.run(CommandLog{}, 60);
    CHECK(fix.building[0].finished());
    CHECK(fix.roster.store.orders()[engineer.index].size() == 1);
    fix.run(CommandLog{}, 1);
    REQUIRE(fix.building.size() == 2);
    CHECK_FALSE(fix.building[1].finished());
}

TEST_CASE("a structure build refuses live occupancy and only unfinished work") {
    Fixture fix;

    rm::unitdef::UnitDef engineerDef;
    engineerDef.name = "engineer";
    engineerDef.buildRate = 10.0f;
    engineerDef.buildableCategory = {{"TESTSTRUCTURE"}};
    const rm::UnitTypeIndex engineerType = fix.roster.addType(engineerDef);

    rm::unitdef::UnitDef structureDef;
    structureDef.name = "structure";
    structureDef.categories = {"TESTSTRUCTURE"};
    structureDef.collisionRadiusElmos = 10.0f;
    structureDef.buildTime = rm::test::mag(60.0f);
    const rm::UnitTypeIndex structureType = fix.roster.addType(structureDef);

    const UnitId firstBuilder = fix.roster.add(engineerType, 300.0f, 300.0f, 0, 500.0f);
    const UnitId secondBuilder = fix.roster.add(engineerType, 320.0f, 300.0f, 0, 500.0f);
    const UnitId blocker = fix.roster.add(engineerType, 400.0f, 400.0f, 0, 500.0f);
    const auto build = [&](UnitId builder, float x, float z) {
        return fix.apply(Command{.tick = 0,
                                 .player = 0,
                                 .kind = CommandKind::Build,
                                 .unit = builder,
                                 .targetX = rm::test::fx(x),
                                 .targetZ = rm::test::fx(z),
                                 .buildType = structureType});
    };

    CHECK_FALSE(build(firstBuilder, 300.0f, 300.0f));  // the builder itself occupies the site
    CHECK_FALSE(build(firstBuilder, 400.0f, 400.0f));
    fix.roster.store.kill(blocker);
    REQUIRE(build(firstBuilder, 400.0f, 400.0f));
    CHECK_FALSE(build(secondBuilder, 405.0f, 400.0f));
    CHECK(fix.building.size() == 1);

    // Finished entries remain as history but no longer occupy ground. The spawned structure
    // would normally become the live blocker; this fixture deliberately has not spawned it.
    fix.building[0].buildTimeRemaining = rm::sim::Mag{};
    REQUIRE(build(secondBuilder, 400.0f, 400.0f));
    CHECK(fix.building.size() == 2);
}

TEST_CASE("an attack with a target is a pursuit: chase, hold in range, finish on the kill") {
    Fixture fixture;

    // An armed, mobile attacker type. The fixture's default is toothless, and a unit with no
    // firing weapon deliberately does not chase — for it an attack is the plain walk it was.
    rm::unitdef::UnitDef armed;
    armed.name = "test_gunner";
    armed.speedElmosPerSecond = 40.0f;
    armed.motion = rm::unitdef::MotionType::Land;
    rm::unitdef::Weapon gun;
    gun.label = "gun";
    gun.damage = rm::sim::magFromFloat(10.0f);
    gun.maxRange = rm::test::fx(100.0f);
    gun.rateOfFire = 1.0f;
    armed.weapons.push_back(gun);
    const rm::UnitTypeIndex type = fixture.roster.addType(armed);
    const UnitId hunter = fixture.roster.add(type, 200.0f, 200.0f, 0, 500.0f);
    const UnitId prey = fixture.theirs;  // at 600, 600 — far outside the 100-elmo reach

    CommandLog log;
    Command attack = moveOrder(0, 0, hunter, 600.0f, 600.0f);
    attack.kind = CommandKind::Attack;
    attack.target = prey;
    log.record(attack);

    // The hunter closes. After a while it is nearer the prey than it started, and once the
    // gap is inside the weapon's reach it HOLDS rather than walking to the prey's feet.
    fixture.run(log, 200);
    const auto gapNow = [&] {
        const rm::sim::Transform& a = fixture.roster.store.transforms()[hunter.index];
        const rm::sim::Transform& b = fixture.roster.store.transforms()[prey.index];
        return rm::sim::groundDistanceElmos({a.x, a.y, a.z}, {b.x, b.y, b.z});
    };
    CHECK(gapNow() <= rm::test::fx(100.0f));
    CHECK_FALSE(fixture.roster.store.orders()[hunter.index].empty());

    // The prey breaks away; the chase re-routes without a fresh order.
    {
        rm::sim::Transform& b = fixture.roster.store.transforms()[prey.index];
        b.x = rm::test::fx(900.0f);
        b.z = rm::test::fx(200.0f);
        fixture.roster.reindex();
    }
    const CommandLog quiet;
    fixture.run(quiet, 300);
    CHECK(gapNow() <= rm::test::fx(100.0f));

    // The kill completes the order: the queue empties on its own.
    fixture.roster.store.health()[prey.index].current = rm::sim::Mag{};
    fixture.run(quiet, 10);
    CHECK(fixture.roster.store.orders()[hunter.index].empty());
}

TEST_CASE("a short-range pursuit closes inside the target's path cell") {
    Fixture fixture;

    rm::unitdef::UnitDef armed;
    armed.name = "test_short_range_gunner";
    armed.speedElmosPerSecond = 40.0f;
    armed.motion = rm::unitdef::MotionType::Land;
    rm::unitdef::Weapon gun;
    gun.label = "short gun";
    gun.damage = rm::sim::magFromFloat(1.0f);
    gun.maxRange = rm::test::fx(12.0f);
    gun.rateOfFire = 1.0f;
    armed.weapons.push_back(gun);
    const UnitId hunter =
        fixture.roster.add(fixture.roster.addType(armed), 200.0f, 200.0f, 0, 500.0f);
    const UnitId prey = fixture.theirs;
    fixture.roster.transform(prey).x = rm::test::fx(630.0f);
    fixture.roster.transform(prey).z = rm::test::fx(630.0f);
    fixture.roster.reindex();

    Command attack = moveOrder(0, 0, hunter, 630.0f, 630.0f);
    attack.kind = CommandKind::Attack;
    attack.target = prey;
    CommandLog log;
    log.record(attack);
    fixture.run(log, 300);

    CHECK(rm::sim::groundDistanceElmos(rm::sim::positionOf(fixture.roster.transform(hunter)),
                                       rm::sim::positionOf(fixture.roster.transform(prey)))
          <= gun.maxRange);
    CHECK_FALSE(fixture.roster.motion(hunter).moving);
}
