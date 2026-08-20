// A unit's order list: queueing, replacing, cancelling, and finishing.
//
// TWO LAYERS, tested separately on purpose. `CommandQueue` is pure — no world, no store — so
// the cancel rules can be pinned exactly, which matters because they are Recoil's semantics
// copied rather than invented and "what feels right" is not a specification. Then the wiring:
// three queued moves actually run in order through the tick, which is §7 P4.1's stated test and
// the only thing that can catch the queue being right and unused.
#include <catch2/catch_test_macros.hpp>

#include "core/sim/CommandQueue.hpp"
#include "core/sim/Pathfinding.hpp"
#include "core/sim/Skirmish.hpp"

#include "support/TestRoster.hpp"

#include <array>
#include <vector>

using rm::sim::Command;
using rm::sim::CommandKind;
using rm::sim::CommandQueue;
using rm::sim::UnitId;
using Result = rm::sim::CommandQueue::Result;

namespace {

[[nodiscard]] Command moveTo(float x, float z, UnitId unit = UnitId{}) {
    return Command{.kind = CommandKind::Move,
                   .unit = unit,
                   .targetX = rm::sim::fxFromFloat(x),
                   .targetZ = rm::sim::fxFromFloat(z)};
}

} // namespace

// --- The queue itself ------------------------------------------------------------------

TEST_CASE("a plain order replaces everything") {
    CommandQueue queue;
    CHECK(queue.give(moveTo(100, 0), true) == Result::Appended);
    CHECK(queue.give(moveTo(200, 0), true) == Result::Appended);
    REQUIRE(queue.size() == 2);

    // Recoil clears the whole queue before appending an unshifted order
    // (`CommandAI.cpp:998-1011`), which is why a right-click makes a unit forget its route.
    CHECK(queue.give(moveTo(300, 0), false) == Result::Replaced);
    REQUIRE(queue.size() == 1);
    REQUIRE(queue.current() != nullptr);
    CHECK(*queue.current() == moveTo(300, 0));
}

TEST_CASE("a shift-order appends behind what is already there") {
    CommandQueue queue;
    CHECK(queue.give(moveTo(100, 0), false) == Result::Replaced);
    CHECK(queue.give(moveTo(200, 0), true) == Result::Appended);
    CHECK(queue.give(moveTo(300, 0), true) == Result::Appended);

    // Order preserved, current first.
    const std::vector<Command> all = queue.all();
    REQUIRE(all.size() == 3);
    CHECK(all[0] == moveTo(100, 0));
    CHECK(all[1] == moveTo(200, 0));
    CHECK(all[2] == moveTo(300, 0));
}

TEST_CASE("shift-clicking a queued waypoint takes it away again") {
    // The behaviour players feel in their hands, and the reason this file is not a bare
    // `std::deque`: Recoil's `GetCancelQueued` (`CommandAI.cpp:1312`) makes a matching
    // shift-order a REMOVAL rather than a second copy.
    CommandQueue queue;
    (void)queue.give(moveTo(100, 0), false);
    (void)queue.give(moveTo(200, 0), true);
    (void)queue.give(moveTo(300, 0), true);

    CHECK(queue.give(moveTo(200, 0), true) == Result::Cancelled);
    const std::vector<Command> all = queue.all();
    REQUIRE(all.size() == 2);
    CHECK(all[0] == moveTo(100, 0));
    CHECK(all[1] == moveTo(300, 0));  // the rest of the route closes up behind it
}

TEST_CASE("cancelling the order in progress says so") {
    // Distinguished from an ordinary cancel because the caller has to interrupt the unit as
    // well as forget the order — Recoil pushes a stop to the front for exactly this
    // (`CommandAI.cpp:1044-1049`).
    CommandQueue queue;
    (void)queue.give(moveTo(100, 0), false);
    (void)queue.give(moveTo(200, 0), true);

    CHECK(queue.give(moveTo(100, 0), true) == Result::CancelledCurrent);
    REQUIRE(queue.size() == 1);
    CHECK(*queue.current() == moveTo(200, 0));
}

TEST_CASE("a queue cannot come to hold two orders for the same place") {
    // The cancel rule has a consequence worth pinning, and it is also why Recoil's "only delete
    // one non-build order" (`CommandAI.cpp:1400`) is not directly observable here: `give` is
    // the only way into the queue, and it cancels a match rather than adding one. So a
    // duplicate can never accumulate, and "remove one" and "remove all" are the same act.
    //
    // Recoil's rule is kept anyway, because the moment something that is not a player click
    // can enqueue — a patrol that crosses itself, an internal order, a formation move — the
    // two stop being the same act, and that is a bad time to discover the loop was unbounded.
    CommandQueue queue;
    (void)queue.give(moveTo(100, 0), false);
    (void)queue.give(moveTo(200, 0), true);

    // Trying to add the head again removes it instead.
    CHECK(queue.give(moveTo(100, 0), true) == Result::CancelledCurrent);
    REQUIRE(queue.size() == 1);
    CHECK(*queue.current() == moveTo(200, 0));

    // And once more: still no duplicate, and now nothing left.
    CHECK(queue.give(moveTo(200, 0), true) == Result::CancelledCurrent);
    CHECK(queue.empty());
}

