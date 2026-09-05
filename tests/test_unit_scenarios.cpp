// Behavioral contracts run real factory products through the same runner as the window.
#include "app/Match.hpp"
#include "app/Interface.hpp"
#include "app/SceneBuild.hpp"
#include "core/data/MoveDef.hpp"
#include "core/sim/StateHash.hpp"
#include "core/unit/BuildTree.hpp"
#include "core/unit/Role.hpp"
#include "core/unit/UnitBlueprint.hpp"
#include "core/ui/CommandPanel.hpp"
#include "core/log/Log.hpp"

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
    rm::HeightField field = flatField();
    rm::app::UnitScene scene;
    rm::app::PassabilitySet passability{field, false, 0.0f};
    rm::vfs::Vfs content;

    Scenario() {
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
    CHECK(std::count(log.begin(), log.end(), '\n') < 20);  // no per-tick flood
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
    for (const std::string name : {"hud-construction.commands", "hud-production.commands"}) {
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
