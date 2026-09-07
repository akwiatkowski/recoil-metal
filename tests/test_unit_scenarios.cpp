// Behavioral contracts run real factory products through the same runner as the window.
#include "app/Match.hpp"
#include "app/Interface.hpp"
#include "app/SceneBuild.hpp"
#include "core/data/MoveDef.hpp"
#include "core/sim/StateHash.hpp"
#include "core/sim/SaveState.hpp"
#include "core/unit/BuildTree.hpp"
#include "core/unit/Role.hpp"
#include "core/unit/UnitBlueprint.hpp"
#include "core/ui/CommandPanel.hpp"
#include "core/log/Log.hpp"
#include "support/FxMatchers.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <unistd.h>

namespace {

std::filesystem::path corpusRoot() {
    const char* home = std::getenv("HOME");
    return home ? std::filesystem::path{home} / "projects/llm/input/faf/units"
                : std::filesystem::path{};
}

rm::HeightField flatField() {
    rm::HeightField field;
    field.squaresX = 128;
    field.squaresZ = 128;
    field.heightScale = 1.0f;
    field.raw.assign(field.sampleCount(), std::uint16_t{0});
    return field;
}

struct Scenario {
    rm::HeightField field;
    rm::app::UnitScene scene;
    rm::app::PassabilitySet passability;
    rm::vfs::Vfs content;

    explicit Scenario(rm::HeightField terrain = flatField(), bool water = false, float level = 0)
        : field(std::move(terrain)), passability(field, water, level) {
        scene.hasWater = water;
        scene.waterLevelElmos = level;
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
        const auto id = scene.store.spawn({
            .type = type,
            .transform = {.x = rm::sim::fxFromFloat(x), .z = rm::sim::fxFromFloat(z)},
            .motion = rm::app::motionFor(def, army),
            .health = rm::sim::initialHealth(def.health),
        });
        if (scene.store.motion()[id.index].canFly) {
            scene.store.transforms()[id.index].y = rm::sim::kAirClearanceElmos;
            scene.store.motion()[id.index].altitudeRef = rm::sim::kAirClearanceElmos;
        }
        return id;
    }

    rm::app::MatchRunner runner() {
        auto result = rm::app::makeMatchRunner(scene, field, passability, content, {}, {});
        result.scripts.clear();
        return result;
    }
};

} // namespace

TEST_CASE("retail interceptor combat tuning reaches its spawned mover", "[corpus][air-combat]") {
    const auto root = corpusRoot();
    if (!std::filesystem::is_directory(root)) SKIP("no retail unit corpus");
    const auto def = rm::unitbp::loadFile(root / "UEA0102/UEA0102_unit.bp");
    REQUIRE(def);
    const auto motion = rm::app::motionFor(*def, 0);
    CHECK(motion.airTurnSpeed == rm::sim::Fx::fromRatio(3, 2));
    CHECK(motion.airCombatTurnSpeed == rm::sim::Fx::fromRatio(3, 2));
    CHECK(motion.airKTurn == rm::sim::kFxOne);
    CHECK(motion.airKTurnDamping == rm::sim::Fx::fromRatio(3, 2));
    CHECK(motion.airBreakOffTrigger == rm::sim::Fx::fromInt(120));
    CHECK(motion.airBreakOffDistance == rm::sim::Fx::fromInt(40));
    CHECK(motion.airMinChangeTicks == 30);
    CHECK(motion.airMaxChangeTicks == 60);
}

TEST_CASE("a saved interceptor continues its turns with the same random stream",
          "[corpus][air-combat][save-state]") {
    using rm::sim::Fx;
    using State = rm::sim::MoveState::AirCombatState;
    const auto root = corpusRoot();
    if (!std::filesystem::is_directory(root)) SKIP("no retail unit corpus");
    const auto def = rm::unitbp::loadFile(root / "UEA0102/UEA0102_unit.bp");
    REQUIRE(def);
    Scenario live;
    const auto fighter = live.spawn(*def, 400, 400);
    const auto target = live.spawn(*def, 400, 200, 1);
    // Isolate the aircraft controller: no outstanding projectile state at the save.
    // Reload timers are ordinary serialized state; blueprint weapons stay unmodified.
    for (auto id : {fighter, target}) {
        live.scene.store.health()[id.index].reloadRemaining.assign(def->weapons.size(), 10000);
        live.scene.store.motion()[id.index].idleLandThreshold = 10000;
    }
    const std::array selection{fighter};
    REQUIRE(rm::app::issueAttack(live.scene, selection, 0, 0, target,
        Fx::fromInt(400), Fx::fromInt(200)));
    auto runner = live.runner();
    for (int tick = 0; tick < 10; ++tick) (void)rm::app::advanceMatch(runner, tick, 0);
    REQUIRE(live.scene.store.motion()[fighter.index].airCombatState >= State::HardTurn);
    REQUIRE(live.scene.store.motion()[fighter.index].airCombatDeadline > 10);
    REQUIRE(live.scene.projectiles.empty());
    SECTION("continue a sustained turn") {}
    SECTION("continue an off-map recovery") {
        live.scene.store.transforms()[fighter.index].x = Fx::fromInt(-20);
        (void)rm::app::advanceMatch(runner, 10, 0);
        REQUIRE(live.scene.store.motion()[fighter.index].airCombatState == State::Recovery);
    }
    const auto saved = rm::sim::SaveState::decode(rm::sim::SaveState::encode({
        .tick = runner.pathService.serviceBeats(), .random = runner.match.random.snapshot(),
        .pathServiceBeats = runner.pathService.serviceBeats(), .units = live.scene.store.snapshot(),
        .economyArmies = rm::sim::EconomyArmyState::capture(runner.match)}));
    REQUIRE(saved);
    Scenario resumed;
    (void)resumed.registerType(*def);
    (void)resumed.registerType(*def);
    resumed.scene.store = rm::sim::UnitStore{saved->units};
    auto continued = resumed.runner();
    saved->economyArmies->restore(continued.match, resumed.scene.economies, resumed.scene.commandersEver);
    continued.match.random = rm::sim::RandomStream{saved->random};
    continued.pathService.restoreServiceBeats(saved->pathServiceBeats);
    continued.match.pathService = &continued.pathService;
    REQUIRE(rm::sim::hashMatch(live.scene.store, runner.match)
        == rm::sim::hashMatch(resumed.scene.store, continued.match));
    std::array<bool, 8> seen{};
    for (int tick = static_cast<int>(saved->tick); tick < 610; ++tick) {
        (void)rm::app::advanceMatch(runner, tick, 0);
        (void)rm::app::advanceMatch(continued, tick, 0);
        INFO("air continuation tick " << tick);
        REQUIRE(rm::sim::hashMatch(live.scene.store, runner.match)
            == rm::sim::hashMatch(resumed.scene.store, continued.match));
        seen[static_cast<std::size_t>(live.scene.store.motion()[fighter.index].airCombatState)] = true;
    }
    CHECK(seen[static_cast<std::size_t>(State::BreakOff)]);
    CHECK(live.scene.projectiles.empty());
    CHECK(live.scene.store.motion()[fighter.index].moving);
}

TEST_CASE("a combat Guard escorts and fights without becoming a builder",
          "[corpus][guard]") {
    const auto root = corpusRoot();
    if (!std::filesystem::is_directory(root)) SKIP("no retail unit corpus");
    const auto tank = rm::unitbp::loadFile(root / "UEL0201/UEL0201_unit.bp");
    const auto generator = rm::unitbp::loadFile(root / "UEB1101/UEB1101_unit.bp");
    REQUIRE(tank);
    REQUIRE(generator);
    REQUIRE_FALSE(tank->isBuilder());
    Scenario scenario;
    scenario.scene.intel.configure(2, rm::sim::Fx::fromInt(1024), rm::sim::Fx::fromInt(1024),
        rm::sim::VisionStyle::ForgedAlliance);
    const auto guard = scenario.spawn(*tank, 100, 200);
    const auto guarded = scenario.spawn(*generator, 300, 200);
    const auto target = scenario.spawn(*generator, 340, 200, 1);
    const std::array guards{guard};
    REQUIRE(rm::app::issueGuard(scenario.scene, guards, 0, 0, guarded));
    REQUIRE(rm::app::issueMove(scenario.scene, guard, 0, 0,
        rm::sim::Fx::fromInt(700), rm::sim::Fx::fromInt(200), true));
    auto runner = scenario.runner();
    std::size_t shots = 0;
    for (int tick = 0; tick < 100; ++tick) {
        shots += rm::app::advanceMatch(runner, tick, 0).shotsFired;
    }
    REQUIRE(scenario.scene.store.orders()[guard.index].active());
    CHECK(scenario.scene.store.orders()[guard.index].active()->kind() == rm::sim::CommandKind::Guard);
    CHECK(scenario.scene.store.transforms()[guard.index].x > rm::sim::Fx::fromInt(100));
    CHECK(scenario.scene.intel.sees(0, rm::sim::IntelKind::Vision,
        rm::sim::Fx::fromInt(340), rm::sim::Fx::fromInt(200)));
    CHECK(shots > 0);
    CHECK((!scenario.scene.store.alive(target)
        || scenario.scene.store.health()[target.index].current < generator->health));
    CHECK(scenario.scene.building.empty());
    CHECK(scenario.scene.catalog.rates(scenario.scene.store.typeAt(guard.index)).buildPerTick
        == rm::sim::Mag{});
    const auto guardOrder = scenario.scene.store.orders()[guard.index].active()->payload().id;
    CHECK(guardOrder == scenario.scene.commands.all().front().id);
    rm::sim::RandomStream random{std::uint32_t{1}};
    const auto saved = rm::sim::SaveState::decode(rm::sim::SaveState::encode({
        .random = random.snapshot(), .units = scenario.scene.store.snapshot()}));
    REQUIRE(saved);
    REQUIRE(saved->units.sharedCommands.size() == 2);
    CHECK(saved->units.sharedCommands.front().kind == rm::sim::CommandKind::Guard);

    Scenario replay;
    replay.scene.intel.configure(2, rm::sim::Fx::fromInt(1024), rm::sim::Fx::fromInt(1024),
        rm::sim::VisionStyle::ForgedAlliance);
    (void)replay.spawn(*tank, 100, 200);
    (void)replay.spawn(*generator, 300, 200);
    (void)replay.spawn(*generator, 340, 200, 1);
    auto replayRunner = replay.runner();
    replayRunner.replay = &scenario.scene.commands;
    for (int tick = 0; tick < 100; ++tick) {
        (void)rm::app::advanceMatch(replayRunner, tick, 0);
    }
    CHECK(rm::sim::hashMatch(replay.scene.store, replayRunner.match)
        == rm::sim::hashMatch(scenario.scene.store, runner.match));
    scenario.scene.store.kill(guarded);
    (void)rm::app::advanceMatch(runner, 100, 0);
    REQUIRE(scenario.scene.store.orders()[guard.index].active());
    CHECK(scenario.scene.store.orders()[guard.index].active()->kind() == rm::sim::CommandKind::Move);
}

TEST_CASE("Guard validates capability ownership alliance and target lifetime", "[corpus][guard]") {
    const auto root = corpusRoot();
    if (!std::filesystem::is_directory(root)) SKIP("no retail unit corpus");
    const auto tank = rm::unitbp::loadFile(root / "UEL0201/UEL0201_unit.bp");
    REQUIRE(tank);
    for (const std::string condition : {"ally", "enemy", "self", "dead", "forbidden", "unauthorized"}) {
        DYNAMIC_SECTION(condition) {
            Scenario scenario;
            scenario.scene.armies[1].alliance = 0;
            auto actorDef = *tank;
            if (condition == "forbidden") {
                actorDef.commandCapsDeclared = true;
                actorDef.commandCaps.clear();
            }
            const auto guard = scenario.spawn(actorDef, 200, 200);
            auto target = scenario.spawn(*tank, 240, 200, 1);
            if (condition == "enemy") scenario.scene.armies[1].alliance = 1;
            if (condition == "self") target = guard;
            if (condition == "dead") scenario.scene.store.kill(target);
            const std::array guards{guard};
            (void)rm::app::issueGuard(scenario.scene, guards,
                condition == "unauthorized" ? 1 : 0, 0, target);
            auto runner = scenario.runner();
            (void)rm::app::advanceMatch(runner, 0, 0);
            CHECK(scenario.scene.store.orders()[guard.index].empty() == (condition != "ally"));
            if (condition == "ally") {
                scenario.scene.armies[1].alliance = 1;
                (void)rm::app::advanceMatch(runner, 1, 0);
                CHECK(scenario.scene.store.orders()[guard.index].empty());
            }
        }
    }
}

