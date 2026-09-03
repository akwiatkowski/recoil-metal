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
#include "core/sim/StateHash.hpp"

#include "support/TestRoster.hpp"

#include <array>
#include <vector>

using rm::sim::Command;
using rm::sim::CommandIssue;
using rm::sim::CommandKind;
using rm::sim::CommandQueue;
using rm::sim::UnitId;
using Result = rm::sim::CommandQueue::Result;

namespace {

[[nodiscard]] Command moveTo(float x, float z, UnitId unit = UnitId{}, bool queued = false) {
    return Command{.kind = CommandKind::Move,
                    .queued = queued,
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
    CHECK(queue.current()->asCommand() == moveTo(300, 0));
}

TEST_CASE("queue clear notifies before reverse removal and inserts the replacement last") {
    struct Seen {
        rm::sim::CommandQueueStatus status;
        rm::CommandId id;
        std::size_t queueSize;
    };
    CommandQueue queue;
    std::vector<Seen> seen;
    queue.setObserver([&seen](const rm::sim::CommandQueueChange& change,
                              const CommandQueue& observed) {
        seen.push_back(Seen{change.status, change.id, observed.size()});
    });
    const auto entry = [](std::uint32_t counter, float x) {
        const auto payload = std::make_shared<const rm::sim::SharedCommand>(
            rm::sim::SharedCommand{
                .source = 0,
                .id = rm::commandId(0, counter),
                .kind = CommandKind::Move,
                .targetX = rm::sim::fxFromFloat(x),
                .originalCount = 1,
                .remainingCount = 1,
            });
        return rm::sim::QueuedCommand{UnitId{0, 1}, payload};
    };
    queue.append(entry(0, 100.0f));
    queue.append(entry(1, 200.0f));
    queue.append(entry(2, 300.0f));
    queue.markCurrentActive();
    seen.clear();

    CHECK(queue.give(entry(3, 400.0f), false) == Result::Replaced);
    REQUIRE(seen.size() == 6);
    CHECK(seen[0].status == rm::sim::CommandQueueStatus::Cleared);
    CHECK(seen[0].queueSize == 3);
    CHECK(seen[1].status == rm::sim::CommandQueueStatus::Removed);
    CHECK(seen[1].id == rm::commandId(0, 2));
    CHECK(seen[2].status == rm::sim::CommandQueueStatus::Removed);
    CHECK(seen[2].id == rm::commandId(0, 1));
    CHECK(seen[3].status == rm::sim::CommandQueueStatus::Aborted);
    CHECK(seen[3].id == rm::commandId(0, 0));
    CHECK(seen[3].queueSize == 1);
    CHECK(seen[4].status == rm::sim::CommandQueueStatus::Removed);
    CHECK(seen[4].id == rm::commandId(0, 0));
    CHECK(seen[4].queueSize == 0);
    CHECK(seen[5].status == rm::sim::CommandQueueStatus::Inserted);
    CHECK(seen[5].id == rm::commandId(0, 3));
    CHECK(seen[5].queueSize == 1);
}

TEST_CASE("only removing the queue head emits aborted") {
    CommandQueue queue;
    std::vector<rm::sim::CommandQueueStatus> statuses;
    queue.setObserver([&statuses](const rm::sim::CommandQueueChange& change,
                                  const CommandQueue&) {
        statuses.push_back(change.status);
    });
    (void)queue.give(moveTo(100, 0), false);
    (void)queue.give(moveTo(200, 0), true);
    statuses.clear();

    CHECK(queue.give(moveTo(200, 0), true) == Result::Cancelled);
    CHECK(statuses == std::vector{rm::sim::CommandQueueStatus::Removed});
    statuses.clear();
    CHECK(queue.give(moveTo(100, 0), true) == Result::CancelledCurrent);
    CHECK(statuses == std::vector{rm::sim::CommandQueueStatus::Aborted,
                                  rm::sim::CommandQueueStatus::Removed});
}

TEST_CASE("exact rotation moves only the named shared command and keeps the active head") {
    const auto payload = [](std::uint32_t counter) {
        return std::make_shared<const rm::sim::SharedCommand>(rm::sim::SharedCommand{
            .source = 0,
            .id = rm::commandId(0, counter),
            .kind = CommandKind::Build,
            .buildType = 7,
            .creationSerial = counter,
            .originalCount = 1,
            .remainingCount = 1,
        });
    };
    const auto first = payload(1);
    const auto selected = payload(2);
    const auto equalNeighbor = payload(3);
    CommandQueue queue;
    queue.append(rm::sim::QueuedCommand{UnitId{0, 1}, first});
    queue.append(rm::sim::QueuedCommand{UnitId{0, 1}, selected});
    queue.append(rm::sim::QueuedCommand{UnitId{0, 1}, equalNeighbor});
    queue.markCurrentActive();

    REQUIRE(queue.cycleExact(selected.get()) == 1);
    REQUIRE(queue.entries().size() == 3);
    CHECK(queue.entries()[0].payload().id == first->id);
    CHECK(queue.entries()[1].payload().id == equalNeighbor->id);
    CHECK(queue.entries()[2].payload().id == selected->id);
    REQUIRE(queue.active() != nullptr);
    CHECK(queue.active()->payload().id == first->id);
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
    CHECK(queue.current()->asCommand() == moveTo(200, 0));
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
    CHECK(queue.current()->asCommand() == moveTo(200, 0));

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

    const rm::sim::QueuedCommand* next = queue.finish();
    REQUIRE(next != nullptr);
    CHECK(next->asCommand() == moveTo(200, 0));
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
    def.categories = {"LAND"};
    def.speedElmosPerSecond = 200.0f;  // fast, so the waypoints are reached in few ticks
    def.turnRateRadiansPerSecond = 100.0f;
    return def;
}

[[nodiscard]] rm::unitdef::UnitDef fighterDef(float minimumRange = 0.0f) {
    rm::unitdef::UnitDef def = walkerDef();
    def.visionRadiusElmos = 140.0f;
    rm::unitdef::Weapon weapon;
    weapon.label = "test gun";
    weapon.role = rm::unitdef::WeaponRole::DirectFire;
    weapon.targetPriorities = {{"LAND"}};
    weapon.damage = rm::sim::magFromFloat(10.0f);
    weapon.maxRange = rm::sim::fxFromFloat(100.0f);
    weapon.minRange = rm::sim::fxFromFloat(minimumRange);
    weapon.rateOfFire = 1.0f;
    weapon.muzzleVelocityElmosPerSecond = 100.0f;
    def.weapons.push_back(weapon);
    return def;
}

} // namespace

TEST_CASE("accepted commands consume one match-global creation serial") {
    const rm::HeightField field = flatField();
    const rm::sim::Terrain terrain{field};
    const rm::sim::PassabilityGrid grid =
        rm::sim::buildPassability(field, 0.0f, 60.0f, 0.0f);

    rm::test::Roster roster;
    const rm::UnitTypeIndex type = roster.addType(walkerDef());
    const UnitId first = roster.add(type, 40.0f, 40.0f, 0, 100.0f);
    const UnitId second = roster.add(type, 40.0f, 80.0f, 0, 100.0f);
    const std::vector<rm::sim::Army> armies = rm::sim::freeForAll(1);
    const std::vector<rm::sim::Player> players{rm::sim::Player{.index = 0, .army = 0}};
    const auto apply = [&](const Command& command) {
        return rm::sim::applyCommand(command, roster.store, roster.catalog, players, armies,
                                     terrain, grid, roster.rate);
    };

    Command rejected = moveTo(200.0f, 40.0f, UnitId{99, 1});
    rejected.tick = 7;
    CHECK_FALSE(apply(rejected));
    CHECK(roster.store.nextCommandSerial() == 0);

    Command firstMove = moveTo(200.0f, 40.0f, first);
    firstMove.tick = 7;
    REQUIRE(apply(firstMove));
    REQUIRE(roster.store.orders()[first.index].current() != nullptr);
    CHECK(roster.store.orders()[first.index].currentEntry()->payload().creationSerial == 0);

    Command secondMove = moveTo(200.0f, 80.0f, second);
    secondMove.tick = 7;
    REQUIRE(apply(secondMove));
    REQUIRE(roster.store.orders()[second.index].current() != nullptr);
    CHECK(roster.store.orders()[second.index].currentEntry()->payload().creationSerial == 1);

    Command queued = moveTo(300.0f, 80.0f, second, true);
    queued.tick = 7;
    REQUIRE(apply(queued));
    REQUIRE(roster.store.orders()[second.index].entries().size() == 2);
    CHECK(roster.store.orders()[second.index].entries()[1].payload().creationSerial == 2);

    // The second click creates command serial 3 even though cancellation means that command
    // never enters the queue. Accepted command creation and queue retention are separate facts.
    REQUIRE(apply(queued));
    CHECK(roster.store.nextCommandSerial() == 4);
    REQUIRE(roster.store.orders()[second.index].current() != nullptr);
    CHECK(roster.store.orders()[second.index].currentEntry()->payload().creationSerial == 1);

    Command stop{.tick = 7, .kind = CommandKind::Stop, .unit = first};
    REQUIRE(apply(stop));
    CHECK(roster.store.nextCommandSerial() == 5);
    CHECK(roster.store.orders()[first.index].empty());

    roster.store.kill(first);
    const UnitId replacement = roster.add(type, 40.0f, 40.0f, 0, 100.0f);
    REQUIRE(replacement.index == first.index);
    Command replacementMove = moveTo(240.0f, 40.0f, replacement);
    replacementMove.tick = 7;
    REQUIRE(apply(replacementMove));
    REQUIRE(roster.store.orders()[replacement.index].current() != nullptr);
    CHECK(roster.store.orders()[replacement.index].currentEntry()->payload().creationSerial == 5);
    CHECK(roster.store.nextCommandSerial() == 6);
}

TEST_CASE("one grouped issue shares immutable intent and keeps execution local") {
    const rm::HeightField field = flatField();
    const rm::sim::Terrain terrain{field};
    const rm::sim::PassabilityGrid grid =
        rm::sim::buildPassability(field, 0.0f, 60.0f, 0.0f);

    rm::test::Roster roster;
    const rm::UnitTypeIndex type = roster.addType(walkerDef());
    const UnitId first = roster.add(type, 40.0f, 40.0f, 0, 100.0f);
    const UnitId second = roster.add(type, 40.0f, 80.0f, 0, 100.0f);
    const std::vector<rm::sim::Army> armies = rm::sim::freeForAll(1);
    const std::vector<rm::sim::Player> players{rm::sim::Player{.index = 0, .army = 0}};

    const CommandIssue issue{
        .tick = 7,
        .source = 0,
        .id = rm::commandId(0, 7),
        .player = 0,
        .kind = CommandKind::AttackMove,
        .units = {second, first, second},
        .targetX = rm::sim::fxFromFloat(300.0f),
        .targetZ = rm::sim::fxFromFloat(300.0f),
    };
    const rm::sim::ApplyCommandResult result = rm::sim::applyCommand(
        issue, roster.store, roster.catalog, players, armies, terrain,
        [&grid](UnitId) { return &grid; }, roster.rate);

    const std::vector<UnitId> canonical{first, second};
    CHECK(result.accepted == canonical);
    CHECK(roster.store.nextCommandSerial() == 1);

    const rm::sim::QueuedCommand* firstEntry =
        roster.store.orders()[first.index].currentEntry();
    const rm::sim::QueuedCommand* secondEntry =
        roster.store.orders()[second.index].currentEntry();
    REQUIRE(firstEntry != nullptr);
    REQUIRE(secondEntry != nullptr);
    CHECK(&firstEntry->payload() == &secondEntry->payload());
    CHECK(firstEntry->payload().units == canonical);
    CHECK(firstEntry->payload().creationSerial == 0);
    CHECK(firstEntry->unit() == first);
    CHECK(secondEntry->unit() == second);

    roster.store.orders()[first.index].currentEntryMutable()->setTarget(UnitId{99, 1});
    roster.store.orders()[first.index].currentEntryMutable()->setTargetPosition(
        rm::sim::fxFromFloat(111.0f), rm::sim::fxFromFloat(222.0f));
    CHECK(firstEntry->target() == UnitId{99, 1});
    CHECK(secondEntry->target() == UnitId{});
    CHECK(firstEntry->targetX() == rm::sim::fxFromFloat(111.0f));
    CHECK(secondEntry->targetX() == issue.targetX);
    CHECK(firstEntry->payload().target == UnitId{});
}

TEST_CASE("a grouped issue skips refused members without putting them in shared state") {
    const rm::HeightField field = flatField();
    const rm::sim::Terrain terrain{field};
    const rm::sim::PassabilityGrid grid =
        rm::sim::buildPassability(field, 0.0f, 60.0f, 0.0f);

    rm::test::Roster roster;
    const rm::UnitTypeIndex type = roster.addType(walkerDef());
    const UnitId accepted = roster.add(type, 40.0f, 40.0f, 0, 100.0f);
    const UnitId unauthorized = roster.add(type, 80.0f, 40.0f, 1, 100.0f);
    const UnitId dead = roster.add(type, 120.0f, 40.0f, 0, 100.0f);
    roster.store.kill(dead);
    const std::vector<rm::sim::Army> armies = rm::sim::freeForAll(2);
    const std::vector<rm::sim::Player> players{rm::sim::Player{.index = 0, .army = 0}};

    const auto apply = [&](std::vector<UnitId> units, rm::CommandId id) {
        return rm::sim::applyCommand(
            CommandIssue{
                .source = 0,
                .id = id,
                .player = 0,
                .kind = CommandKind::Move,
                .units = std::move(units),
                .targetX = rm::sim::fxFromFloat(300.0f),
                .targetZ = rm::sim::fxFromFloat(300.0f),
            },
            roster.store, roster.catalog, players, armies, terrain,
            [&](UnitId unit) {
                // Dead and unauthorized handles are rejected before a resolver may index them.
                CHECK(roster.store.alive(unit));
                CHECK(unit == accepted);
                return &grid;
            },
            roster.rate);
    };

    const rm::sim::ApplyCommandResult result =
        apply({unauthorized, accepted, dead, accepted}, rm::commandId(0, 1));
    CHECK(result.accepted == std::vector{accepted});
    CHECK(roster.store.nextCommandSerial() == 1);
    REQUIRE(roster.store.orders()[accepted.index].size() == 1);
    CHECK(roster.store.orders()[unauthorized.index].empty());
    const rm::sim::QueuedCommand* entry = roster.store.orders()[accepted.index].current();
    REQUIRE(entry != nullptr);
    CHECK(entry->payload().units == std::vector{accepted});

    CHECK_FALSE(apply({unauthorized, dead}, rm::commandId(0, 2)));
    CHECK(roster.store.nextCommandSerial() == 1);
}

TEST_CASE("source-tagged command IDs are unique only while a queue owns them") {
    const rm::HeightField field = flatField();
    const rm::sim::Terrain terrain{field};
    const rm::sim::PassabilityGrid grid =
        rm::sim::buildPassability(field, 0.0f, 60.0f, 0.0f);

    rm::test::Roster roster;
    const rm::UnitTypeIndex type = roster.addType(walkerDef());
    const UnitId first = roster.add(type, 40.0f, 40.0f, 0, 100.0f);
    const UnitId second = roster.add(type, 40.0f, 80.0f, 1, 100.0f);
    const std::vector<rm::sim::Army> armies = rm::sim::freeForAll(2);
    const std::vector<rm::sim::Player> players{
        rm::sim::Player{.index = 0, .army = 0},
        rm::sim::Player{.index = 1, .army = 1},
    };
    const auto apply = [&](CommandIssue issue) {
        return rm::sim::applyCommand(issue, roster.store, roster.catalog, players, armies,
                                     terrain, [&grid](UnitId) { return &grid; }, roster.rate);
    };

    CommandIssue firstIssue{
        .source = 0,
        .id = rm::commandId(0, 7),
        .player = 0,
        .kind = CommandKind::Move,
        .units = {first},
        .targetX = rm::sim::fxFromFloat(300.0f),
        .targetZ = rm::sim::fxFromFloat(40.0f),
    };
    REQUIRE(apply(firstIssue));
    CHECK(roster.store.commandIdLive(firstIssue.id));

    // The same live ID is rejected before it can replace or append anything.
    firstIssue.targetZ = rm::sim::fxFromFloat(300.0f);
    CHECK_FALSE(apply(firstIssue));
    CHECK(roster.store.orders()[first.index].size() == 1);

    CommandIssue mismatched = firstIssue;
    mismatched.id = rm::commandId(1, 7);
    CHECK_FALSE(apply(mismatched));
    CommandIssue unknown = firstIssue;
    unknown.source = 2;
    unknown.id = rm::commandId(2, 7);
    unknown.player = 2;
    CHECK_FALSE(apply(unknown));

    // The same local counter under another registered source is a different ID.
    const CommandIssue secondIssue{
        .source = 1,
        .id = rm::commandId(1, 7),
        .player = 1,
        .kind = CommandKind::Move,
        .units = {second},
        .targetX = rm::sim::fxFromFloat(300.0f),
        .targetZ = rm::sim::fxFromFloat(80.0f),
    };
    REQUIRE(apply(secondIssue));
    CHECK(roster.store.commandIdLive(secondIssue.id));

    roster.store.orders()[first.index].clear();
    CHECK_FALSE(roster.store.commandIdLive(firstIssue.id));
    firstIssue.targetZ = rm::sim::fxFromFloat(200.0f);
    REQUIRE(apply(firstIssue));  // reuse after final release

    roster.store.kill(first);
    CHECK(roster.store.orders()[first.index].empty());
    CHECK_FALSE(roster.store.commandIdLive(firstIssue.id));
}

TEST_CASE("count exhaustion removes one exact shared command from every member queue") {
    const rm::HeightField field = flatField();
    const rm::sim::Terrain terrain{field};
    const rm::sim::PassabilityGrid grid =
        rm::sim::buildPassability(field, 0.0f, 60.0f, 0.0f);

    rm::test::Roster roster;
    const rm::UnitTypeIndex type = roster.addType(walkerDef());
    const UnitId first = roster.add(type, 40.0f, 40.0f, 0, 100.0f);
    const UnitId second = roster.add(type, 40.0f, 80.0f, 0, 100.0f);
    const std::vector<rm::sim::Army> armies = rm::sim::freeForAll(1);
    const std::vector<rm::sim::Player> players{rm::sim::Player{.index = 0, .army = 0}};
    const auto apply = [&](const CommandIssue& issue) {
        return rm::sim::applyCommand(issue, roster.store, roster.catalog, players, armies,
                                     terrain, [&grid](UnitId) { return &grid; }, roster.rate);
    };

    const CommandIssue shared{
        .source = 0,
        .id = rm::commandId(0, 41),
        .player = 0,
        .kind = CommandKind::Move,
        .units = {second, first},
        .targetX = rm::sim::fxFromFloat(200.0f),
        .targetZ = rm::sim::fxFromFloat(200.0f),
        .count = 2,
    };
    const CommandIssue follower{
        .source = 0,
        .id = rm::commandId(0, 42),
        .player = 0,
        .kind = CommandKind::Move,
        .queued = true,
        .units = {first, second},
        .targetX = rm::sim::fxFromFloat(300.0f),
        .targetZ = rm::sim::fxFromFloat(300.0f),
    };
    REQUIRE(apply(shared));
    REQUIRE(apply(follower));
    REQUIRE(roster.store.orders()[first.index].size() == 2);
    REQUIRE(roster.store.orders()[second.index].size() == 2);

    {
        const std::shared_ptr<rm::sim::SharedCommand> command =
            roster.store.liveCommand(shared.id);
        REQUIRE(command != nullptr);
        CHECK(command->originalCount == 2);
        CHECK(command->remainingCount == 2);
    }
    REQUIRE(roster.store.decreaseCommandCount(shared.id));
    CHECK(roster.store.liveCommand(shared.id)->remainingCount == 1);
    CHECK(roster.store.orders()[first.index].size() == 2);

    REQUIRE(roster.store.decreaseCommandCount(shared.id));
    CHECK_FALSE(roster.store.commandIdLive(shared.id));
    for (const UnitId unit : std::array{first, second}) {
        REQUIRE(roster.store.orders()[unit.index].size() == 1);
        const rm::sim::QueuedCommand* remaining = roster.store.orders()[unit.index].current();
        REQUIRE(remaining != nullptr);
        CHECK(remaining->payload().id == follower.id);
        CHECK(remaining->payload().targetX == follower.targetX);
    }
}

TEST_CASE("command intake orders sources before preserving FIFO within a source") {
    const rm::HeightField field = flatField();
    const rm::sim::Terrain terrain{field};
    const rm::sim::PassabilityGrid grid =
        rm::sim::buildPassability(field, 0.0f, 60.0f, 0.0f);

    rm::test::Roster roster;
    const rm::UnitTypeIndex type = roster.addType(walkerDef());
    const UnitId unit = roster.add(type, 40.0f, 40.0f, 0, 100.0f);
    const std::vector<rm::sim::Army> armies = rm::sim::freeForAll(1);
    const std::vector<rm::sim::Player> players{
        rm::sim::Player{.index = 0, .army = 0},
        rm::sim::Player{.index = 1, .army = 0},
    };
    rm::sim::CommandBuffer intake;
    const auto submitMove = [&](rm::CommandSource source, float x,
                                rm::sim::CommandPhase phase = rm::sim::CommandPhase::PreTick) {
        return intake.submit(
            CommandIssue{
                .tick = 9,
                .phase = phase,
                .source = source,
                .player = static_cast<rm::PlayerIndex>(source),
                .kind = CommandKind::Move,
                .units = {unit},
                .targetX = rm::sim::fxFromFloat(x),
                .targetZ = rm::sim::fxFromFloat(40.0f),
            },
            roster.store);
    };

    REQUIRE(submitMove(1, 100.0f));
    REQUIRE(submitMove(1, 300.0f));
    REQUIRE(submitMove(0, 200.0f));
    REQUIRE(submitMove(0, 400.0f, rm::sim::CommandPhase::PostSpawn));

    const std::vector<CommandIssue> pre =
        intake.take(9, rm::sim::CommandPhase::PreTick);
    REQUIRE(pre.size() == 3);
    CHECK(pre[0].id == rm::commandId(0, 0));
    CHECK(pre[1].id == rm::commandId(1, 0));
    CHECK(pre[2].id == rm::commandId(1, 1));
    CHECK(intake.size() == 1);

    for (const CommandIssue& issue : pre) {
        REQUIRE(rm::sim::applyCommand(issue, roster.store, roster.catalog, players, armies,
                                      terrain, [&grid](UnitId) { return &grid; }, roster.rate));
    }
    REQUIRE(roster.store.orders()[unit.index].current() != nullptr);
    CHECK(roster.store.orders()[unit.index].current()->payload().id
          == rm::commandId(1, 1));
    CHECK(roster.store.orders()[unit.index].current()->targetX()
          == rm::sim::fxFromFloat(300.0f));

    const std::vector<CommandIssue> post =
        intake.take(9, rm::sim::CommandPhase::PostSpawn);
    REQUIRE(post.size() == 1);
    CHECK(post[0].targetX == rm::sim::fxFromFloat(400.0f));
    CHECK(intake.empty());
}

TEST_CASE("replay preserves an ID consumed by an issue that no unit accepted") {
    const rm::HeightField field = flatField();
    const rm::sim::Terrain terrain{field};
    const rm::sim::PassabilityGrid grid =
        rm::sim::buildPassability(field, 0.0f, 60.0f, 0.0f);
    rm::test::Roster live;
    const rm::UnitTypeIndex type = live.addType(walkerDef());
    const UnitId dead = live.add(type, 40.0f, 40.0f, 0, 100.0f);
    live.store.kill(dead);
    const std::vector<rm::sim::Army> armies = rm::sim::freeForAll(1);
    const std::vector<rm::sim::Player> players{rm::sim::Player{.index = 0, .army = 0}};
    rm::sim::CommandBuffer input;
    REQUIRE(input.submit(CommandIssue{
                             .tick = 3,
                             .source = 0,
                             .player = 0,
                             .kind = CommandKind::Stop,
                             .units = {dead},
                         },
                         live.store)
            == rm::commandId(0, 0));
    std::vector<CommandIssue> due = input.take(3, rm::sim::CommandPhase::PreTick);
    REQUIRE(due.size() == 1);
    const rm::sim::ApplyCommandResult result = rm::sim::applyCommand(
        due.front(), live.store, live.catalog, players, armies, terrain,
        [&grid](UnitId) { return &grid; }, live.rate);
    REQUIRE(result.accepted.empty());

    due.front().units = result.accepted;
    rm::sim::CommandLog log;
    REQUIRE(log.record(due.front()));
    REQUIRE(log.all().front().units.empty());

    rm::test::Roster replay;
    rm::sim::CommandBuffer replayInput;
    REQUIRE(replayInput.submit(log.all().front(), replay.store) == rm::commandId(0, 0));
    CHECK(replay.store.nextCommandCounter(0) == 1);
    const std::vector<CommandIssue> replayed =
        replayInput.take(3, rm::sim::CommandPhase::PreTick);
    REQUIRE(replayed.size() == 1);
    CHECK(replayed.front().units.empty());
}

TEST_CASE("a grouped first patrol stores each origin outside the shared payload") {
    const rm::HeightField field = flatField();
    const rm::sim::Terrain terrain{field};
    const rm::sim::PassabilityGrid grid =
        rm::sim::buildPassability(field, 0.0f, 60.0f, 0.0f);

    rm::test::Roster roster;
    const rm::UnitTypeIndex type = roster.addType(walkerDef());
    const UnitId first = roster.add(type, 40.0f, 40.0f, 0, 100.0f);
    const UnitId second = roster.add(type, 80.0f, 80.0f, 0, 100.0f);
    const std::vector<rm::sim::Army> armies = rm::sim::freeForAll(1);
    const std::vector<rm::sim::Player> players{rm::sim::Player{.index = 0, .army = 0}};

    const CommandIssue issue{
        .source = 0,
        .id = rm::commandId(0, 1),
        .kind = CommandKind::Patrol,
        .units = {second, first},
        .targetX = rm::sim::fxFromFloat(300.0f),
        .targetZ = rm::sim::fxFromFloat(300.0f),
    };
    REQUIRE(rm::sim::applyCommand(issue, roster.store, roster.catalog, players, armies, terrain,
                                  [&grid](UnitId) { return &grid; }, roster.rate));

    REQUIRE(roster.store.orders()[first.index].size() == 1);
    REQUIRE(roster.store.orders()[second.index].size() == 1);
    CHECK(roster.store.nextCommandSerial() == 1);
    const rm::sim::QueuedCommand* firstEntry =
        roster.store.orders()[first.index].currentEntry();
    const rm::sim::QueuedCommand* secondEntry =
        roster.store.orders()[second.index].currentEntry();
    REQUIRE(firstEntry != nullptr);
    REQUIRE(secondEntry != nullptr);
    CHECK(&firstEntry->payload() == &secondEntry->payload());
    CHECK(firstEntry->patrolOrigin()
          == std::optional{std::array{rm::sim::fxFromFloat(40.0f),
                                     rm::sim::fxFromFloat(40.0f)}});
    CHECK(secondEntry->patrolOrigin()
          == std::optional{std::array{rm::sim::fxFromFloat(80.0f),
                                     rm::sim::fxFromFloat(80.0f)}});
}

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
                                &building));
    for (std::size_t leg = 1; leg < legs.size(); ++leg) {
        CHECK(rm::sim::applyCommand(moveTo(legs[leg][0], legs[leg][1], walker, true), roster.store,
                                    roster.catalog, players, armies, terrain, grid, roster.rate,
                                    &building));
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

TEST_CASE("a queued command on an idle unit starts in the dispatch stage") {
    const rm::HeightField field = flatField();
    const rm::sim::Terrain terrain{field};
    const rm::sim::PassabilityGrid grid =
        rm::sim::buildPassability(field, 0.0f, 60.0f, 0.0f);

    rm::test::Roster roster;
    const rm::UnitTypeIndex type = roster.addType(walkerDef());
    const UnitId walker = roster.add(type, 40.0f, 40.0f, 0, 100.0f);
    const std::vector<rm::sim::Army> armies = rm::sim::freeForAll(1);
    const std::vector<rm::sim::Player> players{rm::sim::Player{.index = 0, .army = 0}};
    const std::vector<const rm::sim::PassabilityGrid*> grids{&grid};

    REQUIRE(rm::sim::applyCommand(moveTo(300.0f, 40.0f, walker, true), roster.store,
                                  roster.catalog, players, armies, terrain, grid, roster.rate));
    CHECK_FALSE(roster.store.motion()[walker.index].moving);
    CHECK(roster.store.orders()[walker.index].active() == nullptr);

    CHECK(rm::sim::advanceOrders(roster.store, roster.catalog, terrain, grids, roster.rate) == 1);
    REQUIRE(roster.store.orders()[walker.index].size() == 1);
    REQUIRE(roster.store.orders()[walker.index].active() != nullptr);
    CHECK(roster.store.motion()[walker.index].moving);
}

TEST_CASE("replacement kind controls movement teardown between clear and insert") {
    const rm::HeightField field = flatField();
    const rm::sim::Terrain terrain{field};
    const rm::sim::PassabilityGrid grid =
        rm::sim::buildPassability(field, 0.0f, 60.0f, 0.0f);
    rm::test::Roster roster;
    rm::unitdef::UnitDef engineerDef = walkerDef();
    engineerDef.name = "engineer";
    engineerDef.buildRate = 10.0f;
    engineerDef.buildableCategory = {{"STRUCTURE"}};
    const rm::UnitTypeIndex engineerType = roster.addType(engineerDef);
    rm::unitdef::UnitDef structureDef;
    structureDef.name = "structure";
    structureDef.categories = {"STRUCTURE"};
    structureDef.buildTime = rm::sim::magFromFloat(100.0f);
    const rm::UnitTypeIndex structureType = roster.addType(structureDef);
    const UnitId engineer = roster.add(engineerType, 40.0f, 40.0f, 0, 100.0f);
    const std::vector<rm::sim::Army> armies = rm::sim::freeForAll(1);
    const std::vector<rm::sim::Player> players{rm::sim::Player{.index = 0, .army = 0}};
    std::vector<rm::sim::Construction> building;

    REQUIRE(rm::sim::applyCommand(moveTo(100.0f, 40.0f, engineer), roster.store,
                                  roster.catalog, players, armies, terrain, grid, roster.rate,
                                  &building));
    REQUIRE(roster.store.motion()[engineer.index].moving);

    struct Seen {
        rm::sim::CommandQueueStatus status;
        bool moving;
    };
    std::vector<Seen> seen;
    roster.store.orders()[engineer.index].setObserver(
        [&seen, &roster, engineer](const rm::sim::CommandQueueChange& change,
                                   const CommandQueue&) {
            if (change.status == rm::sim::CommandQueueStatus::Cleared
                || change.status == rm::sim::CommandQueueStatus::Inserted) {
                seen.push_back(Seen{change.status,
                                    roster.store.motion()[engineer.index].moving});
            }
        });

    REQUIRE(rm::sim::applyCommand(moveTo(200.0f, 40.0f, engineer), roster.store,
                                  roster.catalog, players, armies, terrain, grid, roster.rate,
                                  &building));
    REQUIRE(seen.size() == 2);
    CHECK(seen[0].status == rm::sim::CommandQueueStatus::Cleared);
    CHECK(seen[0].moving);
    CHECK(seen[1].status == rm::sim::CommandQueueStatus::Inserted);
    CHECK_FALSE(seen[1].moving);  // Move is not in C-212's keep set.
    CHECK(roster.store.motion()[engineer.index].moving);  // replacement route now published

    seen.clear();
    Command build{
        .player = 0,
        .kind = CommandKind::Build,
        .unit = engineer,
        .targetX = rm::sim::fxFromFloat(300.0f),
        .targetZ = rm::sim::fxFromFloat(300.0f),
        .buildType = structureType,
    };
    REQUIRE(rm::sim::applyCommand(build, roster.store, roster.catalog, players, armies, terrain,
                                  grid, roster.rate, &building));
    REQUIRE(seen.size() == 2);
    CHECK(seen[0].status == rm::sim::CommandQueueStatus::Cleared);
    CHECK(seen[0].moving);
    CHECK(seen[1].status == rm::sim::CommandQueueStatus::Inserted);
    CHECK(seen[1].moving);  // BuildMobile preserves the outgoing move during insertion.
    CHECK_FALSE(roster.store.motion()[engineer.index].moving);  // construction then takes over
}

TEST_CASE("ordinary unstartable commands cascade within one dispatch beat") {
    const rm::HeightField field = flatField();
    const rm::sim::Terrain terrain{field};
    const rm::sim::PassabilityGrid grid =
        rm::sim::buildPassability(field, 0.0f, 60.0f, 0.0f);

    rm::test::Roster roster;
    const rm::UnitTypeIndex type = roster.addType(walkerDef());
    const UnitId walker = roster.add(type, 40.0f, 40.0f, 0, 100.0f);
    const std::vector<rm::sim::Army> armies = rm::sim::freeForAll(1);
    const std::vector<rm::sim::Player> players{rm::sim::Player{.index = 0, .army = 0}};
    const std::vector<const rm::sim::PassabilityGrid*> grids{&grid};

    Command firstDead = moveTo(100.0f, 40.0f, walker, true);
    firstDead.kind = CommandKind::Attack;
    firstDead.target = UnitId{99, 1};
    REQUIRE(rm::sim::applyCommand(firstDead, roster.store, roster.catalog, players, armies,
                                  terrain, grid, roster.rate));

    Command secondDead = firstDead;
    secondDead.target = UnitId{98, 1};
    REQUIRE(rm::sim::applyCommand(secondDead, roster.store, roster.catalog, players, armies,
                                  terrain, grid, roster.rate));
    REQUIRE(rm::sim::applyCommand(moveTo(300.0f, 40.0f, walker, true), roster.store,
                                  roster.catalog, players, armies, terrain, grid, roster.rate));
    REQUIRE(roster.store.orders()[walker.index].size() == 3);

    CHECK(rm::sim::advanceOrders(roster.store, roster.catalog, terrain, grids, roster.rate) == 1);
    const std::vector<Command> orders = roster.store.orders()[walker.index].all();
    REQUIRE(orders.size() == 1);
    CHECK(orders[0].kind == CommandKind::Move);
    CHECK(roster.store.motion()[walker.index].moving);
}

TEST_CASE("an attack-position cycles only while another command follows") {
    const rm::HeightField field = flatField();
    const rm::sim::Terrain terrain{field};
    const rm::sim::PassabilityGrid grid =
        rm::sim::buildPassability(field, 0.0f, 60.0f, 0.0f);

    rm::test::Roster roster;
    const rm::UnitTypeIndex type = roster.addType(walkerDef());
    const UnitId walker = roster.add(type, 40.0f, 40.0f, 0, 100.0f);
    const std::vector<rm::sim::Army> armies = rm::sim::freeForAll(1);
    const std::vector<rm::sim::Player> players{rm::sim::Player{.index = 0, .army = 0}};
    const std::vector<const rm::sim::PassabilityGrid*> grids{&grid};

    Command attack = moveTo(300.0f, 40.0f, walker);
    attack.kind = CommandKind::Attack;
    REQUIRE(rm::sim::applyCommand(attack, roster.store, roster.catalog, players, armies,
                                  terrain, grid, roster.rate));

    SECTION("it retires when it is the only entry") {
        roster.store.motion()[walker.index].moving = false;

        CHECK(rm::sim::advanceOrders(roster.store, roster.catalog, terrain, grids, roster.rate)
              == 0);
        CHECK(roster.store.orders()[walker.index].empty());
    }

    SECTION("it rotates behind the next entry") {
        REQUIRE(rm::sim::applyCommand(moveTo(300.0f, 300.0f, walker, true), roster.store,
                                      roster.catalog, players, armies, terrain, grid,
                                      roster.rate));
        roster.store.motion()[walker.index].moving = false;

        CHECK(rm::sim::advanceOrders(roster.store, roster.catalog, terrain, grids, roster.rate)
              == 0);
        const std::vector<Command> orders = roster.store.orders()[walker.index].all();
        REQUIRE(orders.size() == 2);
        CHECK(orders[0].kind == CommandKind::Move);
        CHECK(orders[0].targetZ == rm::sim::fxFromFloat(300.0f));
        CHECK(orders[1].kind == CommandKind::Attack);
        CHECK(orders[1].targetZ == rm::sim::fxFromFloat(40.0f));
        CHECK_FALSE(roster.store.motion()[walker.index].moving);
        CHECK(roster.store.orders()[walker.index].active() == nullptr);

        CHECK(rm::sim::advanceOrders(roster.store, roster.catalog, terrain, grids, roster.rate)
              == 1);
        REQUIRE(roster.store.orders()[walker.index].active() != nullptr);
        CHECK(roster.store.motion()[walker.index].moving);
    }
}

TEST_CASE("an attack-entity retires instead of cycling when its target dies") {
    const rm::HeightField field = flatField();
    const rm::sim::Terrain terrain{field};
    const rm::sim::PassabilityGrid grid =
        rm::sim::buildPassability(field, 0.0f, 60.0f, 0.0f);

    rm::test::Roster roster;
    const rm::UnitTypeIndex fighterType = roster.addType(fighterDef());
    const rm::UnitTypeIndex targetType = roster.addType(walkerDef());
    const UnitId fighter = roster.add(fighterType, 40.0f, 40.0f, 0, 100.0f);
    const UnitId target = roster.add(targetType, 300.0f, 40.0f, 1, 100.0f);
    const std::vector<rm::sim::Army> armies = rm::sim::freeForAll(2);
    const std::vector<rm::sim::Player> players{rm::sim::Player{.index = 0, .army = 0}};
    const std::vector<const rm::sim::PassabilityGrid*> grids{&grid, &grid};

    Command attack = moveTo(300.0f, 40.0f, fighter);
    attack.kind = CommandKind::Attack;
    attack.target = target;
    REQUIRE(rm::sim::applyCommand(attack, roster.store, roster.catalog, players, armies,
                                  terrain, grid, roster.rate));

    SECTION("it stops immediately when it has no follower") {
        roster.store.kill(target);

        CHECK(rm::sim::advanceOrders(roster.store, roster.catalog, terrain, grids, roster.rate)
              == 0);
        CHECK(roster.store.orders()[fighter.index].empty());
        CHECK_FALSE(roster.store.motion()[fighter.index].moving);
        CHECK(roster.store.motion()[fighter.index].path.empty());
    }

    SECTION("it dispatches a follower in the same beat") {
        REQUIRE(rm::sim::applyCommand(moveTo(300.0f, 300.0f, fighter, true), roster.store,
                                      roster.catalog, players, armies, terrain, grid,
                                      roster.rate));
        roster.store.kill(target);

        CHECK(rm::sim::advanceOrders(roster.store, roster.catalog, terrain, grids, roster.rate)
              == 1);
        const std::vector<Command> orders = roster.store.orders()[fighter.index].all();
        REQUIRE(orders.size() == 1);
        CHECK(orders[0].kind == CommandKind::Move);
    }
}

TEST_CASE("attack-move stops for a visible enemy then resumes its destination") {
    const rm::HeightField field = flatField();
    const rm::sim::Terrain terrain{field};
    const rm::sim::PassabilityGrid grid = rm::sim::buildPassability(field, 0.0f, 60.0f, 0.0f);

    rm::test::Roster roster;
    const rm::UnitTypeIndex fighterType = roster.addType(fighterDef());
    const rm::UnitTypeIndex targetType = roster.addType(walkerDef());
    const UnitId fighter = roster.add(fighterType, 40.0f, 40.0f, 0, 100.0f);
    const UnitId enemy = roster.add(targetType, 240.0f, 40.0f, 1, 100.0f);
    std::vector<rm::sim::Army> armies = rm::sim::freeForAll(2);
    const std::vector<rm::sim::Player> players{rm::sim::Player{.index = 0, .army = 0}};
    const std::vector<const rm::sim::PassabilityGrid*> grids{&grid, &grid};

    Command attackMove = moveTo(500.0f, 40.0f, fighter);
    attackMove.kind = CommandKind::AttackMove;
    REQUIRE(rm::sim::applyCommand(attackMove, roster.store, roster.catalog, players, armies,
                                  terrain, grid, roster.rate));

    rm::sim::Match match{.armies = armies, .economies = {}, .passability = grids,
                         .commandersEver = {}};
    rm::sim::Intel intel;
    intel.configure(2, rm::sim::fxFromFloat(field.widthElmos()),
                    rm::sim::fxFromFloat(field.depthElmos()),
                    rm::sim::VisionStyle::ForgedAlliance);
    match.intel = &intel;
    bool engaged = false;
    for (int tick = 0; tick < 300; ++tick) {
        (void)rm::sim::tickSkirmish(roster.store, roster.catalog, match, terrain, roster.rate);
        const rm::sim::QueuedCommand* current =
            roster.store.orders()[fighter.index].current();
        if (current != nullptr && current->target() == enemy) {
            engaged = true;
            CHECK_FALSE(roster.store.motion()[fighter.index].moving);
            CHECK(current->targetX() == rm::sim::fxFromFloat(500.0f));
            break;
        }
    }
    REQUIRE(engaged);

    roster.transform(enemy).z = rm::sim::fxFromFloat(400.0f);
    (void)rm::sim::tickSkirmish(roster.store, roster.catalog, match, terrain, roster.rate);
    REQUIRE(roster.store.orders()[fighter.index].current() != nullptr);
    CHECK(roster.store.orders()[fighter.index].current()->target().generation == 0);
    CHECK(roster.store.motion()[fighter.index].moving);

    roster.health(enemy).current = rm::sim::Mag{};
    for (int tick = 0; tick < 500 && !roster.store.orders()[fighter.index].empty(); ++tick) {
        (void)rm::sim::tickSkirmish(roster.store, roster.catalog, match, terrain, roster.rate);
    }

    CHECK(roster.store.orders()[fighter.index].empty());
    CHECK(rm::sim::fxToFloat(roster.store.transforms()[fighter.index].x) > 450.0f);
}

TEST_CASE("an interceptor's attack-move ignores surface units") {
    const rm::HeightField field = flatField();
    const rm::sim::Terrain terrain{field};
    const rm::sim::PassabilityGrid grid =
        rm::sim::buildPassability(field, 0.0f, 60.0f, 0.0f);

    rm::test::Roster roster;
    rm::unitdef::UnitDef interceptor = fighterDef();
    interceptor.motion = rm::unitdef::MotionType::Air;
    interceptor.weapons[0].targetLayers = rm::unitdef::TargetLayerMask::Air;
    rm::unitdef::UnitDef aircraft = walkerDef();
    aircraft.motion = rm::unitdef::MotionType::Air;
    const UnitId fighter = roster.add(roster.addType(interceptor), 40.0f, 40.0f, 0, 100.0f);
    (void)roster.add(roster.addType(walkerDef()), 80.0f, 40.0f, 1, 100.0f);
    const UnitId enemyAir =
        roster.add(roster.addType(aircraft), 120.0f, 40.0f, 1, 100.0f);
    std::vector<rm::sim::Army> armies = rm::sim::freeForAll(2);
    const std::vector<rm::sim::Player> players{rm::sim::Player{.index = 0, .army = 0}};
    const std::vector<const rm::sim::PassabilityGrid*> grids{&grid, &grid};

    Command attackMove = moveTo(300.0f, 40.0f, fighter);
    attackMove.kind = CommandKind::AttackMove;
    REQUIRE(rm::sim::applyCommand(attackMove, roster.store, roster.catalog, players, armies,
                                  terrain, grid, roster.rate));
    rm::sim::Match match{.armies = armies, .economies = {}, .passability = grids,
                         .commandersEver = {}};
    (void)rm::sim::tickSkirmish(roster.store, roster.catalog, match, terrain, roster.rate);

    REQUIRE(roster.store.orders()[fighter.index].current() != nullptr);
    CHECK(roster.store.orders()[fighter.index].current()->target() == enemyAir);
}

TEST_CASE("an interceptor refuses an explicit attack on a surface unit") {
    const rm::HeightField field = flatField();
    const rm::sim::Terrain terrain{field};
    const rm::sim::PassabilityGrid grid =
        rm::sim::buildPassability(field, 0.0f, 60.0f, 0.0f);

    rm::test::Roster roster;
    rm::unitdef::UnitDef interceptor = fighterDef();
    interceptor.motion = rm::unitdef::MotionType::Air;
    interceptor.weapons[0].targetLayers = rm::unitdef::TargetLayerMask::Air;
    const UnitId fighter = roster.add(roster.addType(interceptor), 40.0f, 40.0f, 0, 100.0f);
    const UnitId surface =
        roster.add(roster.addType(walkerDef()), 120.0f, 40.0f, 1, 100.0f);
    const std::vector<rm::sim::Army> armies = rm::sim::freeForAll(2);
    const std::vector<rm::sim::Player> players{rm::sim::Player{.index = 0, .army = 0}};
    const Command attack{.kind = CommandKind::Attack,
                         .unit = fighter,
                         .targetX = roster.transform(surface).x,
                         .targetZ = roster.transform(surface).z,
                         .target = surface};

    CHECK_FALSE(rm::sim::applyCommand(attack, roster.store, roster.catalog, players, armies,
                                      terrain, grid, roster.rate));
    CHECK(roster.store.orders()[fighter.index].empty());
    CHECK_FALSE(roster.store.motion()[fighter.index].moving);
}

TEST_CASE("attack-move does not stop inside every weapon's minimum range") {
    const rm::HeightField field = flatField();
    const rm::sim::Terrain terrain{field};
    const rm::sim::PassabilityGrid grid = rm::sim::buildPassability(field, 0.0f, 60.0f, 0.0f);

    rm::test::Roster roster;
    const rm::UnitTypeIndex fighterType = roster.addType(fighterDef(80.0f));
    const rm::UnitTypeIndex targetType = roster.addType(walkerDef());
    const UnitId fighter = roster.add(fighterType, 40.0f, 40.0f, 0, 100.0f);
    (void)roster.add(targetType, 100.0f, 40.0f, 1, 100.0f);
    std::vector<rm::sim::Army> armies = rm::sim::freeForAll(2);
    const std::vector<rm::sim::Player> players{rm::sim::Player{.index = 0, .army = 0}};
    const std::vector<const rm::sim::PassabilityGrid*> grids{&grid, &grid};

    Command attackMove = moveTo(300.0f, 40.0f, fighter);
    attackMove.kind = CommandKind::AttackMove;
    REQUIRE(rm::sim::applyCommand(attackMove, roster.store, roster.catalog, players, armies,
                                  terrain, grid, roster.rate));
    rm::sim::Match match{.armies = armies, .economies = {}, .passability = grids,
                         .commandersEver = {}};
    (void)rm::sim::tickSkirmish(roster.store, roster.catalog, match, terrain, roster.rate);

    REQUIRE(roster.store.orders()[fighter.index].current() != nullptr);
    CHECK(roster.store.orders()[fighter.index].current()->target().generation == 0);
    CHECK(roster.store.motion()[fighter.index].moving);
}

TEST_CASE("attack-move resumes its waypoint when a target retreats out of reach") {
    const rm::HeightField field = flatField();
    const rm::sim::Terrain terrain{field};
    rm::sim::PassabilityGrid grid = rm::sim::buildPassability(field, 0.0f, 60.0f, 0.0f);
    for (int z = 0; z < grid.cellsZ; ++z) {
        grid.passable[static_cast<std::size_t>(z * grid.cellsX + 4)] = 0;
    }

    rm::test::Roster roster;
    const rm::UnitTypeIndex fighterType = roster.addType(fighterDef());
    const rm::UnitTypeIndex targetType = roster.addType(walkerDef());
    const UnitId fighter = roster.add(fighterType, 40.0f, 40.0f, 0, 100.0f);
    const UnitId enemy = roster.add(targetType, 120.0f, 40.0f, 1, 100.0f);
    std::vector<rm::sim::Army> armies = rm::sim::freeForAll(2);
    const std::vector<rm::sim::Player> players{rm::sim::Player{.index = 0, .army = 0}};
    const std::vector<const rm::sim::PassabilityGrid*> grids{&grid, &grid};

    Command attackMove = moveTo(180.0f, 40.0f, fighter);
    attackMove.kind = CommandKind::AttackMove;
    REQUIRE(rm::sim::applyCommand(attackMove, roster.store, roster.catalog, players, armies,
                                  terrain, grid, roster.rate));
    rm::sim::Match match{.armies = armies, .economies = {}, .passability = grids,
                         .commandersEver = {}};
    (void)rm::sim::tickSkirmish(roster.store, roster.catalog, match, terrain, roster.rate);
    REQUIRE(roster.store.orders()[fighter.index].current() != nullptr);
    REQUIRE(roster.store.orders()[fighter.index].current()->target() == enemy);

    roster.transform(enemy).x = rm::sim::fxFromFloat(400.0f);
    (void)rm::sim::tickSkirmish(roster.store, roster.catalog, match, terrain, roster.rate);

    REQUIRE(roster.store.orders()[fighter.index].current() != nullptr);
    CHECK(roster.store.orders()[fighter.index].current()->target().generation == 0);
    CHECK(roster.store.motion()[fighter.index].moving);
    CHECK(roster.store.motion()[fighter.index].path.back()[0]
          < rm::sim::fxFromFloat(256.0f));
}

TEST_CASE("patrol keeps cycling between its destination and starting point") {
    const rm::HeightField field = flatField();
    const rm::sim::Terrain terrain{field};
    const rm::sim::PassabilityGrid grid = rm::sim::buildPassability(field, 0.0f, 60.0f, 0.0f);

    rm::test::Roster roster;
    const rm::UnitTypeIndex type = roster.addType(walkerDef());
    const UnitId walker = roster.add(type, 40.0f, 40.0f, 0, 100.0f);
    std::vector<rm::sim::Army> armies = rm::sim::freeForAll(1);
    const std::vector<rm::sim::Player> players{rm::sim::Player{.index = 0, .army = 0}};
    const std::vector<const rm::sim::PassabilityGrid*> grids{&grid};

    Command patrol = moveTo(300.0f, 40.0f, walker);
    patrol.kind = CommandKind::Patrol;
    REQUIRE(rm::sim::applyCommand(patrol, roster.store, roster.catalog, players, armies,
                                  terrain, grid, roster.rate));
    REQUIRE(roster.store.orders()[walker.index].size() == 1);

    Command third = patrol;
    third.queued = true;
    third.targetX = rm::sim::fxFromFloat(300.0f);
    third.targetZ = rm::sim::fxFromFloat(300.0f);
    REQUIRE(rm::sim::applyCommand(third, roster.store, roster.catalog, players, armies,
                                  terrain, grid, roster.rate));
    REQUIRE(roster.store.orders()[walker.index].size() == 2);

    rm::sim::Match match{.armies = armies, .economies = {}, .passability = grids,
                         .commandersEver = {}};
    std::vector<std::array<rm::sim::Fx, 2>> destinations;
    for (int tick = 0; tick < 1600 && destinations.size() < 4; ++tick) {
        const rm::sim::QueuedCommand* current = roster.store.orders()[walker.index].current();
        REQUIRE(current != nullptr);
        const std::array<rm::sim::Fx, 2> destination{current->targetX(), current->targetZ()};
        if (destinations.empty() || destinations.back() != destination) {
            destinations.push_back(destination);
        }
        (void)rm::sim::tickSkirmish(roster.store, roster.catalog, match, terrain, roster.rate);
        CHECK(roster.store.orders()[walker.index].size() == 2);
    }

    REQUIRE(destinations.size() >= 4);
    CHECK(destinations[0] == std::array{rm::sim::fxFromFloat(300.0f),
                                       rm::sim::fxFromFloat(40.0f)});
    CHECK(destinations[1] == std::array{rm::sim::fxFromFloat(40.0f),
                                       rm::sim::fxFromFloat(40.0f)});
    CHECK(destinations[2] == std::array{rm::sim::fxFromFloat(300.0f),
                                       rm::sim::fxFromFloat(300.0f)});
    CHECK(destinations[3] == destinations[0]);
}

TEST_CASE("a lone patrol retires instead of rotating forever") {
    const rm::HeightField field = flatField();
    const rm::sim::Terrain terrain{field};
    const rm::sim::PassabilityGrid grid =
        rm::sim::buildPassability(field, 0.0f, 60.0f, 0.0f);

    rm::test::Roster roster;
    const rm::UnitTypeIndex type = roster.addType(walkerDef());
    const UnitId walker = roster.add(type, 40.0f, 40.0f, 0, 100.0f);
    const std::vector<rm::sim::Army> armies = rm::sim::freeForAll(1);
    const std::vector<rm::sim::Player> players{rm::sim::Player{.index = 0, .army = 0}};
    const std::vector<const rm::sim::PassabilityGrid*> grids{&grid};

    Command patrol = moveTo(300.0f, 40.0f, walker);
    patrol.kind = CommandKind::Patrol;
    REQUIRE(rm::sim::applyCommand(patrol, roster.store, roster.catalog, players, armies,
                                  terrain, grid, roster.rate));
    REQUIRE(roster.store.orders()[walker.index].size() == 1);

    // Simulate an out-of-band edit that leaves one ordinary patrol waypoint but removes the
    // first entry's hidden return leg. Retail's shared-count and guard paths can produce this;
    // the dispatch path must consume the waypoint rather than cycle it by itself.
    rm::sim::CommandQueue& queue = roster.store.orders()[walker.index];
    Command lone = queue.current()->asCommand();
    lone.targetX = rm::sim::fxFromFloat(40.0f);
    lone.targetZ = rm::sim::fxFromFloat(40.0f);
    queue.clear();
    (void)queue.give(lone, /*queued=*/false);
    roster.store.transforms()[walker.index].x = rm::sim::fxFromFloat(300.0f);
    roster.store.motion()[walker.index].moving = false;

    CHECK(rm::sim::advanceOrders(roster.store, roster.catalog, terrain, grids, roster.rate) == 1);
    REQUIRE(roster.store.orders()[walker.index].active() != nullptr);

    roster.store.motion()[walker.index].moving = false;
    CHECK(rm::sim::advanceOrders(roster.store, roster.catalog, terrain, grids, roster.rate) == 0);
    CHECK(roster.store.orders()[walker.index].empty());
}

TEST_CASE("a queued patrol joins the current cycle before its oldest waypoint") {
    const rm::HeightField field = flatField();
    const rm::sim::Terrain terrain{field};
    const rm::sim::PassabilityGrid grid =
        rm::sim::buildPassability(field, 0.0f, 60.0f, 0.0f);

    rm::test::Roster roster;
    const rm::UnitTypeIndex type = roster.addType(walkerDef());
    const UnitId walker = roster.add(type, 40.0f, 40.0f, 0, 100.0f);
    const std::vector<rm::sim::Army> armies = rm::sim::freeForAll(1);
    const std::vector<rm::sim::Player> players{rm::sim::Player{.index = 0, .army = 0}};

    Command patrol = moveTo(300.0f, 40.0f, walker);
    patrol.tick = 7;
    patrol.kind = CommandKind::Patrol;
    REQUIRE(rm::sim::applyCommand(patrol, roster.store, roster.catalog, players, armies,
                                  terrain, grid, roster.rate));

    patrol.queued = true;
    patrol.targetX = rm::sim::fxFromFloat(300.0f);
    patrol.targetZ = rm::sim::fxFromFloat(300.0f);
    REQUIRE(rm::sim::applyCommand(patrol, roster.store, roster.catalog, players, armies,
                                  terrain, grid, roster.rate));

    rm::sim::CommandQueue& queue = roster.store.orders()[walker.index];
    REQUIRE(queue.size() == 2);
    (void)queue.cycle();

    // Every input shares a tick, so only the command-creation serial can identify the first
    // destination after it rotates behind the other two waypoints.
    patrol.targetX = rm::sim::fxFromFloat(40.0f);
    patrol.targetZ = rm::sim::fxFromFloat(300.0f);
    REQUIRE(rm::sim::applyCommand(patrol, roster.store, roster.catalog, players, armies,
                                  terrain, grid, roster.rate));

    const std::vector<Command> orders = queue.all();
    REQUIRE(orders.size() == 3);
    CHECK(orders[0].targetX == rm::sim::fxFromFloat(300.0f));
    CHECK(orders[0].targetZ == rm::sim::fxFromFloat(300.0f));
    CHECK(orders[1].targetX == rm::sim::fxFromFloat(40.0f));
    CHECK(orders[1].targetZ == rm::sim::fxFromFloat(300.0f));
    CHECK(orders[2].targetX == rm::sim::fxFromFloat(300.0f));
    CHECK(orders[2].targetZ == rm::sim::fxFromFloat(40.0f));
}

TEST_CASE("cancelling a two-point patrol dissolves its synthetic endpoint") {
    const rm::HeightField field = flatField();
    const rm::sim::Terrain terrain{field};
    const rm::sim::PassabilityGrid grid = rm::sim::buildPassability(field, 0.0f, 60.0f, 0.0f);

    rm::test::Roster roster;
    const rm::UnitTypeIndex type = roster.addType(walkerDef());
    const UnitId walker = roster.add(type, 40.0f, 40.0f, 0, 100.0f);
    std::vector<rm::sim::Army> armies = rm::sim::freeForAll(1);
    const std::vector<rm::sim::Player> players{rm::sim::Player{.index = 0, .army = 0}};

    Command patrol = moveTo(300.0f, 40.0f, walker);
    patrol.kind = CommandKind::Patrol;
    REQUIRE(rm::sim::applyCommand(patrol, roster.store, roster.catalog, players, armies,
                                  terrain, grid, roster.rate));
    patrol.queued = true;
    REQUIRE(rm::sim::applyCommand(patrol, roster.store, roster.catalog, players, armies,
                                  terrain, grid, roster.rate));

    CHECK(roster.store.orders()[walker.index].empty());
    CHECK_FALSE(roster.store.motion()[walker.index].moving);
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
                                players, armies, terrain, open, roster.rate, nullptr));
    REQUIRE(roster.store.orders()[walker.index].size() == 1);

    CHECK_FALSE(rm::sim::applyCommand(moveTo(300.0f, 300.0f, walker), roster.store,
                                      roster.catalog, players, armies, terrain, closed,
                                      roster.rate, nullptr));
    // The original order is still there.
    REQUIRE(roster.store.orders()[walker.index].size() == 1);
    CHECK(roster.store.orders()[walker.index].current()->asCommand()
          == moveTo(200.0f, 40.0f, walker));
}

