// Player-perspective coverage for persist claims (see docs/fa-exe-analysis-plan.md).
// Filled by the coverage-test wave; each TEST_CASE cites its claim IDs.
//
// The persistence contract a player can observe: a saved match resumes to the
// same future (C-006/C-154), a command log replays to the same match
// (C-150/C-151), and target acquisition's tie-break is deterministic
// (C-107). The wire-format and ring-buffer internals are deliberately
// different from retail's and are not what these tests pin.
#include "app/Match.hpp"
#include "app/Interface.hpp"
#include "app/SceneBuild.hpp"
#include "core/data/MoveDef.hpp"
#include "core/map/Scmap.hpp"
#include "core/sim/Army.hpp"
#include "core/sim/Combat.hpp"
#include "core/sim/SaveState.hpp"
#include "core/sim/StateHash.hpp"
#include "support/FxMatchers.hpp"
#include "support/TestRoster.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <vector>
#include <unistd.h>

namespace {

rm::HeightField flatField() {
    rm::HeightField field;
    field.squaresX = 128;
    field.squaresZ = 128;
    field.heightScale = 1.0f;
    field.raw.assign(field.sampleCount(), std::uint16_t{0});
    return field;
}

// The same harness test_unit_scenarios.cpp uses: a scene with two armies, the
// player driving army 0, and spawn/runner helpers that go through the real
// catalog and command intake.
struct Scenario {
    rm::HeightField field;
    rm::app::UnitScene scene;
    rm::app::PassabilitySet passability;
    rm::vfs::Vfs content;

    explicit Scenario(rm::HeightField terrain = flatField())
        : field(std::move(terrain)), passability(field, false, 0) {
        scene.armies = rm::sim::freeForAll(2);
        scene.players = rm::sim::onePlayerPerArmy(2, 0);
        scene.playerArmy = 0;
        scene.economies.resize(2);
        scene.commandersEver.assign(2, 0);
    }

    rm::UnitTypeIndex registerType(const rm::unitdef::UnitDef& def) {
        scene.definitions.push_back(def);
        const auto type = scene.catalog.add(&scene.definitions.back(), rm::app::gAppTickRate);
        scene.setTypeTraits(type, rm::data::moveDefFor(def), def.meshToElmos);
        const std::string path = "/units/" + def.name + "/" + def.name + "_unit.bp";
        scene.setPathForType(type, path);
        scene.typeForBlueprint.emplace(path, type);
        return type;
    }

    rm::sim::UnitId spawn(const rm::unitdef::UnitDef& def, float x, float z, int army = 0) {
        const auto type = registerType(def);
        return scene.store.spawn({
            .type = type,
            .transform = {.x = rm::sim::fxFromFloat(x), .z = rm::sim::fxFromFloat(z)},
            .motion = rm::app::motionFor(def, army),
            .health = rm::sim::initialHealth(def.health),
        });
    }

    rm::app::MatchRunner runner() {
        auto result = rm::app::makeMatchRunner(scene, field, passability, content, {}, {});
        result.match.baseStorage = rm::app::kStartingStorage;
        result.scripts.clear();
        return result;
    }
};

/// A mobile combat unit for the scenario tests. `motion` must be Land — the
/// default None makes the traversal grid impassable and a move order completes
/// instantly without moving.
[[nodiscard]] rm::unitdef::UnitDef tankDef() {
    rm::unitdef::UnitDef def;
    def.name = "test_tank";
    def.categories = {"LAND", "MOBILE"};
    def.motion = rm::unitdef::MotionType::Land;
    def.speedElmosPerSecond = 30.0f;
    def.health = rm::sim::Mag::fromInt(100);
    return def;
}

/// An unarmed thing to shoot at, for the acquisition tests.
[[nodiscard]] rm::unitdef::UnitDef targetDef() {
    rm::unitdef::UnitDef def;
    def.name = "test_target";
    def.categories = {"LAND"};
    return def;
}

/// One direct-fire gun that can reach everything placed in these tests.
[[nodiscard]] rm::unitdef::Weapon directFire() {
    rm::unitdef::Weapon weapon;
    weapon.label = "test gun";
    weapon.role = rm::unitdef::WeaponRole::DirectFire;
    weapon.targetPriorities = {{"LAND"}};
    weapon.damage = rm::test::mag(10.0f);
    weapon.maxRange = rm::test::fx(300.0f);
    weapon.rateOfFire = 1.0f;
    weapon.muzzleVelocityElmosPerSecond = 100.0f;
    return weapon;
}

} // namespace