TEST_CASE("retail Mantis repairs an ally without gaining reclaim capability",
          "[corpus][exceptional]") {
    const auto root = corpusRoot();
    if (!std::filesystem::is_directory(root)) SKIP("no retail unit corpus");
    const auto mantis = rm::unitbp::loadFile(root / "URL0107/URL0107_unit.bp");
    REQUIRE(mantis);
    REQUIRE(mantis->hasCommandCap("RULEUCC_Repair"));
    REQUIRE_FALSE(mantis->hasCommandCap("RULEUCC_Reclaim"));
    Scenario scenario;
    const auto repairer = scenario.spawn(*mantis, 200, 200);
    const auto target = scenario.spawn(*mantis, 215, 200);
    const auto damaged = mantis->health - rm::sim::Mag::fromInt(5);
    scenario.scene.store.health()[target.index].current = damaged;
    auto runner = scenario.runner();
    runner.match.baseStorage = {rm::sim::Mag::fromInt(1000), rm::sim::Mag::fromInt(10000)};
    scenario.scene.economies[0].stored = runner.match.baseStorage;
    const std::array selection{repairer};
    REQUIRE(rm::app::issueRepair(scenario.scene, selection, 0, 0, target));
    const int limit = static_cast<int>(rm::app::gAppTickRate.ticks(rm::sim::Seconds{30}));
    for (int tick = 0; tick < limit; ++tick) {
        (void)rm::app::advanceMatch(runner, tick, 0);
    }
    CHECK(scenario.scene.store.health()[target.index].current == mantis->health);
    CHECK(scenario.scene.store.orders()[repairer.index].empty());
    CHECK(scenario.scene.economies[0].stored.mass < runner.match.baseStorage.mass);
}

TEST_CASE("retail Aurora crosses water above the seabed and returns to land",
          "[corpus][exceptional]") {
    const auto root = corpusRoot();
    if (!std::filesystem::is_directory(root)) SKIP("no retail unit corpus");
    const auto aurora = rm::unitbp::loadFile(root / "UAL0201/UAL0201_unit.bp");
    REQUIRE(aurora);
    REQUIRE(aurora->motion == rm::unitdef::MotionType::Hover);
    // A shallow channel with gentle shores: no slope barrier can conceal a water-layer bug.
    auto field = flatField();
    for (int z = 0; z < field.verticesZ(); ++z) {
        for (int x = 0; x < field.verticesX(); ++x) {
            const int shore = std::clamp(std::max(40 - x, x - 88), 0, 8);
            field.raw[static_cast<std::size_t>(z * field.verticesX() + x)] =
                static_cast<std::uint16_t>(shore * 2);
        }
    }
    Scenario scenario(std::move(field), true, 8);
    const auto unit = scenario.spawn(*aurora, 160, 200);
    auto runner = scenario.runner();
    REQUIRE(rm::app::issueMove(scenario.scene, unit, 0, 0,
        rm::sim::Fx::fromInt(840), rm::sim::Fx::fromInt(200)));
    bool crossed = false;
    const int limit = static_cast<int>(rm::app::gAppTickRate.ticks(rm::sim::Seconds{60}));
    for (int tick = 0; tick < limit; ++tick) {
        (void)rm::app::advanceMatch(runner, tick, 0);
        const auto& at = scenario.scene.store.transforms()[unit.index];
        if (at.x > rm::sim::Fx::fromInt(350) && at.x < rm::sim::Fx::fromInt(680)) {
            crossed = true;
            REQUIRE(at.y >= rm::sim::Fx::fromInt(8));
        }
    }
    CHECK(crossed);
    const auto& at = scenario.scene.store.transforms()[unit.index];
    CHECK(at.x > rm::sim::Fx::fromInt(820));
    CHECK(at.y >= rm::sim::Fx::fromInt(16));
}

TEST_CASE("projectile guidance does not require a silo economy table", "[capability][guidance]") {
    const auto traits = rm::unitbp::loadProjectileTraits(R"(
        ProjectileBlueprint { Physics = { TrackTarget = true, TurnRate = 80,
            Acceleration = 2, MaxSpeed = 25 } }
    )");
    REQUIRE(traits);
    CHECK(traits->trackTarget);
    CHECK(traits->turnRateDegreesPerSecond == 80.0f);
    CHECK(traits->accelerationElmosPerSecond2 == 16.0f);
    CHECK(traits->maxSpeedElmosPerSecond == 200.0f);
    CHECK(traits->buildTime == rm::sim::Mag{});
}

TEST_CASE("guided shots turn within their budget and do not follow recycled targets", "[guidance]") {
    Scenario scenario;
    rm::unitdef::UnitDef def;
    def.health = rm::sim::Mag::fromInt(100);
    const auto target = scenario.spawn(def, 300, 200, 1);
    scenario.scene.store.transforms()[target.index].y = rm::sim::Fx::fromInt(100);
    rm::sim::Projectile shot;
    shot.position = {rm::sim::Fx::fromInt(200), rm::sim::Fx::fromInt(100), rm::sim::Fx::fromInt(200)};
    shot.velocity[2] = rm::sim::Fx::fromInt(10);
    shot.guidanceTarget = target;
    shot.turnPerTick = rm::app::gAppTickRate.bradPerTick(80.0f * 0.017453292519943295f);
    shot.maxSpeedPerTick = rm::sim::Fx::fromInt(20);
    shot.accelerationPerTickSquared = rm::sim::Fx::fromRatio(16, 100);
    shot.ticksRemaining = static_cast<int>(rm::app::gAppTickRate.ticks(rm::sim::Seconds{1.0f}));
    shot.firedByArmy = 0;
    std::vector shots{shot};
    rm::sim::advanceProjectiles(shots, scenario.scene.store, scenario.scene.armies,
        scenario.scene.terrain(scenario.field), rm::app::gAppTickRate);
    REQUIRE(shots.size() == 1);
    CHECK(shots.front().velocity[0] > rm::sim::Fx{});
    CHECK(rm::sim::fxBearing(shots.front().velocity[0], shots.front().velocity[2])
          <= shot.turnPerTick + 2); // CORDIC/angle quantization, two binary radians.
    CHECK(rm::sim::fxHypot(shots.front().velocity[0], shots.front().velocity[2]) > rm::sim::Fx::fromInt(10));
    scenario.scene.store.kill(target);
    const auto replacement = scenario.spawn(def, 100, 200, 1);
    REQUIRE(replacement.index == target.index);
    REQUIRE(replacement.generation != target.generation);
    const auto velocity = shots.front().velocity;
    rm::sim::advanceProjectiles(shots, scenario.scene.store, scenario.scene.armies,
        scenario.scene.terrain(scenario.field), rm::app::gAppTickRate);
    REQUIRE(shots.size() == 1);
    CHECK(shots.front().velocity == velocity);
}

TEST_CASE("guided projectile state participates in the match hash", "[guidance][state-hash]") {
    Scenario scenario;
    auto runner = scenario.runner();
    scenario.scene.projectiles.resize(1);
    auto& shot = scenario.scene.projectiles.front();
    shot.guidanceTarget = {1, 1};
    shot.turnPerTick = 100;
    const auto original = shot;
    const auto hash = rm::sim::hashMatch(scenario.scene.store, runner.match);
    for (int field = 0; field < 5; ++field) {
        shot = original;
        if (field == 0) ++shot.guidanceTarget.index;
        if (field == 1) ++shot.guidanceTarget.generation;
        if (field == 2) ++shot.turnPerTick;
        if (field == 3) shot.accelerationPerTickSquared = rm::sim::Fx::fromInt(1);
        if (field == 4) shot.maxSpeedPerTick = rm::sim::Fx::fromInt(1);
        CHECK(rm::sim::hashMatch(scenario.scene.store, runner.match) != hash);
    }
}

TEST_CASE("world construction bars follow selection hover progress and sight", "[ui][world-progress]") {
    Scenario scenario;
    rm::unitdef::UnitDef def;
    def.visionRadiusElmos = 100;
    def.health = rm::sim::Mag::fromInt(100);
    const auto builder = scenario.spawn(def, 200, 200);
    scenario.scene.playerArmy = rm::sim::kNoArmy;
    rm::sim::Construction work{
        .buildTimeRemaining = rm::sim::Mag::fromInt(30),
        .totalBuildTime = rm::sim::Mag::fromInt(60),
    };
    work.builder = builder;
    work.blueprintIndex = scenario.scene.store.typeAt(builder.index);
    work.position = {rm::sim::Fx::fromInt(200), {}, rm::sim::Fx::fromInt(200)};
    scenario.scene.building.push_back(work);
    rm::OrbitCamera camera;
    camera.target = simd_make_float3(200, 0, 200);
    camera.distance = 200;
    std::vector<rm::text::Glyph> glyphs(rm::text::kGlyphCount);
    const rm::text::Font font{.glyphs = glyphs, .lineHeight = 18,
        .solidUv = {0.5f, 0.5f, 0.6f, 0.6f}};
    const auto viewport = rm::ui::UiViewport::authored(1600, 900);
    const std::array selected{builder};
    const auto draw = [&](std::span<const rm::sim::UnitId> selection,
                          std::optional<std::array<float, 2>> cursor = std::nullopt) {
        rm::ui::Geometry geometry;
        const auto count = rm::app::appendConstructionBars(geometry, scenario.scene, camera,
            scenario.field, font, viewport, selection, cursor);
        return std::pair{count, geometry};
    };
    CHECK(draw({}).first == 0);
    const auto [count, geometry] = draw(selected);
    REQUIRE(count == 1);
    REQUIRE(geometry.worldOverlay.solid.size() == 12);
    const auto& vertices = geometry.worldOverlay.solid;
    const auto width = [&](std::size_t first) {
        float left = vertices[first].position[0], right = left;
        for (std::size_t i = first; i < first + 6; ++i) {
            left = std::min(left, vertices[i].position[0]);
            right = std::max(right, vertices[i].position[0]);
        }
        return right - left;
    };
    CHECK(width(6) == width(0) / 2);
    const auto extent = viewport.hudExtent();
    CHECK(draw({}, std::array{extent.width / 2, extent.height / 2}).first == 1);
    CHECK(draw({}, std::array{0.0f, 0.0f}).first == 0);
    scenario.scene.building.front().buildTimeRemaining = {};
    CHECK(draw(selected).first == 0);
    scenario.scene.building.front() = work;
    scenario.scene.playerArmy = 0;
    scenario.scene.intel.configure(2, rm::sim::Fx::fromInt(1024), rm::sim::Fx::fromInt(1024),
        rm::sim::VisionStyle::ForgedAlliance);
    CHECK(draw(selected).first == 0);  // selection cannot bypass fog
    scenario.scene.playerArmy = rm::sim::kNoArmy;
    scenario.scene.store.kill(builder);
    CHECK(draw(selected).first == 0);
}

TEST_CASE("factory panel clicks control the real production queue", "[corpus][ui][production]") {
    const auto root = corpusRoot();
    if (!std::filesystem::is_directory(root)) SKIP("no retail unit corpus at " + root.string());
    const auto factory = rm::unitbp::loadFile(root / "UEB0101/UEB0101_unit.bp");
    const auto tank = rm::unitbp::loadFile(root / "UEL0201/UEL0201_unit.bp");
    REQUIRE(factory);
    REQUIRE(tank);
    Scenario scenario;
    const auto builder = scenario.spawn(*factory, 200, 200);
    const auto type = scenario.registerType(*tank);
    auto& bank = scenario.scene.economies[0];
    bank.storage = bank.stored = {rm::sim::Mag::fromInt(10000), rm::sim::Mag::fromInt(100000)};
    auto runner = scenario.runner();
    const auto frame = rm::ui::frameLayout(rm::ui::UiViewport::authored(1600, 900));
    const auto rect = rm::ui::productionPanelRect(frame);
    const auto repeat = rm::ui::productionRepeatRect(rect);
    const auto clear = rm::ui::productionClearRect(rect);
    const auto click = [&](rm::ui::Rect button, rm::PlayerIndex player, rm::TickIndex tick) {
        return rm::app::submitProductionControl(scenario.scene, builder, player, tick,
            frame, button.x + 1, button.y + 1);
    };
    REQUIRE(rm::app::gatherProduction(scenario.scene, builder));
    CHECK_FALSE(click(clear, 0, 0));  // idle clear is disabled
    REQUIRE(rm::app::issueBuild(scenario.scene, builder, 0, 0, type,
        rm::sim::Fx::fromInt(240), rm::sim::Fx::fromInt(200), true));
    REQUIRE(rm::app::issueBuild(scenario.scene, builder, 0, 0, type,
        rm::sim::Fx::fromInt(240), rm::sim::Fx::fromInt(200), true));
    for (int tick = 0; tick < 20; ++tick) (void)rm::app::advanceMatch(runner, tick, 0);
    const auto view = rm::app::gatherProduction(scenario.scene, builder);
    REQUIRE(view);
    REQUIRE_FALSE(view->queue.empty());
    std::uint32_t count = 0;
    for (const auto& row : view->queue) count += row.count;
    CHECK(count == 2);
    CHECK(view->queue.front().id == "UEL0201");
    CHECK(view->building);
    CHECK(view->progress > 0);
    CHECK(view->progress < 1);
    REQUIRE(click(repeat, 0, 20));
    (void)rm::app::advanceMatch(runner, 20, 0);
    CHECK(rm::app::gatherProduction(scenario.scene, builder)->repeat);
    REQUIRE(click(repeat, 1, 21));  // queued, but dispatch must reject the enemy player
    (void)rm::app::advanceMatch(runner, 21, 0);
    CHECK(rm::app::gatherProduction(scenario.scene, builder)->repeat);
    REQUIRE(click(repeat, 0, 22));
    (void)rm::app::advanceMatch(runner, 22, 0);
    CHECK_FALSE(rm::app::gatherProduction(scenario.scene, builder)->repeat);
    REQUIRE(click(clear, 0, 23));
    for (int tick = 23; tick < 1200; ++tick) (void)rm::app::advanceMatch(runner, tick, 0);
    const auto stopped = rm::app::gatherProduction(scenario.scene, builder);
    REQUIRE(stopped);
    CHECK(stopped->queue.empty());
    CHECK_FALSE(stopped->building);
    CHECK(scenario.scene.store.liveCount() == 1);
    scenario.scene.store.kill(builder);
    CHECK_FALSE(click(repeat, 0, 1200));
    CHECK_FALSE(rm::app::gatherProduction(scenario.scene, builder));
}

