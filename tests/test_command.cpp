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
#include "core/sim/Intel.hpp"
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
using rm::sim::CommandIssue;
using rm::sim::CommandKind;
using rm::sim::CommandLog;
using rm::sim::CommandPhase;
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
    rm::sim::PathService paths;
    rm::TickIndex nextTick = 0;

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
                                     terrain, grid, roster.rate, &building, nullptr, nullptr,
                                     &paths);
    }

    [[nodiscard]] bool apply(const CommandIssue& issue) {
        return static_cast<bool>(rm::sim::applyCommand(
            issue, roster.store, roster.catalog, players, armies, terrain,
            [this](UnitId) { return &grid; }, roster.rate, &building, nullptr, nullptr,
            &paths));
    }

    /// Runs the match forward, applying whatever the log says on each tick — which is the
    /// replay loop, and the only loop either a live match or a replay needs.
    void run(const CommandLog& log, rm::TickIndex ticks, rm::sim::Intel* intel = nullptr) {
        std::vector<rm::sim::Projectile> shots;
        std::vector<rm::sim::Economy> economies(2);
        const std::vector<int> commandersEver(2, 0);

        for (rm::TickIndex tick = nextTick; tick < nextTick + ticks; ++tick) {
            for (const CommandIssue& issue : log.at(tick, CommandPhase::PreTick)) {
                (void)apply(issue);
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
                                  .pathService = &paths,
                                  .commandersEver = commandersEver,
                                  .intel = intel};
            (void)rm::sim::tickSkirmish(roster.store, roster.catalog, match, terrain);
            for (const CommandIssue& issue : log.at(tick, CommandPhase::PostSpawn)) {
                (void)apply(issue);
            }
        }
        nextTick += ticks;
    }

    [[nodiscard]] rm::StateHash hash() {
        std::vector<rm::sim::Economy> economies(2);
        const std::vector<int> commandersEver(2, 0);
        rm::sim::Match match{.armies = armies,
                              .economies = economies,
                              .pathService = &paths,
                              .commandersEver = commandersEver};
        return rm::sim::hashMatch(roster.store, match);
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

[[nodiscard]] CommandIssue logged(Command command, std::uint32_t counter = 0,
                                  CommandPhase phase = CommandPhase::PreTick) {
    return CommandIssue{
        .tick = command.tick,
        .phase = phase,
        .source = static_cast<rm::CommandSource>(command.player),
        .id = rm::commandId(static_cast<rm::CommandSource>(command.player), counter),
        .player = command.player,
        .kind = command.kind,
        .queued = command.queued,
        .units = {command.unit},
        .targetX = command.targetX,
        .targetZ = command.targetZ,
        .target = command.target,
        .buildType = command.buildType,
    };
}

} // namespace

