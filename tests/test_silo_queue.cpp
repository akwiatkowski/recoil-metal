// The silo build queue — C-241's CAiSiloBuildImpl +0x20 list, the half of the
// counted-projectile contract the plain `stored < capacity` auto-refill could not express.
//
// The retail facts under test (`ART-E001`, C-081/C-241):
//   * the queue is ONE FIFO list per unit, tagged by silo slot (tactical 0, nuke 1);
//   * `SiloIsFull(type)` counts STORED plus QUEUED of that type, so a mid-build silo
//     refuses further entries;
//   * an idle silo (state 0, empty queue) re-queues itself — tactical first, the nuke
//     slot only when the tactical `SiloAddBuild` could not take;
//   * `IssueSiloBuildTactical`/`IssueSiloBuildNuke` reach the same `SiloAddBuild`, so a
//     player click and the auto-refill share one admission rule;
//   * only the queue HEAD runs an economy event — a unit builds one missile at a time;
//   * the auto-refill has an off switch (`SetAutoMode`, the order button's right-click).
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "core/map/HeightField.hpp"
#include "core/sim/Command.hpp"
#include "core/sim/Economy.hpp"
#include "core/sim/Pathfinding.hpp"
#include "core/sim/SaveState.hpp"
#include "core/sim/Skirmish.hpp"
#include "core/unit/UnitDef.hpp"

#include "support/TestRoster.hpp"

#include <vector>

namespace {

using rm::sim::Army;
using rm::sim::CommandIssue;
using rm::sim::CommandKind;
using rm::sim::Economy;
using rm::sim::Match;
using rm::sim::Player;
using rm::sim::SiloAmmo;
using rm::sim::SiloBuild;
using rm::sim::UnitId;
using rm::unitdef::UnitDef;

[[nodiscard]] rm::HeightField flatField() {
    rm::HeightField field;
    field.squaresX = 100;
    field.squaresZ = 100;
    field.baseHeight = 0.0f;
    field.heightScale = 1.0f;
    field.raw.assign(field.sampleCount(), std::uint16_t{0});
    return field;
}

[[nodiscard]] rm::sim::Resources res(float mass, float energy) {
    return {.mass = rm::sim::magFromFloat(mass), .energy = rm::sim::magFromFloat(energy)};
}

[[nodiscard]] Economy banked(float mass, float energy) {
    Economy economy;
    economy.storage = res(mass, energy);
    economy.stored = res(mass, energy);
    return economy;
}

/// A ten-tick, cheap-round silo record for `owner`'s slot. Build rate is folded into the
/// numbers already: 100 of build time at 10 a tick and a per-tick cost the bank meets.
[[nodiscard]] SiloAmmo record(UnitId owner, bool nuke, int capacity) {
    return rm::sim::makeSiloAmmo(owner, nuke ? 1u : 0u, nuke, capacity,
                                 res(100.0f, 1000.0f), rm::sim::magFromFloat(100.0f),
                                 rm::sim::magFromFloat(10.0f));
}

/// One beat of the silo half of a tick: what `tickSkirmish` does for one army when the
/// match carries the queue.
void beat(Economy& economy, std::vector<SiloAmmo>& ammo, std::vector<SiloBuild>& queue) {
    rm::sim::tickEconomy(economy, {}, {}, std::span<SiloAmmo>{ammo}, false, {},
                         rm::sim::kNoArmy, {}, {}, {}, &queue);
}

[[nodiscard]] UnitDef siloDef() {
    UnitDef def;
    def.name = "test_silo";
    def.categories = {"STRUCTURE"};
    rm::unitdef::Weapon launcher;
    launcher.label = "TacMissile";
    launcher.countedProjectile = true;
    launcher.manualFire = true;
    launcher.nukeWeapon = false;
    launcher.maxProjectileStorage = 2;
    def.weapons.push_back(launcher);
    return def;
}

} // namespace

TEST_CASE("an idle silo re-queues itself, tactical before nuke", "[silo][queue]") {
    // C-241's state 0: an empty queue on an auto-mode silo calls SiloAddBuild(tactical)
    // and only falls to the nuke slot when the tactical push cannot take.
    const UnitId owner{5, 1};
    Economy economy = banked(100000.0f, 1000000.0f);
    std::vector<SiloAmmo> ammo{record(owner, false, 2), record(owner, true, 1)};
    std::vector<SiloBuild> queue;

    beat(economy, ammo, queue);
    REQUIRE(queue.size() == 1);
    CHECK(queue.front().owner == owner);
    CHECK(queue.front().slot == 0);

    // With the tactical slot already counting a queued build toward its capacity the next
    // beats keep the head running — the nuke does not jump in beside it.
    beat(economy, ammo, queue);
    CHECK(queue.size() == 1);
    CHECK(queue.front().slot == 0);
}

