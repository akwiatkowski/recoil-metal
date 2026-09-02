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

TEST_CASE("the app match runner reacquires an attack-move target inside its playable rectangle") {
    const rm::HeightField field = flatField();
    rm::app::UnitScene scene;
    scene.armies = rm::sim::freeForAll(2);
    scene.players = rm::sim::onePlayerPerArmy(2, 0);
    scene.economies.assign(2, rm::sim::Economy{});
    scene.commandersEver.assign(2, 0);

    rm::unitdef::UnitDef fighter;
    fighter.name = "test_fighter";
    fighter.categories = {"TARGET"};
    fighter.motion = rm::unitdef::MotionType::Land;
    fighter.speedElmosPerSecond = 10.0f;
    fighter.weapons = {rm::unitdef::Weapon{
        .role = rm::unitdef::WeaponRole::DirectFire,
        .damage = rm::sim::Mag::fromInt(1),
        .maxRange = rm::sim::Fx::fromInt(300),
        .rateOfFire = 1.0f,
        .targetPriorities = {{"TARGET"}},
        .turreted = true,
    }};
    scene.definitions.push_back(fighter);
    const rm::UnitTypeIndex fighterType =
        scene.catalog.add(&scene.definitions.back(), rm::app::gAppTickRate);
    scene.setTypeTraits(fighterType, rm::data::moveDefFor(fighter), 1.0f);

    const auto spawn = [&](float x, int army) {
        return scene.store.spawn(rm::sim::UnitStore::Spawn{
            .type = fighterType,
            .transform = {.x = rm::sim::fxFromFloat(x), .z = rm::sim::fxFromFloat(40.0f)},
            .motion = rm::app::motionFor(fighter, army),
            .health = rm::sim::initialHealth(rm::sim::Mag::fromInt(100)),
        });
    };
    const rm::sim::UnitId attacker = spawn(40.0f, 0);
    const rm::sim::UnitId firstTarget = spawn(100.0f, 1);
    const rm::sim::UnitId replacementTarget = spawn(160.0f, 1);

    const rm::sim::PlayableRect playable{
        .minX = rm::sim::fxFromFloat(0.0f),
        .maxX = rm::sim::fxFromFloat(256.0f),
        .minZ = rm::sim::fxFromFloat(0.0f),
        .maxZ = rm::sim::fxFromFloat(256.0f),
    };
    rm::app::PassabilitySet passability{field, false, 0.0f};
    rm::vfs::Vfs content;
    rm::app::MatchRunner runner =
        rm::app::makeMatchRunner(scene, field, passability, content, {}, {}, playable);
    runner.scripts.clear();

    REQUIRE(rm::app::issueMove(scene, attacker, 0, 0, rm::sim::fxFromFloat(240.0f),
                                rm::sim::fxFromFloat(40.0f), false,
                                rm::sim::CommandKind::AttackMove));
    (void)rm::app::advanceMatch(runner, 0, 0.0f);
    REQUIRE(scene.store.orders()[attacker.index].current()->target() == firstTarget);

    scene.store.transforms()[firstTarget.index].x = rm::sim::fxFromFloat(300.0f);
    (void)rm::app::advanceMatch(runner, 1, 0.0f);

    REQUIRE(scene.store.orders()[attacker.index].current() != nullptr);
    CHECK(scene.store.orders()[attacker.index].current()->target() == replacementTarget);
}

