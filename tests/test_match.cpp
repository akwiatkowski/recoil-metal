#include "app/Match.hpp"

#include "core/data/MoveDef.hpp"
#include "core/map/HeightField.hpp"
#include "core/sim/StateHash.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <string_view>
#include <vector>

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

} // namespace

TEST_CASE("the match runner preserves same-army path FIFO latency live and in replay") {
    const auto run = [](bool replay) {
        const rm::HeightField field = flatField();
        rm::app::UnitScene scene;
        scene.armies = rm::sim::freeForAll(2);
        scene.players = rm::sim::onePlayerPerArmy(2, 0);
        scene.economies.assign(2, rm::sim::Economy{});
        scene.commandersEver.assign(2, 0);

        rm::unitdef::UnitDef tank;
        tank.name = "test_tank";
        tank.motion = rm::unitdef::MotionType::Land;
        tank.speedElmosPerSecond = 10.0f;
        scene.definitions.push_back(tank);
        const rm::UnitTypeIndex type =
            scene.catalog.add(&scene.definitions.back(), rm::app::gAppTickRate);
        scene.setTypeTraits(type, rm::data::moveDefFor(tank), 1.0f);

        const auto spawn = [&](float z) {
            return scene.store.spawn(rm::sim::UnitStore::Spawn{
                .type = type,
                .transform = {.x = rm::sim::fxFromFloat(200.0f), .z = rm::sim::fxFromFloat(z)},
                .motion = rm::app::motionFor(tank, 0),
                .health = rm::sim::initialHealth(rm::sim::Mag::fromInt(100)),
            });
        };
        const rm::sim::UnitId first = spawn(200.0f);
        const rm::sim::UnitId second = spawn(300.0f);

        rm::app::PassabilitySet passability{field, false, 0.0f};
        rm::vfs::Vfs content;
        rm::sim::CommandLog replayLog;
        rm::app::MatchRunner runner =
            rm::app::makeMatchRunner(scene, field, passability, content, {}, {});
        runner.scripts.clear();

        if (replay) {
            REQUIRE(replayLog.record(rm::sim::CommandIssue{
                .tick = 0,
                .source = static_cast<rm::CommandSource>(0),
                .id = rm::commandId(static_cast<rm::CommandSource>(0), 0),
                .player = 0,
                .kind = rm::sim::CommandKind::Move,
                .units = {first},
                .targetX = rm::sim::fxFromFloat(500.0f),
                .targetZ = rm::sim::fxFromFloat(200.0f),
            }));
            REQUIRE(replayLog.record(rm::sim::CommandIssue{
                .tick = 0,
                .source = static_cast<rm::CommandSource>(0),
                .id = rm::commandId(static_cast<rm::CommandSource>(0), 1),
                .player = 0,
                .kind = rm::sim::CommandKind::Move,
                .units = {second},
                .targetX = rm::sim::fxFromFloat(500.0f),
                .targetZ = rm::sim::fxFromFloat(300.0f),
            }));
            runner.replay = &replayLog;
        } else {
            REQUIRE(rm::app::issueMove(scene, first, 0, 0, rm::sim::fxFromFloat(500.0f),
                                        rm::sim::fxFromFloat(200.0f)));
            REQUIRE(rm::app::issueMove(scene, second, 0, 0, rm::sim::fxFromFloat(500.0f),
                                        rm::sim::fxFromFloat(300.0f)));
        }

        (void)rm::app::advanceMatch(runner, 0, 0.0f);
        CHECK(scene.store.motion()[first.index].path.empty());
        CHECK(scene.store.motion()[second.index].path.empty());

        (void)rm::app::advanceMatch(runner, 1, 0.0f);
        CHECK_FALSE(scene.store.motion()[first.index].path.empty());
        CHECK(scene.store.motion()[second.index].path.empty());

        (void)rm::app::advanceMatch(runner, 2, 0.0f);
        CHECK_FALSE(scene.store.motion()[second.index].path.empty());
    };

    SECTION("live submissions") { run(false); }
    SECTION("replay submissions") { run(true); }
}