TEST_CASE("factory cancellation removes only the named entry and replays", "[corpus][factory-cancel]") {
    const auto root = corpusRoot();
    if (!std::filesystem::is_directory(root)) SKIP("no retail unit corpus");
    const auto factory = rm::unitbp::loadFile(root / "UEB0101/UEB0101_unit.bp");
    const auto tank = rm::unitbp::loadFile(root / "UEL0201/UEL0201_unit.bp");
    REQUIRE(factory);
    REQUIRE(tank);
    for (const std::size_t cancelledRow : {std::size_t{0}, std::size_t{1}}) {
        DYNAMIC_SECTION("cancel " << (cancelledRow == 0 ? "active" : "pending")) {
            Scenario live;
            const auto builder = live.spawn(*factory, 200, 200);
            const auto type = live.registerType(*tank);
            live.scene.economies[0].stored = {rm::sim::Mag::fromInt(1000), rm::sim::Mag::fromInt(10000)};
            auto runner = live.runner();
            for (int entry = 0; entry < 3; ++entry) {
                REQUIRE(rm::app::submitCommand(live.scene, rm::sim::CommandIssue{
                    .tick = 0, .source = 0, .player = 0,
                    .kind = rm::sim::CommandKind::Build, .queued = true,
                    .units = {builder}, .buildType = type,
                    .count = entry == 1 ? 5u : 1u,
                }));
            }
            for (int tick = 0; tick < 5; ++tick) (void)rm::app::advanceMatch(runner, tick, 0);
            auto& queue = live.scene.store.orders()[builder.index];
            REQUIRE(queue.size() == 3);
            REQUIRE(live.scene.building.size() == 1);
            const auto firstId = queue.entries()[0].payload().id;
            const auto secondId = queue.entries()[1].payload().id;
            const auto lastId = queue.entries()[2].payload().id;
            const auto cancelId = queue.entries()[cancelledRow].payload().id;
            const auto remaining = live.scene.building.front().buildTimeRemaining;
            REQUIRE(rm::app::issueCancelFactoryBuild(live.scene, builder, 1, 5, cancelId));
            (void)rm::app::advanceMatch(runner, 5, 0);
            CHECK(queue.size() == 3);  // another player cannot edit the queue
            const auto frame = rm::ui::frameLayout(rm::ui::UiViewport::authored(1600, 900));
            const auto button = rm::ui::productionCancelRect(rm::ui::productionPanelRect(frame), cancelledRow);
            REQUIRE(rm::app::submitProductionControl(live.scene, builder, 0, 6, frame,
                button.x + 1, button.y + 1));
            (void)rm::app::advanceMatch(runner, 6, 0);
            REQUIRE(queue.size() == 2);
            CHECK(queue.entries()[0].payload().id == (cancelledRow == 0 ? secondId : firstId));
            CHECK(queue.entries()[1].payload().id == lastId);
            REQUIRE(live.scene.building.size() == 1);
            if (cancelledRow == 0) {
                CHECK(live.scene.building.front().buildTimeRemaining > remaining);
                CHECK(queue.entries()[0].payload().remainingCount == 5);
            } else {
                CHECK(live.scene.building.front().buildTimeRemaining < remaining);
            }
            REQUIRE(rm::app::issueCancelFactoryBuild(live.scene, builder, 0, 7, cancelId));
            (void)rm::app::advanceMatch(runner, 7, 0);
            CHECK(queue.size() == 2);  // stale UI clicks never remove the replacement row

            const auto path = std::filesystem::temp_directory_path() / "rm-factory-cancel.commands";
            REQUIRE(rm::sim::writeCommandLog(live.scene.commands, path.string()));
            const auto commands = rm::sim::readCommandLog(path.string());
            REQUIRE(commands);
            CHECK(std::ranges::equal(commands->all(), live.scene.commands.all()));
            CHECK(commands->all()[4].cancelCommandId == cancelId);
            Scenario replay;
            (void)replay.spawn(*factory, 200, 200);
            (void)replay.registerType(*tank);
            replay.scene.economies[0].stored = {rm::sim::Mag::fromInt(1000), rm::sim::Mag::fromInt(10000)};
            auto replayRunner = replay.runner();
            replayRunner.replay = &*commands;
            for (int tick = 0; tick < 8; ++tick) (void)rm::app::advanceMatch(replayRunner, tick, 0);
            CHECK(rm::sim::hashMatch(replay.scene.store, replayRunner.match)
                == rm::sim::hashMatch(live.scene.store, runner.match));
            for (int tick = 8; tick < 80; ++tick) {
                (void)rm::app::advanceMatch(runner, tick, 0);
                (void)rm::app::advanceMatch(replayRunner, tick, 0);
                CHECK(rm::sim::hashMatch(replay.scene.store, replayRunner.match)
                    == rm::sim::hashMatch(live.scene.store, runner.match));
            }
            std::filesystem::remove(path);
        }
    }
}

TEST_CASE("cancelling one factory leaves a shared build on the other factory", "[corpus][factory-cancel]") {
    const auto root = corpusRoot();
    if (!std::filesystem::is_directory(root)) SKIP("no retail unit corpus");
    const auto factory = rm::unitbp::loadFile(root / "UEB0101/UEB0101_unit.bp");
    const auto tank = rm::unitbp::loadFile(root / "UEL0201/UEL0201_unit.bp");
    REQUIRE(factory);
    REQUIRE(tank);
    Scenario scene;
    const auto first = scene.spawn(*factory, 200, 200);
    const auto second = scene.spawn(*factory, 400, 200);
    const auto type = scene.registerType(*tank);
    REQUIRE(scene.scene.store.setFactoryRepeat(first, true));
    const auto order = rm::app::submitCommand(scene.scene, rm::sim::CommandIssue{
        .source = 0, .player = 0, .kind = rm::sim::CommandKind::Build,
        .units = {first, second}, .buildType = type, .count = 3,
    });
    REQUIRE(order);
    auto runner = scene.runner();
    (void)rm::app::advanceMatch(runner, 0, 0);
    REQUIRE(scene.scene.building.size() == 2);
    REQUIRE(rm::app::issueCancelFactoryBuild(scene.scene, first, 0, 1, *order));
    for (int tick = 1; tick < 20; ++tick) (void)rm::app::advanceMatch(runner, tick, 0);
    CHECK(scene.scene.store.orders()[first.index].empty());
    CHECK(scene.scene.store.factoryRepeat(first));
    REQUIRE(scene.scene.store.orders()[second.index].size() == 1);
    CHECK(scene.scene.store.orders()[second.index].active()->payload().id == *order);
    CHECK(scene.scene.store.orders()[second.index].active()->payload().remainingCount == 3);
    REQUIRE(scene.scene.building.size() == 1);
    CHECK(scene.scene.building.front().builder == second);
}

TEST_CASE("factory cancellation aborts its own build behind a standing Guard", "[corpus][factory-cancel]") {
    const auto root = corpusRoot();
    if (!std::filesystem::is_directory(root)) SKIP("no retail unit corpus");
    const auto factory = rm::unitbp::loadFile(root / "UEB0101/UEB0101_unit.bp");
    const auto tank = rm::unitbp::loadFile(root / "UEL0201/UEL0201_unit.bp");
    const auto scout = rm::unitbp::loadFile(root / "UEL0101/UEL0101_unit.bp");
    REQUIRE(factory);
    REQUIRE(tank);
    REQUIRE(scout);
    Scenario scene;
    const auto builder = scene.spawn(*factory, 200, 200);
    const auto guarded = scene.spawn(*factory, 250, 200);
    const auto tankType = scene.registerType(*tank);
    const auto scoutType = scene.registerType(*scout);
    const std::array selected{builder};
    REQUIRE(rm::app::issueGuard(scene.scene, selected, 0, 0, guarded));
    REQUIRE(rm::app::issueBuild(scene.scene, builder, 0, 0, tankType, {}, {}, true));
    REQUIRE(rm::app::issueBuild(scene.scene, builder, 0, 0, scoutType, {}, {}, true));
    auto runner = scene.runner();
    (void)rm::app::advanceMatch(runner, 0, 0);
    (void)rm::app::advanceMatch(runner, 1, 0);
    auto& queue = scene.scene.store.orders()[builder.index];
    REQUIRE(queue.size() == 3);
    REQUIRE(queue.active()->kind() == rm::sim::CommandKind::Guard);
    REQUIRE(scene.scene.building.size() == 1);
    REQUIRE(scene.scene.building.front().blueprintIndex == tankType);
    REQUIRE(rm::app::issueCancelFactoryBuild(scene.scene, builder, 0, 2, queue.entries()[1].payload().id));
    (void)rm::app::advanceMatch(runner, 2, 0);
    REQUIRE(queue.size() == 2);
    CHECK(queue.active()->kind() == rm::sim::CommandKind::Guard);
    REQUIRE(scene.scene.building.size() == 1);
    CHECK(scene.scene.building.front().blueprintIndex == scoutType);
}

TEST_CASE("mirrored factory work never acquires a later matching build command", "[corpus][factory-cancel]") {
    const auto root = corpusRoot();
    if (!std::filesystem::is_directory(root)) SKIP("no retail unit corpus");
    const auto factory = rm::unitbp::loadFile(root / "UEB0101/UEB0101_unit.bp");
    const auto tank = rm::unitbp::loadFile(root / "UEL0201/UEL0201_unit.bp");
    REQUIRE(factory);
    REQUIRE(tank);
    Scenario scenario;
    const auto builder = scenario.spawn(*factory, 200, 200);
    const auto guarded = scenario.spawn(*factory, 250, 200);
    const auto type = scenario.registerType(*tank);
    const std::array selected{builder};
    REQUIRE(rm::app::issueGuard(scenario.scene, selected, 0, 0, guarded));
    REQUIRE(rm::app::submitCommand(scenario.scene, rm::sim::CommandIssue{
        .source = 0, .player = 0, .kind = rm::sim::CommandKind::Build,
        .units = {guarded}, .buildType = type, .count = 2,
    }));
    auto runner = scenario.runner();
    (void)rm::app::advanceMatch(runner, 0, 0);
    (void)rm::app::advanceMatch(runner, 1, 0);
    const auto work = [&]() -> rm::sim::Construction& {
        const auto found = std::ranges::find_if(scenario.scene.building, [&](const auto& child) {
            return child.builder == builder && !child.finished();
        });
        REQUIRE(found != scenario.scene.building.end());
        return *found;
    };
    REQUIRE(work().retainedCommandId == rm::kInvalidCommandId);
    REQUIRE(rm::app::issueBuild(scenario.scene, builder, 0, 2, type, {}, {}, true));
    (void)rm::app::advanceMatch(runner, 2, 0);
    auto& queue = scenario.scene.store.orders()[builder.index];
    REQUIRE(queue.size() == 2);
    const auto ownId = queue.entries()[1].payload().id;
    SECTION("cancelling the new row leaves the existing mirror child") {
        const auto remaining = work().buildTimeRemaining;
        REQUIRE(rm::app::issueCancelFactoryBuild(scenario.scene, builder, 0, 3, ownId));
        (void)rm::app::advanceMatch(runner, 3, 0);
        CHECK(queue.size() == 1);
        CHECK(work().retainedCommandId == rm::kInvalidCommandId);
        CHECK(work().buildTimeRemaining <= remaining);
    }
    SECTION("completing the mirror child starts the new row without consuming it") {
        work().buildTimeRemaining = rm::sim::Mag::fromRaw(1);
        work().fundedLastTick = rm::sim::kFxOne;
        (void)rm::app::advanceMatch(runner, 3, 0);
        // Completion spawns a unit, which may reallocate the store's queue array.
        const auto& after = scenario.scene.store.orders()[builder.index];
        REQUIRE(after.size() == 2);
        CHECK(after.entries()[1].payload().id == ownId);
        CHECK(work().retainedCommandId == ownId);
    }
}

