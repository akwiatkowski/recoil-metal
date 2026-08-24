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

[[nodiscard]] rm::unitdef::UnitDef fighterDef(float minimumRange = 0.0f) {
    rm::unitdef::UnitDef def = walkerDef();
    def.visionRadiusElmos = 140.0f;
    rm::unitdef::Weapon weapon;
    weapon.label = "test gun";
    weapon.role = rm::unitdef::WeaponRole::DirectFire;
    weapon.damage = rm::sim::magFromFloat(10.0f);
    weapon.maxRange = rm::sim::fxFromFloat(100.0f);
    weapon.minRange = rm::sim::fxFromFloat(minimumRange);
    weapon.rateOfFire = 1.0f;
    weapon.muzzleVelocityElmosPerSecond = 100.0f;
    def.weapons.push_back(weapon);
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
        const Command* current = roster.store.orders()[fighter.index].current();
        if (current != nullptr && current->target == enemy) {
            engaged = true;
            CHECK_FALSE(roster.store.motion()[fighter.index].moving);
            CHECK(current->targetX == rm::sim::fxFromFloat(500.0f));
            break;
        }
    }
    REQUIRE(engaged);

    roster.transform(enemy).z = rm::sim::fxFromFloat(400.0f);
    (void)rm::sim::tickSkirmish(roster.store, roster.catalog, match, terrain, roster.rate);
    REQUIRE(roster.store.orders()[fighter.index].current() != nullptr);
    CHECK(roster.store.orders()[fighter.index].current()->target.generation == 0);
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
    CHECK(roster.store.orders()[fighter.index].current()->target == enemyAir);
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
    CHECK(roster.store.orders()[fighter.index].current()->target.generation == 0);
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
    REQUIRE(roster.store.orders()[fighter.index].current()->target == enemy);

    roster.transform(enemy).x = rm::sim::fxFromFloat(400.0f);
    (void)rm::sim::tickSkirmish(roster.store, roster.catalog, match, terrain, roster.rate);

    REQUIRE(roster.store.orders()[fighter.index].current() != nullptr);
    CHECK(roster.store.orders()[fighter.index].current()->target.generation == 0);
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
    REQUIRE(roster.store.orders()[walker.index].size() == 2);

    Command third = patrol;
    third.queued = true;
    third.targetX = rm::sim::fxFromFloat(300.0f);
    third.targetZ = rm::sim::fxFromFloat(300.0f);
    REQUIRE(rm::sim::applyCommand(third, roster.store, roster.catalog, players, armies,
                                  terrain, grid, roster.rate));
    REQUIRE(roster.store.orders()[walker.index].size() == 3);

    rm::sim::Match match{.armies = armies, .economies = {}, .passability = grids,
                         .commandersEver = {}};
    std::vector<std::array<rm::sim::Fx, 2>> destinations;
    for (int tick = 0; tick < 1600 && destinations.size() < 4; ++tick) {
        const Command* current = roster.store.orders()[walker.index].current();
        REQUIRE(current != nullptr);
        const std::array<rm::sim::Fx, 2> destination{current->targetX, current->targetZ};
        if (destinations.empty() || destinations.back() != destination) {
            destinations.push_back(destination);
        }
        (void)rm::sim::tickSkirmish(roster.store, roster.catalog, match, terrain, roster.rate);
        CHECK(roster.store.orders()[walker.index].size() == 3);
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
    CHECK(*roster.store.orders()[walker.index].current() == moveTo(200.0f, 40.0f, walker));
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

    // The move has arrived, but this tick's table predates the newly registered product. The
    // build stays at the head rather than being dropped as though it had already run.
    roster.store.motion()[engineer.index].moving = false;
    const std::vector<const rm::sim::PassabilityGrid*> staleGrids{&closed};
    CHECK(rm::sim::advanceOrders(roster.store, roster.catalog, terrain, staleGrids, roster.rate,
                                 &building)
          == 0);
    CHECK(building.empty());
    REQUIRE(roster.store.orders()[engineer.index].current() != nullptr);
    CHECK(roster.store.orders()[engineer.index].current()->kind == CommandKind::Build);

    // On the next tick the product grid exists. The builder grid is deliberately closed, so a
    // successful start proves the deferred command selected productType's grid.
    const std::vector<const rm::sim::PassabilityGrid*> grids{&closed, &open};
    CHECK(rm::sim::advanceOrders(roster.store, roster.catalog, terrain, grids, roster.rate,
                                 &building)
          == 1);
    REQUIRE(building.size() == 1);
    CHECK(building.front().blueprintIndex == productType);
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