TEST_CASE("near enough counts as the same order") {
    // `kCancelDistance` is Recoil's `COMMAND_CANCEL_DIST = 17` elmos, which is about one unit's
    // own width — the FA corpus's median `SizeX` is 2.3 ogrids, 18.4 elmos. A player clicking
    // twice within a unit's width meant one place.
    CommandQueue queue;
    (void)queue.give(moveTo(100, 100), false);

    CHECK(queue.give(moveTo(110, 100), true) == Result::CancelledCurrent);  // 10 elmos away
    CHECK(queue.empty());

    (void)queue.give(moveTo(100, 100), false);
    CHECK(queue.give(moveTo(100, 130), true) == Result::Appended);  // 30 elmos: a real waypoint
    CHECK(queue.size() == 2);
}

TEST_CASE("different kinds of order at the same place are both kept") {
    CommandQueue queue;
    (void)queue.give(moveTo(100, 0), false);

    Command attack = moveTo(100, 0);
    attack.kind = CommandKind::Attack;
    CHECK(queue.give(attack, true) == Result::Appended);
    CHECK(queue.size() == 2);
}

TEST_CASE("two different buildings on the same spot are a plan, not a duplicate") {
    // Stricter than Recoil, which compares footprints only. Two different structures queued at
    // one place is a sequence a player might mean; the same structure twice is a slip.
    CommandQueue queue;
    Command first{.kind = CommandKind::Build, .buildType = 3};
    Command second{.kind = CommandKind::Build, .buildType = 7};

    (void)queue.give(first, false);
    CHECK(queue.give(second, true) == Result::Appended);
    CHECK(queue.give(first, true) == Result::CancelledCurrent);
}

TEST_CASE("a stop never cancels another stop") {
    // Recoil's predicate needs one or three parameters and a stop has none, so it reaches this
    // answer structurally. It is also the right one: stopping twice is not a request to
    // un-stop.
    const Command stop{.kind = CommandKind::Stop};
    CHECK_FALSE(rm::sim::sameOrder(stop, stop));
}

TEST_CASE("finishing drops the head and hands over the next") {
    CommandQueue queue;
    (void)queue.give(moveTo(100, 0), false);
    (void)queue.give(moveTo(200, 0), true);

    const Command* next = queue.finish();
    REQUIRE(next != nullptr);
    CHECK(*next == moveTo(200, 0));
    CHECK(queue.finish() == nullptr);  // and the queue is empty
    CHECK(queue.empty());
    CHECK(queue.finish() == nullptr);  // finishing an empty queue is not an error
}

// --- The wiring: does a queued route actually run? -------------------------------------

namespace {

/// A flat, wholly walkable map — so a route is a straight line and this is a test about the
/// queue rather than about pathfinding.
[[nodiscard]] rm::HeightField flatField() {
    rm::HeightField field;
    field.squaresX = 64;
    field.squaresZ = 64;
    field.baseHeight = 0.0f;
    field.heightScale = 1.0f;
    field.raw.assign(field.sampleCount(), std::uint16_t{0});
    return field;
}

[[nodiscard]] rm::unitdef::UnitDef walkerDef() {
    rm::unitdef::UnitDef def;
    def.name = "test_walker";
    def.speedElmosPerSecond = 200.0f;  // fast, so the waypoints are reached in few ticks
    def.turnRateRadiansPerSecond = 100.0f;
    return def;
}

} // namespace