TEST_CASE("saved economies and armies continue construction sharing and defeat timers", "[corpus][economy-save]") {
    const auto root = corpusRoot();
    if (!std::filesystem::is_directory(root)) SKIP("no retail unit corpus");
    const auto engineer = rm::unitbp::loadFile(root / "UEL0105/UEL0105_unit.bp");
    const auto generator = rm::unitbp::loadFile(root / "UEB1101/UEB1101_unit.bp");
    REQUIRE(engineer);
    REQUIRE(generator);
    Scenario live;
    const auto builder = live.spawn(*engineer, 200, 200, 1);
    const auto donor = live.spawn(*generator, 800, 800, 0);
    const auto defeatedUnit = live.spawn(*generator, 900, 800, 2);
    const auto type = live.registerType(*generator);
    live.scene.armies = rm::sim::freeForAll(3);
    live.scene.armies[1].alliance = 0;
    live.scene.players = rm::sim::onePlayerPerArmy(3, 1);
    live.scene.economies.resize(3);
    live.scene.commandersEver.resize(3);
    live.scene.economies[0].stored = {rm::sim::Mag::fromInt(10000), rm::sim::Mag::fromInt(100000)};
    // Enough mass for the authored generator; its energy comes partly from the allied donor.
    live.scene.economies[1].stored = {rm::sim::Mag::fromInt(100), rm::sim::Mag::fromInt(50)};
    REQUIRE(rm::app::issueBuild(live.scene, builder, 1, 0, type,
        rm::sim::Fx::fromInt(225), rm::sim::Fx::fromInt(200)));
    auto runner = live.runner();
    for (int tick = 0; tick < 10; ++tick) (void)rm::app::advanceMatch(runner, tick, 0);
    REQUIRE(live.scene.store.alive(donor));
    REQUIRE(live.scene.building.size() == 1);
    REQUIRE_FALSE(live.scene.building.front().finished());
    REQUIRE(live.scene.building.front().buildTimeRemaining < live.scene.building.front().totalBuildTime);
    REQUIRE(live.scene.economies[1].sharedIn.energy > rm::sim::Mag{});
    SECTION("funded work and the next allied gift") {}
    SECTION("pending winner and delayed defeated-army cleanup") {
        live.scene.armies[2].defeated = true;
        runner.match.winnerPending = true;
        runner.match.pendingWinner = 0;
        runner.match.winnerStableTicks = 100;
        runner.match.defeatCleanupRemainingTicks = {0, 0, 25};
    }
    const auto encoded = rm::sim::SaveState::encode({.tick = 10, .random = runner.match.random.snapshot(),
        .pathServiceBeats = runner.pathService.serviceBeats(), .units = live.scene.store.snapshot(),
        .siloAmmo = live.scene.siloAmmo, .redirects = live.scene.redirects,
        .economyArmies = rm::sim::EconomyArmyState::capture(runner.match)});
    const auto saved = rm::sim::SaveState::decode(encoded);
    REQUIRE(saved);
    REQUIRE(saved->economyArmies);
    // Restore into fresh owners, retaining only the immutable map and blueprint registration order.
    Scenario resumed;
    (void)resumed.spawn(*engineer, 200, 200, 1);
    (void)resumed.spawn(*generator, 800, 800, 0);
    (void)resumed.spawn(*generator, 900, 800, 2);
    (void)resumed.registerType(*generator);
    resumed.scene.store = rm::sim::UnitStore{saved->units};
    resumed.scene.armies = saved->economyArmies->armies;
    resumed.scene.players = rm::sim::onePlayerPerArmy(3, 1);
    resumed.scene.siloAmmo = saved->siloAmmo;
    resumed.scene.redirects = saved->redirects;
    auto resumedRunner = resumed.runner();
    resumedRunner.match.random = rm::sim::RandomStream{saved->random};
    saved->economyArmies->restore(resumedRunner.match, resumed.scene.economies, resumed.scene.commandersEver);
    resumedRunner.pathService.restoreServiceBeats(saved->pathServiceBeats);
    resumedRunner.match.pathService = &resumedRunner.pathService;
    REQUIRE(rm::sim::hashMatch(resumed.scene.store, resumedRunner.match)
        == rm::sim::hashMatch(live.scene.store, runner.match));
    CHECK(resumed.scene.economies[1].requestedLastTick.mass == live.scene.economies[1].requestedLastTick.mass);
    CHECK(resumed.scene.economies[1].usageLastTick.energy == live.scene.economies[1].usageLastTick.energy);
    for (int tick = static_cast<int>(saved->tick); tick < 700; ++tick) {
        INFO("continued tick " << tick);
        (void)rm::app::advanceMatch(runner, tick, 0);
        (void)rm::app::advanceMatch(resumedRunner, tick, 0);
        REQUIRE(rm::sim::hashMatch(resumed.scene.store, resumedRunner.match)
            == rm::sim::hashMatch(live.scene.store, runner.match));
    }
    CHECK(live.scene.building.front().finished());
    CHECK(resumed.scene.building.front().finished());
    if (saved->economyArmies->winnerPending) {
        CHECK(runner.match.over);
        CHECK(resumedRunner.match.over);
        CHECK_FALSE(resumed.scene.store.alive(defeatedUnit));
    }
}

TEST_CASE("ordinary artillery splash damages nearby wrecks through the match", "[corpus][wreck-combat]") {
    const auto root = corpusRoot();
    if (!std::filesystem::is_directory(root)) SKIP("no retail unit corpus");
    const auto artillery = rm::unitbp::loadFile(root / "UEL0103/UEL0103_unit.bp");
    const auto generator = rm::unitbp::loadFile(root / "UEB1101/UEB1101_unit.bp");
    REQUIRE(artillery);
    REQUIRE(generator);
    Scenario scenario;
    const auto gun = scenario.spawn(*artillery, 200, 200);
    const auto target = scenario.spawn(*generator, 340, 200, 1);
    const rm::sim::Feature wreckBody{
        .at = {rm::sim::Fx::fromInt(340), {}, rm::sim::Fx::fromInt(200)},
        .radiusElmos = rm::sim::Fx::fromInt(4), .fromType = scenario.scene.store.typeAt(target.index),
        .armyIndex = 0, .health = rm::sim::Mag::fromInt(1000),
        .maximumHealth = rm::sim::Mag::fromInt(1000),
        .maximumMassReclaim = rm::sim::Mag::fromInt(100),
        .massRemaining = rm::sim::Mag::fromInt(100),
        .reclaimWorkRemaining = rm::sim::Mag::fromInt(100),
        .reclaimWorkTotal = rm::sim::Mag::fromInt(100),
    };
    const auto wreck = scenario.scene.features.add(wreckBody);
    REQUIRE(wreck == gun); // Separate pools intentionally share numeric handles.
    const std::array guns{gun};
    REQUIRE(rm::app::issueAttack(scenario.scene, guns, 0, 0, target,
        rm::sim::Fx::fromInt(340), rm::sim::Fx::fromInt(200)));
    auto runner = scenario.runner();
    std::size_t shots = 0;
    for (int tick = 0; tick < 100; ++tick) shots += rm::app::advanceMatch(runner, tick, 0).shotsFired;
    REQUIRE(shots > 0);
    const auto* remaining = scenario.scene.features.find(wreck);
    CHECK((remaining == nullptr || remaining->health < rm::sim::Mag::fromInt(1000)));
    CHECK((remaining == nullptr || remaining->massRemaining < rm::sim::Mag::fromInt(100)));
    CHECK(scenario.scene.store.health()[gun.index].current == artillery->health);
    Scenario replay;
    (void)replay.spawn(*artillery, 200, 200);
    (void)replay.spawn(*generator, 340, 200, 1);
    REQUIRE(replay.scene.features.add(wreckBody) == wreck);
    auto replayRunner = replay.runner();
    replayRunner.replay = &scenario.scene.commands;
    for (int tick = 0; tick < 100; ++tick) (void)rm::app::advanceMatch(replayRunner, tick, 0);
    REQUIRE(rm::sim::hashMatch(replay.scene.store, replayRunner.match)
        == rm::sim::hashMatch(scenario.scene.store, runner.match));
    for (int tick = 100; tick < 160; ++tick) {
        (void)rm::app::advanceMatch(runner, tick, 0);
        (void)rm::app::advanceMatch(replayRunner, tick, 0);
        REQUIRE(rm::sim::hashMatch(replay.scene.store, replayRunner.match)
            == rm::sim::hashMatch(scenario.scene.store, runner.match));
    }
}

TEST_CASE("construction inspector explains partial funding and the allocation limit", "[ui][funding]") {
    Scenario scene;
    rm::unitdef::UnitDef def;
    const auto builder = scene.spawn(def, 200, 200);
    rm::sim::Construction work{
        .armyIndex = 0,
        .buildTimeRemaining = rm::sim::Mag::fromInt(30),
        .totalBuildTime = rm::sim::Mag::fromInt(60),
    };
    work.builder = builder;
    work.fundedLastTick = rm::sim::kFxOne / rm::sim::Fx::fromInt(4);
    scene.scene.building.push_back(work);
    scene.scene.economies[0].massIsBinding = true;
    auto card = rm::app::constructionCard(scene.scene, builder);
    REQUIRE(card);
    REQUIRE(card->rows.size() <= 2);  // the progress inspector's visible row budget
    CHECK(card->rows.back().value == "25% FUNDED");
    CHECK(card->rows.back().label == "MASS");
    scene.scene.economies[0].massIsBinding = false;
    CHECK(rm::app::constructionCard(scene.scene, builder)->rows.back().label == "ENERGY");
    scene.scene.building.front().fundedLastTick = rm::sim::Fx::fromRaw(1);
    CHECK(rm::app::constructionCard(scene.scene, builder)->rows.back().value == "<1% FUNDED");
    scene.scene.building.front().fundedLastTick = {};
    CHECK(rm::app::constructionCard(scene.scene, builder)->rows.back().value == "STALLED");
    scene.scene.building.front().fundedLastTick = rm::sim::kFxOne;
    CHECK(rm::app::constructionCard(scene.scene, builder)->rows.back().value == "ACTIVE");
}

TEST_CASE("order logging joins submission rejection and construction transitions", "[corpus][order-trace]") {
    const auto root = corpusRoot();
    if (!std::filesystem::is_directory(root)) SKIP("no retail unit corpus at " + root.string());
    const auto engineer = rm::unitbp::loadFile(root / "UEL0105/UEL0105_unit.bp");
    const auto generator = rm::unitbp::loadFile(root / "UEB1101/UEB1101_unit.bp");
    REQUIRE(engineer);
    REQUIRE(generator);
    const auto path = std::filesystem::temp_directory_path()
        / ("recoil-order-trace-" + std::to_string(getpid()) + ".log");
    std::ofstream{path, std::ios::trunc}.close();
    struct RestoreLog {
        ~RestoreLog() { (void)rm::log::configure({}); }
    } restore;
    REQUIRE(rm::log::configure({.level = rm::log::Level::Debug,
        .filePath = path.string(), .stderrEnabled = false}));
    Scenario scene;
    const auto builder = scene.spawn(*engineer, 200, 200);
    const auto product = scene.registerType(*generator);
    auto& bank = scene.scene.economies[0];
    const rm::sim::Resources supply{rm::sim::Mag::fromInt(10000), rm::sim::Mag::fromInt(100000)};
    bank.storage = bank.stored = supply;
    auto runner = scene.runner();
    int tick = 0;
    const auto advance = [&](int count) {
        for (int i = 0; i < count; ++i) (void)rm::app::advanceMatch(runner, tick++, 0);
    };
    REQUIRE(rm::app::issueBuild(scene.scene, builder, 0, 0, product,
        rm::sim::Fx::fromInt(240), rm::sim::Fx::fromInt(200)));
    advance(20);
    REQUIRE(rm::app::issueMove(scene.scene, builder, 1, static_cast<rm::TickIndex>(tick),
        {}, {}, false, rm::sim::CommandKind::Stop));  // unauthorized player
    bank.stored = {};
    advance(40);
    bank.stored = supply;
    advance(1200);
    std::ifstream input{path};
    const std::string log{std::istreambuf_iterator<char>{input}, {}};
    for (const auto* stage : {"submitted", "accepted", "rejected", "started", "stalled",
                              "resumed", "completed"}) {
        INFO(stage);
        CHECK(log.find(std::string{"stage="} + stage) != std::string::npos);
    }
    CHECK(log.find("command=0 unit=0:1") != std::string::npos);
    CHECK(log.find("[construction] tick=") != std::string::npos);
    std::size_t economyLines = 0, summaries = 0;
    std::istringstream lines{log};
    for (std::string line; std::getline(lines, line);) {
        const auto start = line.find("[economy] tick=");
        if (start == std::string::npos) continue;
        ++economyLines;
        const int loggedTick = std::stoi(line.substr(start + std::string{"[economy] tick="}.size()));
        CHECK(loggedTick % 50 == 0); // Five seconds at the app's 10 Hz clock.
        if (line.find("stored=") != std::string::npos) ++summaries;
    }
    CHECK(summaries == 26 * scene.scene.economies.size()); // Ticks 0..1250, each army.
    CHECK(log.find("type=UEB1101 income/s=M:0.00,E:20.00") != std::string::npos);
    CHECK(static_cast<std::size_t>(std::count(log.begin(), log.end(), '\n')) - economyLines < 20);
}

