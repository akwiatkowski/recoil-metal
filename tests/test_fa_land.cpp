// Player-perspective coverage for land claims (see docs/fa-exe-analysis-plan.md).
//
// WP-20/21/26's pathing claims: the per-army service budget (C-174), unit
// reservations never invalidating the path graph (C-177), and GrowthFormation's
// block-width thresholds (C-178). The claims already pinned live in
// test_command.cpp and test_pathfinding.cpp; these cover the implemented but
// unproven remainder.

#include "core/map/HeightField.hpp"
#include "core/sim/Command.hpp"
#include "core/sim/PathService.hpp"
#include "core/sim/Pathfinding.hpp"
#include "core/sim/Skirmish.hpp"
#include "core/sim/Terrain.hpp"
#include "core/sim/UnitStore.hpp"

#include "support/FxMatchers.hpp"
#include "support/TestRoster.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <memory>
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

/// A match with two armies, one unit each, and one player driving each — the
/// same fixture test_command.cpp runs its order cases through.
struct Fixture {
    rm::HeightField field = flatField();
    rm::sim::Terrain terrain{field, false, 0.0f, nullptr, {},
                             rm::sim::PlacementMode::Free};
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
        def.categories = {"LAND"};
        const rm::UnitTypeIndex type = roster.addType(def);
        mine = roster.add(type, 200.0f, 200.0f, 0, 500.0f);
        theirs = roster.add(type, 600.0f, 600.0f, 1, 500.0f);
    }

    std::vector<rm::sim::Construction> building;

    [[nodiscard]] bool apply(const Command& command) {
        return rm::sim::applyCommand(command, roster.store, roster.catalog, players,
                                     armies, terrain, grid, roster.rate, &building,
                                     nullptr, nullptr, &paths);
    }

    /// Runs the match forward, applying whatever the log says on each tick.
    void run(const CommandLog& log, rm::TickIndex ticks) {
        std::vector<rm::sim::Projectile> shots;
        std::vector<rm::sim::Economy> economies(2);
        const std::vector<int> commandersEver(2, 0);

        for (rm::TickIndex tick = nextTick; tick < nextTick + ticks; ++tick) {
            for (const CommandIssue& issue : log.at(tick, CommandPhase::PreTick)) {
                (void)rm::sim::applyCommand(
                    issue, roster.store, roster.catalog, players, armies, terrain,
                    [this](UnitId) { return &grid; }, roster.rate, &building, nullptr,
                    nullptr, &paths);
            }
            const std::vector<const rm::sim::PassabilityGrid*> grids(
                roster.catalog.size(), &grid);
            rm::sim::Match match{.armies = armies,
                                 .economies = economies,
                                 .projectiles = &shots,
                                 .building = &building,
                                 .passability = grids,
                                 .pathService = &paths,
                                 .commandersEver = commandersEver};
            (void)rm::sim::tickSkirmish(roster.store, roster.catalog, match, terrain);
        }
        nextTick += ticks;
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

TEST_CASE("C-174: one army's path work is capped at 1000 steps a beat", "[fa-land]") {
    // The claim's number first: `path_ArmyBudget=1000` is the fixed allowance
    // one army's service may spend per beat.
    CHECK(rm::sim::kPathArmyBudget == 1000);

    // And the budget is real: a route whose field needs more than one beat's
    // allowance does not publish on the first working beat — it resumes where
    // it stopped. A 512-square map is 64x64 cells; a corner-to-corner field
    // settles well over a thousand of them.
    rm::HeightField big;
    big.squaresX = 512;
    big.squaresZ = 512;
    big.baseHeight = 0.0f;
    big.heightScale = 1.0f;
    big.raw.assign(big.sampleCount(), std::uint16_t{0});
    const auto grid =
        std::make_shared<rm::sim::PassabilityGrid>(rm::sim::buildPassability(big, -1000.0f));

    rm::sim::PathService paths;
    paths.enqueue(rm::sim::PathRequest{.unit = UnitId{0, 1},
                                       .command = rm::CommandId{1},
                                       .army = 0,
                                       .fromX = rm::test::fx(32.0f),
                                       .fromZ = rm::test::fx(32.0f),
                                       .targetX = rm::test::fx(4000.0f),
                                       .targetZ = rm::test::fx(4000.0f),
                                       .grid = grid});

    // Beat one admits the request; beat two is the first that spends work.
    CHECK(paths.service().empty());
    CHECK(paths.service().empty());
    REQUIRE(paths.activeFields()[0].has_value());

    // The field is still owed work — if a single beat could settle a
    // corner-to-corner search the budget would be decorative.
    const rm::sim::FlowField* field = paths.activeFields()[0]->field.get();
    CHECK(field->reach(paths.activeFields()[0]->startCell)
          == rm::sim::FlowField::Reach::Pending);

    // Resumed across beats, the same request does publish — eventually, and
    // without re-asking.
    std::vector<rm::sim::PathResult> done;
    for (int i = 0; i < 32 && done.empty(); ++i) {
        done = paths.service();
    }
    REQUIRE(done.size() == 1);
    CHECK_FALSE(done.front().path.empty());
}

TEST_CASE("C-177: a parked unit does not block the route through it", "[fa-land]") {
    // Unit reservations NEVER invalidate the path graph: a mobile unit standing
    // on the corridor is a congestion problem for the mover, not a wall for the
    // router. The published route runs straight through the parked unit's cell;
    // dealing with the body in the way is the congestion pass's job, tested in
    // test_movement.cpp.
    Fixture fix;
    const rm::UnitTypeIndex type = fix.roster.store.typeAt(fix.mine.index);
    const UnitId parked = fix.roster.add(type, 512.0f, 200.0f, 0, 500.0f);
    // Still mobile — `speedPerTick` is what separates a unit from a structure
    // in the blocking layer, and this one keeps its authored speed.
    REQUIRE(fix.roster.motion(parked).speedPerTick > rm::sim::Fx{});

    fix.run(CommandLog{}, 1);  // let the tick's blocking layer see the world
    const int parkedCell = fix.grid.cellAtWorld(rm::test::fx(200.0f)) * fix.grid.cellsX
                           + fix.grid.cellAtWorld(rm::test::fx(512.0f));
    CHECK(rm::sim::blockingCells(fix.roster.store, fix.grid)[
              static_cast<std::size_t>(parkedCell)]
          == 0);

    REQUIRE(fix.apply(moveOrder(0, 0, fix.mine, 900.0f, 200.0f)));
    fix.run(CommandLog{}, 4);

    const std::vector<std::array<rm::sim::Fx, 2>>& path =
        fix.roster.motion(fix.mine).path;
    REQUIRE_FALSE(path.empty());
    // The corridor is one 64-elmo cell row centred on z = 200. A route that
    // treated the parked unit as a wall would leave it; this one does not.
    for (const auto& point : path) {
        CHECK(point[1] > rm::test::fx(200.0f - 64.0f));
        CHECK(point[1] < rm::test::fx(200.0f + 64.0f));
    }
}

TEST_CASE("C-178: GrowthFormation widens its front row at the block thresholds",
          "[fa-land]") {
    // ART-S007's land-block widths: a group of up to twelve fills a four-wide
    // front row; the thirteenth member starts a five-wide one. Slots are
    // assigned once at issue time, so the count standing on the clicked line
    // IS the formation's front row.
    const auto frontRowSize = [](std::size_t units) {
        Fixture fix;
        const rm::UnitTypeIndex type = fix.roster.store.typeAt(fix.mine.index);
        std::vector<UnitId> group{fix.mine};
        for (std::size_t i = 1; i < units; ++i) {
            group.push_back(fix.roster.add(type, 200.0f,
                                           300.0f + 40.0f * static_cast<float>(i), 0,
                                           500.0f));
        }
        const rm::sim::Fx clickX = rm::test::fx(900.0f);
        const rm::sim::Fx clickZ = rm::test::fx(200.0f);
        const CommandIssue issue{.tick = 0,
                                 .source = 0,
                                 .id = rm::commandId(0, 0),
                                 .player = 0,
                                 .kind = CommandKind::Move,
                                 .units = group,
                                 .targetX = clickX,
                                 .targetZ = clickZ};
        const rm::sim::ApplyCommandResult result = rm::sim::applyCommand(
            issue, fix.roster.store, fix.roster.catalog, fix.players, fix.armies,
            fix.terrain, [&fix](UnitId) { return &fix.grid; }, fix.roster.rate,
            &fix.building, nullptr, nullptr, &fix.paths);
        REQUIRE(result.accepted.size() == units);

        std::size_t front = 0;
        for (const UnitId member : group) {
            const auto& entries = fix.roster.store.orders()[member.index].entries();
            REQUIRE(entries.size() == 1);
            if (entries.front().targetZ() == clickZ) {
                ++front;
            }
        }
        return front;
    };

    CHECK(frontRowSize(12) == 4);   // FourWide's ceiling
    CHECK(frontRowSize(13) == 5);   // FiveWide's floor
}
