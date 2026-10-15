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
    rm::UnitTypeIndex ship;
    Fixture() {
        rm::unitdef::UnitDef def;
        def.name = "test_tank";
        def.categories = {"LAND", "DIRECTFIRE", "TECH1"};
        tank = roster.addType(def);
        rm::unitdef::UnitDef hull;
        hull.name = "test_frigate";
        hull.categories = {"MOBILE", "NAVAL"};
        hull.motion = rm::unitdef::MotionType::Water;
        ship = roster.addType(hull);
    }
    std::uint32_t sequence = 0;

    [[nodiscard]] rm::sim::ApplyCommandResult issueMove(
        const std::vector<UnitId>& units, float x, float z,
        CommandKind kind = CommandKind::Move) {
        const CommandIssue issue{.tick = 0,
                                 .source = 0,
                                 .id = rm::commandId(0, ++sequence),
                                 .player = 0,
                                 .kind = kind,
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

TEST_CASE("a grouped attack-move uses AttackFormation block widths", "[formation]") {
    // Retail `AttackFormation` (ART-S007 lua/formations.lua:619-641) routes land units
    // through its own block thresholds, not `GrowthFormation`'s: up to sixteen members
    // fill an eight-wide two-row block, so a nine-tank group attack-move holds eight on
    // the anchor line with exactly one trailing a diameter behind it.
    Fixture fix;
    std::vector<UnitId> group;
    for (int i = 0; i < 9; ++i) {
        group.push_back(fix.roster.add(fix.tank, 200.0f,
                                       200.0f + 10.0f * static_cast<float>(i),
                                       0, 500.0f));
    }
    const rm::sim::ApplyCommandResult result =
        fix.issueMove(group, 900.0f, 240.0f, CommandKind::AttackMove);
    REQUIRE(result.accepted == group);

    std::size_t front = 0;
    std::size_t back = 0;
    for (const UnitId member : group) {
        const auto& entries = fix.roster.store.orders()[member.index].entries();
        REQUIRE(entries.size() == 1);
        CHECK(entries.front().kind() == CommandKind::AttackMove);
        const float x = rm::test::asFloat(entries.front().targetX());
        if (x == Catch::Approx(900.0f).margin(0.01f)) {
            ++front;
        } else if (x == Catch::Approx(892.0f).margin(0.01f)) {
            ++back;
        }
    }
    CHECK(front == 8);
    CHECK(back == 1);
}

TEST_CASE("a grouped naval move uses naval block widths", "[formation]") {
    // Retail `GrowthFormation` (ART-S007 lua/formations.lua:698-723) routes sea
    // units through the naval blocks, not the land ones: up to nine hulls fill
    // a three-wide block (`ThreeNavalGrowthFormation`), so a four-frigate group
    // move holds three on the anchor line with exactly one trailing behind it.
    // The sea call passes `spacing` 1.5 (formations.lua:722) and the naval
    // blocks set `LineBreak = 0.5` (formations.lua:451), so the trailing row
    // sits 1.5 diameters (12 elmos at the roster's fixed 4-elmo radius) behind
    // the anchor — 888, not the land 892.
    Fixture fix;
    std::vector<UnitId> group;
    for (int i = 0; i < 4; ++i) {
        group.push_back(fix.roster.add(fix.ship, 200.0f,
                                       200.0f + 10.0f * static_cast<float>(i),
                                       0, 500.0f));
    }
    const rm::sim::ApplyCommandResult result = fix.issueMove(group, 900.0f, 215.0f);
    REQUIRE(result.accepted == group);

    std::size_t front = 0;
    std::size_t back = 0;
    for (const UnitId member : group) {
        const auto& entries = fix.roster.store.orders()[member.index].entries();
        REQUIRE(entries.size() == 1);
        const float x = rm::test::asFloat(entries.front().targetX());
        if (x == Catch::Approx(900.0f).margin(0.01f)) {
            ++front;
        } else if (x == Catch::Approx(888.0f).margin(0.01f)) {
            ++back;
        }
    }
    CHECK(front == 3);
    CHECK(back == 1);
}

TEST_CASE("a grouped naval attack-move uses naval attack widths", "[formation]") {
    // Retail `AttackFormation` (ART-S007 lua/formations.lua:643-668) routes sea
    // units through the naval attack blocks: up to ten hulls fill a five-wide
    // block (`FiveWideNavalAttackFormation`), so a six-frigate group
    // attack-move holds five on the anchor line with one trailing behind it —
    // where the land `AttackFormation` would hold all six on an eight-wide row.
    // Same 1.5-diameter sea spacing (formations.lua:667-668, `LineBreak = 0.5`
    // at 501): the trailer sits at 888, not the land 892.
    Fixture fix;
    std::vector<UnitId> group;
    for (int i = 0; i < 6; ++i) {
        group.push_back(fix.roster.add(fix.ship, 200.0f,
                                       200.0f + 10.0f * static_cast<float>(i),
                                       0, 500.0f));
    }
    const rm::sim::ApplyCommandResult result =
        fix.issueMove(group, 900.0f, 225.0f, CommandKind::AttackMove);
    REQUIRE(result.accepted == group);

    std::size_t front = 0;
    std::size_t back = 0;
    for (const UnitId member : group) {
        const auto& entries = fix.roster.store.orders()[member.index].entries();
        REQUIRE(entries.size() == 1);
        CHECK(entries.front().kind() == CommandKind::AttackMove);
        const float x = rm::test::asFloat(entries.front().targetX());
        if (x == Catch::Approx(900.0f).margin(0.01f)) {
            ++front;
        } else if (x == Catch::Approx(888.0f).margin(0.01f)) {
            ++back;
        }
    }
    CHECK(front == 5);
    CHECK(back == 1);
}

TEST_CASE("a grouped sub move sails its own growth blocks", "[formation]") {
    // Retail `GrowthFormation` (ART-S007 lua/formations.lua:709-721) sails
    // submerged hulls in their own blocks: up to nine subs fill a three-wide
    // block (`ThreeWideSubGrowthFormation`, formations.lua:534-539), so a
    // four-sub group move holds three on the anchor line with one trailing —
    // where the land `FourWide` block would hold all four abreast. Subs sail at
    // the same 1.5 sea spacing (formations.lua:723, `LineBreak = 0.5` at 535):
    // the trailer sits at 888, not the land 892.
    Fixture fix;
    rm::unitdef::UnitDef subDef;
    subDef.name = "test_sub";
    subDef.categories = {"MOBILE", "NAVAL", "SUBMERSIBLE"};
    subDef.motion = rm::unitdef::MotionType::SurfacingSub;
    const rm::UnitTypeIndex sub = fix.roster.addType(subDef);
    std::vector<UnitId> group;
    for (int i = 0; i < 4; ++i) {
        const UnitId id = fix.roster.add(sub, 200.0f,
                                         200.0f + 10.0f * static_cast<float>(i),
                                         0, 500.0f);
        fix.roster.store.motion()[id.index].submersible = true;
        group.push_back(id);
    }
    const rm::sim::ApplyCommandResult result = fix.issueMove(group, 900.0f, 215.0f);
    REQUIRE(result.accepted == group);

    std::size_t front = 0;
    std::size_t back = 0;
    for (const UnitId member : group) {
        const auto& entries = fix.roster.store.orders()[member.index].entries();
        REQUIRE(entries.size() == 1);
        const float x = rm::test::asFloat(entries.front().targetX());
        if (x == Catch::Approx(900.0f).margin(0.01f)) {
            ++front;
        } else if (x == Catch::Approx(888.0f).margin(0.01f)) {
            ++back;
        }
    }
    CHECK(front == 3);
    CHECK(back == 1);
}

TEST_CASE("a grouped air move fans out instead of piling onto the click",
          "[formation]") {
    // Retail `GrowthFormation` (ART-S007 lua/formations.lua:676-728) routes air
    // units through `BlockBuilderAir`'s staggered chevron, not the land block:
    // a four-bomber group move holds nobody on the click itself, fanning out
    // around the anchor instead of piling onto it. `AttackFormation`
    // (formations.lua:669-671) lays the same chevron, so an air attack-move
    // fans out the same way.
    Fixture fix;
    rm::unitdef::UnitDef bomberDef;
    bomberDef.name = "test_bomber";
    bomberDef.categories = {"AIR", "BOMBER", "TECH1"};
    bomberDef.motion = rm::unitdef::MotionType::Air;
    const rm::UnitTypeIndex bomber = fix.roster.addType(bomberDef);
    for (const CommandKind kind : {CommandKind::Move, CommandKind::AttackMove}) {
        // Fresh hulls per leg: a plain order clears the queue, so reusing the
        // group would cancel the first leg's orders against the second's.
        std::vector<UnitId> group;
        for (int i = 0; i < 4; ++i) {
            const UnitId id = fix.roster.add(bomber, 200.0f,
                                             200.0f + 10.0f * static_cast<float>(i),
                                             0, 500.0f);
            fix.roster.store.motion()[id.index].airborne = true;
            group.push_back(id);
        }
        const rm::sim::ApplyCommandResult result = fix.issueMove(group, 900.0f, 215.0f, kind);
        REQUIRE(result.accepted == group);

        std::size_t onAnchor = 0;
        for (const UnitId member : group) {
            const auto& entries = fix.roster.store.orders()[member.index].entries();
            REQUIRE(entries.size() == 1);
            CHECK(entries.front().kind() == kind);
            const float x = rm::test::asFloat(entries.front().targetX());
            const float z = rm::test::asFloat(entries.front().targetZ());
            if (x == Catch::Approx(900.0f).margin(0.01f)
                && z == Catch::Approx(215.0f).margin(0.01f)) {
                ++onAnchor;
            }
        }
        CHECK(onAnchor == 0);
    }
}

TEST_CASE("a grouped sub attack-move sails its own attack blocks", "[formation]") {
    // Retail `AttackFormation` (ART-S007 lua/formations.lua:654-666) sails
    // submerged hulls in their own blocks too: up to ten subs fill a five-wide
    // block (the `FiveWideSubGrowthFormation` reuse, formations.lua:656), so a
    // six-sub group attack-move holds five on the anchor line with one trailing
    // — where the land `TwoRow` eight-wide block would hold all six abreast.
    // Same 1.5 sub spacing (formations.lua:668, `LineBreak = 0.5` at 564):
    // the trailer sits at 888, not the land 892.
    Fixture fix;
    rm::unitdef::UnitDef subDef;
    subDef.name = "test_sub";
    subDef.categories = {"MOBILE", "NAVAL", "SUBMERSIBLE"};
    subDef.motion = rm::unitdef::MotionType::SurfacingSub;
    const rm::UnitTypeIndex sub = fix.roster.addType(subDef);
    std::vector<UnitId> group;
    for (int i = 0; i < 6; ++i) {
        const UnitId id = fix.roster.add(sub, 200.0f,
                                         200.0f + 10.0f * static_cast<float>(i),
                                         0, 500.0f);
        fix.roster.store.motion()[id.index].submersible = true;
        group.push_back(id);
    }
    const rm::sim::ApplyCommandResult result =
        fix.issueMove(group, 900.0f, 225.0f, CommandKind::AttackMove);
    REQUIRE(result.accepted == group);

    std::size_t front = 0;
    std::size_t back = 0;
    for (const UnitId member : group) {
        const auto& entries = fix.roster.store.orders()[member.index].entries();
        REQUIRE(entries.size() == 1);
        CHECK(entries.front().kind() == CommandKind::AttackMove);
        const float x = rm::test::asFloat(entries.front().targetX());
        if (x == Catch::Approx(900.0f).margin(0.01f)) {
            ++front;
        } else if (x == Catch::Approx(888.0f).margin(0.01f)) {
            ++back;
        }
    }
    CHECK(front == 5);
    CHECK(back == 1);
}

TEST_CASE("a grouped naval move spreads slots at sea spacing", "[formation]") {
    // Retail `GrowthFormation` (ART-S007 lua/formations.lua:722) calls
    // `BlockBuilderLand(seaUnitsList, seaBlock, NavalCategories, 1.5)`, and every
    // naval block sets `LineBreak = 0.5` (formations.lua:451-478) — so columns
    // spread 1.5 slot units and rows advance 1 + 0.5 per step: the trailing row
    // sits 1.5 diameters behind the anchor, not one. The roster's fixed 4-elmo
    // radius makes that 12 elmos behind the click.
    Fixture fix;
    std::vector<UnitId> group;
    for (int i = 0; i < 4; ++i) {
        group.push_back(fix.roster.add(fix.ship, 200.0f,
                                       200.0f + 10.0f * static_cast<float>(i),
                                       0, 500.0f));
    }
    const rm::sim::ApplyCommandResult result = fix.issueMove(group, 900.0f, 215.0f);
    REQUIRE(result.accepted == group);

    std::size_t front = 0;
    std::size_t back = 0;
    for (const UnitId member : group) {
        const auto& entries = fix.roster.store.orders()[member.index].entries();
        REQUIRE(entries.size() == 1);
        const float x = rm::test::asFloat(entries.front().targetX());
        if (x == Catch::Approx(900.0f).margin(0.01f)) {
            ++front;
        } else if (x == Catch::Approx(888.0f).margin(0.01f)) {
            ++back;
        }
    }
    CHECK(front == 3);
    CHECK(back == 1);
}

TEST_CASE("a mixed land-and-sea group fans out per domain", "[formation]") {
    // Retail splits `formationUnits` into land/sea/air lists BEFORE block choice
    // (`GrowthFormation` formations.lua:679-723): nine tanks fill their own
    // FourWide land block (4-wide: 4+4+1) while four frigates fill their own
    // ThreeNaval block (3-wide: 3+1) on the same anchor — seven hulls on the
    // anchor line. One shared 13-count block (5-wide: 5+5+3) would hold only
    // five abreast.
    // Both domains centre on the click's z (tanks 205-285, frigates 230-260,
    // each mean 245) so the shared centroid-to-click facing is exact-east like
    // every other case in this file: front rows land exactly on the anchor line
    // and the count below is exact, not tilt-noise.
    Fixture fix;
    std::vector<UnitId> group;
    for (int i = 0; i < 9; ++i) {
        group.push_back(fix.roster.add(fix.tank, 200.0f,
                                       205.0f + 10.0f * static_cast<float>(i),
                                       0, 500.0f));
    }
    for (int i = 0; i < 4; ++i) {
        group.push_back(fix.roster.add(fix.ship, 200.0f,
                                       230.0f + 10.0f * static_cast<float>(i),
                                       0, 500.0f));
    }
    const rm::sim::ApplyCommandResult result = fix.issueMove(group, 900.0f, 245.0f);
    REQUIRE(result.accepted == group);

    std::size_t front = 0;
    for (const UnitId member : group) {
        const auto& entries = fix.roster.store.orders()[member.index].entries();
        REQUIRE(entries.size() == 1);
        if (rm::test::asFloat(entries.front().targetX())
            == Catch::Approx(900.0f).margin(0.01f)) {
            ++front;
        }
    }
    CHECK(front == 7);
}