TEST_CASE("a move command routes a unit, and a stop cancels it") {
    Fixture fix;

    CHECK(fix.apply(moveOrder(0, 0, fix.mine, 500.0f, 200.0f)));
    CHECK(fix.roster.motion(fix.mine).path.empty());
    fix.run(CommandLog{}, 2);
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

TEST_CASE("path requests admitted during a beat wait until the next beat") {
    Fixture fix;
    const UnitId second = fix.roster.add(fix.roster.store.typeAt(fix.mine.index), 200.0f, 300.0f,
                                          0, 500.0f);

    CommandLog log;
    REQUIRE(log.record(logged(moveOrder(0, 0, fix.mine, 500.0f, 200.0f))));
    REQUIRE(log.record(logged(moveOrder(0, 0, second, 500.0f, 300.0f), 1)));

    // Both orders are admitted during beat 0, so neither is eligible for that beat's path service.
    // They wait for beat 1; the army's FIFO budget then admits only the first route.
    fix.run(log, 1);
    CHECK(fix.roster.motion(fix.mine).path.empty());
    CHECK(fix.roster.motion(second).path.empty());

    fix.run(CommandLog{}, 1);
    CHECK_FALSE(fix.roster.motion(fix.mine).path.empty());
    CHECK(fix.roster.motion(second).path.empty());

    fix.run(CommandLog{}, 1);
    CHECK_FALSE(fix.roster.motion(second).path.empty());
}

TEST_CASE("a stopped move is removed before path service spends work on it") {
    Fixture fix;

    REQUIRE(fix.apply(moveOrder(0, 0, fix.mine, 500.0f, 200.0f)));
    REQUIRE(fix.apply(Command{.tick = 0, .player = 0, .kind = CommandKind::Stop, .unit = fix.mine}));
    REQUIRE(fix.apply(moveOrder(0, 0, fix.mine, 600.0f, 200.0f)));

    fix.run(CommandLog{}, 2);
    CHECK_FALSE(fix.roster.motion(fix.mine).path.empty());
    CHECK(fix.roster.motion(fix.mine).path.back()[0] == rm::test::fx(600.0f));
}

TEST_CASE("a short move in one path cell reaches the clicked point") {
    Fixture fix;

    REQUIRE(fix.apply(moveOrder(0, 0, fix.mine, 220.0f, 220.0f)));
    fix.run(CommandLog{}, 2);
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
    fix.run(CommandLog{}, 2);
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
    fix.run(CommandLog{}, 2);
    CHECK(fix.roster.motion(newcomer).moving);
}

TEST_CASE("an unreachable move retries twice after ten idle service beats before retiring") {
    // Driving into the water is a worse answer than not moving, but C-176 keeps the intent
    // through two wait-then-repath attempts before it gives up on the third failure.
    rm::HeightField sunken = flatField();
    sunken.baseHeight = -500.0f;  // the whole map is under water

    Fixture fix;
    fix.grid = rm::sim::buildPassability(sunken, 0.0f);

    CHECK(fix.apply(moveOrder(0, 0, fix.mine, 500.0f, 200.0f)));
    fix.run(CommandLog{}, 2);  // admission, then the first failed search
    CHECK_FALSE(fix.roster.store.orders()[fix.mine.index].empty());

    fix.run(CommandLog{}, 10);  // C-176's wait contains ten complete idle service passes
    CHECK_FALSE(fix.roster.store.orders()[fix.mine.index].empty());
    REQUIRE(fix.paths.retryWaits()[0] == 0);
    REQUIRE(fix.paths.failureCounts()[0] == 1);

    fix.run(CommandLog{}, 1);  // only now does the second failed search run
    CHECK_FALSE(fix.roster.store.orders()[fix.mine.index].empty());
    REQUIRE(fix.paths.retryWaits()[0] == 10);
    REQUIRE(fix.paths.failureCounts()[0] == 2);

    fix.run(CommandLog{}, 10);  // the second wait is also ten complete idle service passes
    REQUIRE(fix.paths.retryWaits()[0] == 0);
    REQUIRE(fix.paths.failureCounts()[0] == 2);

    fix.run(CommandLog{}, 1);  // the third failure retires the intent
    CHECK(fix.roster.store.orders()[fix.mine.index].empty());
}

TEST_CASE("a path retry wait changes the authoritative hash") {
    rm::HeightField sunken = flatField();
    sunken.baseHeight = -500.0f;

    Fixture fix;
    fix.grid = rm::sim::buildPassability(sunken, 0.0f);
    REQUIRE(fix.apply(moveOrder(0, 0, fix.mine, 500.0f, 200.0f)));
    fix.run(CommandLog{}, 2);  // admission, then the failed search starts C-176's wait

    const rm::StateHash waitingTenBeats = fix.hash();
    fix.run(CommandLog{}, 1);
    CHECK(fix.hash() != waitingTenBeats);
}

TEST_CASE("a published route retries a newly blocked final cell on its due pass") {
    Fixture fix;

    // Put the route in cell (0, 0): its phase is due at tick zero and then every 91 ticks.
    // The target is far enough away that the route remains published until the next due pass.
    fix.roster.transform(fix.mine).x = rm::test::fx(32.0f);
    fix.roster.transform(fix.mine).z = rm::test::fx(32.0f);
    fix.roster.reindex();
    constexpr rm::TickIndex kNextDueTick = 91;
    REQUIRE(rm::sim::pathPhaseDue(0, 0, fix.grid.cellsX, 0));
    REQUIRE(rm::sim::pathPhaseDue(0, 0, fix.grid.cellsX, kNextDueTick));

    REQUIRE(fix.apply(moveOrder(0, 0, fix.mine, 900.0f, 32.0f)));
    fix.run(CommandLog{}, 2);  // admission, then publication
    REQUIRE_FALSE(fix.roster.motion(fix.mine).path.empty());
    fix.roster.motion(fix.mine).speedPerTick = {};  // keep the published route in place

    const int finalX = fix.grid.cellAtWorld(rm::test::fx(900.0f));
    const int finalZ = fix.grid.cellAtWorld(rm::test::fx(32.0f));
    fix.grid.passable[static_cast<std::size_t>(finalZ * fix.grid.cellsX + finalX)] = 0;

    // A pass not selected by the existing phase gate leaves the published route alone.
    REQUIRE_FALSE(rm::sim::pathPhaseDue(0, 0, fix.grid.cellsX, 2));
    fix.run(CommandLog{}, 1);
    CHECK_FALSE(fix.roster.motion(fix.mine).path.empty());

    fix.run(CommandLog{}, kNextDueTick - 3);
    REQUIRE_FALSE(fix.roster.motion(fix.mine).path.empty());

    const rm::CommandId command =
        fix.roster.store.orders()[fix.mine.index].active()->payload().id;
    fix.run(CommandLog{}, 1);  // tick 91: the next due pass

    // A stale route is withdrawn, but its command remains alive while the path service owns the
    // retry; a blocked destination must not turn a previously accepted player order into a drop.
    CHECK(fix.roster.motion(fix.mine).path.empty());
    CHECK_FALSE(fix.roster.store.orders()[fix.mine.index].empty());
    CHECK(fix.paths.contains(fix.mine, command));
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

TEST_CASE("a factory builds mobile products on its own pad, not the clicked plan") {
    Fixture fix;

    rm::unitdef::UnitDef factoryDef;
    factoryDef.name = "air_factory";
    factoryDef.categories = {"FACTORY"};
    factoryDef.buildRate = 10.0f;
    factoryDef.buildableCategory = {{"AIRPRODUCT"}};
    const rm::UnitTypeIndex factoryType = fix.roster.addType(factoryDef);

    rm::unitdef::UnitDef aircraftDef;
    aircraftDef.name = "aircraft";
    aircraftDef.categories = {"AIRPRODUCT"};
    aircraftDef.motion = rm::unitdef::MotionType::Air;
    aircraftDef.speedElmosPerSecond = 10.0f;
    const rm::UnitTypeIndex aircraftType = fix.roster.addType(aircraftDef);

    const UnitId factory = fix.roster.add(factoryType, 300.0f, 300.0f, 0, 500.0f);
    const Command build{.tick = 0,
                        .player = 0,
                        .kind = CommandKind::Build,
                        .unit = factory,
                        .targetX = rm::test::fx(600.0f),
                        .targetZ = rm::test::fx(600.0f),
                        .buildType = aircraftType};

    REQUIRE(fix.apply(build));
    REQUIRE(fix.building.size() == 1);
    CHECK(fix.building[0].position[0] == rm::test::fx(300.0f));
    CHECK(fix.building[0].position[2] == rm::test::fx(300.0f));
}

TEST_CASE("a factory ignores an unplaceable plan for a land product") {
    Fixture fix;

    rm::unitdef::UnitDef factoryDef;
    factoryDef.name = "land_factory";
    factoryDef.categories = {"FACTORY"};
    factoryDef.buildRate = 10.0f;
    factoryDef.buildableCategory = {{"LANDPRODUCT"}};
    const rm::UnitTypeIndex factoryType = fix.roster.addType(factoryDef);

    rm::unitdef::UnitDef tankDef;
    tankDef.name = "tank";
    tankDef.categories = {"LANDPRODUCT"};
    tankDef.speedElmosPerSecond = 10.0f;
    const rm::UnitTypeIndex tankType = fix.roster.addType(tankDef);

    const UnitId factory = fix.roster.add(factoryType, 300.0f, 300.0f, 0, 500.0f);
    const Command build{.tick = 0,
                        .player = 0,
                        .kind = CommandKind::Build,
                        .unit = factory,
                        .targetX = rm::test::fx(600.0f),  // off the 512-elmo fixture map
                        .targetZ = rm::test::fx(600.0f),
                        .buildType = tankType};

    REQUIRE(fix.apply(build));
    REQUIRE(fix.building.size() == 1);
    CHECK(fix.building[0].position[0] == rm::test::fx(300.0f));
    CHECK(fix.building[0].position[2] == rm::test::fx(300.0f));
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

TEST_CASE("a log returns semantic issues by tick and phase") {
    CommandLog log;
    REQUIRE(log.record(logged(moveOrder(0, 0, UnitId{0, 1}, 100.0f, 100.0f))));
    REQUIRE(log.record(logged(moveOrder(5, 0, UnitId{0, 1}, 200.0f, 100.0f), 1)));
    REQUIRE(log.record(logged(moveOrder(5, 1, UnitId{1, 1}, 300.0f, 100.0f), 0)));
    REQUIRE(log.record(logged(moveOrder(5, 0, UnitId{2, 1}, 400.0f, 100.0f), 2,
                              CommandPhase::PostSpawn)));

    CHECK(log.at(0, CommandPhase::PreTick).size() == 1);
    CHECK(log.at(1, CommandPhase::PreTick).empty());
    CHECK(log.at(5, CommandPhase::PreTick).size() == 2);
    CHECK(log.at(5, CommandPhase::PostSpawn).size() == 1);
    CHECK(log.at(1000, CommandPhase::PreTick).empty());
    CHECK(log.lastTick() == 5);
    CHECK(log.at(5, CommandPhase::PreTick)[0].source == 0);
    CHECK(log.at(5, CommandPhase::PreTick)[1].source == 1);

    CHECK_FALSE(log.record(logged(moveOrder(5, 0, UnitId{0, 1}, 0.0f, 0.0f), 3)));
    CHECK_FALSE(log.record(logged(moveOrder(3, 0, UnitId{0, 1}, 0.0f, 0.0f), 3)));
}

TEST_CASE("a grouped semantic log round-trips through a strict versioned file") {
    CommandLog original;
    REQUIRE(original.record(CommandIssue{
        .tick = 7,
        .phase = CommandPhase::PreTick,
        .source = 1,
        .id = rm::commandId(1, 42),
        .player = 1,
        .kind = CommandKind::Attack,
        .queued = true,
        .units = {UnitId{3, 7}, UnitId{9, 2}},
        .targetX = rm::test::fx(-42.5f),
        .targetZ = rm::test::fx(0.125f),
        .target = UnitId{12, 4},
        .count = 9,
    }));
    REQUIRE(original.record(CommandIssue{
        .tick = 8,
        .phase = CommandPhase::PostSpawn,
        .source = 1,
        .id = rm::commandId(1, 43),
        .player = 1,
        .kind = CommandKind::Build,
        .units = {UnitId{9, 2}},
        .targetX = rm::test::fx(64.0f),
        .targetZ = rm::test::fx(64.0f),
        .buildType = 5,
    }));
    REQUIRE(original.record(CommandIssue{
        .tick = 9,
        .phase = CommandPhase::PreTick,
        .source = 1,
        .id = rm::commandId(1, 44),
        .player = 1,
        .kind = CommandKind::Stop,
        .units = {},
    }));

    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "rm-command-log-test.txt";
    REQUIRE(rm::sim::writeCommandLog(original, path.string(), [](std::uint32_t type) {
        return type == 5 ? "/units/test build.bp" : std::string{};
    }));

    std::vector<std::string> paths;
    const std::optional<CommandLog> read = rm::sim::readCommandLog(path.string(), &paths);
    REQUIRE(read.has_value());
    CHECK(std::ranges::equal(read->all(), original.all()));
    REQUIRE(paths.size() == 3);
    CHECK(paths[0].empty());
    CHECK(paths[1] == "/units/test build.bp");
    CHECK(paths[2].empty());

    std::filesystem::remove(path);
}

TEST_CASE("the semantic reader rejects legacy, malformed, and truncated logs") {
    CHECK_FALSE(rm::sim::readCommandLog("/nonexistent/rm-command-log").has_value());

    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "rm-command-log-bad.txt";
    {
        std::ofstream out{path};
        out << "0 0 move 1 1 100 200 0\n";
    }
    CHECK_FALSE(rm::sim::readCommandLog(path.string()).has_value());
    {
        std::ofstream out{path, std::ios::trunc};
        out << "recoil-metal semantic command log\nversion 2\nissue-count 0\n";
    }
    CHECK_FALSE(rm::sim::readCommandLog(path.string()).has_value());
    {
        std::ofstream out{path, std::ios::trunc};
        out << "recoil-metal semantic command log\nversion 1\nissue-count 2\n"
               "0 pre-tick 0 0 0 move 0 1 100 200 0 0 0 1 1 1 \"\"\n";
    }
    CHECK_FALSE(rm::sim::readCommandLog(path.string()).has_value());
    {
        std::ofstream out{path, std::ios::trunc};
        out << "recoil-metal semantic command log\nversion 1\nissue-count 1\n"
               "0 pre-tick 0 0 0 move 0 1 100 200 0 0 0 2 4 1 3 1 \"\"\n";
    }
    CHECK_FALSE(rm::sim::readCommandLog(path.string()).has_value());
    std::filesystem::remove(path);
}

TEST_CASE("a queued route survives file round-trip and replay") {
    CommandLog original;
    REQUIRE(original.record(logged(moveOrder(0, 0, UnitId{0, 1}, 300.0f, 200.0f))));
    Command second = moveOrder(0, 0, UnitId{0, 1}, 300.0f, 500.0f);
    second.queued = true;
    REQUIRE(original.record(logged(second, 1)));

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
        human.record(logged(moveOrder(0, 0, fix.mine, 400.0f, 200.0f), 0));
        human.record(logged(moveOrder(30, 0, fix.mine, 400.0f, 600.0f), 1));
        human.record(logged(Command{.tick = 60,
                                    .player = 0,
                                    .kind = CommandKind::Stop,
                                    .unit = fix.mine}, 2));
    }

    // A log as a script would produce it: one decision a second, for the other army.
    CommandLog script;
    {
        Fixture fix;
        for (rm::TickIndex tick = 0; tick < 100; tick += 10) {
            script.record(logged(moveOrder(tick, 1, fix.theirs, 300.0f,
                                           300.0f + static_cast<float>(tick)),
                                 static_cast<std::uint32_t>(tick / 10)));
        }
    }

    // Both logs, concatenated in tick order — which is what a real match's log is: two sources
    // interleaved. That the two are indistinguishable once recorded IS the property.
    CommandLog both;
    {
        std::vector<CommandIssue> merged;
        merged.insert(merged.end(), human.all().begin(), human.all().end());
        merged.insert(merged.end(), script.all().begin(), script.all().end());
        std::stable_sort(merged.begin(), merged.end(),
                         [](const CommandIssue& a, const CommandIssue& b) {
                             return a.tick < b.tick;
                         });
        for (const CommandIssue& issue : merged) {
            both.record(issue);
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
        log.record(logged(moveOrder(0, 0, fix.mine, 400.0f, 200.0f), 0));
        log.record(logged(moveOrder(30, 0, fix.mine, 400.0f, 600.0f), 1));
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
    log.record(logged(moveOrder(0, 0, human.mine, 400.0f, 200.0f)));
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
    fix.run(CommandLog{}, 2);
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
    log.record(logged(attack));

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
    log.record(logged(attack));
    fixture.run(log, 300);

    CHECK(rm::sim::groundDistanceElmos(rm::sim::positionOf(fixture.roster.transform(hunter)),
                                       rm::sim::positionOf(fixture.roster.transform(prey)))
          <= gun.maxRange);
    CHECK_FALSE(fixture.roster.motion(hunter).moving);
}

TEST_CASE("an explicit attack fires at its ordered target, not the automatic nearest target") {
    Fixture fixture;

    rm::unitdef::UnitDef turret;
    turret.name = "test_turreted_beam";
    rm::unitdef::Weapon beam;
    beam.label = "beam";
    beam.role = rm::unitdef::WeaponRole::DirectFire;
    beam.damage = rm::sim::magFromFloat(10.0f);
    beam.maxRange = rm::test::fx(200.0f);
    beam.rateOfFire = static_cast<float>(fixture.roster.rate.ticksPerSecond());
    beam.beam = true;
    beam.turreted = true;
    turret.weapons.push_back(beam);
    const UnitId attacker =
        fixture.roster.add(fixture.roster.addType(turret), 200.0f, 200.0f, 0, 500.0f);
    const UnitId decoy = fixture.roster.add(fixture.roster.store.typeAt(fixture.theirs.index),
                                            250.0f, 200.0f, 1, 500.0f);
    const UnitId target = fixture.theirs;
    fixture.roster.transform(target).x = rm::test::fx(350.0f);
    fixture.roster.transform(target).z = rm::test::fx(200.0f);
    fixture.roster.reindex();

    const rm::sim::Mag targetHealth = fixture.roster.store.health()[target.index].current;
    const rm::sim::Mag decoyHealth = fixture.roster.store.health()[decoy.index].current;

    // Without an order, automatic acquisition selects the nearer eligible enemy.
    fixture.run(CommandLog{}, 1);
    CHECK(fixture.roster.store.health()[decoy.index].current < decoyHealth);
    CHECK(fixture.roster.store.health()[target.index].current == targetHealth);

    Command attack = moveOrder(fixture.nextTick, 0, attacker, 350.0f, 200.0f);
    attack.kind = CommandKind::Attack;
    attack.target = target;
    CommandLog log;
    REQUIRE(log.record(logged(attack)));

    const rm::sim::Mag decoyHealthAfterAutomatic =
        fixture.roster.store.health()[decoy.index].current;
    fixture.run(log, 1);

    CHECK(fixture.roster.store.health()[target.index].current < targetHealth);
    CHECK(fixture.roster.store.health()[decoy.index].current == decoyHealthAfterAutomatic);
}

TEST_CASE("an explicit attack cannot fire after its target is no longer seen") {
    Fixture fixture;

    rm::unitdef::UnitDef turret;
    turret.name = "test_turreted_beam";
    rm::unitdef::Weapon beam;
    beam.label = "beam";
    beam.role = rm::unitdef::WeaponRole::DirectFire;
    beam.damage = rm::sim::magFromFloat(10.0f);
    beam.maxRange = rm::test::fx(200.0f);
    beam.rateOfFire = static_cast<float>(fixture.roster.rate.ticksPerSecond());
    beam.beam = true;
    beam.turreted = true;
    turret.weapons.push_back(beam);
    const UnitId attacker =
        fixture.roster.add(fixture.roster.addType(turret), 200.0f, 200.0f, 0, 500.0f);

    rm::unitdef::UnitDef targetDef;
    targetDef.name = "test_target";
    const rm::UnitTypeIndex targetType = fixture.roster.addType(targetDef);
    const UnitId target = fixture.roster.add(targetType, 300.0f, 200.0f, 1, 500.0f);

    rm::unitdef::UnitDef spotterDef;
    spotterDef.name = "test_spotter";
    spotterDef.visionRadiusElmos = 200.0f;
    const UnitId spotter = fixture.roster.add(fixture.roster.addType(spotterDef), 200.0f,
                                              220.0f, 0, 500.0f);

    rm::sim::Intel intel;
    const rm::sim::Fx mapWidth =
        rm::sim::Fx::fromInt(fixture.field.squaresX * rm::kSquareSize);
    const rm::sim::Fx mapDepth =
        rm::sim::Fx::fromInt(fixture.field.squaresZ * rm::kSquareSize);
    intel.configure(2, mapWidth, mapDepth, rm::sim::VisionStyle::ForgedAlliance);

    Command attack = moveOrder(0, 0, attacker, 300.0f, 200.0f);
    attack.kind = CommandKind::Attack;
    attack.target = target;
    CommandLog log;
    REQUIRE(log.record(logged(attack)));
    fixture.run(log, 1, &intel);
    const rm::sim::Mag visibleHealth = fixture.roster.store.health()[target.index].current;
    REQUIRE(visibleHealth < rm::sim::magFromFloat(500.0f));

    fixture.roster.store.health()[spotter.index].current = rm::sim::Mag{};
    // Intel updates before the dead unit is retired, so this tick spends the spotter's final
    // sight and retires it. The next tick must reach the forced-target Seen gate without it.
    fixture.run(CommandLog{}, 1, &intel);
    const rm::sim::Mag retiredSpotterHealth = fixture.roster.store.health()[target.index].current;
    fixture.run(CommandLog{}, 1, &intel);

    CHECK(fixture.roster.store.health()[target.index].current == retiredSpotterHealth);
}