TEST_CASE("a queued mobile product waits for and starts on its own grid") {
    const rm::HeightField field = flatField();
    const rm::sim::Terrain terrain{field};
    const rm::sim::PassabilityGrid open =
        rm::sim::buildPassability(field, 0.0f, 60.0f, 0.0f);
    const rm::sim::PassabilityGrid closed =
        rm::sim::buildPassability(field, 1.0e6f, 60.0f, 0.0f);

    rm::test::Roster roster;
    rm::unitdef::UnitDef engineerDef = walkerDef();
    engineerDef.buildRate = 10.0f;
    // A mobile test double for the factory-specific queue path: keeping movement lets the
    // order ahead exercise deferred start, while the category selects repeatable products.
    engineerDef.categories = {"FACTORY"};
    engineerDef.buildableCategory = {{"TESTSTRUCTURE"}};
    const rm::UnitTypeIndex engineerType = roster.addType(engineerDef);

    rm::unitdef::UnitDef productDef = walkerDef();
    productDef.name = "product";
    productDef.categories = {"TESTSTRUCTURE"};
    productDef.collisionRadiusElmos = 8.0f;
    const rm::UnitTypeIndex productType = roster.addType(productDef);

    const UnitId engineer = roster.add(engineerType, 40.0f, 40.0f, 0, 100.0f);
    const std::vector<rm::sim::Army> armies = rm::sim::freeForAll(1);
    const std::vector<rm::sim::Player> players{rm::sim::Player{.index = 0, .army = 0}};
    std::vector<rm::sim::Construction> building;

    REQUIRE(rm::sim::applyCommand(moveTo(200.0f, 40.0f, engineer), roster.store,
                                  roster.catalog, players, armies, terrain, open, roster.rate,
                                  &building));
    Command build{.tick = 0,
                  .player = 0,
                  .kind = CommandKind::Build,
                  .queued = true,
                  .unit = engineer,
                  .targetX = rm::sim::fxFromFloat(400.0f),
                  .targetZ = rm::sim::fxFromFloat(400.0f),
                  .buildType = productType};
    REQUIRE(rm::sim::applyCommand(build, roster.store, roster.catalog, players, armies,
                                  terrain, closed, roster.rate, &building));
    const std::deque<rm::sim::QueuedCommand>& queued =
        roster.store.orders()[engineer.index].entries();
    REQUIRE(queued.size() == 2);
    CHECK(queued[1].payload().creationSerial == 1);
    CHECK(roster.store.nextCommandSerial() == 2);

    // The move has arrived, but this tick's table predates the newly registered product. The
    // build stays at the head rather than being dropped as though it had already run.
    roster.store.motion()[engineer.index].moving = false;
    const std::vector<const rm::sim::PassabilityGrid*> staleGrids{&closed};
    CHECK(rm::sim::advanceOrders(roster.store, roster.catalog, terrain, staleGrids, roster.rate,
                                 &building)
          == 0);
    CHECK(building.empty());
    REQUIRE(roster.store.orders()[engineer.index].current() != nullptr);
    CHECK(roster.store.orders()[engineer.index].current()->kind() == CommandKind::Build);

    // On the next tick the product grid exists. The builder grid is deliberately closed, so a
    // successful start proves the deferred command selected productType's grid.
    const std::vector<const rm::sim::PassabilityGrid*> grids{&closed, &open};
    CHECK(rm::sim::advanceOrders(roster.store, roster.catalog, terrain, grids, roster.rate,
                                 &building)
          == 1);
    REQUIRE(building.size() == 1);
    CHECK(building.front().blueprintIndex == productType);
}

