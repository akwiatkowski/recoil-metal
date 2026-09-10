// Turret aim end to end: a turreted unit with a live target draws with its
// yaw/pitch bones deflected, and returns to rest when the target dies.
//
// Fully synthetic — no content, no models on disk. The batch carries a rig resolved
// from a four-bone model, the sim supplies the per-weapon target, and gather reads
// the same path the frame loop does. dtSeconds is zero, so the slew snaps and the
// assertion sees the goal exactly.
#include "app/Match.hpp"

#include "core/data/MoveDef.hpp"
#include "core/map/HeightField.hpp"
#include <catch2/catch_test_macros.hpp>

#include "support/FxMatchers.hpp"

#include <numbers>

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

    scene.publish(4);
    scene.publish(4);
    scene.gatherForDrawing(1.0f, nullptr, {}, 0.0f);
    REQUIRE(scene.batches[0].instances.size() == 2);
    // One instance aims ~90 degrees to the side; the other (the target, whose own
    // turret aims back) is equally deflected. Both are far from rest.
    bool aimed = false;
    for (const rm::UnitInstance& instance : scene.batches[0].instances) {
        if (std::abs(instance.builderYaw) > 1.0f) {
            aimed = true;
        }
    }
    CHECK(aimed);
}