TEST_CASE("C-107: an exact distance tie falls to slot order, deterministically",
          "[fa-persist]") {
    // The claim's surviving half: candidates that compare equal on score are
    // decided by the grid's ascending-slot order, never by position or luck.
    // (3,4) and (4,3) are both exactly 5 elmos out — an exact tie in the
    // squared-distance score the acquisition pass compares.
    const std::vector<rm::sim::Army> armies = rm::sim::freeForAll(2);
    const rm::unitdef::Weapon weapon = directFire();

    rm::test::Roster first;
    const rm::UnitTypeIndex type = first.addType(targetDef());
    const rm::sim::UnitId a = first.add(type, 3.0f, 4.0f, 1, 100.0f);  // slot 0
    const rm::sim::UnitId b = first.add(type, 4.0f, 3.0f, 1, 100.0f);  // slot 1
    CHECK(rm::sim::nearestTarget(rm::test::at(0, 0, 0), 0, weapon,
                               first.store, armies, nullptr, &first.catalog) == a);

    // Swap the spawn order and the OTHER position wins: the tie-break follows
    // the slot, not the geometry.
    rm::test::Roster second;
    const rm::UnitTypeIndex type2 = second.addType(targetDef());
    const rm::sim::UnitId b2 = second.add(type2, 4.0f, 3.0f, 1, 100.0f);  // slot 0 now
    const rm::sim::UnitId a2 = second.add(type2, 3.0f, 4.0f, 1, 100.0f);
    CHECK(rm::sim::nearestTarget(rm::test::at(0, 0, 0), 0, weapon,
                               second.store, armies, nullptr, &second.catalog) == b2);
    (void)b;
    (void)a2;
}

TEST_CASE("C-107: an incumbent holds an exact tie but loses to a nearer enemy",
          "[fa-persist]") {
    // Stickiness is the player's "my tank keeps shooting what it was shooting":
    // the incumbent is seeded before the grid walk and only a STRICTLY better
    // score dislodges it — an equal score never does.
    const std::vector<rm::sim::Army> armies = rm::sim::freeForAll(2);
    const rm::unitdef::Weapon weapon = directFire();

    rm::test::Roster roster;
    const rm::UnitTypeIndex type = roster.addType(targetDef());
    const rm::sim::UnitId earlier = roster.add(type, 3.0f, 4.0f, 1, 100.0f);  // slot 0, dist 5
    const rm::sim::UnitId incumbent = roster.add(type, 4.0f, 3.0f, 1, 100.0f);  // slot 1, dist 5

    // Equal score: the incumbent wins over the earlier slot it would lose to
    // as a fresh candidate.
    CHECK(rm::sim::nearestTarget(rm::test::at(0, 0, 0), 0, weapon, roster.store, armies,
                                 nullptr, &roster.catalog, std::nullopt, incumbent)
          == incumbent);

    // A strictly nearer candidate dislodges it — stickiness is a tie-break,
    // not a lock.
    const rm::sim::UnitId nearer = roster.add(type, 0.0f, 4.0f, 1, 100.0f);  // dist 4
    CHECK(rm::sim::nearestTarget(rm::test::at(0, 0, 0), 0, weapon, roster.store, armies,
                                 nullptr, &roster.catalog, std::nullopt, incumbent)
          == nearer);
    (void)earlier;
}

TEST_CASE("C-006/C-154: a resumed save continues the identical match", "[fa-persist]") {
    // The player-facing determinism contract: save mid-order, resume, and the
    // continued run is the same match — the guarantee retail's beat checksums
    // exist to verify. Since v37 the path service's queues and in-flight flow
    // fields ride the save, so the hash stream itself must match tick by tick,
    // not just observable positions.
    Scenario live;
    const rm::unitdef::UnitDef tank = tankDef();
    const rm::sim::UnitId unit = live.spawn(tank, 300, 300);
    auto runner = live.runner();

    int tick = 0;
    REQUIRE(rm::app::issueMove(live.scene, unit, 0, static_cast<rm::TickIndex>(tick),
                               rm::sim::fxFromFloat(600.0f), rm::sim::fxFromFloat(600.0f)));
    // Walk partway, then save: the resume must continue mid-order, not restart.
    for (; tick < 40; ++tick) {
        (void)rm::app::advanceMatch(runner, tick, 0);
    }
    const auto at40 = live.scene.store.transforms()[unit.index];
    INFO("position at save: " << rm::sim::fxToFloat(at40.x) << ","
         << rm::sim::fxToFloat(at40.z));
    INFO("queue empty: " << live.scene.store.orders()[unit.index].empty());

    const auto saved = rm::sim::SaveState::decode(rm::sim::SaveState::encode({
        .tick = static_cast<rm::TickIndex>(tick),
        .random = runner.match.random.snapshot(),
        .pathServiceBeats = runner.pathService.serviceBeats(),
        .units = live.scene.store.snapshot(),
        .economyArmies = rm::sim::EconomyArmyState::capture(runner.match),
        .pathService = runner.pathService.snapshot()}));
    REQUIRE(saved);

    Scenario resumed;
    (void)resumed.registerType(tank);
    resumed.scene.store = rm::sim::UnitStore{saved->units};
    auto continued = resumed.runner();
    saved->economyArmies->restore(continued.match, resumed.scene.economies,
                                  resumed.scene.commandersEver);
    continued.match.random = rm::sim::RandomStream{saved->random};
    continued.pathService.restoreServiceBeats(saved->pathServiceBeats);
    // Requests rebind their movement grid through the same resolver the
    // command intake uses — the twin scenario's grid is content-identical,
    // so its fingerprint matches the saved one.
    const auto gridFor = [&](rm::sim::UnitId unit) -> std::shared_ptr<const rm::sim::PassabilityGrid> {
        if (!resumed.scene.store.alive(unit)) return nullptr;
        const auto type = static_cast<std::size_t>(resumed.scene.store.typeAt(unit.index));
        return std::shared_ptr<const rm::sim::PassabilityGrid>(
            &continued.passability.gridFor(resumed.scene, type), [](const rm::sim::PassabilityGrid*) {});
    };
    continued.pathService.restore(*saved->pathService, gridFor);
    continued.match.pathService = &continued.pathService;

    // Every subsequent tick must produce identical observable state on both
    // timelines — the resumed match is not "close", it is the same match.
    for (int step = 0; step < 300; ++step) {
        INFO("continuation tick " << tick);
        REQUIRE(rm::sim::hashMatch(live.scene.store, runner.match)
                == rm::sim::hashMatch(resumed.scene.store, continued.match));
        (void)rm::app::advanceMatch(runner, tick, 0);
        (void)rm::app::advanceMatch(continued, tick, 0);
        ++tick;
    }
}