TEST_CASE("factory repeat consumes a shared count before cycling mobile production") {
    const rm::HeightField field = flatField();
    const rm::sim::Terrain terrain{field};
    const rm::sim::PassabilityGrid grid =
        rm::sim::buildPassability(field, 0.0f, 60.0f, 0.0f);

    rm::test::Roster roster;
    rm::unitdef::UnitDef factoryDef;
    factoryDef.name = "factory";
    factoryDef.categories = {"FACTORY"};
    factoryDef.buildRate = 10.0f;
    factoryDef.buildableCategory = {{"PRODUCT"}};
    const rm::UnitTypeIndex factoryType = roster.addType(factoryDef);
    rm::unitdef::UnitDef productDef = walkerDef();
    productDef.name = "product";
    productDef.categories = {"PRODUCT"};
    productDef.buildTime = rm::sim::magFromFloat(100.0f);
    const rm::UnitTypeIndex productType = roster.addType(productDef);
    const UnitId factory = roster.add(factoryType, 40.0f, 40.0f, 0, 100.0f);
    const std::vector<rm::sim::Army> armies = rm::sim::freeForAll(1);
    const std::vector<rm::sim::Player> players{rm::sim::Player{.index = 0, .army = 0}};
    const std::vector<const rm::sim::PassabilityGrid*> grids{&grid, &grid};
    std::vector<rm::sim::Construction> building;

    const CommandIssue first{.source = 0,
                             .id = rm::commandId(0, 41),
                               .player = 0,
                               .kind = CommandKind::Build,
                               .units = {factory},
                               .buildType = productType,
                               .count = 2};
    REQUIRE(rm::sim::applyCommand(first, roster.store, roster.catalog, players, armies, terrain,
                                  [&grid](UnitId) { return &grid; }, roster.rate, &building));
    REQUIRE(rm::sim::applyCommand(
        Command{.kind = CommandKind::Build,
                .queued = true,
                .unit = factory,
                .buildType = productType},
        roster.store, roster.catalog, players, armies, terrain, grid, roster.rate, &building));
    const rm::CommandId repeated = roster.store.orders()[factory.index].current()->payload().id;
    rm::sim::CommandBuffer input;
    REQUIRE(input.submit(CommandIssue{.source = 0,
                                      .player = 0,
                                      .kind = CommandKind::ToggleFactoryRepeat,
                                      .units = {factory}},
                         roster.store));
    const std::vector<CommandIssue> repeatToggle =
        input.take(0, rm::sim::CommandPhase::PreTick);
    REQUIRE(repeatToggle.size() == 1);
    REQUIRE(rm::sim::applyCommand(repeatToggle.front(), roster.store, roster.catalog, players,
                                  armies, terrain, [&grid](UnitId) { return &grid; }, roster.rate));
    REQUIRE(roster.store.factoryRepeat(factory));

    // The first completion consumes one member of the shared repeat batch. It stays ahead of
    // the follower to start its final repetition rather than cycling early.
    building.front().buildTimeRemaining = rm::sim::Mag{};
    CHECK(rm::sim::advanceOrders(roster.store, roster.catalog, terrain, grids, roster.rate,
                                  &building)
          == 1);
    const std::deque<rm::sim::QueuedCommand>& queue = roster.store.orders()[factory.index].entries();
    REQUIRE(queue.size() == 2);
    CHECK(queue.front().payload().id == repeated);
    CHECK(queue.back().payload().id != repeated);
    CHECK(roster.store.liveCommand(repeated)->remainingCount == 1);
    CHECK(roster.store.liveCommand(repeated)->originalCount == 2);
    REQUIRE(roster.store.orders()[factory.index].active() != nullptr);
    CHECK(roster.store.orders()[factory.index].active()->payload().id == repeated);

    // The final completion restores the original count and rotates the repeat behind its
    // follower, where the next dispatch can start that follower normally.
    building.back().buildTimeRemaining = rm::sim::Mag{};
    CHECK(rm::sim::advanceOrders(roster.store, roster.catalog, terrain, grids, roster.rate,
                                  &building)
          == 1);
    REQUIRE(queue.size() == 2);
    CHECK(queue.front().payload().id != repeated);
    CHECK(queue.back().payload().id == repeated);
    CHECK(roster.store.liveCommand(repeated)->remainingCount
          == roster.store.liveCommand(repeated)->originalCount);
    CHECK(roster.store.liveCommand(repeated)->originalCount == 2);
    REQUIRE(roster.store.orders()[factory.index].active() != nullptr);
    CHECK(roster.store.orders()[factory.index].active()->payload().id != repeated);

    // Turning repeat back off does not change the ordinary completion path.
    REQUIRE(input.submit(CommandIssue{.source = 0,
                                      .player = 0,
                                      .kind = CommandKind::ToggleFactoryRepeat,
                                      .units = {factory}},
                         roster.store));
    const std::vector<CommandIssue> repeatToggleOff =
        input.take(0, rm::sim::CommandPhase::PreTick);
    REQUIRE(repeatToggleOff.size() == 1);
    REQUIRE(rm::sim::applyCommand(repeatToggleOff.front(), roster.store, roster.catalog, players,
                                  armies, terrain, [&grid](UnitId) { return &grid; }, roster.rate));
    REQUIRE_FALSE(roster.store.factoryRepeat(factory));
    building.back().buildTimeRemaining = rm::sim::Mag{};
    CHECK(rm::sim::advanceOrders(roster.store, roster.catalog, terrain, grids, roster.rate,
                                 &building)
          == 1);
    REQUIRE(roster.store.orders()[factory.index].size() == 1);
    CHECK(roster.store.orders()[factory.index].current()->payload().id == repeated);

    building.back().buildTimeRemaining = rm::sim::Mag{};
    CHECK(rm::sim::advanceOrders(roster.store, roster.catalog, terrain, grids, roster.rate,
                                 &building)
          == 1);
    REQUIRE(roster.store.orders()[factory.index].active() != nullptr);
    CHECK(roster.store.orders()[factory.index].active()->payload().id == repeated);
    CHECK(roster.store.liveCommand(repeated)->remainingCount == 1);

    building.back().buildTimeRemaining = rm::sim::Mag{};
    CHECK(rm::sim::advanceOrders(roster.store, roster.catalog, terrain, grids, roster.rate,
                                 &building)
          == 0);
    CHECK(roster.store.orders()[factory.index].empty());
}