TEST_CASE("the match runner replays a build and its post-spawn roll-off semantically") {
    struct Result {
        std::vector<rm::sim::CommandIssue> accepted;
        std::uint32_t sourceCounter = 0;
        rm::CommandSerial commandSerial = 0;
        rm::sim::UnitId spawned;
        rm::CommandSerial spawnedOrderSerial = 0;
        rm::StateHash hash = 0;
    };

    const auto run = [](const rm::sim::CommandLog* replay) {
        const rm::HeightField field = flatField();
        rm::app::UnitScene scene;
        scene.armies = rm::sim::freeForAll(1);
        scene.players = rm::sim::onePlayerPerArmy(1, 0);
        scene.economies.assign(1, rm::sim::Economy{});
        scene.commandersEver.assign(1, 0);

        rm::unitdef::UnitDef factory;
        factory.name = "test_factory";
        factory.buildRate = 60.0f;
        factory.buildableCategory = {{"TEST_TANK"}};
        scene.definitions.push_back(factory);
        const rm::UnitTypeIndex factoryType =
            scene.catalog.add(&scene.definitions.back(), rm::app::gAppTickRate);
        scene.setTypeTraits(factoryType, rm::data::moveDefFor(factory), 1.0f);

        rm::unitdef::UnitDef tank;
        tank.name = "test_tank";
        tank.categories = {"TEST_TANK"};
        tank.motion = rm::unitdef::MotionType::Land;
        tank.speedElmosPerSecond = 10.0f;
        tank.buildTime = rm::sim::magFromFloat(1.0f);
        scene.definitions.push_back(tank);
        const rm::UnitTypeIndex tankType =
            scene.catalog.add(&scene.definitions.back(), rm::app::gAppTickRate);
        scene.setTypeTraits(tankType, rm::data::moveDefFor(tank), 1.0f);
        constexpr std::string_view kTankPath{"/test_tank"};
        scene.setPathForType(tankType, kTankPath);
        scene.typeForBlueprint.emplace(kTankPath, tankType);

        const rm::sim::UnitId factoryId = scene.store.spawn(rm::sim::UnitStore::Spawn{
            .type = factoryType,
            .transform = {.x = rm::sim::fxFromFloat(200.0f),
                          .z = rm::sim::fxFromFloat(200.0f)},
            .motion = rm::app::motionFor(factory, 0),
            .health = rm::sim::initialHealth(rm::sim::Mag::fromInt(100)),
        });

        rm::app::PassabilitySet passability{field, false, 0.0f};
        rm::vfs::Vfs content;
        rm::app::MatchRunner runner =
            rm::app::makeMatchRunner(scene, field, passability, content, {}, {});
        runner.scripts.clear();
        runner.replay = replay;

        if (replay == nullptr) {
            REQUIRE(rm::app::issueBuild(scene, factoryId, 0, 0, tankType,
                                        rm::sim::fxFromFloat(200.0f),
                                        rm::sim::fxFromFloat(200.0f)));
        }

        for (int tick = 0; tick < 4; ++tick) {
            (void)rm::app::advanceMatch(runner, tick, 0.0f);
        }

        const rm::sim::UnitId spawned = scene.store.idAt(1);
        const rm::sim::QueuedCommand* spawnedOrder =
            scene.store.orders()[spawned.index].currentEntry();
        REQUIRE(spawnedOrder != nullptr);

        Result result{
            .accepted = {scene.commands.all().begin(), scene.commands.all().end()},
            .sourceCounter = scene.store.nextCommandCounter(0),
            .commandSerial = scene.store.nextCommandSerial(),
            .spawned = spawned,
            .spawnedOrderSerial = spawnedOrder->payload().creationSerial,
            .hash = rm::sim::hashMatch(scene.store, runner.match),
        };

        if (replay != nullptr) {
            const std::vector<rm::sim::CommandIssue> recorded{replay->all().begin(),
                                                               replay->all().end()};
            scene.store.orders()[spawned.index].currentEntryMutable()->setTargetPosition(
                rm::sim::fxFromFloat(300.0f), rm::sim::fxFromFloat(300.0f));
            CHECK(std::equal(replay->all().begin(), replay->all().end(), recorded.begin(),
                             recorded.end()));
        }
        return result;
    };

    const Result live = run(nullptr);
    REQUIRE(live.accepted.size() == 2);
    CHECK(live.accepted[0].phase == rm::sim::CommandPhase::PreTick);
    CHECK(live.accepted[0].kind == rm::sim::CommandKind::Build);
    CHECK(live.accepted[1].phase == rm::sim::CommandPhase::PostSpawn);
    CHECK(live.accepted[1].kind == rm::sim::CommandKind::Move);
    CHECK(live.accepted[0].units == std::vector{rm::sim::UnitId{0, 1}});
    CHECK(live.accepted[1].units == std::vector{live.spawned});
    const auto rolloff = rm::sim::rolloffPoint(
        {rm::sim::fxFromFloat(200.0f), {}, rm::sim::fxFromFloat(200.0f)},
        rm::sim::Fx::fromInt(128 * rm::kSquareSize / 2),
        rm::sim::Fx::fromInt(128 * rm::kSquareSize / 2));
    CHECK(live.accepted[1].targetX == rolloff[0]);
    CHECK(live.accepted[1].targetZ == rolloff[1]);
    CHECK(live.sourceCounter == 2);
    CHECK(live.commandSerial == 2);
    CHECK(live.spawnedOrderSerial == 1);

    rm::sim::CommandLog replayLog;
    for (const rm::sim::CommandIssue& issue : live.accepted) {
        REQUIRE(replayLog.record(issue));
    }
    const Result replay = run(&replayLog);

    CHECK(replay.accepted == live.accepted);
    CHECK(replay.sourceCounter == live.sourceCounter);
    CHECK(replay.commandSerial == live.commandSerial);
    CHECK(replay.spawned == live.spawned);
    CHECK(replay.spawnedOrderSerial == live.spawnedOrderSerial);
    CHECK(replay.hash == live.hash);
}