TEST_CASE("three queued moves run in order") {
    // §7 P4.1's stated test, end to end through the tick — which is the part the pure cases
    // above cannot reach: a queue that is correct and never consulted would pass all of them.
    const rm::HeightField field = flatField();
    const rm::sim::Terrain terrain{field};
    const rm::sim::PassabilityGrid grid = rm::sim::buildPassability(field, 0.0f, 60.0f, 0.0f);

    rm::test::Roster roster;
    const rm::UnitTypeIndex type = roster.addType(walkerDef());
    const UnitId walker = roster.add(type, 40.0f, 40.0f, 0, 100.0f);
    const std::vector<const rm::sim::PassabilityGrid*> grids{&grid};

    std::vector<rm::sim::Army> armies = rm::sim::freeForAll(1);
    const std::vector<rm::sim::Player> players{rm::sim::Player{.index = 0, .army = 0}};
    std::vector<rm::sim::Construction> building;

    // A leg east, a leg north, a leg west — chosen so each is a different direction and
    // arriving at one cannot be mistaken for arriving at another.
    const std::array<std::array<float, 2>, 3> legs{{{200.0f, 40.0f},
                                                    {200.0f, 200.0f},
                                                    {40.0f, 200.0f}}};

    CHECK(rm::sim::applyCommand(moveTo(legs[0][0], legs[0][1], walker), roster.store,
                                roster.catalog, players, armies, terrain, grid, roster.rate,
                                &building, false));
    for (std::size_t leg = 1; leg < legs.size(); ++leg) {
        CHECK(rm::sim::applyCommand(moveTo(legs[leg][0], legs[leg][1], walker), roster.store,
                                    roster.catalog, players, armies, terrain, grid, roster.rate,
                                    &building, true));
    }
    CHECK(roster.store.orders()[walker.index].size() == 3);

    // Run the ticks and record which leg the unit is heading for as it changes.
    std::vector<std::size_t> legsSeen;
    rm::sim::Match match{.armies = armies,
                         .economies = {},
                         .passability = grids,
                         .commandersEver = {}};
    for (int tick = 0; tick < 400; ++tick) {
        (void)rm::sim::tickSkirmish(roster.store, roster.catalog, match, terrain, roster.rate);
        const CommandQueue& queue = roster.store.orders()[walker.index];
        const std::size_t remaining = queue.size();
        if (legsSeen.empty() || legsSeen.back() != remaining) {
            legsSeen.push_back(remaining);
        }
        if (queue.empty()) {
            break;
        }
    }

    // Three legs, consumed one at a time and in order.
    REQUIRE(legsSeen.size() == 4);
    CHECK(legsSeen[0] == 3);
    CHECK(legsSeen[1] == 2);
    CHECK(legsSeen[2] == 1);
    CHECK(legsSeen[3] == 0);

    // And it finished where the last leg said, not where the first did.
    const rm::sim::Transform& at = roster.store.transforms()[walker.index];
    CHECK(rm::sim::fxToFloat(at.x) < 100.0f);
    CHECK(rm::sim::fxToFloat(at.z) > 150.0f);
}

TEST_CASE("a refused plain order changes nothing at all") {
    // Not even the queue. That is what keeps "a refused order is not part of the match" true —
    // if an unroutable click cleared the route the unit was following, a misclick into the sea
    // would stop a unit dead and there would be no record of why.
    const rm::HeightField field = flatField();
    const rm::sim::Terrain terrain{field};
    // A grid nothing can cross: every route search fails.
    const rm::sim::PassabilityGrid closed =
        rm::sim::buildPassability(field, 1.0e6f, 60.0f, 0.0f);
    const rm::sim::PassabilityGrid open = rm::sim::buildPassability(field, 0.0f, 60.0f, 0.0f);

    rm::test::Roster roster;
    const rm::UnitTypeIndex type = roster.addType(walkerDef());
    const UnitId walker = roster.add(type, 40.0f, 40.0f, 0, 100.0f);
    std::vector<rm::sim::Army> armies = rm::sim::freeForAll(1);
    const std::vector<rm::sim::Player> players{rm::sim::Player{.index = 0, .army = 0}};

    CHECK(rm::sim::applyCommand(moveTo(200.0f, 40.0f, walker), roster.store, roster.catalog,
                                players, armies, terrain, open, roster.rate, nullptr, false));
    REQUIRE(roster.store.orders()[walker.index].size() == 1);

    CHECK_FALSE(rm::sim::applyCommand(moveTo(300.0f, 300.0f, walker), roster.store,
                                      roster.catalog, players, armies, terrain, closed,
                                      roster.rate, nullptr, false));
    // The original order is still there.
    REQUIRE(roster.store.orders()[walker.index].size() == 1);
    CHECK(*roster.store.orders()[walker.index].current() == moveTo(200.0f, 40.0f, walker));
}

TEST_CASE("a recycled slot does not inherit the dead unit's route") {
    // The tombstone rule applied to orders. A corpse keeps its arrays so the death blast can
    // read them, so the queue is cleared when something new moves INTO the slot rather than
    // when the old occupant dies. Without that, the next unit spawned would set off along a
    // route it was never given — the same class of bug `UnitId`'s generation exists to stop.
    const rm::HeightField field = flatField();
    const rm::sim::Terrain terrain{field};
    const rm::sim::PassabilityGrid grid = rm::sim::buildPassability(field, 0.0f, 60.0f, 0.0f);

    rm::test::Roster roster;
    const rm::UnitTypeIndex type = roster.addType(walkerDef());
    const UnitId doomed = roster.add(type, 40.0f, 40.0f, 0, 100.0f);
    std::vector<rm::sim::Army> armies = rm::sim::freeForAll(1);
    const std::vector<rm::sim::Player> players{rm::sim::Player{.index = 0, .army = 0}};

    CHECK(rm::sim::applyCommand(moveTo(200.0f, 40.0f, doomed), roster.store, roster.catalog,
                                players, armies, terrain, grid, roster.rate, nullptr, false));
    CHECK(roster.store.orders()[doomed.index].size() == 1);

    roster.store.kill(doomed);
    const UnitId newcomer = roster.add(type, 40.0f, 40.0f, 0, 100.0f);
    REQUIRE(newcomer.index == doomed.index);  // the slot, reused
    CHECK(roster.store.orders()[newcomer.index].empty());
}