TEST_CASE("one factory finishing a grouped build leaves its sibling factory's order owned") {
    const rm::HeightField field = flatField();
    const rm::sim::Terrain terrain{field};
    const rm::sim::PassabilityGrid grid =
        rm::sim::buildPassability(field, 0.0f, 60.0f, 0.0f);

    rm::test::Roster roster;
    rm::unitdef::UnitDef factoryDef;
    factoryDef.name = "factory";
    factoryDef.categories = {"FACTORY"};
    factoryDef.buildRate = 10.0f;
    factoryDef.buildableCategory = {{"PRODUCT"}};
    const rm::UnitTypeIndex factoryType = roster.addType(factoryDef);
    rm::unitdef::UnitDef productDef = walkerDef();
    productDef.name = "product";
    productDef.categories = {"PRODUCT"};
    productDef.buildTime = rm::sim::magFromFloat(100.0f);
    const rm::UnitTypeIndex productType = roster.addType(productDef);
    const UnitId first = roster.add(factoryType, 40.0f, 40.0f, 0, 100.0f);
    const UnitId second = roster.add(factoryType, 80.0f, 40.0f, 0, 100.0f);
    const std::vector<rm::sim::Army> armies = rm::sim::freeForAll(1);
    const std::vector<rm::sim::Player> players{rm::sim::Player{.index = 0, .army = 0}};
    const std::vector<const rm::sim::PassabilityGrid*> grids{&grid, &grid};
    std::vector<rm::sim::Construction> building;

    const CommandIssue grouped{.source = 0,
                               .id = rm::commandId(0, 51),
                               .player = 0,
                               .kind = CommandKind::Build,
                               .units = {second, first},
                               .buildType = productType};
    REQUIRE(rm::sim::applyCommand(grouped, roster.store, roster.catalog, players, armies,
                                  terrain, [&grid](UnitId) { return &grid; }, roster.rate,
                                  &building));
    REQUIRE(building.size() == 2);
    const auto firstWork = std::ranges::find(building, first, &rm::sim::Construction::builder);
    REQUIRE(firstWork != building.end());
    firstWork->buildTimeRemaining = rm::sim::Mag{};

    (void)rm::sim::advanceOrders(roster.store, roster.catalog, terrain, grids, roster.rate,
                                 &building);

    CHECK(roster.store.orders()[first.index].empty());
    REQUIRE(roster.store.orders()[second.index].active() != nullptr);
    CHECK(roster.store.orders()[second.index].active()->payload().id == grouped.id);
    const std::shared_ptr<rm::sim::SharedCommand> shared = roster.store.liveCommand(grouped.id);
    REQUIRE(shared != nullptr);
    CHECK(shared->units == std::vector{first, second});
    CHECK(shared->remainingCount == 1);
}