TEST_CASE("a full tactical slot lets the auto-refill take the nuke", "[silo][queue]") {
    const UnitId owner{5, 1};
    Economy economy = banked(100000.0f, 1000000.0f);
    std::vector<SiloAmmo> ammo{record(owner, false, 1), record(owner, true, 1)};
    ammo[0].stored = 1;  // tactical full: stored + queued reaches capacity
    std::vector<SiloBuild> queue;

    beat(economy, ammo, queue);
    REQUIRE(queue.size() == 1);
    CHECK(queue.front().slot == 1);
}

TEST_CASE("a silo is full when stored plus queued reaches capacity", "[silo][queue]") {
    // C-241's SiloIsFull(type): the queued build counts, so a silo never over-queues —
    // one capacity, one in the tube, one in the queue, refused.
    const UnitId owner{5, 1};
    std::vector<SiloAmmo> ammo{record(owner, false, 2)};
    std::vector<SiloBuild> queue;

    REQUIRE(rm::sim::queueSiloBuild(queue, ammo, owner, 0));
    ammo[0].stored = 1;
    CHECK_FALSE(rm::sim::queueSiloBuild(queue, ammo, owner, 0));
    CHECK(queue.size() == 1);
}

TEST_CASE("queued builds run one at a time, head first", "[silo][queue]") {
    // One economy event per unit: two queued entries on one slot spend two productions,
    // not one doubled one.
    const UnitId owner{5, 1};
    Economy economy = banked(100000.0f, 1000000.0f);
    std::vector<SiloAmmo> ammo{record(owner, false, 2)};
    std::vector<SiloBuild> queue;
    REQUIRE(rm::sim::queueSiloBuild(queue, ammo, owner, 0));
    REQUIRE(rm::sim::queueSiloBuild(queue, ammo, owner, 0));

    for (int tick = 0; tick < 10; ++tick) beat(economy, ammo, queue);
    CHECK(ammo[0].stored == 1);
    REQUIRE(queue.size() == 1);

    for (int tick = 0; tick < 10; ++tick) beat(economy, ammo, queue);
    CHECK(ammo[0].stored == 2);
    CHECK(queue.empty());
    // A full silo asks nothing.
    const rm::sim::Resources after = economy.stored;
    beat(economy, ammo, queue);
    CHECK(economy.stored.mass == after.mass);
    CHECK(economy.stored.energy == after.energy);
}

TEST_CASE("a queued nuke waits behind the tactical head", "[silo][queue]") {
    // The queue is per UNIT: a nuke entry behind a tactical head does not run beside it.
    const UnitId owner{5, 1};
    Economy economy = banked(100000.0f, 1000000.0f);
    std::vector<SiloAmmo> ammo{record(owner, false, 2), record(owner, true, 2)};
    std::vector<SiloBuild> queue;
    REQUIRE(rm::sim::queueSiloBuild(queue, ammo, owner, 0));
    REQUIRE(rm::sim::queueSiloBuild(queue, ammo, owner, 1));

    for (int tick = 0; tick < 5; ++tick) beat(economy, ammo, queue);
    CHECK(ammo[0].elapsedTicks > 0);
    CHECK(ammo[1].elapsedTicks == 0);  // the nuke's event has not started

    for (int tick = 0; tick < 10; ++tick) beat(economy, ammo, queue);
    CHECK(ammo[0].stored == 1);
    // The nuke is now the head: either mid-build or already landed.
    CHECK(static_cast<rm::TickCount>(ammo[1].stored) + ammo[1].elapsedTicks > 0);
}