TEST_CASE("real construction lifecycle is reflected by the active inspector", "[corpus][ui][lifecycle]") {
    const auto root = corpusRoot();
    if (!std::filesystem::is_directory(root)) SKIP("no retail unit corpus at " + root.string());
    const auto engineer = rm::unitbp::loadFile(root / "UEL0105/UEL0105_unit.bp");
    const auto generator = rm::unitbp::loadFile(root / "UEB1101/UEB1101_unit.bp");
    REQUIRE(engineer);
    REQUIRE(generator);
    Scenario scene;
    const auto builder = scene.spawn(*engineer, 200, 200);
    const auto product = scene.registerType(*generator);
    auto& economy = scene.scene.economies[0];
    const rm::sim::Resources fullBank{rm::sim::Mag::fromInt(10000), rm::sim::Mag::fromInt(100000)};
    economy.storage = economy.stored = fullBank;
    auto runner = scene.runner();
    int tick = 0;
    const auto advance = [&](float seconds) {
        const int count = static_cast<int>(rm::app::gAppTickRate.ticks(rm::sim::Seconds{seconds}));
        for (int step = 0; step < count; ++step) {
            (void)rm::app::advanceMatch(runner, tick++, 0.0f);
        }
    };
    CHECK_FALSE(rm::app::constructionCard(scene.scene, builder));
    REQUIRE(rm::app::issueBuild(scene.scene, builder, 0, 0, product,
        rm::sim::Fx::fromInt(240), rm::sim::Fx::fromInt(200)));
    advance(2);
    const auto started = rm::app::constructionCard(scene.scene, builder);
    REQUIRE(started);
    REQUIRE(started->progress);
    CHECK(*started->progress > 0);
    CHECK(*started->progress < 1);
    CHECK(started->rows.back().value == "ACTIVE");

    SECTION("stall then funding recovery and completion") {
        economy.stored = {};
        advance(2); // Drain the previous beat's allocation and its retained residue.
        const auto stalled = rm::app::constructionCard(scene.scene, builder);
        REQUIRE(stalled);
        CHECK(stalled->rows.back().value == "STALLED");
        advance(2);
        REQUIRE(rm::app::constructionCard(scene.scene, builder));
        CHECK(rm::app::constructionCard(scene.scene, builder)->progress == stalled->progress);
        economy.stored = fullBank;
        advance(2);
        const auto resumed = rm::app::constructionCard(scene.scene, builder);
        REQUIRE(resumed);
        CHECK(*resumed->progress > *stalled->progress);
        CHECK(resumed->rows.back().value == "ACTIVE");
        advance(120);
        CHECK_FALSE(rm::app::constructionCard(scene.scene, builder));
        REQUIRE(scene.scene.building.size() == 1);
        CHECK(scene.scene.building.front().finished());
        CHECK(scene.scene.store.liveCount() == 2);
    }
    SECTION("stop cancels the active work and its inspector") {
        REQUIRE(rm::app::issueMove(scene.scene, builder, 0, static_cast<rm::TickIndex>(tick),
            {}, {}, false, rm::sim::CommandKind::Stop));
        advance(1);
        CHECK_FALSE(rm::app::constructionCard(scene.scene, builder));
        advance(120);
        CHECK(scene.scene.store.liveCount() == 1);
    }
    SECTION("builder death stops progress and removes its inspector") {
        const auto remaining = scene.scene.building.front().buildTimeRemaining;
        scene.scene.store.kill(builder);
        advance(2);
        CHECK_FALSE(rm::app::constructionCard(scene.scene, builder));
        REQUIRE(scene.scene.building.size() == 1);
        CHECK(scene.scene.building.front().buildTimeRemaining == remaining);
        CHECK(scene.scene.store.liveCount() == 0);
    }
}

TEST_CASE("HUD capture replays have explicit setup-relative identities and blueprint paths", "[ui][replay]") {
    const auto fixtures = std::filesystem::path{__FILE__}.parent_path() / "fixtures";
    for (const std::string name : {"hud-construction.commands", "hud-production.commands", "hud-factory-t2.commands", "hud-extractor-upgrade.commands"}) {
        INFO(name);
        std::vector<std::string> paths;
        const auto log = rm::sim::readCommandLog((fixtures / name).string(), &paths);
        REQUIRE(log);
        REQUIRE_FALSE(log->empty());
        CHECK(log->all().front().tick == 70);
        CHECK(log->all().front().id == 1); // Opening extractor consumed source 0's first ID.
        REQUIRE(paths.size() == log->size());
        for (const auto& path : paths) CHECK(path.starts_with("/units/"));
        if (name == "hud-production.commands") {
            REQUIRE(log->size() == 2);
            CHECK(log->all().back().tick == 4000);
            CHECK(log->all().back().count == 30);
            CHECK(log->all().back().units == std::vector{rm::sim::UnitId{4, 1}});
        }
    }
}

TEST_CASE("every retail T1 land factory product performs its role through the match runner",
          "[corpus][capability][scenario]") {
    const auto root = corpusRoot();
    if (!std::filesystem::is_directory(root)) {
        SKIP("no retail unit corpus at " + root.string());
    }
    std::vector<rm::unitdef::UnitDef> units;
    rm::vfs::Vfs projectiles;
    REQUIRE(projectiles.mountArchive("/Volumes/Samsung_T5/faf/Supreme Commander Forged Alliance/gamedata/projectiles.scd"));
    for (const auto& file : std::filesystem::recursive_directory_iterator{root}) {
        if (!file.is_regular_file() || !file.path().string().ends_with("_unit.bp")) {
            continue;
        }
        auto def = rm::unitbp::loadFile(file.path());
        REQUIRE(def);
        for (auto& weapon : def->weapons) {
            if (weapon.projectileId.empty()) continue;
            if (const auto bytes = projectiles.read(weapon.projectileId)) {
                const auto traits = rm::unitbp::loadProjectileTraits(std::string_view{
                    reinterpret_cast<const char*>(bytes->data()), bytes->size()});
                REQUIRE(traits);
                weapon.projectileTraits = *traits;
            }
        }
        units.push_back(std::move(*def));
    }
    std::size_t tested = 0;
    for (const std::string factoryId : {"UEB0101", "UAB0101", "URB0101", "XSB0101"}) {
        const auto factory = std::ranges::find(units, factoryId, &rm::unitdef::UnitDef::name);
        REQUIRE(factory != units.end());
        for (const auto& def : units) {
            if (!def.hasCategory("TECH1") || !def.hasCategory("MOBILE")
                || !rm::unitdef::matchesExpression(factory->buildableCategory, def)) {
                continue;
            }
            DYNAMIC_SECTION(factoryId << " / " << def.name) {
                // UI-model evidence is separate from execution evidence below. An enabled
                // descriptor is not proof of hit-testing, factory-tray access or special abilities.
                const std::array selection{&def};
                const auto available = rm::ui::commandAvailability(selection);
                const auto permits = [&](rm::sim::CommandKind kind) {
                    const auto descriptor = std::ranges::find(
                        rm::ui::kCommandDescriptors, kind, &rm::ui::CommandDescriptor::kind);
                    REQUIRE(descriptor != rm::ui::kCommandDescriptors.end());
                    return available[static_cast<std::size_t>(
                        descriptor - rm::ui::kCommandDescriptors.begin())];
                };
                CHECK(permits(rm::sim::CommandKind::Move));
                const auto role = rm::unitdef::roleOf(def);
                if (role != rm::unitdef::Role::Scout && role != rm::unitdef::Role::Builder) {
                    CHECK(permits(rm::sim::CommandKind::Attack));
                }
                Scenario scenario;
                const auto unit = scenario.spawn(def, 200.0f, 200.0f);
                auto runner = scenario.runner();
                REQUIRE(rm::app::issueMove(scenario.scene, unit, 0, 0,
                    rm::sim::fxFromFloat(300.0f), rm::sim::fxFromFloat(200.0f)));
                const int ticks = static_cast<int>(rm::app::gAppTickRate.ticks(rm::sim::Seconds{30.0f}));
                for (int tick = 0; tick < ticks; ++tick) {
                    (void)rm::app::advanceMatch(runner, tick, 0.0f);
                }
                REQUIRE(scenario.scene.store.alive(unit));
                CHECK(rm::sim::fxToFloat(scenario.scene.store.transforms()[unit.index].x) > 290.0f);

                // Each job starts in isolation: movement must not accidentally pre-acquire
                // a target or consume the resources that the role scenario needs.
                Scenario job;
                const auto actor = job.spawn(def, 200.0f, 200.0f);
                if (role == rm::unitdef::Role::Scout) {
                    job.scene.intel.configure(2, rm::sim::Fx::fromInt(1024),
                        rm::sim::Fx::fromInt(1024), rm::sim::VisionStyle::ForgedAlliance);
                    auto scan = job.runner();
                    (void)rm::app::advanceMatch(scan, 0, 0.0f);
                    const auto probeX = rm::sim::Fx::fromInt(600);
                    const auto probeZ = rm::sim::Fx::fromInt(200);
                    REQUIRE_FALSE(job.scene.intel.sees(0, rm::sim::IntelKind::Vision, probeX, probeZ));
                    REQUIRE(rm::app::issueMove(job.scene, actor, 0, 1,
                        rm::sim::Fx::fromInt(500), probeZ));
                    for (int tick = 1; tick < ticks; ++tick) {
                        (void)rm::app::advanceMatch(scan, tick, 0.0f);
                    }
                    CHECK(job.scene.intel.sees(0, rm::sim::IntelKind::Vision, probeX, probeZ));
                    CHECK(job.scene.intel.sees(0, rm::sim::IntelKind::Radar, probeX, probeZ));
                } else if (role == rm::unitdef::Role::Builder) {
                    const std::string productId = factoryId.substr(0, 3) + "1101";
                    const auto product = std::ranges::find(units, productId, &rm::unitdef::UnitDef::name);
                    REQUIRE(product != units.end());
                    const auto productType = job.registerType(*product);
                    auto& bank = job.scene.economies[0];
                    bank.storage = bank.stored = {rm::sim::Mag::fromInt(10000), rm::sim::Mag::fromInt(100000)};
                    REQUIRE(rm::app::issueBuild(job.scene, actor, 0, 0, productType,
                        rm::sim::Fx::fromInt(240), rm::sim::Fx::fromInt(200)));
                    auto build = job.runner();
                    const int buildTicks = static_cast<int>(rm::app::gAppTickRate.ticks(rm::sim::Seconds{120.0f}));
                    for (int tick = 0; tick < buildTicks; ++tick) {
                        (void)rm::app::advanceMatch(build, tick, 0.0f);
                    }
                    REQUIRE(job.scene.building.size() == 1);
                    CHECK(job.scene.building.front().finished());
                    CHECK(job.scene.store.liveCount() == 2);
                } else {
                    // A passive target isolates the weapon from retaliation, while retaining
                    // the real target's category, health and movement-layer definitions.
                    const std::string targetId = role == rm::unitdef::Role::AntiAir ? "UEA0101" : "UEB1101";
                    const auto targetDef = std::ranges::find(units, targetId, &rm::unitdef::UnitDef::name);
                    REQUIRE(targetDef != units.end());
                    const auto target = job.spawn(*targetDef, 260.0f, 200.0f, 1);
                    if (role == rm::unitdef::Role::AntiAir) {
                        REQUIRE(rm::app::issueMove(job.scene, target, 1, 0,
                            rm::sim::Fx::fromInt(260), rm::sim::Fx::fromInt(300), false,
                            rm::sim::CommandKind::Patrol));
                    }
                    const std::array actors{actor};
                    REQUIRE(rm::app::issueAttack(job.scene, actors, 0, 0, target,
                        rm::sim::Fx::fromInt(260), rm::sim::Fx::fromInt(200)));
                    auto fight = job.runner();
                    std::size_t shots = 0;
                    bool damaged = false;
                    for (int tick = 0; tick < ticks; ++tick) {
                        shots += rm::app::advanceMatch(fight, tick, 0.0f).shotsFired;
                        damaged = damaged || !job.scene.store.alive(target)
                            || job.scene.store.health()[target.index].current < targetDef->health;
                    }
                    INFO("shots " << shots << " target airborne " << job.scene.store.motion()[target.index].airborne
                        << " y " << rm::sim::fxToFloat(job.scene.store.transforms()[target.index].y));
                    CHECK(damaged);
                }
            }
            ++tested;
        }
    }
    CHECK(tested == 23);
}

TEST_CASE("queued building placements preserve work and complete in click order",
          "[corpus][build-queue]") {
    const auto root = corpusRoot();
    if (!std::filesystem::is_directory(root)) SKIP("retail corpus unavailable");
    const auto engineer = rm::unitbp::loadFile(root / "UEL0105/UEL0105_unit.bp");
    const auto generator = rm::unitbp::loadFile(root / "UEB1101/UEB1101_unit.bp");
    REQUIRE(engineer);
    REQUIRE(generator);
    Scenario job;
    const auto builder = job.spawn(*engineer, 300, 300);
    const auto type = job.registerType(*generator);
    job.scene.economies[0].stored = {rm::sim::Mag::fromInt(650), rm::sim::Mag::fromInt(5000)};
    auto runner = job.runner();
    REQUIRE(rm::app::issueBuild(job.scene, builder, 0, 0, type,
        rm::sim::Fx::fromInt(340), rm::sim::Fx::fromInt(300)));
    for (int tick = 0; tick < 10; ++tick) (void)rm::app::advanceMatch(runner, tick, 0);
    REQUIRE(job.scene.building.size() == 1);
    const auto remaining = job.scene.building.front().buildTimeRemaining;
    REQUIRE(rm::app::issueBuild(job.scene, builder, 0, 10, type,
        rm::sim::Fx::fromInt(300), rm::sim::Fx::fromInt(340), true));
    (void)rm::app::advanceMatch(runner, 10, 0);
    REQUIRE(job.scene.building.size() == 1);
    CHECK(job.scene.building.front().position[0] == rm::sim::Fx::fromInt(340));
    CHECK(job.scene.building.front().buildTimeRemaining < remaining);
    CHECK(job.scene.store.orders()[builder.index].entries().size() == 2);
    for (int tick = 11; tick < 1000; ++tick) (void)rm::app::advanceMatch(runner, tick, 0);
    REQUIRE(job.scene.building.size() == 2);
    CHECK(job.scene.building[0].finished());
    CHECK(job.scene.building[1].finished());
    CHECK(job.scene.building[1].position[2] == rm::sim::Fx::fromInt(340));
    CHECK(job.scene.store.liveCount() == 3);
}