TEST_CASE("a guarding factory reserves one shared build and finishes it without consuming twice") {
    const rm::HeightField field = flatField();
    const rm::sim::Terrain terrain{field};
    const rm::sim::PassabilityGrid grid =
        rm::sim::buildPassability(field, 0.0f, 60.0f, 0.0f);

    rm::test::Roster roster;
    rm::unitdef::UnitDef factoryDef;
    factoryDef.name = "factory";
    factoryDef.categories = {"FACTORY"};
    factoryDef.buildRate = 10.0f;
    factoryDef.buildableCategory = {{"PRODUCT"}};
    const rm::UnitTypeIndex factoryType = roster.addType(factoryDef);
    rm::unitdef::UnitDef productDef = walkerDef();
    productDef.name = "product";
    productDef.categories = {"PRODUCT"};
    productDef.buildTime = rm::sim::magFromFloat(100.0f);
    const rm::UnitTypeIndex productType = roster.addType(productDef);
    const UnitId guard = roster.add(factoryType, 40.0f, 40.0f, 0, 100.0f);
    const UnitId guardee = roster.add(factoryType, 48.0f, 40.0f, 0, 100.0f);
    const std::vector<rm::sim::Army> armies = rm::sim::freeForAll(1);
    const std::vector<rm::sim::Player> players{rm::sim::Player{.index = 0, .army = 0}};
    const std::vector<const rm::sim::PassabilityGrid*> grids{&grid, &grid};
    std::vector<rm::sim::Construction> building;

    const CommandIssue build{.source = 0,
                             .id = rm::commandId(0, 61),
                             .player = 0,
                             .kind = CommandKind::Build,
                             .units = {guardee},
                             .buildType = productType,
                             .count = 2};
    REQUIRE(rm::sim::applyCommand(build, roster.store, roster.catalog, players, armies, terrain,
                                  [&grid](UnitId) { return &grid; }, roster.rate, &building));
    const CommandIssue assist{.source = 0,
                              .id = rm::commandId(0, 62),
                              .player = 0,
                              .kind = CommandKind::Assist,
                              .units = {guard},
                              .target = guardee};
    REQUIRE(rm::sim::applyCommand(assist, roster.store, roster.catalog, players, armies, terrain,
                                  [&grid](UnitId) { return &grid; }, roster.rate, &building));
    const rm::CommandSerial serialAfterInputs = roster.store.nextCommandSerial();
    const std::uint32_t counterAfterInputs = roster.store.nextCommandCounter(0);

    (void)rm::sim::advanceOrders(roster.store, roster.catalog, terrain, grids, roster.rate,
                                 &building);

    REQUIRE(building.size() == 2);
    const auto mirrored = std::ranges::find(building, guard, &rm::sim::Construction::builder);
    REQUIRE(mirrored != building.end());
    CHECK(mirrored->blueprintIndex == productType);
    REQUIRE(roster.store.orders()[guard.index].active() != nullptr);
    CHECK(roster.store.orders()[guard.index].active()->kind() == CommandKind::Assist);
    REQUIRE(roster.store.orders()[guardee.index].active() != nullptr);
    CHECK(roster.store.orders()[guardee.index].active()->payload().id == build.id);
    REQUIRE(roster.store.liveCommand(build.id) != nullptr);
    CHECK(roster.store.liveCommand(build.id)->remainingCount == 1);
    CHECK(roster.store.nextCommandSerial() == serialAfterInputs);
    CHECK(roster.store.nextCommandCounter(0) == counterAfterInputs);

    mirrored->buildTimeRemaining = rm::sim::Mag{};
    (void)rm::sim::advanceOrders(roster.store, roster.catalog, terrain, grids, roster.rate,
                                 &building);
    REQUIRE(roster.store.liveCommand(build.id) != nullptr);
    CHECK(roster.store.liveCommand(build.id)->remainingCount == 1);
    CHECK(roster.store.nextCommandSerial() == serialAfterInputs);
    CHECK(roster.store.nextCommandCounter(0) == counterAfterInputs);

    // Repeat makes the otherwise ineligible lone head available to the guarding factory. Its
    // batch count is restored and the guardee's own active construction remains attached.
    REQUIRE(roster.store.setFactoryRepeat(guardee, true));
    (void)rm::sim::advanceOrders(roster.store, roster.catalog, terrain, grids, roster.rate,
                                 &building);
    REQUIRE(building.size() == 3);
    CHECK(building.back().builder == guard);
    CHECK(roster.store.liveCommand(build.id)->remainingCount == 2);
    REQUIRE(roster.store.orders()[guardee.index].active() != nullptr);
    CHECK(roster.store.orders()[guardee.index].active()->payload().id == build.id);
    CHECK(roster.store.nextCommandSerial() == serialAfterInputs);
    CHECK(roster.store.nextCommandCounter(0) == counterAfterInputs);

    // The child build owns its remaining lifetime. Losing the guarded unit ends Assist only
    // after that child is done; it must not strand an upkeep-charging construction.
    rm::sim::Construction& inFlightMirror = building.back();
    const rm::sim::Mag beforeTargetDeath = inFlightMirror.buildTimeRemaining;
    roster.store.kill(guardee);
    (void)rm::sim::advanceOrders(roster.store, roster.catalog, terrain, grids, roster.rate,
                                 &building);
    CHECK(inFlightMirror.buildTimeRemaining < beforeTargetDeath);
    REQUIRE(roster.store.orders()[guard.index].active() != nullptr);
    CHECK(roster.store.orders()[guard.index].active()->kind() == CommandKind::Assist);
}

