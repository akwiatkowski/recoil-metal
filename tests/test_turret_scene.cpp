// Turret aim end to end: a turreted unit with a live target draws with its
// yaw/pitch bones deflected, and returns to rest when the target dies.
//
// Fully synthetic — no content, no models on disk. The batch carries a rig resolved
// from a four-bone model, the sim supplies the per-weapon target, and gather reads
// the same path the frame loop does. Nonzero frame time exercises visible slew;
// independently specified barrel points measure alignment after settling.
#include "app/Match.hpp"

#include "core/data/MoveDef.hpp"
#include "core/map/HeightField.hpp"
#include "core/log/Log.hpp"
#include <catch2/catch_test_macros.hpp>

#include "support/FxMatchers.hpp"

#include <numbers>
#include <filesystem>
#include <fstream>
#include <iterator>

using Catch::Approx;

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

[[nodiscard]] rm::unitdef::UnitDef turretTank() {
    rm::unitdef::UnitDef tank;
    tank.name = "test_turret_tank";
    tank.motion = rm::unitdef::MotionType::Land;
    tank.speedElmosPerSecond = 10.0f;
    tank.health = rm::sim::Mag::fromInt(100);
    tank.categories = {"LAND"};
    rm::unitdef::Weapon gun;
    gun.label = "test turret gun";
    gun.role = rm::unitdef::WeaponRole::DirectFire;
    gun.targetPriorities = {{"LAND"}};
    gun.damage = rm::test::mag(10.0f);
    gun.maxRange = rm::test::fx(200.0f);
    gun.rateOfFire = 1.0f;
    gun.muzzleVelocityElmosPerSecond = 100.0f;
    gun.turreted = true;
    gun.turretYawBone = "Turret";
    gun.turretPitchBone = "Barrel";
    gun.muzzleBone = "Muzzle";
    gun.turretYawSpeedRadPerSecond = 2.0f;
    gun.turretPitchSpeedRadPerSecond = 1.5f;
    tank.weapons.push_back(gun);
    return tank;
}

[[nodiscard]] rm::Model turretModel() {
    rm::Model model;
    model.family = rm::Family::SupremeCommander;
    model.bones = {
        rm::ModelBone{.name = "Hull", .parent = -1},
        rm::ModelBone{.name = "Turret", .parent = 0, .offset = {{0.0f, 1.0f, 0.0f}},
                      .globalOffset = {{0.0f, 1.0f, 0.0f}}},
        rm::ModelBone{.name = "Barrel", .parent = 1, .globalOffset = {{0.0f, 1.0f, 0.0f}}},
        rm::ModelBone{.name = "Muzzle", .parent = 2, .offset = {{0.0f, 0.0f, 3.0f}},
                      .globalOffset = {{0.0f, 1.0f, 3.0f}}},
    };
    return model;
}

} // namespace