TEST_CASE("structure orders snap to the build grid by default, and not under free placement",
          "[corpus][build-queue][placement]") {
    // The game's rule: an even footprint centres on a grid line, an odd one on a cell centre.
    // Snapping happens once, at intake, so the construction row IS the snapped site.
    const auto root = corpusRoot();
    if (!std::filesystem::is_directory(root)) SKIP("retail corpus unavailable");
    const auto engineer = rm::unitbp::loadFile(root / "UEL0105/UEL0105_unit.bp");
    const auto generator = rm::unitbp::loadFile(root / "UEB1101/UEB1101_unit.bp");
    REQUIRE(engineer);
    REQUIRE(generator);
    REQUIRE(generator->footprintSquaresX > 0);
    const auto click = std::array{rm::sim::fxFromFloat(341.3f), rm::sim::fxFromFloat(299.7f)};
    const auto expectGrid = std::array{
        rm::sim::snapToBuildGrid(click[0], generator->footprintSquaresX),
        rm::sim::snapToBuildGrid(click[1], generator->footprintSquaresZ)};
    // The rule itself, on both parities.
    CHECK(rm::sim::snapToBuildGrid(rm::sim::fxFromFloat(341.3f), 2) == rm::sim::Fx::fromInt(344));
    CHECK(rm::sim::snapToBuildGrid(rm::sim::fxFromFloat(299.7f), 2) == rm::sim::Fx::fromInt(296));
    CHECK(rm::sim::snapToBuildGrid(rm::sim::fxFromFloat(341.3f), 1) == rm::sim::Fx::fromInt(340));
    CHECK(rm::sim::snapToBuildGrid(rm::sim::fxFromFloat(299.7f), 3) == rm::sim::Fx::fromInt(300));
    CHECK(rm::sim::snapToBuildGrid(rm::sim::fxFromFloat(-3.0f), 2) == rm::sim::Fx::fromInt(0));
    CHECK(rm::sim::snapToBuildGrid(rm::sim::fxFromFloat(-5.0f), 2) == rm::sim::Fx::fromInt(-8));

    const auto run = [&](rm::sim::PlacementMode mode) {
        Scenario job;
        job.scene.placementMode = mode;
        const auto builder = job.spawn(*engineer, 300, 300);
        const auto type = job.registerType(*generator);
        job.scene.economies[0].stored = {rm::sim::Mag::fromInt(650), rm::sim::Mag::fromInt(5000)};
        auto runner = job.runner();
        REQUIRE(rm::app::issueBuild(job.scene, builder, 0, 0, type, click[0], click[1]));
        for (int tick = 0; tick < 10; ++tick) (void)rm::app::advanceMatch(runner, tick, 0);
        REQUIRE(job.scene.building.size() == 1);
        return std::array{job.scene.building.front().position[0],
                          job.scene.building.front().position[2]};
    };
    SECTION("grid") {
        const auto site = run(rm::sim::PlacementMode::Grid);
        CHECK(site[0] == expectGrid[0]);
        CHECK(site[1] == expectGrid[1]);
        CHECK(site[0] != click[0]);
    }
    SECTION("free") {
        const auto site = run(rm::sim::PlacementMode::Free);
        CHECK(site[0] == click[0]);
        CHECK(site[1] == click[1]);
    }
    SECTION("the ghost helper agrees with the sim") {
        Scenario job;
        (void)job.spawn(*engineer, 300, 300);
        const auto type = job.registerType(*generator);
        const auto ghost = rm::app::snapBuildSite(job.scene, type, {341.3f, 299.7f});
        CHECK(rm::sim::fxFromFloat(ghost[0]) == expectGrid[0]);
        CHECK(rm::sim::fxFromFloat(ghost[1]) == expectGrid[1]);
        job.scene.placementMode = rm::sim::PlacementMode::Free;
        const auto freeGhost = rm::app::snapBuildSite(job.scene, type, {341.3f, 299.7f});
        CHECK(freeGhost[0] == 341.3f);
    }
}

TEST_CASE("a build site taken during the approach stops the engineer or joins a colleague",
          "[corpus][build-queue]") {
    // The reported bug: an extractor ordered on a far deposit, the engineer walks there and
    // stands idle. The site check runs every beat of the approach; when the site is taken the
    // order was dropped without stopping the walk. Now an enemy on the spot stops the engineer
    // where it is, and an allied colleague already building the same thing is joined.
    const auto root = corpusRoot();
    if (!std::filesystem::is_directory(root)) SKIP("retail corpus unavailable");
    const auto engineer = rm::unitbp::loadFile(root / "UEL0105/UEL0105_unit.bp");
    const auto extractor = rm::unitbp::loadFile(root / "UEB1103/UEB1103_unit.bp");
    REQUIRE(engineer);
    REQUIRE(extractor);
    Scenario job;
    job.scene.resourceDeposits.push_back({rm::unitdef::BuildRestriction::MassDeposit,
                                          rm::sim::Fx::fromInt(420), rm::sim::Fx::fromInt(300)});
    const auto builder = job.spawn(*engineer, 300, 300);
    const auto type = job.registerType(*extractor);
    job.scene.economies[0].stored = {rm::sim::Mag::fromInt(650), rm::sim::Mag::fromInt(5000)};
    auto runner = job.runner();
    const auto x = [&](rm::sim::UnitId id) {
        return rm::sim::fxToFloat(job.scene.store.transforms()[id.index].x);
    };

    SECTION("an enemy standing on the deposit refuses the order and stops the engineer") {
        REQUIRE(rm::app::issueBuild(job.scene, builder, 0, 0, type, rm::sim::Fx::fromInt(420),
                                    rm::sim::Fx::fromInt(300)));
        for (int tick = 0; tick < 5; ++tick) (void)rm::app::advanceMatch(runner, tick, 0);
        REQUIRE(job.scene.store.motion()[builder.index].moving);
        (void)job.spawn(*engineer, 420, 300, 1);
        for (int tick = 5; tick < 200; ++tick) (void)rm::app::advanceMatch(runner, tick, 0);
        CHECK(job.scene.building.empty());
        CHECK(job.scene.store.orders()[builder.index].empty());
        CHECK_FALSE(job.scene.store.motion()[builder.index].moving);
        CHECK(x(builder) < 400.0f);  // stopped where it was refused, not parked on the deposit
    }

    SECTION("an allied colleague already building it is joined until the work completes") {
        REQUIRE(rm::app::issueBuild(job.scene, builder, 0, 0, type, rm::sim::Fx::fromInt(420),
                                    rm::sim::Fx::fromInt(300)));
        const auto colleague = job.spawn(*engineer, 426, 300, 0);
        REQUIRE(rm::app::issueBuild(job.scene, colleague, 0, 0, type, rm::sim::Fx::fromInt(420),
                                    rm::sim::Fx::fromInt(300)));
        // Dispatch creates the construction: the colleague is in reach, the builder routes.
        (void)rm::app::advanceMatch(runner, 0, 0);
        REQUIRE(job.scene.building.size() == 1);
        bool lentRate = false;
        int finishedAt = -1;
        for (int tick = 1; tick < 3000 && finishedAt < 0; ++tick) {
            (void)rm::app::advanceMatch(runner, tick, 0);
            if (job.scene.building.front().assistPerTick > rm::sim::Mag{}) lentRate = true;
            if (job.scene.building.front().finished()) finishedAt = tick;
        }
        REQUIRE(finishedAt > 0);
        CHECK(lentRate);
        CHECK(job.scene.building.size() == 1);  // one extractor, not two
        (void)rm::app::advanceMatch(runner, finishedAt + 1, 0);
        CHECK(job.scene.store.orders()[builder.index].empty());  // the order completed with it
    }
}

TEST_CASE("an extractor can queue its next tier while upgrading", "[corpus][upgrade-chain][headless-ui]") {
    const auto root = corpusRoot();
    if (!std::filesystem::is_directory(root)) SKIP("retail corpus unavailable");
    std::vector<rm::unitdef::UnitDef> defs;
    std::vector<std::string> ids{"UEB1103", "UEB1202", "UEB1302"};
    for (const auto& id : ids) {
        auto def = rm::unitbp::loadFile(root / id / (id + "_unit.bp"));
        REQUIRE(def);
        defs.push_back(*def);
    }
    Scenario job;
    job.scene.roster = rm::data::Roster::build(defs, ids);
    const auto t1 = job.spawn(defs[0], 200, 200);
    const auto t2Type = job.registerType(defs[1]);
    const auto t3Type = job.registerType(defs[2]);
    auto runner = job.runner();
    std::vector<rm::sim::UnitId> selection{t1};
    const auto step = [&](int tick) {
        job.scene.economies[0].stored = rm::app::kStartingStorage;
        (void)rm::app::advanceMatch(runner, tick, 0);
        rm::app::followUpgradeSelection(job.scene, selection);
    };
    const auto clickUpgrade = [&](int tick, std::string_view expected, float width) {
        std::vector<rm::ui::BuildOption> options;
        rm::app::BuildSelection who;
        rm::app::gatherBuildOptions(job.scene, selection.front(), rm::ui::neutralTheme(), options, who);
        REQUIRE(options.size() == 1);
        CHECK(options.front().id == expected);
        const auto frame = rm::ui::frameLayout(rm::ui::UiViewport::full(width, 800));
        const auto panel = rm::ui::buildPanelLayout(frame, options.size());
        REQUIRE(panel.shown == 1);
        const auto origin = rm::ui::buildCellOrigin(panel, 0);
        const auto hit = rm::ui::buildOptionAt(panel, options.size(),
            origin[0] + panel.cellWidth / 2, origin[1] + panel.cellHeight / 2);
        REQUIRE(hit);
        // Same action handler as the real mouse callback; no Shift for either click.
        REQUIRE(rm::app::submitBuildOption(job.scene, job.content, who.builder, 0,
            static_cast<rm::TickIndex>(tick), options[*hit], false));
    };
    clickUpgrade(0, "UEB1202", 1280);
    step(0);
    REQUIRE(job.scene.building.size() == 1);
    SECTION("the tray offers T3 during T2 construction") {
        std::vector<rm::ui::BuildOption> options;
        rm::app::BuildSelection who;
        rm::app::gatherBuildOptions(job.scene, t1, rm::ui::neutralTheme(), options, who);
        REQUIRE(options.size() == 1);
        CHECK(options.front().id == "UEB1302");
        CHECK(options.front().queuedUpgrade);
        CHECK(rm::ui::buildOptionCard(options.front(), rm::ui::GameProfile::Fa).rows.front().value
              == "queued after current upgrade");
    }
    SECTION("the queued command survives replacement and builds T3") {
        const auto remaining = job.scene.building.front().buildTimeRemaining;
        clickUpgrade(1, "UEB1302", 800);
        step(1);
        REQUIRE(job.scene.building.size() == 1);
        CHECK(job.scene.building.front().buildTimeRemaining < remaining);
        REQUIRE(job.scene.store.orders()[t1.index].entries().size() == 2);
        const auto queuedId = job.scene.store.orders()[t1.index].entries().back().payload().id;
        bool cancel = false, cancelActive = false, cancelPending = false;
        SECTION("cancelling T2 also removes its dependent T3") { cancelActive = true; }
        SECTION("the queue panel cancels pending T3 without restarting T2") { cancelPending = true; }
        SECTION("Stop cancels the current and queued upgrades") { cancel = true; }
        SECTION("complete both upgrades across saved command-queue restoration") {}
        if (cancelActive) {
            const auto view = rm::app::gatherProduction(job.scene, t1);
            REQUIRE(view);
            REQUIRE(view->queue.size() == 2);
            const auto frame = rm::ui::frameLayout(rm::ui::UiViewport::full(800, 800));
            const auto button = rm::ui::productionCancelRect(rm::ui::productionPanelRect(frame), 0);
            REQUIRE(rm::app::submitProductionControl(job.scene, t1, 0, 2, frame,
                button.x + 1, button.y + 1));
            step(2);
            CHECK(job.scene.store.alive(t1));
            CHECK(job.scene.building.empty());
            CHECK(job.scene.store.orders()[t1.index].empty());
            CHECK_FALSE(job.scene.store.commandIdLive(queuedId));
            CHECK_FALSE(rm::app::gatherProduction(job.scene, t1));
            return;
        }
        if (cancelPending) {
            const auto view = rm::app::gatherProduction(job.scene, t1);
            REQUIRE(view);
            REQUIRE(view->queue.size() == 2);
            CHECK(view->queue[0].name.starts_with("T2 "));
            CHECK(view->queue[1].name.starts_with("T3 "));
            CHECK(view->building);
            const auto frame = rm::ui::frameLayout(rm::ui::UiViewport::full(1280, 800));
            const auto rect = rm::ui::productionPanelRect(frame);
            const auto repeat = rm::ui::productionRepeatRect(rect);
            CHECK_FALSE(rm::ui::productionCommandAt(rect, *view, repeat.x + 1, repeat.y + 1));
            const auto button = rm::ui::productionCancelRect(rect, 1);
            REQUIRE(rm::app::submitProductionControl(job.scene, t1, 1, 2, frame,
                button.x + 1, button.y + 1));
            step(2);
            CHECK(job.scene.store.orders()[t1.index].size() == 2); // Enemy cannot cancel.
            const auto before = job.scene.building.front().buildTimeRemaining;
            REQUIRE(rm::app::submitProductionControl(job.scene, t1, 0, 3, frame,
                button.x + 1, button.y + 1));
            step(3);
            CHECK(job.scene.store.orders()[t1.index].size() == 1);
            CHECK_FALSE(job.scene.store.commandIdLive(queuedId));
            CHECK(job.scene.building.front().buildTimeRemaining < before);
            int tick = 4;
            while (job.scene.store.alive(t1) && tick < 3000) step(tick++);
            REQUIRE_FALSE(job.scene.store.alive(t1));
            CHECK(job.scene.store.typeAt(selection.front().index) == t2Type);
            CHECK(job.scene.store.orders()[selection.front().index].empty());
            return;
        }
        if (cancel) {
            REQUIRE(rm::app::issueMove(job.scene, t1, 0, 2, {}, {}, false, rm::sim::CommandKind::Stop));
            for (int tick = 2; tick < 100; ++tick) step(tick);
            CHECK(job.scene.store.alive(t1));
            CHECK(job.scene.building.empty());
            CHECK(job.scene.store.orders()[t1.index].empty());
            CHECK_FALSE(job.scene.store.commandIdLive(queuedId));
            return;
        }
        const auto saved = rm::sim::SaveState::decode(rm::sim::SaveState::encode({
            .units = job.scene.store.snapshot()}));
        REQUIRE(saved);
        job.scene.store = rm::sim::UnitStore{saved->units};
        int tick = 2;
        while (job.scene.store.alive(t1) && tick < 3000) step(tick++);
        REQUIRE_FALSE(job.scene.store.alive(t1));
        rm::sim::UnitId t2{};
        for (rm::UnitIndex slot = 0; slot < job.scene.store.slotCount(); ++slot) {
            if (job.scene.store.slotAlive(slot) && job.scene.store.typeAt(slot) == t2Type)
                t2 = job.scene.store.idAt(slot);
        }
        REQUIRE(job.scene.store.alive(t2));
        CHECK(selection == std::vector{t2});
        const auto& queue = job.scene.store.orders()[t2.index];
        REQUIRE(queue.size() == 1);
        CHECK(queue.entries().front().unit() == t2);
        CHECK(queue.entries().front().payload().id == queuedId);
        CHECK(queue.entries().front().payload().units == std::vector{t2});
        while (job.scene.store.alive(t2) && tick < 10000) step(tick++);
        CHECK_FALSE(job.scene.store.alive(t2));
        REQUIRE(job.scene.store.liveCount() == 1);
        bool finalTier = false;
        for (rm::UnitIndex slot = 0; slot < job.scene.store.slotCount(); ++slot)
            if (job.scene.store.slotAlive(slot)) finalTier = job.scene.store.typeAt(slot) == t3Type;
        CHECK(finalTier);
        REQUIRE(selection.size() == 1);
        CHECK(job.scene.store.alive(selection.front()));
        CHECK(job.scene.store.typeAt(selection.front().index) == t3Type);
        CHECK_FALSE(job.scene.store.commandIdLive(queuedId));
    }
}

