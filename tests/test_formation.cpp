// Player-perspective coverage for the ported formation geometry (C-178, C-151).
//
// `GrowthFormation`'s slot layout comes from retail `lua/formations.lua`
// (`BlockBuilderLand`, formations.lua:838-913): columns centre-outward, rows one
// step behind the anchor. The engine rotates those local offsets by the wire
// command's formation quaternion (C-151); with no UI to supply one, the intake
// faces the bearing from the group's centroid to the click. These tests pin the
// rotated result, the shared click anchor, and the canonical fan-out order.

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
#include <vector>

using rm::sim::CommandIssue;
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

/// One army, one player, a flat map — the smallest world a group Move can be
/// issued into. The roster fixes every unit's collision radius at 4 elmos, so a
/// formation slot spacing is one 8-elmo diameter.
struct Fixture {
    rm::HeightField field = flatField();
    rm::sim::Terrain terrain{field, false, 0.0f, nullptr, {},
                             rm::sim::PlacementMode::Free};
    rm::sim::PassabilityGrid grid = rm::sim::buildPassability(field, 0.0f);
    rm::test::Roster roster;
    std::vector<rm::sim::Army> armies = rm::sim::freeForAll(1);
    std::vector<Player> players{rm::sim::Player{.index = 0, .army = 0}};
    rm::sim::PathService paths;
    std::vector<rm::sim::Construction> building;
    rm::UnitTypeIndex tank;

    Fixture() {
        rm::unitdef::UnitDef def;
        def.name = "test_tank";
        def.categories = {"LAND", "DIRECTFIRE", "TECH1"};
        tank = roster.addType(def);
    }

    [[nodiscard]] rm::sim::ApplyCommandResult issueMove(
        const std::vector<UnitId>& units, float x, float z) {
        const CommandIssue issue{.tick = 0,
                                 .source = 0,
                                 .id = rm::commandId(0, 0),
                                 .player = 0,
                                 .kind = CommandKind::Move,
                                 .units = units,
                                 .targetX = rm::test::fx(x),
                                 .targetZ = rm::test::fx(z)};
        return rm::sim::applyCommand(
            issue, roster.store, roster.catalog, players, armies, terrain,
            [this](UnitId) { return &grid; }, roster.rate, &building, nullptr,
            nullptr, &paths);
    }
};

} // namespace

TEST_CASE("a group move rotates the formation to face the click", "[formation]") {
    // Four tanks strung along +Z, clicked due east: the centroid-to-click
    // bearing is a quarter turn, so the formation's local axes land exactly on
    // the world axes — local +z (facing) is world +x, local +x (right flank)
    // is world -z. FourWide's front row is the retail centre-outward sequence
    // -0.5, +0.5, -1.5, +1.5 radii (formations.lua:880-893), which is -4, +4,
    // -12, +12 elmos at the roster's fixed 4-elmo radius.
    Fixture fix;
    std::vector<UnitId> group;
    for (int i = 0; i < 4; ++i) {
        group.push_back(fix.roster.add(fix.tank, 200.0f,
                                       200.0f + 10.0f * static_cast<float>(i),
                                       0, 500.0f));
    }
    const rm::sim::ApplyCommandResult result = fix.issueMove(group, 900.0f, 215.0f);
    REQUIRE(result.accepted == group);

    // Every member's local target shares the anchor's x: the front row sits on
    // the line through the click perpendicular to the facing.
    std::vector<float> flank;
    for (const UnitId member : group) {
        const auto& entries = fix.roster.store.orders()[member.index].entries();
        REQUIRE(entries.size() == 1);
        CHECK(rm::test::near(entries.front().targetX()) == 900.0f);
        flank.push_back(rm::test::asFloat(entries.front().targetZ()));
    }
    std::ranges::sort(flank);
    CHECK(flank[0] == Catch::Approx(215.0f - 12.0f).margin(0.01f));
    CHECK(flank[1] == Catch::Approx(215.0f - 4.0f).margin(0.01f));
    CHECK(flank[2] == Catch::Approx(215.0f + 4.0f).margin(0.01f));
    CHECK(flank[3] == Catch::Approx(215.0f + 12.0f).margin(0.01f));
}