TEST_CASE("a turreted unit aims its drawn turret at its live target", "[turret]") {
    const rm::HeightField field = flatField();
    rm::app::UnitScene scene;
    scene.armies = rm::sim::freeForAll(2);
    scene.players = rm::sim::onePlayerPerArmy(2, 0);
    scene.economies.assign(2, rm::sim::Economy{});

    const rm::unitdef::UnitDef tank = turretTank();
    scene.definitions.push_back(tank);
    const rm::UnitTypeIndex type =
        scene.catalog.add(&scene.definitions.back(), rm::app::gAppTickRate);
    scene.setTypeTraits(type, rm::data::moveDefFor(tank), 1.0f);

    scene.models.push_back(turretModel());
    const rm::TurretAimSpec spec{
        .yawBone = {.name = "Turret"},
        .pitchBone = {.name = "Barrel"},
        .muzzleBone = {.name = "Muzzle"},
        .yawSlew = 2.0f,
        .pitchSlew = 1.5f,
    };
    scene.batches.push_back(rm::UnitBatch{
        .model = &scene.models.back(),
        .turretAim = rm::resolveTurretAim(scene.models.back(), spec),
        .turretWeapon = 0,
    });
    REQUIRE(scene.batches.back().turretAim.exists());
    scene.setBatchForType(type, 0);
    // The sim's own mount: slew, gate and launch origin all read the catalog,
    // not the drawn rig. The muzzle offset is what resolveMuzzleBones would
    // write — the Muzzle bone's rest position in elmos.
    scene.definitions.back().weapons[0].visualMuzzleOffset = {0.0f, 1.0f, 3.0f};
    rm::app::publishTurretMount(scene, type, 1.0f);
    REQUIRE(scene.catalog.turretMount(type).present);

    // Shooter faces +Z; the target stands off to +X, so an aiming turret shows yaw.
    const rm::sim::UnitId shooter = scene.store.spawn(rm::sim::UnitStore::Spawn{
        .type = type,
        .transform = {.x = rm::sim::fxFromFloat(0.0f), .z = rm::sim::fxFromFloat(0.0f)},
        .motion = rm::app::motionFor(tank, 0),
        .health = rm::sim::initialHealth(rm::sim::Mag::fromInt(100)),
    });
    const rm::sim::UnitId target = scene.store.spawn(rm::sim::UnitStore::Spawn{
        .type = type,
        .transform = {.x = rm::sim::fxFromFloat(50.0f), .z = rm::sim::fxFromFloat(0.0f)},
        .motion = rm::app::motionFor(tank, 1),
        .health = rm::sim::initialHealth(rm::sim::Mag::fromInt(100)),
    });

    rm::app::PassabilitySet passability{field, false, 0.0f};
    rm::vfs::Vfs content;
    rm::app::MatchRunner runner =
        rm::app::makeMatchRunner(scene, field, passability, content, {}, {});
    runner.scripts.clear();
    for (int tick = 0; tick < 5; ++tick) {
        (void)rm::app::advanceMatch(runner, tick, 0.0f);
    }
    // The sim acquired the target for the turreted weapon.
    REQUIRE(scene.store.health()[shooter.index].automaticTargets.size() == 1);
    CHECK(scene.store.health()[shooter.index].automaticTargets[0] == target);
    // The SIM slews the ring now: 2 rad/s at ten ticks a second lands the
    // quarter turn in ~8 ticks, and the fire gate holds shots until it does.
    for (int tick = 5; tick < 20; ++tick) {
        (void)rm::app::advanceMatch(runner, tick, 0.0f);
    }

    scene.publish(19);
    scene.publish(19);
    scene.gatherForDrawing(1.0f, nullptr, {}, 0.0f);
    REQUIRE(scene.batches[0].instances.size() == 2);
    for (const rm::UnitInstance& instance : scene.batches[0].instances) {
        // Both shooters have a barrel pivot one unit above ground. Their enemies
        // are 50 units away, at ground level: this expectation never calls aimAt.
        const float dx = instance.position[0] < 25.0f ? 50.0f : -50.0f;
        const auto error = instance.barrelAlignmentErrorDegrees(
            scene.batches[0].turretAim, 2, {0, 1, 0}, {0, 1, 3}, {dx, -1, 0});
        REQUIRE(error.has_value());
        INFO("barrel alignment error (degrees): " << *error);
        // Sub-degree, not zero: the solve aims the MUZZLE at the target while
        // this measures the pitch bone's own axis — a bone base an elmo off the
        // muzzle keeps ~0.2 degrees of parallax no solve can remove.
        CHECK(*error < 0.5f);
    }

    const auto logPath = std::filesystem::temp_directory_path() / "rm-trial-alignment.log";
    std::filesystem::remove(logPath);
    REQUIRE(rm::log::configure({.filePath = logPath.string(), .stderrEnabled = false}));
    struct RestoreLogging {
        ~RestoreLogging() { (void)rm::log::configure({}); }
    } restoreLogging;
    scene.trialAlignmentShots.push_back(rm::sim::Event{
        .kind = rm::sim::EventKind::WeaponFired, .unit = shooter,
        .visualId = "test_turret_tank:test turret gun",
        .launchVelocity = {rm::sim::Fx{}, rm::sim::Fx{}, rm::sim::Fx::fromInt(1)}});
    scene.gatherForDrawing(1.0f, nullptr, {}, 1.0f / 60.0f);
    CHECK(scene.trialAlignmentShots.empty());
    // A deliberately sideways launch must print 90, not a success flag.
    std::ifstream logInput(logPath);
    const std::string logged{std::istreambuf_iterator<char>{logInput}, {}};
    CHECK(logged.find("[trial-aim]") != std::string::npos);
    // The barrel aims at the LIVE target, not exactly ±X, so the sideways
    // launch lands a few degrees off the literal 90 — parse, don't grep.
    const auto degreePos = logged.find("error_deg=");
    REQUIRE(degreePos != std::string::npos);
    CHECK(std::strtof(logged.c_str() + degreePos + 10, nullptr) > 45.0f);
    CHECK(logged.find("weapon=test_turret_tank:test turret gun") != std::string::npos);
    CHECK(logged.find('\033') == std::string::npos);
    std::filesystem::remove(logPath);
}