TEST_CASE("auto-expand sends an idle engineer to the nearest free deposit on its own side",
          "[corpus][auto-expand]") {
    // ADR-109. Two armies seated at x=200 and x=1000 on a 1024-elmo field; the engineer at
    // x=300 sees three deposits. The Mass spot at 340 is nearest; the Hydrocarbon spot at
    // (300, 360) is next; the Mass spot at 700 is nearer to the enemy's start than to ours.
    const auto root = corpusRoot();
    if (!std::filesystem::is_directory(root)) SKIP("retail corpus unavailable");
    const auto engineer = rm::unitbp::loadFile(root / "UEL0105/UEL0105_unit.bp");
    const auto extractor = rm::unitbp::loadFile(root / "UEB1103/UEB1103_unit.bp");
    const auto hydro = rm::unitbp::loadFile(root / "UEB1102/UEB1102_unit.bp");
    REQUIRE(engineer);
    REQUIRE(extractor);
    REQUIRE(hydro);

    Scenario job;
    const std::vector<rm::scenario::Marker> markers{
        {.name = "Mass 1", .type = "Mass", .position = {340.0f, 0.0f, 300.0f}},
        {.name = "Hydro 1", .type = "Hydrocarbon", .position = {300.0f, 0.0f, 360.0f}},
        {.name = "Mass 2", .type = "Mass", .position = {700.0f, 0.0f, 300.0f}},
        {.name = "ARMY_1", .type = "Blank Marker", .position = {200.0f, 0.0f, 300.0f}},
    };
    const std::vector<rm::mapinfo::StartPosition> starts{{.team = 0, .x = 200.0f, .z = 300.0f},
                                                         {.team = 1, .x = 1000.0f, .z = 300.0f}};
    for (const rm::scenario::Marker& marker : markers) {
        const auto kind = rm::app::depositKind(marker);
        if (kind == rm::unitdef::BuildRestriction::None) continue;
        job.scene.resourceDeposits.push_back({kind, rm::sim::fxFromFloat(marker.position[0]),
                                              rm::sim::fxFromFloat(marker.position[2])});
    }
    // The roster is what auto-expand picks blueprints from; registering the structures up
    // front is what lets an empty VFS resolve them (the path is already known to the scene).
    const std::vector<rm::unitdef::UnitDef> corpus{*engineer, *extractor, *hydro};
    const std::vector<std::string> ids{"UEL0105", "UEB1103", "UEB1102"};
    job.scene.roster = rm::data::Roster::build(corpus, ids);
    const auto mexType = job.registerType(*extractor);
    const auto hydroType = job.registerType(*hydro);
    const auto builder = job.spawn(*engineer, 300, 300);
    job.scene.economies[0].stored = {rm::sim::Mag::fromInt(2000), rm::sim::Mag::fromInt(5000)};
    auto runner = job.runner();
    runner.markers = markers;
    runner.starts = starts;

    const auto headOf = [&](rm::sim::UnitId id) {
        return job.scene.store.orders()[id.index].active();
    };
    const auto siteOf = [&](const rm::sim::QueuedCommand& order) {
        return std::array<float, 2>{rm::sim::fxToFloat(order.targetX()),
                                    rm::sim::fxToFloat(order.targetZ())};
    };

    SECTION("nothing happens until the order is put on, and it toggles off again") {
        (void)rm::app::advanceMatch(runner, 0, 0);
        CHECK(headOf(builder) == nullptr);
        const std::array<rm::sim::UnitId, 1> one{builder};
        CHECK(rm::app::toggleAutoExpand(runner, one));
        CHECK(rm::app::autoExpanding(runner, builder));
        CHECK_FALSE(rm::app::toggleAutoExpand(runner, one));
        CHECK_FALSE(rm::app::autoExpanding(runner, builder));
    }

    SECTION("the nearest own-side deposit first, then the hydro, then the enemy's side") {
        const std::array<rm::sim::UnitId, 1> one{builder};
        REQUIRE(rm::app::toggleAutoExpand(runner, one));
        (void)rm::app::advanceMatch(runner, 0, 0);
        const rm::sim::QueuedCommand* first = headOf(builder);
        REQUIRE(first != nullptr);
        CHECK(first->kind() == rm::sim::CommandKind::Build);
        CHECK(first->buildType() == mexType);
        CHECK(siteOf(*first) == std::array<float, 2>{340.0f, 300.0f});

        // The engineer's own pending order claims that deposit: a second idle engineer put on
        // the same standing order the same tick takes the hydro spot with the hydro plant.
        const auto second = job.spawn(*engineer, 300, 310);
        const std::array<rm::sim::UnitId, 1> other{second};
        REQUIRE(rm::app::toggleAutoExpand(runner, other));
        (void)rm::app::advanceMatch(runner, 1, 0);
        const rm::sim::QueuedCommand* hydroOrder = headOf(second);
        REQUIRE(hydroOrder != nullptr);
        CHECK(hydroOrder->buildType() == hydroType);
        CHECK(siteOf(*hydroOrder) == std::array<float, 2>{300.0f, 360.0f});

        // With every own-side deposit spoken for, the enemy-side one is all that is left.
        const auto third = job.spawn(*engineer, 300, 320);
        const std::array<rm::sim::UnitId, 1> last{third};
        REQUIRE(rm::app::toggleAutoExpand(runner, last));
        (void)rm::app::advanceMatch(runner, 2, 0);
        const rm::sim::QueuedCommand* farOrder = headOf(third);
        REQUIRE(farOrder != nullptr);
        CHECK(siteOf(*farOrder) == std::array<float, 2>{700.0f, 300.0f});
    }

    SECTION("a standing extractor claims its deposit, whoever owns it") {
        (void)job.spawn(*extractor, 340, 300, 1);  // the enemy got there first
        const auto from = rm::sim::positionOf(job.scene.store.transforms()[builder.index]);
        const rm::scenario::Marker* pick =
            rm::app::pickExpansionDeposit(job.scene, markers, starts, 0, from);
        REQUIRE(pick != nullptr);
        CHECK(pick->name == "Hydro 1");
    }

    SECTION("a site the sim refuses at dispatch is not asked for again") {
        // A wall of enemy tanks on the nearest deposit: the intake accepts the order (the
        // deposit is real and free of structures), dispatch finds the footprint blocked and
        // drops it. The pass must move on to the hydro spot rather than re-order every tick.
        const auto tank = rm::unitbp::loadFile(root / "UEL0201/UEL0201_unit.bp");
        REQUIRE(tank);
        for (float dz : {-6.0f, 0.0f, 6.0f}) (void)job.spawn(*tank, 340, 300 + dz, 1);
        const std::array<rm::sim::UnitId, 1> one{builder};
        REQUIRE(rm::app::toggleAutoExpand(runner, one));
        for (int tick = 0; tick < 5; ++tick) (void)rm::app::advanceMatch(runner, tick, 0);
        const rm::sim::QueuedCommand* head = headOf(builder);
        REQUIRE(head != nullptr);
        CHECK(head->buildType() == hydroType);
        REQUIRE(runner.autoExpanders.front().refused.size() == 1);

        // The refusal is a grace period, not a verdict: once it lapses and the tanks are gone,
        // a fresh idle engineer is offered the deposit again.
        const rm::TickIndex lapse = runner.autoExpanders.front().refused.front().until;
        for (rm::UnitIndex slot = 0; slot < job.scene.store.transforms().size(); ++slot) {
            if (job.scene.store.slotAlive(slot) && job.scene.armyOf(slot) == 1) {
                job.scene.store.kill(job.scene.store.idAt(slot));
            }
        }
        const auto later = job.spawn(*engineer, 300, 290);
        const std::array<rm::sim::UnitId, 1> next{later};
        REQUIRE(rm::app::toggleAutoExpand(runner, next));
        runner.autoExpanders.back().refused = runner.autoExpanders.front().refused;
        (void)rm::app::advanceMatch(runner, static_cast<int>(lapse), 0);
        REQUIRE(headOf(later) != nullptr);
        CHECK(headOf(later)->buildType() == mexType);
        CHECK(siteOf(*headOf(later)) == std::array<float, 2>{340.0f, 300.0f});
        CHECK(runner.autoExpanders.back().refused.empty());
    }

    SECTION("the standing order resumes after a manual order and stops when the map is full") {
        const std::array<rm::sim::UnitId, 1> one{builder};
        REQUIRE(rm::app::toggleAutoExpand(runner, one));
        // A manual move takes precedence: the pass leaves a busy engineer alone.
        REQUIRE(rm::app::issueMove(job.scene, one, 0, 0, rm::sim::Fx::fromInt(300),
                                   rm::sim::Fx::fromInt(280), false));
        (void)rm::app::advanceMatch(runner, 0, 0);
        REQUIRE(headOf(builder) != nullptr);
        CHECK(headOf(builder)->kind() == rm::sim::CommandKind::Move);
        for (int tick = 1; tick < 400 && headOf(builder) != nullptr
                                     && headOf(builder)->kind() == rm::sim::CommandKind::Move;
             ++tick) {
            (void)rm::app::advanceMatch(runner, tick, 0);
        }
        // Idle again: the next pass hands out the nearest deposit without another click.
        for (int tick = 400; tick < 420 && headOf(builder) == nullptr; ++tick) {
            (void)rm::app::advanceMatch(runner, tick, 0);
        }
        REQUIRE(headOf(builder) != nullptr);
        CHECK(headOf(builder)->kind() == rm::sim::CommandKind::Build);

        // Every deposit taken: the standing order has nothing to say and issues nothing.
        for (std::size_t spot = 0; spot < 3; ++spot) {
            const rm::scenario::Marker& marker = markers[spot];
            (void)job.spawn(marker.isType("Mass") ? *extractor : *hydro, marker.position[0],
                            marker.position[2], 0);
        }
        const auto idle = job.spawn(*engineer, 300, 330);
        const std::array<rm::sim::UnitId, 1> spare{idle};
        REQUIRE(rm::app::toggleAutoExpand(runner, spare));
        for (int tick = 420; tick < 460; ++tick) (void)rm::app::advanceMatch(runner, tick, 0);
        CHECK(headOf(idle) == nullptr);
    }
}