TEST_CASE("factory guard skips a singleton head and locally takes the queued build behind it") {
    const rm::HeightField field = flatField();
    const rm::sim::Terrain terrain{field};
    const rm::sim::PassabilityGrid grid =
        rm::sim::buildPassability(field, 0.0f, 60.0f, 0.0f);

    rm::test::Roster roster;
    rm::unitdef::UnitDef factoryDef;
    factoryDef.name = "factory";
    factoryDef.categories = {"FACTORY"};
    factoryDef.buildRate = 10.0f;
    factoryDef.buildableCategory = {{"PRODUCT"}};
    const rm::UnitTypeIndex factoryType = roster.addType(factoryDef);
    rm::unitdef::UnitDef firstProduct = walkerDef();
    firstProduct.name = "first-product";
    firstProduct.categories = {"PRODUCT"};
    firstProduct.buildTime = rm::sim::magFromFloat(100.0f);
    const rm::UnitTypeIndex firstType = roster.addType(firstProduct);
    rm::unitdef::UnitDef queuedProduct = firstProduct;
    queuedProduct.name = "queued-product";
    const rm::UnitTypeIndex queuedType = roster.addType(queuedProduct);
    const UnitId guard = roster.add(factoryType, 40.0f, 40.0f, 0, 100.0f);
    const UnitId guardee = roster.add(factoryType, 48.0f, 40.0f, 0, 100.0f);
    const UnitId peer = roster.add(factoryType, 80.0f, 40.0f, 0, 100.0f);
    const std::vector<rm::sim::Army> armies = rm::sim::freeForAll(1);
    const std::vector<rm::sim::Player> players{rm::sim::Player{.index = 0, .army = 0}};
    const std::vector<const rm::sim::PassabilityGrid*> grids{&grid, &grid, &grid};
    std::vector<rm::sim::Construction> building;
    const auto apply = [&](const CommandIssue& issue) {
        return rm::sim::applyCommand(issue, roster.store, roster.catalog, players, armies,
                                     terrain, [&grid](UnitId) { return &grid; }, roster.rate,
                                     &building);
    };

    const CommandIssue current{.source = 0,
                               .id = rm::commandId(0, 71),
                               .player = 0,
                               .kind = CommandKind::Build,
                               .units = {guardee},
                               .buildType = firstType};
    const CommandIssue queued{.source = 0,
                              .id = rm::commandId(0, 72),
                              .player = 0,
                              .kind = CommandKind::Build,
                              .queued = true,
                              .units = {peer, guardee},
                              .buildType = queuedType};
    const CommandIssue assist{.source = 0,
                              .id = rm::commandId(0, 73),
                              .player = 0,
                              .kind = CommandKind::Assist,
                              .units = {guard},
                              .target = guardee};
    REQUIRE(apply(current));
    REQUIRE(apply(queued));
    REQUIRE(apply(assist));

    bool repeating = false;
    SECTION("with repeat disabled") {}
    SECTION("with repeat enabled") {
        repeating = true;
        REQUIRE(roster.store.setFactoryRepeat(guardee, true));
    }

    (void)rm::sim::advanceOrders(roster.store, roster.catalog, terrain, grids, roster.rate,
                                 &building);

    REQUIRE(roster.store.orders()[guardee.index].size() == (repeating ? 2 : 1));
    CHECK(roster.store.orders()[guardee.index].current()->payload().id == current.id);
    REQUIRE(roster.store.commandIdLive(queued.id));
    REQUIRE(roster.store.orders()[peer.index].active() != nullptr);
    CHECK(roster.store.orders()[peer.index].active()->payload().id == queued.id);
    const std::shared_ptr<rm::sim::SharedCommand> shared = roster.store.liveCommand(queued.id);
    REQUIRE(shared != nullptr);
    CHECK(shared->units == std::vector{guardee, peer});
    CHECK(shared->remainingCount == 1);
    REQUIRE(building.size() == 3);
    const auto mirrored = std::ranges::find(building, guard, &rm::sim::Construction::builder);
    REQUIRE(mirrored != building.end());
    CHECK(mirrored->blueprintIndex == queuedType);
}

