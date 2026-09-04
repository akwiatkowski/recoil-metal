#include "core/model/BuilderAim.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <numbers>

using Catch::Approx;

namespace {

[[nodiscard]] rm::Model builderModel() {
    rm::Model model;
    model.family = rm::Family::SupremeCommander;
    model.bones = {
        rm::ModelBone{.name = "Root", .parent = -1},
        rm::ModelBone{.name = "Torso", .parent = 0, .offset = {{0.0f, 1.0f, 0.0f}},
                      .globalOffset = {{0.0f, 1.0f, 0.0f}}},
        rm::ModelBone{.name = "Arm", .parent = 1, .globalOffset = {{0.0f, 1.0f, 0.0f}}},
        rm::ModelBone{.name = "Tool", .parent = 2, .offset = {{0.0f, 0.0f, 2.0f}},
                      .globalOffset = {{0.0f, 1.0f, 2.0f}}},
    };
    return model;
}

[[nodiscard]] rm::unitdef::BuilderArmSpec builderSpec() {
    return rm::unitdef::BuilderArmSpec{
        .yawBone = {.name = "Torso"},
        .pitchBone = {.name = "Arm"},
        .aimBone = {.name = "Tool"},
    };
}

} // namespace

TEST_CASE("a builder rig marks only the yaw and pitch subtrees") {
    const rm::BuilderAimRig rig = rm::resolveBuilderAim(builderModel(), builderSpec());
    REQUIRE(rig.exists());
    REQUIRE(rig.boneFlags.size() == 4);
    CHECK(rig.boneFlags[0] == 0U);
    CHECK(rig.boneFlags[1] == rm::kBuilderYawBone);
    CHECK(rig.boneFlags[2] == (rm::kBuilderYawBone | rm::kBuilderPitchBone));
    CHECK(rig.boneFlags[3] == (rm::kBuilderYawBone | rm::kBuilderPitchBone));
}

TEST_CASE("each builder instance can aim the same rig at a different target") {
    const rm::BuilderAimRig rig = rm::resolveBuilderAim(builderModel(), builderSpec());

    const rm::BuilderAimAngles right = rm::builderAimAt(rig, {{10.0f, 1.0f, 0.0f}});
    CHECK(right.yaw == Approx(std::numbers::pi_v<float> * 0.5f));
    CHECK(right.pitch == Approx(0.0f).margin(0.00001f));
    const std::array<float, 3> rightTool =
        rm::applyBuilderAim(rig.aimPoint, rig.boneFlags[3], rig, right);
    CHECK(rightTool[0] == Approx(2.0f));
    CHECK(rightTool[1] == Approx(1.0f));
    CHECK(rightTool[2] == Approx(0.0f).margin(0.00001f));

    const rm::BuilderAimAngles high = rm::builderAimAt(rig, {{0.0f, 3.0f, 2.0f}});
    CHECK(high.yaw == Approx(0.0f).margin(0.00001f));
    CHECK(high.pitch == Approx(-std::numbers::pi_v<float> * 0.25f));
    const std::array<float, 3> highTool =
        rm::applyBuilderAim(rig.aimPoint, rig.boneFlags[3], rig, high);
    CHECK(highTool[1] == Approx(1.0f + std::sqrt(2.0f)));
    CHECK(highTool[2] == Approx(std::sqrt(2.0f)));

    // The root never inherits either manipulator.
    CHECK(rm::applyBuilderAim({{3.0f, 4.0f, 5.0f}}, rig.boneFlags[0], rig, right)
          == std::array<float, 3>{{3.0f, 4.0f, 5.0f}});
}

TEST_CASE("builder aiming obeys authored arcs and slew rates") {
    rm::unitdef::BuilderArmSpec spec = builderSpec();
    spec.yawMaxDegrees = 30.0f;
    spec.yawSlewDegreesPerSecond = 10.0f;
    const rm::BuilderAimRig rig = rm::resolveBuilderAim(builderModel(), spec);
    const rm::BuilderAimAngles target = rm::builderAimAt(rig, {{10.0f, 1.0f, 0.0f}});
    CHECK(target.yaw == Approx(std::numbers::pi_v<float> / 6.0f));

    const rm::BuilderAimAngles first = rm::stepBuilderAim({}, target, rig, 1.0f);
    CHECK(first.yaw == Approx(std::numbers::pi_v<float> / 18.0f));
    CHECK(rm::stepBuilderAim(first, target, rig, 0.0f).yaw == Approx(target.yaw));
}

TEST_CASE("a world target is inverted through the unit placement before aiming") {
    const rm::InstancePlacement placement{
        .position = {{10.0f, 20.0f, 30.0f}},
        .rotationY = std::numbers::pi_v<float> * 0.5f,
        .scale = 2.0f,
    };
    const auto target = rm::builderTargetInModel({{14.0f, 22.0f, 30.0f}}, placement);
    CHECK(target[0] == Approx(0.0f).margin(0.00001f));
    CHECK(target[1] == Approx(1.0f));
    CHECK(target[2] == Approx(2.0f));
}