TEST_CASE("a promoted head bills its own beat, not the completing one", "[silo][queue]") {
    // One economy event per unit is also one event per BEAT (`C-081`): when the head
    // lands in the tube, the next entry becomes head on the NEXT beat — a grant drawn
    // in the completing beat was never counted in that beat's demand, and it would
    // land the follow-on build a tick early.
    const UnitId owner{5, 1};
    Economy economy = banked(100000.0f, 1000000.0f);
    std::vector<SiloAmmo> ammo{record(owner, false, 2), record(owner, true, 2)};
    std::vector<SiloBuild> queue;
    REQUIRE(rm::sim::queueSiloBuild(queue, ammo, owner, 0));
    REQUIRE(rm::sim::queueSiloBuild(queue, ammo, owner, 1));

    for (int tick = 0; tick < 10; ++tick) beat(economy, ammo, queue);
    CHECK(ammo[0].stored == 1);
    CHECK(ammo[0].elapsedTicks == 0);
    // The nuke is head now, but its event has not billed: no funding leaked into the
    // beat that spent the tactical entry.
    CHECK(ammo[1].elapsedTicks == 0);
    CHECK(ammo[1].delivered.mass == rm::sim::Mag{});
    CHECK(ammo[1].delivered.energy == rm::sim::Mag{});

    // Its own ten beats land it on schedule — the queue drains, nothing double-builds.
    for (int tick = 0; tick < 10; ++tick) beat(economy, ammo, queue);
    CHECK(ammo[1].stored == 1);
    CHECK(queue.empty());
}

TEST_CASE("auto-mode off leaves an idle silo empty; a manual build still runs",
          "[silo][queue]") {
    // Retail's right-click on the build button: SetAutoMode(false) stops the state-0
    // refill, and IssueSiloBuild* — the SAME SiloAddBuild — still admits a round.
    const UnitId owner{5, 1};
    Economy economy = banked(100000.0f, 1000000.0f);
    std::vector<SiloAmmo> ammo{record(owner, false, 1)};
    ammo[0].autoBuild = false;
    std::vector<SiloBuild> queue;

    beat(economy, ammo, queue);
    beat(economy, ammo, queue);
    CHECK(queue.empty());
    CHECK(ammo[0].elapsedTicks == 0);

    REQUIRE(rm::sim::queueSiloBuild(queue, ammo, owner, 0));
    for (int tick = 0; tick < 10; ++tick) beat(economy, ammo, queue);
    CHECK(ammo[0].stored == 1);
}

TEST_CASE("a silo-build command queues a round and a full silo refuses it",
          "[silo][queue][command]") {
    const rm::HeightField field = flatField();
    const rm::sim::Terrain terrain{field};
    const rm::sim::PassabilityGrid grid =
        rm::sim::buildPassability(field, 0.0f, 60.0f, 0.0f);

    rm::test::Roster roster;
    const rm::UnitTypeIndex siloType = roster.addType(siloDef());
    const UnitId silo = roster.add(siloType, 40.0f, 40.0f, 0, 500.0f);

    std::vector<SiloAmmo> ammo{record(silo, false, 1)};
    std::vector<SiloBuild> queue;
    const std::vector<Player> players{Player{.index = 0, .army = 0}};
    const std::vector<Army> armies = rm::sim::freeForAll(1);

    const auto issue = [&](std::uint32_t serial) {
        return rm::sim::applyCommand(
            CommandIssue{.source = 0,
                         .id = rm::commandId(0, serial),
                         .player = 0,
                         .kind = CommandKind::SiloBuildTactical,
                         .units = {silo}},
            roster.store, roster.catalog, players, armies, terrain,
            [&grid](UnitId) { return &grid; }, roster.rate, nullptr, nullptr, nullptr,
            nullptr, nullptr, {}, &ammo, &queue);
    };

    CHECK(issue(1).accepted.size() == 1);
    REQUIRE(queue.size() == 1);
    CHECK(queue.front().slot == 0);

    // Capacity one, the queued build already counting: the second click is refused.
    CHECK(issue(2).accepted.empty());
    CHECK(queue.size() == 1);
}