TEST_CASE("a group move trails later rows behind the rotated anchor",
          "[formation]") {
    // Eight tanks, same eastward click: FourWide's second row is one collision
    // diameter behind the anchor in local -z, which is world -x under a
    // quarter-turn facing.
    Fixture fix;
    std::vector<UnitId> group;
    for (int i = 0; i < 8; ++i) {
        group.push_back(fix.roster.add(fix.tank, 200.0f,
                                       200.0f + 10.0f * static_cast<float>(i),
                                       0, 500.0f));
    }
    const rm::sim::ApplyCommandResult result = fix.issueMove(group, 900.0f, 235.0f);
    REQUIRE(result.accepted == group);

    std::size_t front = 0;
    std::size_t back = 0;
    for (const UnitId member : group) {
        const auto& entries = fix.roster.store.orders()[member.index].entries();
        REQUIRE(entries.size() == 1);
        const float x = rm::test::asFloat(entries.front().targetX());
        if (x == Catch::Approx(900.0f).margin(0.01f)) {
            ++front;
        } else if (x == Catch::Approx(892.0f).margin(0.01f)) {
            ++back;
        }
    }
    CHECK(front == 4);
    CHECK(back == 4);
}

TEST_CASE("a group move keeps one shared anchor and canonical fan-out order",
          "[formation]") {
    // The immutable shared command still records the click itself — the
    // rotated slots are per-member local targets, not a rewritten anchor — and
    // the accepted list follows canonical UnitId order regardless of the order
    // the issue named the units in.
    Fixture fix;
    const UnitId first = fix.roster.add(fix.tank, 200.0f, 200.0f, 0, 500.0f);
    const UnitId second = fix.roster.add(fix.tank, 200.0f, 220.0f, 0, 500.0f);
    const UnitId third = fix.roster.add(fix.tank, 200.0f, 240.0f, 0, 500.0f);

    const rm::sim::ApplyCommandResult result =
        fix.issueMove({third, first, second}, 900.0f, 220.0f);
    const std::vector<UnitId> canonical{first, second, third};
    CHECK(result.accepted == canonical);

    const rm::sim::QueuedCommand* firstEntry =
        fix.roster.store.orders()[first.index].currentEntry();
    const rm::sim::QueuedCommand* secondEntry =
        fix.roster.store.orders()[second.index].currentEntry();
    const rm::sim::QueuedCommand* thirdEntry =
        fix.roster.store.orders()[third.index].currentEntry();
    REQUIRE(firstEntry != nullptr);
    REQUIRE(secondEntry != nullptr);
    REQUIRE(thirdEntry != nullptr);
    CHECK(&firstEntry->payload() == &secondEntry->payload());
    CHECK(&firstEntry->payload() == &thirdEntry->payload());
    CHECK(firstEntry->payload().targetX == rm::test::fx(900.0f));
    CHECK(firstEntry->payload().targetZ == rm::test::fx(220.0f));
    CHECK(firstEntry->payload().units == canonical);
}

TEST_CASE("a group move fills front slots in category order", "[formation]") {
    // The committed reduction of retail's DFFirst block order (C-178): combat
    // units fill the front row before engineers. Four tanks plus one engineer
    // under an eastward click puts the engineer in the second row — one
    // diameter behind the anchor — while the tanks hold the front line.
    Fixture fix;
    rm::unitdef::UnitDef engDef;
    engDef.name = "test_engineer";
    engDef.categories = {"LAND", "ENGINEER", "CONSTRUCTION", "TECH1"};
    const rm::UnitTypeIndex engineer = fix.roster.addType(engDef);

    std::vector<UnitId> group;
    for (int i = 0; i < 4; ++i) {
        group.push_back(fix.roster.add(fix.tank, 200.0f,
                                       200.0f + 10.0f * static_cast<float>(i),
                                       0, 500.0f));
    }
    const UnitId eng = fix.roster.add(engineer, 200.0f, 260.0f, 0, 500.0f);
    group.push_back(eng);
    const rm::sim::ApplyCommandResult result = fix.issueMove(group, 900.0f, 224.0f);
    REQUIRE(result.accepted == group);

    const auto& engEntries = fix.roster.store.orders()[eng.index].entries();
    REQUIRE(engEntries.size() == 1);
    CHECK(rm::test::near(engEntries.front().targetX()) == 892.0f);
    for (const UnitId member : group) {
        if (member == eng) {
            continue;
        }
        const auto& entries = fix.roster.store.orders()[member.index].entries();
        REQUIRE(entries.size() == 1);
        CHECK(rm::test::near(entries.front().targetX()) == 900.0f);
    }
}
