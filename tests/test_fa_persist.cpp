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
    CHECK(rm::sim::nearestTarget(rm::test::at(0, 0, 0), 0, rm::UnitIndex{0}, weapon,
                               first.store, armies, nullptr, &first.catalog) == a);

    // Swap the spawn order and the OTHER position wins: the tie-break follows
    // the slot, not the geometry.
    rm::test::Roster second;
    const rm::UnitTypeIndex type2 = second.addType(targetDef());
    const rm::sim::UnitId b2 = second.add(type2, 4.0f, 3.0f, 1, 100.0f);  // slot 0 now
    const rm::sim::UnitId a2 = second.add(type2, 3.0f, 4.0f, 1, 100.0f);
    CHECK(rm::sim::nearestTarget(rm::test::at(0, 0, 0), 0, rm::UnitIndex{0}, weapon,
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
    CHECK(rm::sim::nearestTarget(rm::test::at(0, 0, 0), 0, rm::UnitIndex{0}, weapon, roster.store, armies,
                                 nullptr, &roster.catalog, std::nullopt, incumbent)
          == incumbent);

    // A strictly nearer candidate dislodges it — stickiness is a tie-break,
    // not a lock.
    const rm::sim::UnitId nearer = roster.add(type, 0.0f, 4.0f, 1, 100.0f);  // dist 4
    CHECK(rm::sim::nearestTarget(rm::test::at(0, 0, 0), 0, rm::UnitIndex{0}, weapon, roster.store, armies,
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

TEST_CASE("C-426: a save with intel configured resumes the recon picture", "[fa-persist]") {
    // C-426: the grids persist by reference and re-stamp from unit positions,
    // so the recon history (retained contacts, seen-ever latches, brownout
    // recovery) is what the save must carry. Save WITH it, restore it, and
    // the continued hash stream matches tick by tick.
    Scenario live;
    rm::unitdef::UnitDef dish = tankDef();
    dish.name = "test_dish";
    dish.visionRadiusElmos = 80.0f;  // 10 ogrids at the scale-2 vision grid (C-076)
    dish.radarRadiusElmos = 400.0f;  // 100 ogrids at the scale-4 radar grid (C-076)
    (void)live.spawn(dish, 300, 300, 0);
    rm::unitdef::UnitDef quiet = tankDef();
    quiet.name = "test_quiet";
    const rm::sim::UnitId enemy = live.spawn(quiet, 500, 300, 1);
    // Two alliances (free-for-all seats each army in its own), sized to the
    // field like the app's own configureIntel (SceneBuild.cpp).
    live.scene.intel.configure(2, rm::sim::fxFromFloat(live.field.widthElmos()),
                               rm::sim::fxFromFloat(live.field.depthElmos()),
                               rm::sim::VisionStyle::ForgedAlliance);
    auto runner = live.runner();

    int tick = 0;
    REQUIRE(rm::app::issueMove(live.scene, enemy, 0, static_cast<rm::TickIndex>(tick),
                               rm::sim::fxFromFloat(600.0f), rm::sim::fxFromFloat(300.0f)));
    for (; tick < 40; ++tick) {
        (void)rm::app::advanceMatch(runner, tick, 0);
    }
    // The enemy sat inside the dish's radar for the whole run: alliance 0
    // holds a retained contact the hash reads.
    REQUIRE_FALSE(runner.match.intel->retainedRadarContacts(0).empty());

    const auto saved = rm::sim::SaveState::decode(rm::sim::SaveState::encode({
        .tick = static_cast<rm::TickIndex>(tick),
        .random = runner.match.random.snapshot(),
        .pathServiceBeats = runner.pathService.serviceBeats(),
        .units = live.scene.store.snapshot(),
        .economyArmies = rm::sim::EconomyArmyState::capture(runner.match),
        .pathService = runner.pathService.snapshot(),
        .intel = runner.match.intel->snapshot()}));
    REQUIRE(saved);
    REQUIRE(saved->intel.has_value());

    Scenario resumed;
    (void)resumed.registerType(dish);
    (void)resumed.registerType(quiet);
    resumed.scene.store = rm::sim::UnitStore{saved->units};
    resumed.scene.intel.configure(2, rm::sim::fxFromFloat(resumed.field.widthElmos()),
                                  rm::sim::fxFromFloat(resumed.field.depthElmos()),
                                  rm::sim::VisionStyle::ForgedAlliance);
    auto continued = resumed.runner();
    saved->economyArmies->restore(continued.match, resumed.scene.economies,
                                  resumed.scene.commandersEver);
    continued.match.random = rm::sim::RandomStream{saved->random};
    continued.pathService.restoreServiceBeats(saved->pathServiceBeats);
    const auto gridFor = [&](rm::sim::UnitId unit) -> std::shared_ptr<const rm::sim::PassabilityGrid> {
        if (!resumed.scene.store.alive(unit)) return nullptr;
        const auto type = static_cast<std::size_t>(resumed.scene.store.typeAt(unit.index));
        return std::shared_ptr<const rm::sim::PassabilityGrid>(
            &continued.passability.gridFor(resumed.scene, type), [](const rm::sim::PassabilityGrid*) {});
    };
    continued.pathService.restore(*saved->pathService, gridFor);
    continued.match.pathService = &continued.pathService;
    continued.match.intel->restore(*saved->intel);
    // C-426: the grids persist by reference, so the restore clears them and
    // the next update re-stamps from the restored units — prime once so the
    // first hash compares re-stamped grids, not empty ones.
    continued.match.intel->update(resumed.scene.store, resumed.scene.catalog,
                                  continued.match.armies, nullptr);

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
TEST_CASE("C-552: an intel save resumes with its emitter placements, no priming tick", "[fa-persist]") {
    // The pool-by-pool envelope's emitter slice: the save carries each slot's
    // placement (square, alliance, disable mask, cheat, layer) and its stamped
    // squares, so the restore re-adds the grids directly. No priming `update`
    // is owed — and none may run, because an extra tick would bank brownout
    // recovery and age retained contacts past what the live match holds.
    Scenario live;
    rm::unitdef::UnitDef dish = tankDef();
    dish.name = "test_dish";
    dish.visionRadiusElmos = 80.0f;  // 10 ogrids at the scale-2 vision grid (C-076)
    dish.radarRadiusElmos = 400.0f;  // 100 ogrids at the scale-4 radar grid (C-076)
    dish.sonarRadiusElmos = 200.0f;
    (void)live.spawn(dish, 300, 300, 0);
    rm::unitdef::UnitDef quiet = tankDef();
    quiet.name = "test_quiet";
    const rm::sim::UnitId enemy = live.spawn(quiet, 500, 300, 1);
    live.scene.intel.configure(2, rm::sim::fxFromFloat(live.field.widthElmos()),
                               rm::sim::fxFromFloat(live.field.depthElmos()),
                               rm::sim::VisionStyle::ForgedAlliance);
    auto runner = live.runner();

    int tick = 0;
    REQUIRE(rm::app::issueMove(live.scene, enemy, 0, static_cast<rm::TickIndex>(tick),
                               rm::sim::fxFromFloat(600.0f), rm::sim::fxFromFloat(300.0f)));
    for (; tick < 40; ++tick) {
        (void)rm::app::advanceMatch(runner, tick, 0);
    }
    REQUIRE_FALSE(runner.match.intel->retainedRadarContacts(0).empty());

    const auto saved = rm::sim::SaveState::decode(rm::sim::SaveState::encode({
        .tick = static_cast<rm::TickIndex>(tick),
        .random = runner.match.random.snapshot(),
        .pathServiceBeats = runner.pathService.serviceBeats(),
        .units = live.scene.store.snapshot(),
        .economyArmies = rm::sim::EconomyArmyState::capture(runner.match),
        .pathService = runner.pathService.snapshot(),
        .intel = runner.match.intel->snapshot()}));
    REQUIRE(saved);
    REQUIRE(saved->intel.has_value());

    Scenario resumed;
    (void)resumed.registerType(dish);
    (void)resumed.registerType(quiet);
    resumed.scene.store = rm::sim::UnitStore{saved->units};
    resumed.scene.intel.configure(2, rm::sim::fxFromFloat(resumed.field.widthElmos()),
                                  rm::sim::fxFromFloat(resumed.field.depthElmos()),
                                  rm::sim::VisionStyle::ForgedAlliance);
    auto continued = resumed.runner();
    saved->economyArmies->restore(continued.match, resumed.scene.economies,
                                  resumed.scene.commandersEver);
    continued.match.random = rm::sim::RandomStream{saved->random};
    continued.pathService.restoreServiceBeats(saved->pathServiceBeats);
    const auto gridFor = [&](rm::sim::UnitId unit) -> std::shared_ptr<const rm::sim::PassabilityGrid> {
        if (!resumed.scene.store.alive(unit)) return nullptr;
        const auto type = static_cast<std::size_t>(resumed.scene.store.typeAt(unit.index));
        return std::shared_ptr<const rm::sim::PassabilityGrid>(
            &continued.passability.gridFor(resumed.scene, type), [](const rm::sim::PassabilityGrid*) {});
    };
    continued.pathService.restore(*saved->pathService, gridFor);
    continued.match.pathService = &continued.pathService;
    continued.match.intel->restore(*saved->intel);
    // No priming update: the grids are already re-added from the saved
    // placements, so radar covers the enemy's ground on the first read.
    const rm::sim::Transform& foe = resumed.scene.store.transforms()[enemy.index];
    CHECK(continued.match.intel->sees(0, rm::sim::IntelKind::Radar, foe.x, foe.z));

    for (int step = 0; step < 300; ++step) {
        INFO("continuation tick " << tick);
        REQUIRE(rm::sim::hashMatch(live.scene.store, runner.match)
                == rm::sim::hashMatch(resumed.scene.store, continued.match));
        (void)rm::app::advanceMatch(runner, tick, 0);
        (void)rm::app::advanceMatch(continued, tick, 0);
        ++tick;
    }
}

TEST_CASE("C-552: a pre-placement save still decodes and re-stamps", "[fa-persist]") {
    // Old readers keep the C-426 contract: no placements ride a v58 save (the
    // last version without them — see SaveState.cpp's kVersion58), so the
    // restore clears the grids and one priming update re-stamps from positions.
    Scenario live;
    rm::unitdef::UnitDef dish = tankDef();
    dish.name = "test_dish";
    dish.visionRadiusElmos = 80.0f;
    dish.radarRadiusElmos = 400.0f;
    (void)live.spawn(dish, 300, 300, 0);
    rm::unitdef::UnitDef quiet = tankDef();
    quiet.name = "test_quiet";
    const rm::sim::UnitId enemy = live.spawn(quiet, 500, 300, 1);
    live.scene.intel.configure(2, rm::sim::fxFromFloat(live.field.widthElmos()),
                               rm::sim::fxFromFloat(live.field.depthElmos()),
                               rm::sim::VisionStyle::ForgedAlliance);
    auto runner = live.runner();

    int tick = 0;
    REQUIRE(rm::app::issueMove(live.scene, enemy, 0, static_cast<rm::TickIndex>(tick),
                               rm::sim::fxFromFloat(600.0f), rm::sim::fxFromFloat(300.0f)));
    for (; tick < 40; ++tick) {
        (void)rm::app::advanceMatch(runner, tick, 0);
    }
    REQUIRE_FALSE(runner.match.intel->retainedRadarContacts(0).empty());

    const auto saved = rm::sim::SaveState::decode(rm::sim::SaveState::encodeAtVersion({
        .tick = static_cast<rm::TickIndex>(tick),
        .random = runner.match.random.snapshot(),
        .pathServiceBeats = runner.pathService.serviceBeats(),
        .units = live.scene.store.snapshot(),
        .economyArmies = rm::sim::EconomyArmyState::capture(runner.match),
        .pathService = runner.pathService.snapshot(),
        .intel = runner.match.intel->snapshot()}, 58));
    REQUIRE(saved);
    REQUIRE(saved->intel.has_value());

    Scenario resumed;
    (void)resumed.registerType(dish);
    (void)resumed.registerType(quiet);
    resumed.scene.store = rm::sim::UnitStore{saved->units};
    resumed.scene.intel.configure(2, rm::sim::fxFromFloat(resumed.field.widthElmos()),
                                  rm::sim::fxFromFloat(resumed.field.depthElmos()),
                                  rm::sim::VisionStyle::ForgedAlliance);
    auto continued = resumed.runner();
    saved->economyArmies->restore(continued.match, resumed.scene.economies,
                                  resumed.scene.commandersEver);
    continued.match.random = rm::sim::RandomStream{saved->random};
    continued.pathService.restoreServiceBeats(saved->pathServiceBeats);
    const auto gridFor = [&](rm::sim::UnitId unit) -> std::shared_ptr<const rm::sim::PassabilityGrid> {
        if (!resumed.scene.store.alive(unit)) return nullptr;
        const auto type = static_cast<std::size_t>(resumed.scene.store.typeAt(unit.index));
        return std::shared_ptr<const rm::sim::PassabilityGrid>(
            &continued.passability.gridFor(resumed.scene, type), [](const rm::sim::PassabilityGrid*) {});
    };
    continued.pathService.restore(*saved->pathService, gridFor);
    continued.match.pathService = &continued.pathService;
    continued.match.intel->restore(*saved->intel);
    // The legacy path: grids persist by reference (C-426), so one priming
    // update re-stamps them from the restored units before hashing.
    continued.match.intel->update(resumed.scene.store, resumed.scene.catalog,
                                  continued.match.armies, nullptr);

    for (int step = 0; step < 300; ++step) {
        INFO("continuation tick " << tick);
        REQUIRE(rm::sim::hashMatch(live.scene.store, runner.match)
                == rm::sim::hashMatch(resumed.scene.store, continued.match));
        (void)rm::app::advanceMatch(runner, tick, 0);
        (void)rm::app::advanceMatch(continued, tick, 0);
        ++tick;
    }
}

TEST_CASE("C-552/v60: an intel save resumes under its saved coverage style", "[fa-persist]") {
    // The v60 style byte: a Recoil match raycasts sight and radar against
    // the ground, so a resume decoded into a ForgedAlliance-configured
    // object must still read the saved rule, not the configured one.
    Scenario live;
    rm::unitdef::UnitDef dish = tankDef();
    dish.name = "test_dish";
    dish.visionRadiusElmos = 80.0f;
    dish.radarRadiusElmos = 400.0f;
    dish.sonarRadiusElmos = 200.0f;
    dish.sizeYElmos = 60.0f;  // the sensor mount: Recoil reads eye level, not the ground
    (void)live.spawn(dish, 300, 300, 0);
    rm::unitdef::UnitDef quiet = tankDef();
    quiet.name = "test_quiet";
    quiet.sizeYElmos = 60.0f;
    (void)live.spawn(quiet, 500, 300, 1);
    live.scene.intel.configure(2, rm::sim::fxFromFloat(live.field.widthElmos()),
                               rm::sim::fxFromFloat(live.field.depthElmos()),
                               rm::sim::VisionStyle::Recoil);
    auto runner = live.runner();
    runner.match.intel = &live.scene.intel;
    for (int tick = 0; tick < 40; ++tick) {
        runner.match.intel->update(live.scene.store, live.scene.catalog, runner.match.armies,
                                   nullptr);
    }
    REQUIRE_FALSE(runner.match.intel->retainedRadarContacts(0).empty());

    const auto saved = rm::sim::SaveState::decode(rm::sim::SaveState::encode({
        .tick = 40,
        .random = runner.match.random.snapshot(),
        .pathServiceBeats = runner.pathService.serviceBeats(),
        .units = live.scene.store.snapshot(),
        .economyArmies = rm::sim::EconomyArmyState::capture(runner.match),
        .pathService = runner.pathService.snapshot(),
        .intel = runner.match.intel->snapshot()}));
    REQUIRE(saved);
    REQUIRE(saved->intel.has_value());
    CHECK(saved->intel->hasStyle);
    CHECK(saved->intel->style == rm::sim::VisionStyle::Recoil);

    Scenario resumed;
    (void)resumed.registerType(dish);
    (void)resumed.registerType(quiet);
    resumed.scene.store = rm::sim::UnitStore{saved->units};
    resumed.scene.intel.configure(2, rm::sim::fxFromFloat(resumed.field.widthElmos()),
                                  rm::sim::fxFromFloat(resumed.field.depthElmos()),
                                  rm::sim::VisionStyle::ForgedAlliance);
    resumed.scene.intel.restore(*saved->intel);
    CHECK(resumed.scene.intel.style() == rm::sim::VisionStyle::Recoil);
}
TEST_CASE("C-279/v63: a sonar blip's sense survives a save", "[fa-persist]") {
    // The v63 retained-kind byte: a dead sonar source resumes as Sonar, not
    // radar — the sense is knowledge, and the save carries it.
    Scenario live;
    rm::unitdef::UnitDef ear = tankDef();
    ear.name = "test_ear";
    ear.sonarRadiusElmos = 400.0f;
    (void)live.spawn(ear, 300, 300, 0);
    rm::unitdef::UnitDef hull = tankDef();
    hull.name = "test_hull";
    hull.motion = rm::unitdef::MotionType::SurfacingSub;
    const rm::sim::UnitId diver = live.spawn(hull, 500, 300, 1);
    live.scene.store.motion()[diver.index].submerged = true;
    live.scene.intel.configure(2, rm::sim::fxFromFloat(live.field.widthElmos()),
                               rm::sim::fxFromFloat(live.field.depthElmos()),
                               rm::sim::VisionStyle::ForgedAlliance);
    auto runner = live.runner();
    runner.match.intel = &live.scene.intel;
    runner.match.intel->update(live.scene.store, live.scene.catalog, runner.match.armies,
                               nullptr);
    REQUIRE(rm::sim::contactKindForUnit(0, diver.index, live.scene.store, live.scene.catalog,
                                         runner.match.armies, *runner.match.intel)
            == rm::sim::ContactKind::Sonar);
    live.scene.store.kill(diver);
    runner.match.intel->update(live.scene.store, live.scene.catalog, runner.match.armies,
                               nullptr);
    REQUIRE_FALSE(runner.match.intel->retainedRadarContacts(0).empty());
    REQUIRE(runner.match.intel->retainedRadarContacts(0).front().sonar);

    const auto saved = rm::sim::SaveState::decode(rm::sim::SaveState::encode({
        .tick = 2,
        .random = runner.match.random.snapshot(),
        .pathServiceBeats = runner.pathService.serviceBeats(),
        .units = live.scene.store.snapshot(),
        .economyArmies = rm::sim::EconomyArmyState::capture(runner.match),
        .pathService = runner.pathService.snapshot(),
        .intel = runner.match.intel->snapshot()}));
    REQUIRE(saved);
    REQUIRE(saved->intel.has_value());
    CHECK(saved->intel->retained.front().front().sonar);
}

TEST_CASE("C-552/v60: the playable rect rides the economy save", "[fa-persist]") {
    // The v60 rect: automatic acquisition's scenario bounds gate targeting,
    // so a resume restores them onto the match rather than dropping back to
    // unrestricted scenes.
    Scenario live;
    auto runner = live.runner();
    runner.match.playableRect = rm::sim::PlayableRect{.minX = rm::sim::fxFromFloat(100.0f),
                                                     .maxX = rm::sim::fxFromFloat(900.0f),
                                                     .minZ = rm::sim::fxFromFloat(100.0f),
                                                     .maxZ = rm::sim::fxFromFloat(900.0f)};
    const auto saved = rm::sim::SaveState::decode(rm::sim::SaveState::encode({
        .tick = 1,
        .random = runner.match.random.snapshot(),
        .pathServiceBeats = runner.pathService.serviceBeats(),
        .units = live.scene.store.snapshot(),
        .economyArmies = rm::sim::EconomyArmyState::capture(runner.match),
        .pathService = runner.pathService.snapshot()}));
    REQUIRE(saved);
    REQUIRE(saved->economyArmies.has_value());
    REQUIRE(saved->economyArmies->playableRect.has_value());
    CHECK(saved->economyArmies->playableRect->minX == rm::sim::fxFromFloat(100.0f));
    CHECK(saved->economyArmies->playableRect->maxZ == rm::sim::fxFromFloat(900.0f));

    Scenario resumed;
    auto continued = resumed.runner();
    REQUIRE_FALSE(continued.match.playableRect.has_value());
    saved->economyArmies->restore(continued.match, resumed.scene.economies,
                                  resumed.scene.commandersEver);
    REQUIRE(continued.match.playableRect.has_value());
    CHECK(continued.match.playableRect->maxX == rm::sim::fxFromFloat(900.0f));

    // A pre-v60 save carries no rect — the restore leaves the caller's
    // setup untouched rather than clearing bounds it configured.
    const auto legacy = rm::sim::SaveState::decode(rm::sim::SaveState::encodeAtVersion(
        {.tick = 1,
         .random = runner.match.random.snapshot(),
         .pathServiceBeats = runner.pathService.serviceBeats(),
         .units = live.scene.store.snapshot(),
         .economyArmies = rm::sim::EconomyArmyState::capture(runner.match),
         .pathService = runner.pathService.snapshot()},
        59));
    REQUIRE(legacy);
    REQUIRE(legacy->economyArmies.has_value());
    CHECK_FALSE(legacy->economyArmies->playableRect.has_value());
}

// v64: caller-owned standing orders ride the save — factory rally points by
// builder slot, auto-expand flags with retry/refusal state. A pre-v64 save
// carries none, and the restore leaves the caller's setup untouched.
TEST_CASE("v64: standing orders survive a save round-trip", "[fa-persist]") {
    Scenario live;
    auto runner = live.runner();
    rm::sim::SaveState::StandingOrders orders;
    orders.rally.push_back({.slot = 3, .x = rm::sim::fxFromFloat(400.0f),
                            .z = rm::sim::fxFromFloat(200.0f)});
    rm::sim::SaveState::AutoExpandOrder expand;
    expand.unit = rm::sim::UnitId{.index = 1, .generation = 2};
    expand.retryAt = 42;
    expand.refused.push_back({.x = rm::sim::fxFromFloat(100.0f),
                              .y = rm::sim::fxFromFloat(0.0f),
                              .z = rm::sim::fxFromFloat(200.0f),
                              .until = 99});
    expand.hasLastSite = true;
    expand.lastSite = {rm::sim::fxFromFloat(300.0f), rm::sim::fxFromFloat(0.0f),
                       rm::sim::fxFromFloat(400.0f)};
    orders.expand.push_back(expand);
    const auto saved = rm::sim::SaveState::decode(rm::sim::SaveState::encode({
        .tick = 1,
        .random = runner.match.random.snapshot(),
        .pathServiceBeats = runner.pathService.serviceBeats(),
        .units = live.scene.store.snapshot(),
        .economyArmies = rm::sim::EconomyArmyState::capture(runner.match),
        .pathService = runner.pathService.snapshot(),
        .standingOrders = orders}));
    REQUIRE(saved);
    REQUIRE(saved->standingOrders.has_value());
    REQUIRE(saved->standingOrders->rally.size() == 1);
    CHECK(saved->standingOrders->rally[0].slot == 3);
    CHECK(saved->standingOrders->rally[0].x == rm::sim::fxFromFloat(400.0f));
    CHECK(saved->standingOrders->rally[0].z == rm::sim::fxFromFloat(200.0f));
    REQUIRE(saved->standingOrders->expand.size() == 1);
    const auto& back = saved->standingOrders->expand[0];
    CHECK(back.unit.index == 1);
    CHECK(back.unit.generation == 2);
    CHECK(back.retryAt == 42);
    REQUIRE(back.refused.size() == 1);
    CHECK(back.refused[0].x == rm::sim::fxFromFloat(100.0f));
    CHECK(back.refused[0].until == 99);
    CHECK(back.hasLastSite);
    CHECK(back.lastSite[0] == rm::sim::fxFromFloat(300.0f));

    const auto legacy = rm::sim::SaveState::decode(rm::sim::SaveState::encodeAtVersion(
        {.tick = 1,
         .random = runner.match.random.snapshot(),
         .pathServiceBeats = runner.pathService.serviceBeats(),
         .units = live.scene.store.snapshot(),
         .economyArmies = rm::sim::EconomyArmyState::capture(runner.match),
         .pathService = runner.pathService.snapshot()},
        63));
    REQUIRE(legacy);
    CHECK_FALSE(legacy->standingOrders.has_value());
}

// The app half of v64: capture lifts rally points and auto-expand flags off
// the live scene/runner, and restore drops entries naming dead slots while
// keeping the live ones — a mid-combat app save resumes standing orders.
TEST_CASE("v64: standing orders capture and restore onto the scene", "[fa-persist]") {
    Scenario live;
    rm::unitdef::UnitDef factory;
    factory.name = "test_factory";
    factory.categories = {"FACTORY", "STRUCTURE"};
    const rm::sim::UnitId builder = live.spawn(factory, 300, 300, 0);
    rm::unitdef::UnitDef engineer;
    engineer.name = "test_engineer";
    engineer.categories = {"ENGINEER", "MOBILE", "LAND"};
    engineer.motion = rm::unitdef::MotionType::Land;
    engineer.speedElmosPerSecond = 10.0f;
    const rm::sim::UnitId worker = live.spawn(engineer, 400, 400, 0);
    auto runner = live.runner();
    live.scene.rallyPoints.emplace(builder.index, std::array<float, 2>{500.0f, 500.0f});
    runner.autoExpanders.push_back(rm::app::AutoExpander{.unit = worker, .retryAt = 7});

    const rm::sim::SaveState::StandingOrders captured =
        rm::app::captureStandingOrders(runner);
    REQUIRE(captured.rally.size() == 1);
    REQUIRE(captured.expand.size() == 1);

    Scenario resumed;
    (void)resumed.registerType(factory);
    (void)resumed.registerType(engineer);
    const rm::sim::UnitId keptFactory = resumed.spawn(factory, 300, 300, 0);
    const rm::sim::UnitId keptWorker = resumed.spawn(engineer, 400, 400, 0);
    auto continued = resumed.runner();
    rm::app::restoreStandingOrders(continued, captured);
    CHECK(continued.scene.rallyPoints.size() == 1);
    CHECK(continued.scene.rallyPoints.at(keptFactory.index)[0] == 500.0f);
    REQUIRE(continued.autoExpanders.size() == 1);
    CHECK(continued.autoExpanders[0].unit == keptWorker);
    CHECK(continued.autoExpanders[0].retryAt == 7);

    // Kill both units: the restore drops their entries instead of
    // resurrecting standing orders for the dead.
    resumed.scene.store.kill(keptFactory);
    resumed.scene.store.kill(keptWorker);
    rm::app::restoreStandingOrders(continued, captured);
    CHECK(continued.scene.rallyPoints.empty());
    CHECK(continued.autoExpanders.empty());
}
// v65: the scripted opponent's one-shot wave flag rides the save — one byte
// per army (`rm::sim::Opponent::attackLaunched`, the `launchesAttack`
// once-only gate). A pre-v65 save carries none, and the wave state decodes
// empty — the match resumes with the wave unlaunched, as before.
TEST_CASE("v65: scripted wave flags survive a save round-trip", "[fa-persist]") {
    Scenario live;
    auto runner = live.runner();
    const auto saved = rm::sim::SaveState::decode(rm::sim::SaveState::encode({
        .tick = 1,
        .random = runner.match.random.snapshot(),
        .pathServiceBeats = runner.pathService.serviceBeats(),
        .units = live.scene.store.snapshot(),
        .economyArmies = rm::sim::EconomyArmyState::capture(runner.match),
        .pathService = runner.pathService.snapshot(),
        .opponentWaves = std::vector<std::uint8_t>{1, 0}}));
    REQUIRE(saved);
    REQUIRE(saved->opponentWaves.has_value());
    REQUIRE(saved->opponentWaves->size() == 2);
    CHECK((*saved->opponentWaves)[0] == 1);
    CHECK((*saved->opponentWaves)[1] == 0);

    const auto legacy = rm::sim::SaveState::decode(rm::sim::SaveState::encodeAtVersion(
        {.tick = 1,
         .random = runner.match.random.snapshot(),
         .pathServiceBeats = runner.pathService.serviceBeats(),
         .units = live.scene.store.snapshot(),
         .economyArmies = rm::sim::EconomyArmyState::capture(runner.match),
         .pathService = runner.pathService.snapshot()},
        64));
    REQUIRE(legacy);
    CHECK_FALSE(legacy->opponentWaves.has_value());
}

// The app half of v65: capture lifts the one-shot wave flags off the live
// runner (a resumed match must not re-fire an attack it already launched),
// and restore re-seats them onto scripted opponents only — foreign
// implementations keep their own state, dead detail below the port.
TEST_CASE("v65: wave flags capture and restore onto the runner", "[fa-persist]") {
    Scenario live;
    auto runner = live.runner();
    // The harness silences opponents for determinism; re-seat them so the
    // wave flags have owners to capture from.
    runner.scripts = rm::app::makeScriptedOpponents(live.scene.armies.size());
    REQUIRE(runner.scripts.size() == 2);
    auto* first = dynamic_cast<rm::ai::ScriptedOpponent*>(runner.scripts[0].get());
    REQUIRE(first != nullptr);
    first->setAttackLaunched();
    const std::vector<std::uint8_t> captured = rm::app::captureOpponentWaves(runner);
    REQUIRE(captured.size() == 2);
    CHECK(captured[0] == 1);
    CHECK(captured[1] == 0);

    Scenario resumed;
    auto continued = resumed.runner();
    continued.scripts = rm::app::makeScriptedOpponents(resumed.scene.armies.size());
    REQUIRE(continued.scripts.size() == 2);
    rm::app::restoreOpponentWaves(continued, captured);
    CHECK(continued.scripts[0]->attackLaunched());
    CHECK_FALSE(continued.scripts[1]->attackLaunched());
}

// v67: submitted-but-unapplied command input rides the save — a mid-combat
// app save resumes the player's in-flight click instead of dropping it.
// A pre-v67 save carries none, and the restore re-queues without minting
// new IDs (the allocator state already rode the unit snapshot).
TEST_CASE("v67: pending command input survives a save round-trip", "[fa-persist]") {
    Scenario live;
    const rm::sim::UnitId unit = live.spawn(tankDef(), 300, 300, 0);
    auto runner = live.runner();
    rm::sim::CommandIssue issue;
    issue.tick = 5;
    issue.phase = rm::sim::CommandPhase::PreTick;
    issue.source = 0;
    issue.player = 0;
    issue.kind = rm::sim::CommandKind::Move;
    issue.units = {unit};
    issue.targetX = rm::sim::fxFromFloat(600.0f);
    issue.targetZ = rm::sim::fxFromFloat(600.0f);
    REQUIRE(live.scene.commandInput.submit(issue, live.scene.store).has_value());
    const auto saved = rm::sim::SaveState::decode(rm::sim::SaveState::encode({
        .tick = 5,
        .random = runner.match.random.snapshot(),
        .pathServiceBeats = runner.pathService.serviceBeats(),
        .units = live.scene.store.snapshot(),
        .economyArmies = rm::sim::EconomyArmyState::capture(runner.match),
        .pathService = runner.pathService.snapshot(),
        .pendingInput = std::vector<rm::sim::CommandIssue>(
            live.scene.commandInput.pending().begin(),
            live.scene.commandInput.pending().end())}));
    REQUIRE(saved);
    REQUIRE(saved->pendingInput.has_value());
    REQUIRE(saved->pendingInput->size() == 1);
    const rm::sim::CommandIssue& back = saved->pendingInput->front();
    CHECK(back.tick == 5);
    CHECK(back.kind == rm::sim::CommandKind::Move);
    REQUIRE(back.units.size() == 1);
    CHECK(back.units[0] == unit);
    CHECK(back.targetX == rm::sim::fxFromFloat(600.0f));
    CHECK(back.targetZ == rm::sim::fxFromFloat(600.0f));

    rm::sim::CommandBuffer resumed;
    resumed.restorePending(*saved->pendingInput);
    const std::vector<rm::sim::CommandIssue> due =
        resumed.take(5, rm::sim::CommandPhase::PreTick);
    REQUIRE(due.size() == 1);
    CHECK(due[0].id == back.id);
    CHECK(resumed.empty());

    const auto legacy = rm::sim::SaveState::decode(rm::sim::SaveState::encodeAtVersion(
        {.tick = 5,
         .random = runner.match.random.snapshot(),
         .pathServiceBeats = runner.pathService.serviceBeats(),
         .units = live.scene.store.snapshot(),
         .economyArmies = rm::sim::EconomyArmyState::capture(runner.match),
         .pathService = runner.pathService.snapshot()},
        66));
    REQUIRE(legacy);
    CHECK_FALSE(legacy->pendingInput.has_value());
}
// The app half of v67: capture lifts submitted-but-unapplied input off the
// live scene's buffer, and restore re-queues it verbatim — a mid-combat app
// save resumes the player's in-flight click instead of dropping it.
TEST_CASE("v67: pending input capture and restore onto the scene", "[fa-persist]") {
    Scenario live;
    const rm::sim::UnitId unit = live.spawn(tankDef(), 300, 300, 0);
    auto runner = live.runner();
    rm::sim::CommandIssue issue;
    issue.tick = 5;
    issue.phase = rm::sim::CommandPhase::PreTick;
    issue.source = 0;
    issue.player = 0;
    issue.kind = rm::sim::CommandKind::Move;
    issue.units = {unit};
    issue.targetX = rm::sim::fxFromFloat(600.0f);
    issue.targetZ = rm::sim::fxFromFloat(600.0f);
    REQUIRE(live.scene.commandInput.submit(issue, live.scene.store).has_value());
    const std::vector<rm::sim::CommandIssue> captured =
        rm::app::capturePendingInput(runner);
    REQUIRE(captured.size() == 1);
    CHECK(captured[0].tick == 5);
    CHECK(captured[0].kind == rm::sim::CommandKind::Move);

    Scenario resumed;
    const rm::sim::UnitId kept = resumed.spawn(tankDef(), 300, 300, 0);
    auto continued = resumed.runner();
    REQUIRE(continued.scene.commandInput.empty());
    rm::app::restorePendingInput(continued, captured);
    REQUIRE(continued.scene.commandInput.size() == 1);
    const std::vector<rm::sim::CommandIssue> due =
        continued.scene.commandInput.take(5, rm::sim::CommandPhase::PreTick);
    REQUIRE(due.size() == 1);
    CHECK(due[0].id == captured[0].id);
    CHECK(due[0].units.size() == 1);
    CHECK(due[0].units[0] == kept);
}
// The retail-replay-transport edge `C-150/C-151` leaves open: the log writer
// spells `toggle-script-bit`, `offer-draw`, `sacrifice` and `gift`
// (`commandKindName`), but the reader's `kindFromName` vocabulary stops at
// `self-destruct` — a saved replay holding one of those four issues fails to
// parse instead of round-tripping.
TEST_CASE("v7 log: toggle-script-bit, offer-draw, sacrifice and gift round-trip", "[fa-persist]") {
    rm::sim::CommandLog log;
    const rm::sim::CommandKind kinds[] = {
        rm::sim::CommandKind::ToggleScriptBit, rm::sim::CommandKind::OfferDraw,
        rm::sim::CommandKind::Sacrifice, rm::sim::CommandKind::Gift};
    for (rm::sim::CommandKind kind : kinds) {
        rm::sim::CommandIssue issue;
        issue.tick = 1;
        issue.phase = rm::sim::CommandPhase::PreTick;
        issue.source = 0;
        issue.id = rm::commandId(0, static_cast<std::uint32_t>(log.size()));
        issue.player = 0;
        issue.kind = kind;
        issue.scriptBit = kind == rm::sim::CommandKind::ToggleScriptBit ? 0 : 1;
        REQUIRE(log.record(std::move(issue)));
    }
    const auto path = std::filesystem::temp_directory_path()
        / ("rm-fa-persist-kinds-" + std::to_string(::getpid()) + ".commands");
    REQUIRE(rm::sim::writeCommandLog(log, path.string()));
    const auto back = rm::sim::readCommandLog(path.string());
    REQUIRE(back);
    REQUIRE(back->size() == 4);
    CHECK(back->all()[0].kind == rm::sim::CommandKind::ToggleScriptBit);
    CHECK(back->all()[1].kind == rm::sim::CommandKind::OfferDraw);
    CHECK(back->all()[2].kind == rm::sim::CommandKind::Sacrifice);
    CHECK(back->all()[3].kind == rm::sim::CommandKind::Gift);
    std::filesystem::remove(path);
}
