// Turret aiming: same two-axis machinery as builder arms, resolved from a weapon's
// turret bones instead of a BuilderArmSpec.
//
// A tank turret is a yaw ring with a pitching barrel on top, and the test model is
// exactly that: Hull → Turret → Barrel → Muzzle. The rig must mark the yaw/pitch
// subtrees, pivot on the ring and trunnion, and aim the muzzle at a target.
#include "core/model/BuilderAim.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <numbers>

using Catch::Approx;

namespace {

[[nodiscard]] rm::Model tankModel() {
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

[[nodiscard]] rm::TurretAimSpec turretSpec() {
    return rm::TurretAimSpec{
        .yawBone = {.name = "Turret"},
        .pitchBone = {.name = "Barrel"},
        .muzzleBone = {.name = "Muzzle"},
        .yawMin = -1.3f,
        .yawMax = 1.3f,
        .yawSlew = 2.0f,
        .pitchMin = -0.2f,
        .pitchMax = 0.5f,
        .pitchSlew = 1.5f,
    };
}

} // namespace

TEST_CASE("a turret rig marks only the ring and barrel subtrees", "[turret]") {
    const rm::BuilderAimRig rig = rm::resolveTurretAim(tankModel(), turretSpec());
    REQUIRE(rig.exists());
    REQUIRE(rig.boneFlags.size() == 4);
    CHECK(rig.boneFlags[0] == 0U);
    CHECK(rig.boneFlags[1] == rm::kBuilderYawBone);
    CHECK(rig.boneFlags[2] == (rm::kBuilderYawBone | rm::kBuilderPitchBone));
    CHECK(rig.boneFlags[3] == (rm::kBuilderYawBone | rm::kBuilderPitchBone));
    CHECK(rig.yawSlew == Approx(2.0f));
    CHECK(rig.pitchSlew == Approx(1.5f));
}

TEST_CASE("a turret aims its muzzle at a target within its arc", "[turret]") {
    const rm::BuilderAimRig rig = rm::resolveTurretAim(tankModel(), turretSpec());
    // Dead ahead and level: no deflection either way.
    const rm::BuilderAimAngles ahead = rm::builderAimAt(rig, {{0.0f, 1.0f, 10.0f}});
    CHECK(ahead.yaw == Approx(0.0f).margin(0.00001f));
    CHECK(ahead.pitch == Approx(0.0f).margin(0.00001f));
    // Hard right is past the ±1.3 rad arc: the yaw clamps, it does not follow.
    const rm::BuilderAimAngles side = rm::builderAimAt(rig, {{10.0f, 1.0f, 0.0f}});
    CHECK(side.yaw == Approx(1.3f));
}

TEST_CASE("a turret slews toward its goal at the weapon's rates", "[turret]") {
    const rm::BuilderAimRig rig = rm::resolveTurretAim(tankModel(), turretSpec());
    const rm::BuilderAimAngles goal{.yaw = 1.3f, .pitch = 0.5f};
    // A fifth of a second at 2 rad/s yaw and 1.5 rad/s pitch: both move their
    // rate × dt, neither arrives.
    const rm::BuilderAimAngles stepped = rm::stepBuilderAim({}, goal, rig, 0.2f);
    CHECK(stepped.yaw == Approx(0.4f));
    CHECK(stepped.pitch == Approx(0.3f));
}

TEST_CASE("a turret rig without named bones does not exist", "[turret]") {
    rm::TurretAimSpec spec = turretSpec();
    spec.pitchBone = {.name = "NoSuchBone"};
    CHECK_FALSE(rm::resolveTurretAim(tankModel(), spec).exists());
}

TEST_CASE("turret bone names match case-insensitively", "[turret]") {
    // The corpus's bone spelling is not reliable; a turret lost to capitalisation
    // would aim with its hull while reporting a rig that exists.
    rm::Model model = tankModel();
    model.bones[1].name = "turret";
    model.bones[2].name = "BARREL";
    model.bones[3].name = "Muzzle";
    CHECK(rm::resolveTurretAim(model, turretSpec()).exists());
}