// The review of 64167db/9a9f2d0 named three gaps in the auto-expand and station coverage
// (item 61524). These close them: the END-TO-END chain (a finished mex handing the engineer
// its next deposit), the DETERMINISM half (two runs of a match with an assisting station hash
// identically), and the REPLAY half (a log that contains auto-expand builds reproduces the
// match without the standing order existing). The Kennel scenario beside them is item 61432:
// the same station behaviour against the RETAIL blueprint rather than a synthetic one.

TEST_CASE("auto-expand hands the engineer the next deposit once its mex is standing",
          "[corpus][auto-expand]") {
    const auto root = corpusRoot();
    if (!std::filesystem::is_directory(root)) SKIP("retail corpus unavailable");
    const auto engineer = rm::unitbp::loadFile(root / "UEL0105/UEL0105_unit.bp");
    const auto extractor = rm::unitbp::loadFile(root / "UEB1103/UEB1103_unit.bp");
    const auto hydro = rm::unitbp::loadFile(root / "UEB1102/UEB1102_unit.bp");
    REQUIRE(engineer);
    REQUIRE(extractor);
    REQUIRE(hydro);

    Scenario job;
    const std::vector<rm::scenario::Marker> markers{
        {.name = "Mass 1", .type = "Mass", .position = {340.0f, 0.0f, 300.0f}},
        {.name = "Hydro 1", .type = "Hydrocarbon", .position = {300.0f, 0.0f, 360.0f}},
        {.name = "ARMY_1", .type = "Blank Marker", .position = {200.0f, 0.0f, 300.0f}},
    };
    const std::vector<rm::mapinfo::StartPosition> starts{{.team = 0, .x = 200.0f, .z = 300.0f},
                                                          {.team = 1, .x = 1000.0f, .z = 300.0f}};
    for (const rm::scenario::Marker& marker : markers) {
        const auto kind = rm::app::depositKind(marker);
        if (kind == rm::unitdef::BuildRestriction::None) continue;
        job.scene.resourceDeposits.push_back({kind, rm::sim::fxFromFloat(marker.position[0]),
                                              rm::sim::fxFromFloat(marker.position[2])});
    }
    const std::vector<rm::unitdef::UnitDef> corpus{*engineer, *extractor, *hydro};
    const std::vector<std::string> ids{"UEL0105", "UEB1103", "UEB1102"};
    job.scene.roster = rm::data::Roster::build(corpus, ids);
    const auto mexType = job.registerType(*extractor);
    const auto hydroType = job.registerType(*hydro);
    const auto builder = job.spawn(*engineer, 300, 300);
    job.scene.economies[0].stored = {rm::sim::Mag::fromInt(2000), rm::sim::Mag::fromInt(5000)};
    auto runner = job.runner();
    runner.markers = markers;
    runner.starts = starts;

    const auto headOf = [&](rm::sim::UnitId id) {
        return job.scene.store.orders()[id.index].active();
    };
    const auto standingAt = [&](float x, float z) {
        for (rm::UnitIndex slot = 0; slot < job.scene.store.transforms().size(); ++slot) {
            if (!job.scene.store.slotAlive(slot) || job.scene.armyOf(slot) != 0) continue;
            const auto& where = job.scene.store.transforms()[slot];
            if (rm::sim::fxToFloat(where.x) == x && rm::sim::fxToFloat(where.z) == z) return true;
        }
        return false;
    };

    const std::array<rm::sim::UnitId, 1> one{builder};
    REQUIRE(rm::app::toggleAutoExpand(runner, one));

    // Phase 1: the first deposit is ordered and BUILT — walked to, paid for, finished, and
    // standing on the map as a unit. Not merely ordered: the gap this closes is everything
    // between "ordered" and "standing", because only a finished construction empties the
    // engineer's queue again while the standing extractor claims the site for good.
    int tick = 0;
    bool mexOrdered = false;
    for (; tick < 4000 && !standingAt(340.0f, 300.0f); ++tick) {
        (void)rm::app::advanceMatch(runner, tick, 0);
        if (!mexOrdered && headOf(builder) != nullptr
            && headOf(builder)->kind() == rm::sim::CommandKind::Build) {
            REQUIRE(headOf(builder)->buildType() == mexType);
            mexOrdered = true;
        }
    }
    REQUIRE(mexOrdered);
    REQUIRE(standingAt(340.0f, 300.0f));

    // Phase 2: idle again with the standing order still on and nobody clicking anything, the
    // engineer must be handed the hydro deposit. The pass throttles an engineer it has just
    // looked at to once a second, so give it that long.
    const rm::sim::QueuedCommand* next = nullptr;
    for (int wait = 0; wait < 30 && next == nullptr; ++wait, ++tick) {
        (void)rm::app::advanceMatch(runner, tick, 0);
        next = headOf(builder);
    }
    REQUIRE(next != nullptr);
    CHECK(next->kind() == rm::sim::CommandKind::Build);
    CHECK(next->buildType() == hydroType);
    CHECK(std::array{rm::sim::fxToFloat(next->targetX()), rm::sim::fxToFloat(next->targetZ())}
          == std::array<float, 2>{300.0f, 360.0f});
}

// The Kennel-assist corpus test (KB item 61432) is POSTPONED. Three attempts to stand a live
// Build next to a spawned XEB0104 in this harness all failed the same way: the Build order is
// accepted at intake and then never materialises a construction row — no rows, no events, the
// engineer's queue empty — across site distances 5..40 elmos, with a Mass deposit under the
// site and the roster registered. Root cause not yet found; the synthetic-fixture coverage in
// test_assist.cpp stands until it is.

TEST_CASE("a match with an engineering station standing over live work hashes identically"
          " across two runs",
          "[corpus][station][determinism]") {
    // Item 61524's second gap. The station scan runs inside the sim's assist pass, so its
    // choices are live state; two fresh runs of the same scenario must agree or the standing
    // order's replay promise is false wherever a station is standing. The live work is driven
    // by auto-expand — the known-good Build path in this harness — with the Kennel standing
    // between the two deposits so every construction the engineer founds passes its scan.
    const auto root = corpusRoot();
    if (!std::filesystem::is_directory(root)) SKIP("retail corpus unavailable");
    const auto kennel = rm::unitbp::loadFile(root / "XEB0104/XEB0104_unit.bp");
    const auto engineer = rm::unitbp::loadFile(root / "UEL0105/UEL0105_unit.bp");
    const auto extractor = rm::unitbp::loadFile(root / "UEB1103/UEB1103_unit.bp");
    const auto hydro = rm::unitbp::loadFile(root / "UEB1102/UEB1102_unit.bp");
    REQUIRE(kennel);
    REQUIRE(engineer);
    REQUIRE(extractor);
    REQUIRE(hydro);

    const std::vector<rm::scenario::Marker> markers{
        {.name = "Mass 1", .type = "Mass", .position = {340.0f, 0.0f, 300.0f}},
        {.name = "Hydro 1", .type = "Hydrocarbon", .position = {300.0f, 0.0f, 360.0f}},
        {.name = "ARMY_1", .type = "Blank Marker", .position = {200.0f, 0.0f, 300.0f}},
    };
    const std::vector<rm::mapinfo::StartPosition> starts{{.team = 0, .x = 200.0f, .z = 300.0f},
                                                          {.team = 1, .x = 1000.0f, .z = 300.0f}};

    const auto play = [&](rm::StateHash* final) {
        Scenario job;
        for (const rm::scenario::Marker& marker : markers) {
            const auto kind = rm::app::depositKind(marker);
            if (kind == rm::unitdef::BuildRestriction::None) continue;
            job.scene.resourceDeposits.push_back(
                {kind, rm::sim::fxFromFloat(marker.position[0]),
                 rm::sim::fxFromFloat(marker.position[2])});
        }
        const std::vector<rm::unitdef::UnitDef> corpus{*engineer, *extractor, *hydro};
        const std::vector<std::string> ids{"UEL0105", "UEB1103", "UEB1102"};
        job.scene.roster = rm::data::Roster::build(corpus, ids);
        (void)job.registerType(*extractor);
        (void)job.registerType(*hydro);
        (void)job.spawn(*kennel, 320, 330);  // between the deposits, alive and scanning
        const auto builder = job.spawn(*engineer, 300, 300);
        job.scene.economies[0].stored = {rm::sim::Mag::fromInt(2000), rm::sim::Mag::fromInt(5000)};
        auto runner = job.runner();
        runner.markers = markers;
        runner.starts = starts;
        const std::array<rm::sim::UnitId, 1> one{builder};
        REQUIRE(rm::app::toggleAutoExpand(runner, one));
        std::size_t builds = 0;
        for (int tick = 0; tick < 400; ++tick) {
            (void)rm::app::advanceMatch(runner, tick, 0);
            builds = job.scene.building.size();
        }
        REQUIRE(builds >= 1);  // live work existed for the station's scan to see
        *final = rm::sim::hashMatch(job.scene.store, runner.match);
    };

    rm::StateHash first{};
    rm::StateHash second{};
    play(&first);
    play(&second);
    CHECK(first == second);
}

TEST_CASE("a command log holding auto-expand builds replays to the same match",
          "[corpus][auto-expand][replay]") {
    // Item 61524's third gap. ADR-109's promise is that a replay needs no knowledge of the
    // standing order: every build the pass issues is an ordinary logged Build. Run the live
    // match with auto-expand on, then replay its log into a fresh scenario with the flag
    // never set, and demand the same state hash.
    const auto root = corpusRoot();
    if (!std::filesystem::is_directory(root)) SKIP("retail corpus unavailable");
    const auto engineer = rm::unitbp::loadFile(root / "UEL0105/UEL0105_unit.bp");
    const auto extractor = rm::unitbp::loadFile(root / "UEB1103/UEB1103_unit.bp");
    const auto hydro = rm::unitbp::loadFile(root / "UEB1102/UEB1102_unit.bp");
    REQUIRE(engineer);
    REQUIRE(extractor);
    REQUIRE(hydro);

    const std::vector<rm::scenario::Marker> markers{
        {.name = "Mass 1", .type = "Mass", .position = {340.0f, 0.0f, 300.0f}},
        {.name = "Hydro 1", .type = "Hydrocarbon", .position = {300.0f, 0.0f, 360.0f}},
        {.name = "ARMY_1", .type = "Blank Marker", .position = {200.0f, 0.0f, 300.0f}},
    };
    const std::vector<rm::mapinfo::StartPosition> starts{{.team = 0, .x = 200.0f, .z = 300.0f},
                                                          {.team = 1, .x = 1000.0f, .z = 300.0f}};
    const auto seat = [&](Scenario& job) {
        for (const rm::scenario::Marker& marker : markers) {
            const auto kind = rm::app::depositKind(marker);
            if (kind == rm::unitdef::BuildRestriction::None) continue;
            job.scene.resourceDeposits.push_back(
                {kind, rm::sim::fxFromFloat(marker.position[0]),
                 rm::sim::fxFromFloat(marker.position[2])});
        }
        const std::vector<rm::unitdef::UnitDef> corpus{*engineer, *extractor, *hydro};
        const std::vector<std::string> ids{"UEL0105", "UEB1103", "UEB1102"};
        job.scene.roster = rm::data::Roster::build(corpus, ids);
        (void)job.registerType(*extractor);
        (void)job.registerType(*hydro);
        const auto builder = job.spawn(*engineer, 300, 300);
        job.scene.economies[0].stored = {rm::sim::Mag::fromInt(2000), rm::sim::Mag::fromInt(5000)};
        return builder;
    };

    constexpr int kTicks = 900;  // long enough for the mex to finish and the hydro to be ordered
    Scenario live;
    const auto builder = seat(live);
    auto liveRunner = live.runner();
    liveRunner.markers = markers;
    liveRunner.starts = starts;
    const std::array<rm::sim::UnitId, 1> one{builder};
    REQUIRE(rm::app::toggleAutoExpand(liveRunner, one));
    std::size_t loggedBuilds = 0;
    for (int tick = 0; tick < kTicks; ++tick) {
        (void)rm::app::advanceMatch(liveRunner, tick, 0);
        loggedBuilds = static_cast<std::size_t>(std::count_if(
            live.scene.commands.all().begin(), live.scene.commands.all().end(),
            [](const rm::sim::CommandIssue& issue) {
                return issue.kind == rm::sim::CommandKind::Build;
            }));
    }
    REQUIRE(loggedBuilds >= 2);  // the mex AND the chained hydro, or the run proves nothing

    const auto path = std::filesystem::temp_directory_path() / "rm-auto-expand-replay.commands";
    REQUIRE(rm::sim::writeCommandLog(live.scene.commands, path.string(),
                                     [&live](std::uint32_t type) {
                                         return std::string{live.scene.pathOf(
                                             static_cast<rm::UnitTypeIndex>(type))};
                                     }));
    std::vector<std::string> buildPaths;
    const auto replayLog = rm::sim::readCommandLog(path.string(), &buildPaths);
    REQUIRE(replayLog.has_value());

    Scenario replay;
    (void)seat(replay);
    auto replayRunner = replay.runner();
    replayRunner.markers = markers;
    replayRunner.starts = starts;
    replayRunner.replay = &*replayLog;
    replayRunner.replayPaths = &buildPaths;
    for (int tick = 0; tick < kTicks; ++tick) {
        (void)rm::app::advanceMatch(replayRunner, tick, 0);
    }
    CHECK(rm::sim::hashMatch(replay.scene.store, replayRunner.match)
          == rm::sim::hashMatch(live.scene.store, liveRunner.match));
    std::filesystem::remove(path);
}