TEST_CASE("a guarding factory builds its own queued product before the guardee's") {
    const rm::HeightField field = flatField();
    const rm::sim::Terrain terrain{field};
    const rm::sim::PassabilityGrid grid =
        rm::sim::buildPassability(field, 0.0f, 60.0f, 0.0f);
    rm::test::Roster roster;
    rm::unitdef::UnitDef factoryDef;
    factoryDef.name = "factory";
    factoryDef.categories = {"FACTORY"};
    factoryDef.buildRate = 10.0f;
    factoryDef.buildableCategory = {{"PRODUCT"}};
    const rm::UnitTypeIndex factoryType = roster.addType(factoryDef);
    rm::unitdef::UnitDef ownProduct = walkerDef();
    ownProduct.name = "own-product";
    ownProduct.categories = {"PRODUCT"};
    ownProduct.buildTime = rm::sim::magFromFloat(100.0f);
    const rm::UnitTypeIndex ownProductType = roster.addType(ownProduct);
    rm::unitdef::UnitDef guardedProduct = ownProduct;
    guardedProduct.name = "guarded-product";
    const rm::UnitTypeIndex guardedProductType = roster.addType(guardedProduct);
    const UnitId guard = roster.add(factoryType, 40.0f, 40.0f, 0, 100.0f);
    const UnitId guardee = roster.add(factoryType, 48.0f, 40.0f, 0, 100.0f);
    const std::vector<rm::sim::Army> armies = rm::sim::freeForAll(1);
    const std::vector<rm::sim::Player> players{rm::sim::Player{.index = 0, .army = 0}};
    const std::vector<const rm::sim::PassabilityGrid*> grids{&grid, &grid, &grid};
    std::vector<rm::sim::Construction> building;
    const auto apply = [&](const CommandIssue& issue) {
        return rm::sim::applyCommand(issue, roster.store, roster.catalog, players, armies,
                                     terrain, [&grid](UnitId) { return &grid; }, roster.rate,
                                     &building);
    };
    const CommandIssue assist{.source = 0, .id = rm::commandId(0, 81), .player = 0,
                              .kind = CommandKind::Assist, .units = {guard}, .target = guardee};
    const CommandIssue own{.source = 0,
                           .id = rm::commandId(0, 82),
                           .player = 0,
                           .kind = CommandKind::Build,
                           .queued = true,
                           .units = {guard},
                           .buildType = ownProductType};
    const CommandIssue guarded{.source = 0,
                               .id = rm::commandId(0, 83),
                               .player = 0,
                               .kind = CommandKind::Build,
                               .units = {guardee},
                               .buildType = guardedProductType};
    REQUIRE(apply(assist));
    REQUIRE(apply(own));
    REQUIRE(apply(guarded));
    const std::shared_ptr<rm::sim::SharedCommand> ownPayload = roster.store.liveCommand(own.id);
    const std::shared_ptr<rm::sim::SharedCommand> guardedPayload =
        roster.store.liveCommand(guarded.id);
    REQUIRE(ownPayload != nullptr);
    REQUIRE(guardedPayload != nullptr);
    const rm::CommandSerial serialAfterInputs = roster.store.nextCommandSerial();
    const std::uint32_t counterAfterInputs = roster.store.nextCommandCounter(0);

    (void)rm::sim::advanceOrders(roster.store, roster.catalog, terrain, grids, roster.rate,
                                 &building);

    REQUIRE(building.size() == 2);
    const auto ownWork = std::ranges::find(building, guard, &rm::sim::Construction::builder);
    REQUIRE(ownWork != building.end());
    CHECK(ownWork->blueprintIndex == ownProductType);
    REQUIRE(roster.store.orders()[guard.index].active() != nullptr);
    CHECK(&roster.store.orders()[guard.index].active()->payload() != ownPayload.get());
    CHECK(roster.store.orders()[guard.index].active()->kind() == CommandKind::Assist);
    CHECK(roster.store.orders()[guard.index].entries().back().payload().id == own.id);
    CHECK(roster.store.liveCommand(own.id) == ownPayload);
    CHECK(ownPayload->id == own.id);
    REQUIRE(roster.store.orders()[guardee.index].active() != nullptr);
    CHECK(roster.store.orders()[guardee.index].active()->payload().id == guarded.id);
    CHECK(roster.store.liveCommand(guarded.id) == guardedPayload);
    CHECK(guardedPayload->remainingCount == 1);
    CHECK(roster.store.nextCommandSerial() == serialAfterInputs);
    CHECK(roster.store.nextCommandCounter(0) == counterAfterInputs);
}

TEST_CASE("a guarding factory retires its own queued build through normal count and repeat") {
    const rm::HeightField field = flatField();
    const rm::sim::Terrain terrain{field};
    const rm::sim::PassabilityGrid grid =
        rm::sim::buildPassability(field, 0.0f, 60.0f, 0.0f);
    rm::test::Roster roster;
    rm::unitdef::UnitDef factoryDef;
    factoryDef.name = "factory";
    factoryDef.categories = {"FACTORY"};
    factoryDef.buildRate = 10.0f;
    factoryDef.buildableCategory = {{"PRODUCT"}};
    const rm::UnitTypeIndex factoryType = roster.addType(factoryDef);
    rm::unitdef::UnitDef product = walkerDef();
    product.name = "product";
    product.categories = {"PRODUCT"};
    product.buildTime = rm::sim::magFromFloat(100.0f);
    const rm::UnitTypeIndex productType = roster.addType(product);
    const UnitId guard = roster.add(factoryType, 40.0f, 40.0f, 0, 100.0f);
    const UnitId guardee = roster.add(factoryType, 48.0f, 40.0f, 0, 100.0f);
    const std::vector<rm::sim::Army> armies = rm::sim::freeForAll(1);
    const std::vector<rm::sim::Player> players{rm::sim::Player{.index = 0, .army = 0}};
    const std::vector<const rm::sim::PassabilityGrid*> grids{&grid, &grid};
    std::vector<rm::sim::Construction> building;
    const auto apply = [&](const CommandIssue& issue) {
        return rm::sim::applyCommand(issue, roster.store, roster.catalog, players, armies,
                                     terrain, [&grid](UnitId) { return &grid; }, roster.rate,
                                     &building);
    };
    const CommandIssue assist{.source = 0, .id = rm::commandId(0, 91), .player = 0,
                              .kind = CommandKind::Assist, .units = {guard}, .target = guardee};
    const CommandIssue own{.source = 0,
                           .id = rm::commandId(0, 92),
                           .player = 0,
                           .kind = CommandKind::Build,
                           .queued = true,
                           .units = {guard},
                           .buildType = productType,
                           .count = 1};
    REQUIRE(apply(assist));
    REQUIRE(apply(own));
    (void)rm::sim::advanceOrders(roster.store, roster.catalog, terrain, grids, roster.rate,
                                 &building);
    const auto ownWork = std::ranges::find(building, guard, &rm::sim::Construction::builder);
    REQUIRE(ownWork != building.end());
    // Leave one sub-tick of work: `activeConstruction` only returns UNFINISHED records, so the
    // completion ladder runs only when `advanceConstruction` itself lands the beat on zero —
    // which is also the only way it ever runs in a live match.
    ownWork->buildTimeRemaining = rm::sim::magFromFloat(0.5);

    bool repeating = false;
    SECTION("without repeat") {}
    SECTION("with repeat") {
        repeating = true;
        REQUIRE(roster.store.setFactoryRepeat(guard, true));
    }

    (void)rm::sim::advanceOrders(roster.store, roster.catalog, terrain, grids, roster.rate,
                                 &building);

    // The ordinary dispatcher ladder (`C-211`): a final-count completion removes the entry,
    // unless factory repeat restores the batch and cycles it behind the retained Assist head.
    REQUIRE(roster.store.orders()[guard.index].active() != nullptr);
    CHECK(roster.store.orders()[guard.index].active()->kind() == CommandKind::Assist);
    if (repeating) {
        const std::shared_ptr<rm::sim::SharedCommand> ownPayload =
            roster.store.liveCommand(own.id);
        REQUIRE(ownPayload != nullptr);
        CHECK(ownPayload->remainingCount == ownPayload->originalCount);
        CHECK(roster.store.orders()[guard.index].size() == 2);
    } else {
        CHECK(roster.store.liveCommand(own.id) == nullptr);
        CHECK(roster.store.orders()[guard.index].size() == 1);
    }
}

