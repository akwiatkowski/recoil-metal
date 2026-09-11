// Recoil slides: a barrel kicks back along its aim axis when its weapon fires,
// then runs home at the authored return speed.
//
// The rig is one subtree flag per bone (the recoil bone and everything bolted to
// it); the motion is one scalar per instance (0 at rest, 1 fully kicked) that the
// shader multiplies by the batch's travel distance. Kick on WeaponFired, decay by
// return speed — both caller-side, both deterministic.
#include "core/model/BuilderAim.hpp"

#include "app/Match.hpp"
#include "core/data/MoveDef.hpp"
#include "core/map/HeightField.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "support/FxMatchers.hpp"

using Catch::Approx;
#include <cstdint>

namespace {

[[nodiscard]] rm::Model gunModel() {
    rm::Model model;
    model.family = rm::Family::SupremeCommander;
    model.bones = {
        rm::ModelBone{.name = "Hull", .parent = -1},
        rm::ModelBone{.name = "Turret", .parent = 0, .offset = {{0.0f, 1.0f, 0.0f}},
                      .globalOffset = {{0.0f, 1.0f, 0.0f}}},
        rm::ModelBone{.name = "Barrel", .parent = 1, .globalOffset = {{0.0f, 1.0f, 0.0f}}},
        rm::ModelBone{.name = "Muzzle", .parent = 2, .offset = {{0.0f, 0.0f, 3.0f}},
                      .globalOffset = {{0.0f, 1.0f, 3.0f}}},
        rm::ModelBone{.name = "Rack", .parent = 2, .offset = {{0.5f, 0.0f, 1.0f}},
                      .globalOffset = {{0.5f, 1.0f, 1.0f}}},
    };
    return model;
}

} // namespace

TEST_CASE("a recoil rig marks only the rack subtree", "[recoil]") {
    const std::vector<std::uint32_t> flags = rm::resolveRecoilFlags(gunModel(), "Rack");
    REQUIRE(flags.size() == 5);
    CHECK(flags[0] == 0U);
    CHECK(flags[1] == 0U);
    CHECK(flags[2] == 0U);
    CHECK(flags[3] == 0U);
    CHECK(flags[4] == rm::kBuilderRecoilBone);
}

TEST_CASE("recoil bone names match case-insensitively", "[recoil]") {
    CHECK(rm::resolveRecoilFlags(gunModel(), "NoSuchBone").empty());
    rm::Model model = gunModel();
    model.bones[4].name = "rack";
    const std::vector<std::uint32_t> flags = rm::resolveRecoilFlags(model, "Rack");
    REQUIRE(flags.size() == 5);
    CHECK(flags[4] == rm::kBuilderRecoilBone);
}

TEST_CASE("recoil decays linearly home and never goes negative", "[recoil]") {
    // A quarter of the travel per step: four steps home from a full kick.
    CHECK(rm::stepRecoil(1.0f, 0.25f) == Approx(0.75f));
    CHECK(rm::stepRecoil(0.75f, 0.25f) == Approx(0.5f));
    CHECK(rm::stepRecoil(0.1f, 0.25f) == 0.0f);
    CHECK(rm::stepRecoil(0.0f, 0.25f) == 0.0f);
}

TEST_CASE("a fired gun draws its barrel back, then runs it home", "[recoil]") {
    // End to end through the match: two enemies in range, the shots kick the
    // slide to full and the ticks run it home. Fully synthetic like the turret
    // scene test — the batch carries a resolved rack, the sim fires the gun.
    rm::HeightField field;
    field.squaresX = 128;
    field.squaresZ = 128;
    field.baseHeight = 0.0f;
    field.heightScale = 1.0f;
    field.raw.assign(field.sampleCount(), std::uint16_t{0});
    rm::app::UnitScene scene;
    scene.armies = rm::sim::freeForAll(2);
    scene.players = rm::sim::onePlayerPerArmy(2, 0);
    scene.economies.assign(2, rm::sim::Economy{});

    rm::unitdef::UnitDef tank;
    tank.name = "test_recoil_tank";
    tank.motion = rm::unitdef::MotionType::Land;
    tank.speedElmosPerSecond = 10.0f;
    tank.health = rm::sim::Mag::fromInt(100);
    tank.categories = {"LAND"};
    rm::unitdef::Weapon gun;
    gun.label = "test recoil gun";
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
    gun.recoilBone = "Rack";
    gun.recoilDistanceMesh = 2.0f;
    gun.recoilReturnSpeedMeshPerSecond = 2.0f;
    tank.weapons.push_back(gun);
    scene.definitions.push_back(tank);
    const rm::UnitTypeIndex type =
        scene.catalog.add(&scene.definitions.back(), rm::app::gAppTickRate);
    scene.setTypeTraits(type, rm::data::moveDefFor(tank), 1.0f);

    scene.models.push_back(gunModel());
    scene.batches.push_back(rm::UnitBatch{
        .model = &scene.models.back(),
        .turretAim = rm::resolveTurretAim(scene.models.back(),
                                          rm::TurretAimSpec{
                                              .yawBone = {.name = "Turret"},
                                              .pitchBone = {.name = "Barrel"},
                                              .muzzleBone = {.name = "Muzzle"},
                                              .yawSlew = 2.0f,
                                              .pitchSlew = 1.5f,
                                          }),
        .turretWeapon = 0,
        .recoilFlags = rm::resolveRecoilFlags(scene.models.back(), "Rack"),
        .recoilDistanceElmos = 2.0f,
        .recoilReturnPerTick = 0.1f,
    });
    REQUIRE(!scene.batches.back().recoilFlags.empty());
    scene.setBatchForType(type, 0);

    (void)scene.store.spawn(rm::sim::UnitStore::Spawn{
        .type = type,
        .transform = {.x = rm::sim::fxFromFloat(0.0f), .z = rm::sim::fxFromFloat(0.0f)},
        .motion = rm::app::motionFor(tank, 0),
        .health = rm::sim::initialHealth(rm::sim::Mag::fromInt(100)),
    });
    (void)scene.store.spawn(rm::sim::UnitStore::Spawn{
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
    for (int tick = 0; tick < 8; ++tick) {
        (void)rm::app::advanceMatch(runner, tick, 0.0f);
    }
    REQUIRE(runner.shotsFired > 0);

    scene.publish(7);
    scene.publish(7);
    scene.gatherForDrawing(1.0f, nullptr, {}, 0.0f);
    REQUIRE(scene.batches[0].instances.size() == 2);
    // Kicked but not yet home: a shot left within the last second and the
    // return takes a full one.
    bool kicked = false;
    for (const rm::UnitInstance& instance : scene.batches[0].instances) {
        if (instance.recoil > 0.0f) {
            kicked = true;
        }
    }
    CHECK(kicked);
}