TEST_CASE("C-006/C-154: an idle save resumes to an identical hash stream",
          "[fa-persist]") {
    // The hash-surface half of the contract: with no path work in flight, a
    // save captures everything the hash reads, so the resumed match hashes
    // identically tick by tick — the property a desync check relies on.
    Scenario live;
    const rm::unitdef::UnitDef tank = tankDef();
    (void)live.spawn(tank, 300, 300);
    auto runner = live.runner();

    int tick = 0;
    for (; tick < 40; ++tick) {
        (void)rm::app::advanceMatch(runner, tick, 0);
    }

    const auto saved = rm::sim::SaveState::decode(rm::sim::SaveState::encode({
        .tick = static_cast<rm::TickIndex>(tick),
        .random = runner.match.random.snapshot(),
        .pathServiceBeats = runner.pathService.serviceBeats(),
        .units = live.scene.store.snapshot(),
        .economyArmies = rm::sim::EconomyArmyState::capture(runner.match)}));
    REQUIRE(saved);

    Scenario resumed;
    (void)resumed.registerType(tank);
    resumed.scene.store = rm::sim::UnitStore{saved->units};
    auto continued = resumed.runner();
    saved->economyArmies->restore(continued.match, resumed.scene.economies,
                                  resumed.scene.commandersEver);
    continued.match.random = rm::sim::RandomStream{saved->random};
    continued.pathService.restoreServiceBeats(saved->pathServiceBeats);
    continued.match.pathService = &continued.pathService;

    for (int step = 0; step < 300; ++step) {
        INFO("continuation tick " << tick);
        REQUIRE(rm::sim::hashMatch(live.scene.store, runner.match)
                == rm::sim::hashMatch(resumed.scene.store, continued.match));
        (void)rm::app::advanceMatch(runner, tick, 0);
        (void)rm::app::advanceMatch(continued, tick, 0);
        ++tick;
    }
}

TEST_CASE("C-150/C-151: the recorded command log replays to the identical match",
          "[fa-persist]") {
    // §1.3's criterion, player-facing: the log a match leaves behind is the
    // match. Replaying it through the same intake produces the same final
    // state — the semantic CommandIssue stream is our counterpart of retail's
    // 24-opcode wire (the byte layout is a documented divergence).
    Scenario live;
    const rm::unitdef::UnitDef tank = tankDef();
    const rm::sim::UnitId unit = live.spawn(tank, 300, 300);
    auto runner = live.runner();

    int tick = 0;
    REQUIRE(rm::app::issueMove(live.scene, unit, 0, static_cast<rm::TickIndex>(tick),
                               rm::sim::fxFromFloat(600.0f), rm::sim::fxFromFloat(300.0f)));
    for (; tick < 60; ++tick) {
        (void)rm::app::advanceMatch(runner, tick, 0);
    }
    // A second order mid-run, so the log holds more than one record.
    REQUIRE(rm::app::issueMove(live.scene, unit, 0, static_cast<rm::TickIndex>(tick),
                               rm::sim::fxFromFloat(300.0f), rm::sim::fxFromFloat(600.0f)));
    for (; tick < 160; ++tick) {
        (void)rm::app::advanceMatch(runner, tick, 0);
    }
    REQUIRE(live.scene.commands.size() == 2);

    const auto path = std::filesystem::temp_directory_path()
        / ("rm-fa-persist-" + std::to_string(::getpid()) + ".commands");
    REQUIRE(rm::sim::writeCommandLog(live.scene.commands, path.string()));
    const auto log = rm::sim::readCommandLog(path.string());
    REQUIRE(log);
    REQUIRE(log->size() == live.scene.commands.size());

    Scenario replayed;
    (void)replayed.spawn(tank, 300, 300);
    auto playback = replayed.runner();
    playback.replay = &*log;
    for (int frame = 0; frame < tick; ++frame) {
        (void)rm::app::advanceMatch(playback, frame, 0);
    }
    CHECK(rm::sim::hashMatch(live.scene.store, runner.match)
          == rm::sim::hashMatch(replayed.scene.store, playback.match));
    std::filesystem::remove(path);
}