TEST_CASE("a guarding factory completion retires the first of identical own builds") {
    const rm::HeightField field = flatField();
    const rm::sim::Terrain terrain{field};
    const rm::sim::PassabilityGrid grid =
        rm::sim::buildPassability(field, 0.0f, 60.0f, 0.0f);
    rm::test::Roster roster;
    rm::unitdef::UnitDef factoryDef;
    factoryDef.name = "factory";
    factoryDef.categories = {"FACTORY"};
    factoryDef.buildRate = 10.0f;
    factoryDef.buildableCategory = {{"PRODUCT"}};
    const rm::UnitTypeIndex factoryType = roster.addType(factoryDef);
    rm::unitdef::UnitDef product = walkerDef();
    product.name = "product";
    product.categories = {"PRODUCT"};
    product.buildTime = rm::sim::magFromFloat(100.0f);
    const rm::UnitTypeIndex productType = roster.addType(product);
    const UnitId guard = roster.add(factoryType, 40.0f, 40.0f, 0, 100.0f);
    const UnitId guardee = roster.add(factoryType, 48.0f, 40.0f, 0, 100.0f);
    const std::vector<rm::sim::Army> armies = rm::sim::freeForAll(1);
    const std::vector<rm::sim::Player> players{rm::sim::Player{.index = 0, .army = 0}};
    const std::vector<const rm::sim::PassabilityGrid*> grids{&grid, &grid};
    std::vector<rm::sim::Construction> building;
    const auto apply = [&](const CommandIssue& issue) {
        return rm::sim::applyCommand(issue, roster.store, roster.catalog, players, armies,
                                     terrain, [&grid](UnitId) { return &grid; }, roster.rate,
                                     &building);
    };
    const CommandIssue assist{.source = 0, .id = rm::commandId(0, 101), .player = 0,
                              .kind = CommandKind::Assist, .units = {guard}, .target = guardee};
    const CommandIssue first{.source = 0,
                             .id = rm::commandId(0, 102),
                             .player = 0,
                             .kind = CommandKind::Build,
                             .queued = true,
                             .units = {guard},
                             .buildType = productType};
    const CommandIssue second{.source = 0,
                              .id = rm::commandId(0, 103),
                              .player = 0,
                              .kind = CommandKind::Build,
                              .queued = true,
                              .units = {guard},
                              .buildType = productType};
    REQUIRE(apply(assist));
    REQUIRE(apply(first));
    REQUIRE(apply(second));
    REQUIRE(roster.store.orders()[guard.index].active() != nullptr);
    CHECK(roster.store.orders()[guard.index].active()->kind() == CommandKind::Assist);

    (void)rm::sim::advanceOrders(roster.store, roster.catalog, terrain, grids, roster.rate,
                                 &building);
    const auto started = std::ranges::find(building, guard, &rm::sim::Construction::builder);
    REQUIRE(started != building.end());
    started->buildTimeRemaining = rm::sim::magFromFloat(0.5f);

    SECTION("without repeat") {
        (void)rm::sim::advanceOrders(roster.store, roster.catalog, terrain, grids, roster.rate,
                                     &building);

        const std::deque<rm::sim::QueuedCommand>& entries =
            roster.store.orders()[guard.index].entries();
        REQUIRE(entries.size() == 2);
        CHECK(entries[0].kind() == CommandKind::Assist);
        CHECK(entries[1].payload().id == second.id);
        CHECK_FALSE(roster.store.commandIdLive(first.id));
    }

    SECTION("with repeat") {
        REQUIRE(roster.store.setFactoryRepeat(guard, true));
        (void)rm::sim::advanceOrders(roster.store, roster.catalog, terrain, grids, roster.rate,
                                     &building);

        const std::deque<rm::sim::QueuedCommand>& entries =
            roster.store.orders()[guard.index].entries();
        REQUIRE(entries.size() == 3);
        CHECK(entries[0].kind() == CommandKind::Assist);
        CHECK(entries[1].payload().id == second.id);
        CHECK(entries[2].payload().id == first.id);
    }
}

TEST_CASE("factory repeat toggles through a logged semantic issue and replays") {
    const rm::HeightField field = flatField();
    const rm::sim::Terrain terrain{field};
    const rm::sim::PassabilityGrid grid =
        rm::sim::buildPassability(field, 0.0f, 60.0f, 0.0f);
    rm::unitdef::UnitDef factoryDef;
    factoryDef.name = "factory";
    factoryDef.categories = {"FACTORY"};
    const std::vector<rm::sim::Army> armies = rm::sim::freeForAll(1);
    const std::vector<rm::sim::Player> players{rm::sim::Player{.index = 0, .army = 0}};
    const auto makeRoster = [&] {
        rm::test::Roster roster;
        const UnitId factory = roster.add(roster.addType(factoryDef), 40.0f, 40.0f, 0, 100.0f);
        return std::pair{std::move(roster), factory};
    };

    auto [live, factory] = makeRoster();
    rm::sim::CommandBuffer input;
    REQUIRE(input.submit(CommandIssue{.tick = 7,
                                      .source = 0,
                                      .player = 0,
                                      .kind = CommandKind::ToggleFactoryRepeat,
                                      .units = {factory}},
                         live.store));
    std::vector<CommandIssue> due = input.take(7, rm::sim::CommandPhase::PreTick);
    REQUIRE(due.size() == 1);
    const rm::sim::ApplyCommandResult accepted = rm::sim::applyCommand(
        due.front(), live.store, live.catalog, players, armies, terrain,
        [&grid](UnitId) { return &grid; }, live.rate);
    due.front().units = accepted.accepted;
    rm::sim::CommandLog log;
    REQUIRE(log.record(due.front()));
    CHECK(live.store.factoryRepeat(factory));

    auto [replay, replayFactory] = makeRoster();
    rm::sim::CommandBuffer replayInput;
    REQUIRE(replayInput.submit(log.all().front(), replay.store));
    const std::vector<CommandIssue> replayed =
        replayInput.take(7, rm::sim::CommandPhase::PreTick);
    REQUIRE(replayed.size() == 1);
    CHECK(rm::sim::applyCommand(replayed.front(), replay.store, replay.catalog, players, armies,
                                terrain, [&grid](UnitId) { return &grid; }, replay.rate));
    CHECK(replay.store.factoryRepeat(replayFactory));
}

TEST_CASE("factory repeat state contributes to the match hash") {
    rm::test::Roster disabled;
    rm::test::Roster enabled;
    const rm::UnitTypeIndex disabledType = disabled.addType(walkerDef());
    const rm::UnitTypeIndex enabledType = enabled.addType(walkerDef());
    const UnitId disabledUnit = disabled.add(disabledType, 40.0f, 40.0f, 0, 100.0f);
    const UnitId enabledUnit = enabled.add(enabledType, 40.0f, 40.0f, 0, 100.0f);
    REQUIRE(disabledUnit == enabledUnit);
    REQUIRE(enabled.store.setFactoryRepeat(enabledUnit, true));

    std::vector<rm::sim::Army> armies = rm::sim::freeForAll(1);
    const rm::sim::Match match{.armies = armies,
                               .economies = {},
                               .passability = {},
                               .commandersEver = {}};
    CHECK(rm::sim::hashMatch(disabled.store, match) != rm::sim::hashMatch(enabled.store, match));
}

TEST_CASE("a queued immobile structure falls back to its builder grid") {
    const rm::HeightField field = flatField();
    const rm::sim::Terrain terrain{field};
    const rm::sim::PassabilityGrid open =
        rm::sim::buildPassability(field, 0.0f, 60.0f, 0.0f);
    const rm::sim::PassabilityGrid empty;

    rm::test::Roster roster;
    rm::unitdef::UnitDef engineerDef = walkerDef();
    engineerDef.buildRate = 10.0f;
    engineerDef.buildableCategory = {{"TESTSTRUCTURE"}};
    const rm::UnitTypeIndex engineerType = roster.addType(engineerDef);

    rm::unitdef::UnitDef structureDef;
    structureDef.name = "structure";
    structureDef.categories = {"TESTSTRUCTURE"};
    structureDef.collisionRadiusElmos = 8.0f;
    const rm::UnitTypeIndex structureType = roster.addType(structureDef);

    const UnitId engineer = roster.add(engineerType, 40.0f, 40.0f, 0, 100.0f);
    const std::vector<rm::sim::Army> armies = rm::sim::freeForAll(1);
    const std::vector<rm::sim::Player> players{rm::sim::Player{.index = 0, .army = 0}};
    std::vector<rm::sim::Construction> building;

    REQUIRE(rm::sim::applyCommand(moveTo(200.0f, 40.0f, engineer), roster.store,
                                  roster.catalog, players, armies, terrain, open, roster.rate,
                                  &building));
    const Command build{.tick = 0,
                        .player = 0,
                        .kind = CommandKind::Build,
                        .queued = true,
                        .unit = engineer,
                        .targetX = rm::sim::fxFromFloat(400.0f),
                        .targetZ = rm::sim::fxFromFloat(400.0f),
                        .buildType = structureType};
    REQUIRE(rm::sim::applyCommand(build, roster.store, roster.catalog, players, armies,
                                  terrain, open, roster.rate, &building));

    roster.store.motion()[engineer.index].moving = false;
    const std::vector<const rm::sim::PassabilityGrid*> grids{&open, &empty};
    CHECK(rm::sim::advanceOrders(roster.store, roster.catalog, terrain, grids, roster.rate,
                                 &building)
          == 1);
    REQUIRE(building.size() == 1);
    CHECK(building.front().blueprintIndex == structureType);
}

TEST_CASE("an aircraft flies directly across a map no ground unit can route") {
    const rm::HeightField field = flatField();
    const rm::sim::Terrain terrain{field};
    const rm::sim::PassabilityGrid closed =
        rm::sim::buildPassability(field, 1.0e6f, 60.0f, 0.0f);

    rm::test::Roster roster;
    rm::unitdef::UnitDef aircraft = walkerDef();
    aircraft.motion = rm::unitdef::MotionType::Air;
    const rm::UnitTypeIndex type = roster.addType(aircraft);
    const UnitId flyer = roster.add(type, 40.0f, 40.0f, 0, 100.0f);
    std::vector<rm::sim::Army> armies = rm::sim::freeForAll(1);
    const std::vector<rm::sim::Player> players{rm::sim::Player{.index = 0, .army = 0}};
    const std::vector<const rm::sim::PassabilityGrid*> grids{&closed};

    REQUIRE(rm::sim::applyCommand(moveTo(400.0f, 400.0f, flyer), roster.store,
                                  roster.catalog, players, armies, terrain, closed, roster.rate));
    CHECK(roster.store.motion()[flyer.index].path.empty());

    rm::sim::Match match{.armies = armies, .economies = {}, .passability = grids,
                         .commandersEver = {}};
    for (int tick = 0; tick < 600 && !roster.store.orders()[flyer.index].empty(); ++tick) {
        (void)rm::sim::tickSkirmish(roster.store, roster.catalog, match, terrain, roster.rate);
    }
    CHECK(roster.store.orders()[flyer.index].empty());
    CHECK(rm::sim::fxToFloat(roster.store.transforms()[flyer.index].x) > 350.0f);
    CHECK(rm::sim::fxToFloat(roster.store.transforms()[flyer.index].z) > 350.0f);
    CHECK(roster.store.transforms()[flyer.index].y == rm::sim::kAirClearanceElmos);
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
                                players, armies, terrain, grid, roster.rate, nullptr));
    CHECK(roster.store.orders()[doomed.index].size() == 1);

    roster.store.kill(doomed);
    const UnitId newcomer = roster.add(type, 40.0f, 40.0f, 0, 100.0f);
    REQUIRE(newcomer.index == doomed.index);  // the slot, reused
    CHECK(roster.store.orders()[newcomer.index].empty());
}

TEST_CASE("the order queue stops taking shift-clicks at retail's cap, and a plain order still lands") {
    // `kCommandQueueCap`, read out of `Sim::IssueCommand` at `0x006f7e30`-`0x006f7e48`
    // (`C-231` refining `C-214`). The BOUNDARY is the part worth a test: retail compares the
    // element count against `0x1f4` with `jbe`, so a queue already holding 500 still accepts
    // one more and 501 is the ceiling a player can reach. Off by one in either direction and
    // this passes for the wrong reason.
    const rm::HeightField field = flatField();
    const rm::sim::Terrain terrain{field};
    const rm::sim::PassabilityGrid grid = rm::sim::buildPassability(field, 0.0f, 60.0f, 0.0f);

    rm::test::Roster roster;
    const rm::UnitTypeIndex type = roster.addType(walkerDef());
    const UnitId walker = roster.add(type, 40.0f, 40.0f, 0, 100.0f);
    const std::vector<rm::sim::Army> armies = rm::sim::freeForAll(1);
    const std::vector<rm::sim::Player> players{rm::sim::Player{.index = 0, .army = 0}};

    // Distinct destinations, so nothing is taken for a duplicate and cancelled instead.
    const auto waypoint = [walker](std::size_t nth) {
        return moveTo(60.0f + 30.0f * static_cast<float>(nth), 40.0f, walker, /*queued=*/true);
    };

    for (std::size_t nth = 0; nth < rm::sim::kCommandQueueCap; ++nth) {
        REQUIRE(rm::sim::applyCommand(waypoint(nth), roster.store, roster.catalog, players,
                                      armies, terrain, grid, roster.rate));
    }
    const CommandIssue repeated{
        .source = 0,
        .id = rm::commandId(0, static_cast<std::uint32_t>(rm::sim::kCommandQueueCap)),
        .player = 0,
        .kind = CommandKind::Move,
        .queued = true,
        .units = {walker},
        .targetX = waypoint(rm::sim::kCommandQueueCap).targetX,
        .targetZ = waypoint(rm::sim::kCommandQueueCap).targetZ,
        .count = 1000,
    };
    REQUIRE(rm::sim::applyCommand(repeated, roster.store, roster.catalog, players, armies,
                                  terrain, [&grid](UnitId) { return &grid; }, roster.rate));
    REQUIRE(roster.store.orders()[walker.index].size() == rm::sim::kCommandQueueCap + 1);

    // 501 held, so the next shift-click is refused — and refused means it changed nothing.
    CHECK_FALSE(rm::sim::applyCommand(waypoint(rm::sim::kCommandQueueCap + 1), roster.store,
                                      roster.catalog, players, armies, terrain, grid,
                                      roster.rate));
    CHECK(roster.store.orders()[walker.index].size() == rm::sim::kCommandQueueCap + 1);

    // But an order that CLEARS the queue is exempt, which is what keeps a unit at the cap from
    // becoming uncommandable: retail bypasses the whole test when the clear flag is set.
    CHECK(rm::sim::applyCommand(moveTo(300.0f, 40.0f, walker, /*queued=*/false), roster.store,
                                roster.catalog, players, armies, terrain, grid, roster.rate));
    CHECK(roster.store.orders()[walker.index].size() == 1);
}