TEST_CASE("the silo auto toggle holds the refill without losing queued work",
          "[silo][queue][command]") {
    const rm::HeightField field = flatField();
    const rm::sim::Terrain terrain{field};
    const rm::sim::PassabilityGrid grid =
        rm::sim::buildPassability(field, 0.0f, 60.0f, 0.0f);

    rm::test::Roster roster;
    const rm::UnitTypeIndex siloType = roster.addType(siloDef());
    const UnitId silo = roster.add(siloType, 40.0f, 40.0f, 0, 500.0f);

    std::vector<SiloAmmo> ammo{record(silo, false, 1)};
    std::vector<SiloBuild> queue;
    const std::vector<Player> players{Player{.index = 0, .army = 0}};
    const std::vector<Army> armies = rm::sim::freeForAll(1);

    const auto toggle = [&](std::uint32_t serial) {
        return rm::sim::applyCommand(
            CommandIssue{.source = 0,
                         .id = rm::commandId(0, serial),
                         .player = 0,
                         .kind = CommandKind::ToggleSiloAuto,
                         .units = {silo}},
            roster.store, roster.catalog, players, armies, terrain,
            [&grid](UnitId) { return &grid; }, roster.rate, nullptr, nullptr, nullptr,
            nullptr, nullptr, {}, &ammo, &queue);
    };

    CHECK(toggle(1).accepted.size() == 1);
    CHECK_FALSE(ammo.front().autoBuild);

    // Auto off: no refill arrives. The toggle alone never bills.
    Economy economy = banked(100000.0f, 1000000.0f);
    beat(economy, ammo, queue);
    CHECK(queue.empty());

    // Back on, the next beat restocks.
    CHECK(toggle(2).accepted.size() == 1);
    CHECK(ammo.front().autoBuild);
    beat(economy, ammo, queue);
    REQUIRE(queue.size() == 1);
    CHECK(queue.front().slot == 0);

    // A unit with no silo record at all has no mode to toggle — same store, so its
    // generational handle cannot collide with the silo's.
    const rm::UnitTypeIndex tankType = roster.addType([] {
        UnitDef def;
        def.name = "test_tank";
        def.categories = {"LAND"};
        return def;
    }());
    const UnitId tank = roster.add(tankType, 60.0f, 60.0f, 0, 500.0f);
    const auto refused = rm::sim::applyCommand(
        CommandIssue{.source = 0,
                     .id = rm::commandId(0, 9),
                     .player = 0,
                     .kind = CommandKind::ToggleSiloAuto,
                     .units = {tank}},
        roster.store, roster.catalog, players, armies, terrain,
        [&grid](UnitId) { return &grid; }, roster.rate, nullptr, nullptr, nullptr,
        nullptr, nullptr, {}, &ammo, &queue);
    CHECK(refused.accepted.empty());
}

TEST_CASE("a dead silo leaves neither records nor queued builds", "[silo][queue]") {
    const rm::HeightField field = flatField();
    rm::test::Roster roster;
    const rm::UnitTypeIndex siloType = roster.addType(siloDef());
    const UnitId silo = roster.add(siloType, 40.0f, 40.0f, 0, 500.0f);

    std::vector<SiloAmmo> ammo{record(silo, false, 2)};
    std::vector<SiloBuild> queue;
    REQUIRE(rm::sim::queueSiloBuild(queue, ammo, silo, 0));

    std::vector<Army> armies = rm::sim::freeForAll(1);
    std::vector<Economy> economies(1, banked(100000.0f, 1000000.0f));
    const std::vector<int> commandersEver(1, 0);
    Match match{.armies = armies,
                .economies = economies,
                .siloAmmo = &ammo,
                .siloQueue = &queue,
                .commandersEver = commandersEver};

    roster.store.health()[silo.index].current = rm::sim::Mag{};

    rm::sim::tickSkirmish(roster.store, roster.catalog, match, rm::sim::Terrain{field});
    CHECK(ammo.empty());
    CHECK(queue.empty());
}

TEST_CASE("the silo queue and auto flag survive a save round-trip", "[silo][queue][save]") {
    rm::sim::SaveState state;
    const UnitId owner{5, 1};
    state.siloAmmo.push_back(record(owner, false, 2));
    state.siloAmmo.back().stored = 1;
    state.siloAmmo.back().autoBuild = false;
    state.siloQueue.push_back(SiloBuild{.owner = owner, .slot = 0});
    state.siloQueue.push_back(SiloBuild{.owner = owner, .slot = 1});

    const std::optional<rm::sim::SaveState> decoded =
        rm::sim::SaveState::decode(rm::sim::SaveState::encode(state));
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->siloAmmo.size() == 1);
    CHECK(decoded->siloAmmo.front().stored == 1);
    CHECK_FALSE(decoded->siloAmmo.front().autoBuild);
    REQUIRE(decoded->siloQueue.size() == 2);
    CHECK(decoded->siloQueue[0].owner == owner);
    CHECK(decoded->siloQueue[0].slot == 0);
    CHECK(decoded->siloQueue[1].slot == 1);
}