TEST_CASE("march supplies the map rectangle to automatic target acquisition") {
    const rm::HeightField field = flatField();
    rm::app::UnitScene scene;
    scene.armies = rm::sim::freeForAll(2);
    scene.players = rm::sim::onePlayerPerArmy(2, 0);
    scene.economies.assign(2, rm::sim::Economy{});
    scene.commandersEver.assign(2, 0);

    rm::unitdef::UnitDef fighter;
    fighter.name = "test_fighter";
    fighter.motion = rm::unitdef::MotionType::Land;
    fighter.speedElmosPerSecond = 10.0f;
    fighter.weapons = {rm::unitdef::Weapon{
        .role = rm::unitdef::WeaponRole::DirectFire,
        .damage = rm::sim::Mag::fromInt(1),
        .maxRange = rm::sim::Fx::fromInt(100),
        .rateOfFire = 1.0f,
        .targetPriorities = {{"TARGET"}},
        .turreted = true,
    }};
    scene.definitions.push_back(fighter);
    const rm::UnitTypeIndex fighterType =
        scene.catalog.add(&scene.definitions.back(), rm::app::gAppTickRate);
    scene.setTypeTraits(fighterType, rm::data::moveDefFor(fighter), 1.0f);

    rm::unitdef::UnitDef target;
    target.name = "test_target";
    target.categories = {"TARGET"};
    target.motion = rm::unitdef::MotionType::Land;
    scene.definitions.push_back(target);
    const rm::UnitTypeIndex targetType =
        scene.catalog.add(&scene.definitions.back(), rm::app::gAppTickRate);
    scene.setTypeTraits(targetType, rm::data::moveDefFor(target), 1.0f);

    const auto spawn = [&](rm::UnitTypeIndex type, float x, int army) {
        return scene.store.spawn(rm::sim::UnitStore::Spawn{
            .type = type,
            .transform = {.x = rm::sim::fxFromFloat(x), .z = rm::sim::fxFromFloat(40.0f)},
            .motion = rm::app::motionFor(scene.definitions[type], army),
            .health = rm::sim::initialHealth(rm::sim::Mag::fromInt(100)),
        });
    };
    const float width = field.widthElmos();
    const rm::sim::UnitId attacker = spawn(fighterType, width - 20.0f, 0);
    const rm::sim::UnitId outside = spawn(targetType, width + 10.0f, 1);
    const rm::sim::UnitId inside = spawn(targetType, width - 60.0f, 1);

    rm::app::PassabilitySet passability{field, false, 0.0f};
    rm::vfs::Vfs content;
    std::vector<rm::Particle> dust;
    rm::app::march(scene, field, passability,
                   rm::app::MarchOptions{.orderAll = false,
                                         .seconds = rm::app::gAppTickRate.secondsPerTick()},
                   {}, dust, content, {}, {});

    REQUIRE(scene.store.health()[attacker.index].automaticTargets.size() == 1);
    CHECK(scene.store.health()[attacker.index].automaticTargets[0] == inside);
    CHECK(scene.store.health()[attacker.index].automaticTargets[0] != outside);
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

TEST_CASE("the match runner preserves queued, stopping, replacement, and roll-off commands live and in replay") {
    struct QueueHead {
        rm::sim::UnitId owner;
        rm::sim::CommandKind kind = rm::sim::CommandKind::Stop;
        rm::CommandSerial creationSerial = 0;
    };
    struct Result {
        std::vector<rm::sim::CommandIssue> accepted;
        std::uint32_t sourceCounter = 0;
        rm::CommandSerial commandSerial = 0;
        rm::sim::UnitId stopped;
        rm::sim::UnitId replaced;
        rm::sim::UnitId spawned;
        std::vector<QueueHead> stoppedQueue;
        std::vector<QueueHead> queueHeads;
        bool replacedAlive = false;
        bool spawnedAlive = false;
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

        const auto spawn = [&](rm::UnitTypeIndex type, float x, float z) {
            return scene.store.spawn(rm::sim::UnitStore::Spawn{
                .type = type,
                .transform = {.x = rm::sim::fxFromFloat(x), .z = rm::sim::fxFromFloat(z)},
                .motion = rm::app::motionFor(scene.definitions[type], 0),
                .health = rm::sim::initialHealth(rm::sim::Mag::fromInt(100)),
            });
        };
        const rm::sim::UnitId factoryId = spawn(factoryType, 200.0f, 200.0f);
        const rm::sim::UnitId stopped = spawn(tankType, 300.0f, 200.0f);

        rm::app::PassabilitySet passability{field, false, 0.0f};
        rm::vfs::Vfs content;
        rm::app::MatchRunner runner =
            rm::app::makeMatchRunner(scene, field, passability, content, {}, {});
        runner.scripts.clear();
        runner.replay = replay;

        if (replay == nullptr) {
            REQUIRE(rm::app::issueMove(scene, stopped, 0, 0, rm::sim::fxFromFloat(400.0f),
                                       rm::sim::fxFromFloat(200.0f)));
            REQUIRE(rm::app::issueMove(scene, stopped, 0, 0, rm::sim::fxFromFloat(500.0f),
                                       rm::sim::fxFromFloat(200.0f), true));
        }
        (void)rm::app::advanceMatch(runner, 0, 0.0f);

        const auto queuedHead = [&](rm::sim::UnitId unit) {
            const rm::sim::QueuedCommand* entry = scene.store.orders()[unit.index].currentEntry();
            REQUIRE(entry != nullptr);
            return QueueHead{.owner = entry->unit(),
                             .kind = entry->kind(),
                             .creationSerial = entry->payload().creationSerial};
        };
        const rm::sim::CommandQueue& stoppedOrders = scene.store.orders()[stopped.index];
        REQUIRE(stoppedOrders.size() == 2);
        REQUIRE(stoppedOrders.currentEntry() == &stoppedOrders.entries()[0]);
        REQUIRE(stoppedOrders.activeEntry() == stoppedOrders.currentEntry());
        CHECK(stoppedOrders.entries()[0].kind() == rm::sim::CommandKind::Move);
        CHECK(stoppedOrders.entries()[0].payload().creationSerial == 0);
        CHECK(stoppedOrders.entries()[1].kind() == rm::sim::CommandKind::Move);
        CHECK(stoppedOrders.entries()[1].payload().creationSerial == 1);
        const std::vector<QueueHead> stoppedQueue = {
            queuedHead(stopped),
            QueueHead{.owner = stoppedOrders.entries()[1].unit(),
                      .kind = stoppedOrders.entries()[1].kind(),
                      .creationSerial = stoppedOrders.entries()[1].payload().creationSerial},
        };

        if (replay == nullptr) {
            REQUIRE(rm::app::issueMove(scene, stopped, 0, 1, {}, {}, false,
                                       rm::sim::CommandKind::Stop));
        }
        (void)rm::app::advanceMatch(runner, 1, 0.0f);
        CHECK(scene.store.orders()[stopped.index].empty());
        CHECK_FALSE(scene.store.motion()[stopped.index].moving);
        CHECK(scene.store.motion()[stopped.index].path.empty());

        scene.store.kill(stopped);
        const rm::sim::UnitId replaced = spawn(tankType, 300.0f, 200.0f);

        if (replay == nullptr) {
            REQUIRE(rm::app::issueMove(scene, replaced, 0, 2, rm::sim::fxFromFloat(500.0f),
                                       rm::sim::fxFromFloat(200.0f)));
            REQUIRE(rm::app::issueBuild(scene, factoryId, 0, 2, tankType,
                                         rm::sim::fxFromFloat(200.0f),
                                         rm::sim::fxFromFloat(200.0f)));
        }

        for (int tick = 2; tick < 5; ++tick) {
            (void)rm::app::advanceMatch(runner, tick, 0.0f);
        }

        const rm::sim::UnitId spawned = scene.store.idAt(2);
        const auto queueHead = [&](rm::sim::UnitId unit) {
            const rm::sim::QueuedCommand* entry = scene.store.orders()[unit.index].currentEntry();
            REQUIRE(entry != nullptr);
            return QueueHead{.owner = entry->unit(),
                             .kind = entry->kind(),
                             .creationSerial = entry->payload().creationSerial};
        };

        return Result{
            .accepted = {scene.commands.all().begin(), scene.commands.all().end()},
            .sourceCounter = scene.store.nextCommandCounter(0),
            .commandSerial = scene.store.nextCommandSerial(),
            .stopped = stopped,
            .replaced = replaced,
            .spawned = spawned,
            .stoppedQueue = stoppedQueue,
            .queueHeads = {queueHead(replaced), queueHead(spawned)},
            .replacedAlive = scene.store.alive(replaced),
            .spawnedAlive = scene.store.alive(spawned),
            .hash = rm::sim::hashMatch(scene.store, runner.match),
        };
    };

    const Result live = run(nullptr);
    REQUIRE(live.accepted.size() == 6);
    CHECK_FALSE(live.accepted[0].queued);
    CHECK(live.accepted[0].kind == rm::sim::CommandKind::Move);
    CHECK(live.accepted[0].units == std::vector{live.stopped});
    CHECK(live.accepted[1].queued);
    CHECK(live.accepted[1].kind == rm::sim::CommandKind::Move);
    CHECK(live.accepted[1].units == std::vector{live.stopped});
    CHECK_FALSE(live.accepted[2].queued);
    CHECK(live.accepted[2].kind == rm::sim::CommandKind::Stop);
    CHECK(live.accepted[2].units == std::vector{live.stopped});
    CHECK(live.accepted[3].units == std::vector{live.replaced});
    CHECK(live.accepted[5].phase == rm::sim::CommandPhase::PostSpawn);
    CHECK(live.accepted[5].kind == rm::sim::CommandKind::Move);
    CHECK(live.accepted[5].units == std::vector{live.spawned});
    const auto rolloff = rm::sim::rolloffPoint(
        {rm::sim::fxFromFloat(200.0f), {}, rm::sim::fxFromFloat(200.0f)},
        rm::sim::Fx::fromInt(128 * rm::kSquareSize / 2),
        rm::sim::Fx::fromInt(128 * rm::kSquareSize / 2));
    CHECK(live.accepted[5].targetX == rolloff[0]);
    CHECK(live.accepted[5].targetZ == rolloff[1]);
    CHECK(live.sourceCounter == 6);
    CHECK(live.commandSerial == 6);
    REQUIRE(live.stoppedQueue.size() == 2);
    CHECK(live.stoppedQueue[0].owner == live.stopped);
    CHECK(live.stoppedQueue[0].kind == rm::sim::CommandKind::Move);
    CHECK(live.stoppedQueue[0].creationSerial == 0);
    CHECK(live.stoppedQueue[1].owner == live.stopped);
    CHECK(live.stoppedQueue[1].kind == rm::sim::CommandKind::Move);
    CHECK(live.stoppedQueue[1].creationSerial == 1);
    REQUIRE(live.queueHeads.size() == 2);
    CHECK(live.queueHeads[0].owner == live.replaced);
    CHECK(live.queueHeads[0].kind == rm::sim::CommandKind::Move);
    CHECK(live.queueHeads[0].creationSerial == 3);
    CHECK(live.queueHeads[1].owner == live.spawned);
    CHECK(live.queueHeads[1].kind == rm::sim::CommandKind::Move);
    CHECK(live.queueHeads[1].creationSerial == 5);
    CHECK(live.replacedAlive);
    CHECK(live.spawnedAlive);

    rm::sim::CommandLog replayLog;
    for (const rm::sim::CommandIssue& issue : live.accepted) {
        REQUIRE(replayLog.record(issue));
    }
    const Result replay = run(&replayLog);

    CHECK(replay.accepted == live.accepted);
    CHECK(replay.sourceCounter == live.sourceCounter);
    CHECK(replay.commandSerial == live.commandSerial);
    CHECK(replay.stopped == live.stopped);
    CHECK(replay.replaced == live.replaced);
    CHECK(replay.spawned == live.spawned);
    REQUIRE(replay.stoppedQueue.size() == live.stoppedQueue.size());
    for (std::size_t index = 0; index < live.stoppedQueue.size(); ++index) {
        CHECK(replay.stoppedQueue[index].owner == live.stoppedQueue[index].owner);
        CHECK(replay.stoppedQueue[index].kind == live.stoppedQueue[index].kind);
        CHECK(replay.stoppedQueue[index].creationSerial == live.stoppedQueue[index].creationSerial);
    }
    REQUIRE(replay.queueHeads.size() == live.queueHeads.size());
    for (std::size_t index = 0; index < live.queueHeads.size(); ++index) {
        CHECK(replay.queueHeads[index].owner == live.queueHeads[index].owner);
        CHECK(replay.queueHeads[index].kind == live.queueHeads[index].kind);
        CHECK(replay.queueHeads[index].creationSerial == live.queueHeads[index].creationSerial);
    }
    CHECK(replay.replacedAlive == live.replacedAlive);
    CHECK(replay.spawnedAlive == live.spawnedAlive);
    CHECK(replay.hash == live.hash);
}

TEST_CASE("the match runner dispatches a live repair through replay's command path") {
    struct Result {
        std::vector<rm::sim::CommandIssue> commands;
        rm::sim::Mag health;
        std::uint64_t hash = 0;
    };
    const auto run = [](const rm::sim::CommandLog* replay) {
        const rm::HeightField field = flatField();
        rm::app::UnitScene scene;
        scene.armies = rm::sim::freeForAll(1);
        scene.players = rm::sim::onePlayerPerArmy(1, 0);
        scene.economies.assign(1, rm::sim::Economy{});
        scene.commandersEver.assign(1, 0);

        rm::unitdef::UnitDef engineer;
        engineer.name = "test_engineer";
        engineer.buildRate = 10.0f;
        scene.definitions.push_back(engineer);
        const rm::UnitTypeIndex engineerType =
            scene.catalog.add(&scene.definitions.back(), rm::app::gAppTickRate);
        scene.setTypeTraits(engineerType, rm::data::moveDefFor(engineer), 1.0f);

        rm::unitdef::UnitDef tank;
        tank.name = "test_tank";
        tank.buildCostMass = rm::sim::Mag::fromInt(100);
        tank.buildCostEnergy = rm::sim::Mag::fromInt(200);
        tank.buildTime = rm::sim::Mag::fromInt(100);
        scene.definitions.push_back(tank);
        const rm::UnitTypeIndex tankType =
            scene.catalog.add(&scene.definitions.back(), rm::app::gAppTickRate);
        scene.setTypeTraits(tankType, rm::data::moveDefFor(tank), 1.0f);

        const auto spawn = [&](rm::UnitTypeIndex type, float x) {
            return scene.store.spawn(rm::sim::UnitStore::Spawn{
                .type = type,
                .transform = {.x = rm::sim::fxFromFloat(x), .z = rm::sim::fxFromFloat(200.0f)},
                .motion = rm::app::motionFor(scene.definitions[type], 0),
                .health = rm::sim::initialHealth(rm::sim::Mag::fromInt(100)),
            });
        };
        const rm::sim::UnitId builder = spawn(engineerType, 200.0f);
        const rm::sim::UnitId target = spawn(tankType, 210.0f);
        scene.store.health()[target.index].current = rm::sim::Mag::fromInt(50);

        rm::app::PassabilitySet passability{field, false, 0.0f};
        rm::vfs::Vfs content;
        rm::app::MatchRunner runner =
            rm::app::makeMatchRunner(scene, field, passability, content, {}, {});
        runner.scripts.clear();
        runner.replay = replay;
        runner.match.baseStorage = {.mass = rm::sim::Mag::fromInt(1000),
                                    .energy = rm::sim::Mag::fromInt(1000)};
        scene.economies[0].stored = runner.match.baseStorage;

        if (replay == nullptr) {
            REQUIRE(rm::app::issueRepair(scene, std::span{&builder, std::size_t{1}}, 0, 0,
                                         target));
        }
        for (int tick = 0; tick < 3; ++tick) {
            (void)rm::app::advanceMatch(runner, tick, 0.0f);
        }

        REQUIRE(scene.commands.size() == 1);
        CHECK(scene.commands.all()[0].kind == rm::sim::CommandKind::Repair);
        CHECK(scene.commands.all()[0].units == std::vector{builder});
        REQUIRE(scene.store.orders()[builder.index].current() != nullptr);
        CHECK(scene.store.orders()[builder.index].current()->kind() == rm::sim::CommandKind::Repair);
        CHECK(scene.store.health()[target.index].current > rm::sim::Mag::fromInt(50));
        return Result{.commands = {scene.commands.all().begin(), scene.commands.all().end()},
                      .health = scene.store.health()[target.index].current,
                      .hash = rm::sim::hashMatch(scene.store, runner.match)};
    };

    const Result live = run(nullptr);
    rm::sim::CommandLog replay;
    REQUIRE(replay.record(live.commands[0]));
    const Result replayed = run(&replay);
    CHECK(replayed.commands == live.commands);
    CHECK(replayed.health == live.health);
    CHECK(replayed.hash == live.hash);
}
