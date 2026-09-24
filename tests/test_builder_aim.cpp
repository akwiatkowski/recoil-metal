#include "core/model/BuilderAim.hpp"

#include "core/sim/Manipulator.hpp"
#include "core/unit/UnitBlueprint.hpp"
#include "core/unit/UnitDef.hpp"

#include "support/TestRoster.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <numbers>
#include <string>
#include <vector>

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

TEST_CASE("builder_aim: a Seraphim blueprint with BuildBones_2 produces two BuilderArmSpec records "
          "at load",
          "[builder-aim][C-249]") {
    // The Seraphim half of `C-249`. `General.BuildBones_2` mirrors the primary
    // `General.BuildBones` table and gets its own `BuilderArmSpec`; the loader is
    // additive (`UnitBlueprint.cpp`) — a blueprint that names only the primary
    // keeps an empty second `BuilderArmSpec` whose `.exists()` is `false`, so the
    // lifecycle that adds the manipulator record treats the slot as absent.
    const auto def = rm::unitbp::load(R"(
        UnitBlueprint {
            Defense = { MaxHealth = 100 },
            Display = { UniformScale = 1.0 },
            Physics = { MotionType = 'RULEUMT_Amphibious', MaxSpeed = 1.0, TurnRate = 90 },
            SizeX = 1,
            SizeZ = 1,
            General = {
                BuildBones = {
                    YawBone = "Yaw1",
                    PitchBone = "Pitch1",
                    AimBone = "Tool1",
                },
                BuildBones_2 = {
                    YawBone = "Yaw2",
                    PitchBone = "Pitch2",
                    AimBone = "Tool2",
                    YawMin = -45,
                    YawMax = 45,
                },
            },
        }
    )", "seraphim_unit.bp");
    REQUIRE(def.has_value());
    // Both specs present: the loader wrote two parallel records.
    REQUIRE(def->builderArm.exists());
    REQUIRE(def->builderArm2.exists());
    // Each rig points at its own bone triple — the second rig is not a typo of
    // the first.
    CHECK(def->builderArm.yawBone.name == "Yaw1");
    CHECK(def->builderArm.pitchBone.name == "Pitch1");
    CHECK(def->builderArm.aimBone.name == "Tool1");
    CHECK(def->builderArm2.yawBone.name == "Yaw2");
    CHECK(def->builderArm2.pitchBone.name == "Pitch2");
    CHECK(def->builderArm2.aimBone.name == "Tool2");
    // Authored arcs: the second rig's narrower yaw window made it through the
    // loader alongside the retail defaults for the rest.
    CHECK(def->builderArm.yawMinDegrees == Approx(-180.0f));
    CHECK(def->builderArm2.yawMinDegrees == Approx(-45.0f));
    CHECK(def->builderArm2.yawMaxDegrees == Approx(45.0f));
    // The primary-only sibling: the second rig is absent, second `Spec` defaults
    // to empty and `.exists()` says `false`. This is the contract every non-
    // Seraphim blueprint carries into the lifecycle.
    const auto onlyPrimary = rm::unitbp::load(R"(
        UnitBlueprint {
            Defense = { MaxHealth = 100 },
            Display = { UniformScale = 1.0 },
            Physics = { MotionType = 'RULEUMT_Amphibious', MaxSpeed = 1.0, TurnRate = 90 },
            SizeX = 1,
            SizeZ = 1,
            General = {
                BuildBones = {
                    YawBone = "Yaw",
                    PitchBone = "Pitch",
                    AimBone = "Tool",
                },
            },
        }
    )", "single_builder.bp");
    REQUIRE(onlyPrimary.has_value());
    CHECK(onlyPrimary->builderArm.exists());
    CHECK_FALSE(onlyPrimary->builderArm2.exists());
}

TEST_CASE("builder_aim C-249: a unit with primary and second builder rigs owns two BuilderArm "
          "records, each tracking its own rig",
          "[builder-aim][C-249]") {
    // The Seraphim twin-rig seam. A unit whose `UnitDef` names two
    // `BuilderArmSpec`s gets two `Manipulator` records on its list — one tagged
    // with `rigIndex == 0`, one with `rigIndex == 1` — and `syncBuilderArms`
    // toggles each one's `enabled` byte on the same construction-named builder
    // proxy that the primary has always used.
    rm::test::Roster roster;
    rm::unitdef::UnitDef seraphim;
    seraphim.name = "seraphim_twin";
    // Primary rig: Torso/Arm/Tool — the same triple the model-level test uses.
    seraphim.builderArm.yawBone = {.name = "Torso"};
    seraphim.builderArm.pitchBone = {.name = "Arm"};
    seraphim.builderArm.aimBone = {.name = "Tool"};
    // Second rig: the Seraphim ACU's mirrored arm — distinct bone names so a
    // collapse cannot read as the primary.
    seraphim.builderArm2.yawBone = {.name = "TorsoBack"};
    seraphim.builderArm2.pitchBone = {.name = "ArmBack"};
    seraphim.builderArm2.aimBone = {.name = "ToolBack"};
    const auto twinType = roster.addType(seraphim);
    const auto twin = roster.add(twinType, 40.0f, 40.0f, 0, 100.0f);

    // Sibling carrying only the primary rig (the same shape the existing C-249
    // test asserts against): it owns exactly one record and never gets a second.
    rm::unitdef::UnitDef oneArm = seraphim;
    oneArm.builderArm2 = {};
    const auto loneType = roster.addType(oneArm);
    const auto lone = roster.add(loneType, 80.0f, 80.0f, 0, 100.0f);

    std::vector<rm::sim::Construction> building;

    rm::sim::syncBuilderArms(roster.store, roster.catalog, building);

    auto armsOf = [&](rm::sim::UnitId who) {
        return roster.store.manipulators()[who.index];
    };
    auto armAt = [](const std::vector<rm::sim::Manipulator>& list,
                    std::int32_t rigIndex) -> const rm::sim::Manipulator* {
        const auto found = std::ranges::find_if(
            list, [rigIndex](const rm::sim::Manipulator& m) {
                return m.kind == rm::sim::ManipulatorKind::BuilderArm
                       && m.rigIndex == rigIndex;
            });
        return found != list.end() ? &*found : nullptr;
    };

    // Twin rig: two records, identical precedence (5, retail's authored value),
    // tagged with their rig index.
    const auto& twinArms = armsOf(twin);
    REQUIRE(twinArms.size() == 2);
    REQUIRE(armAt(twinArms, 0) != nullptr);
    REQUIRE(armAt(twinArms, 1) != nullptr);
    CHECK(armAt(twinArms, 0)->precedence == 5);
    CHECK(armAt(twinArms, 1)->precedence == 5);
    // Both records start parked (idle builder → "the bay is not open").
    CHECK_FALSE(armAt(twinArms, 0)->enabled);
    CHECK_FALSE(armAt(twinArms, 1)->enabled);

    // Lone-rig sibling: only the primary ever resolves — the second rig's empty
    // `BuilderArmSpec` is `.exists() == false`, so the lifecycle stays at one
    // record. This is the same gate the primary rig has always read.
    const auto& loneArms = armsOf(lone);
    REQUIRE(loneArms.size() == 1);
    CHECK(armAt(loneArms, 0) != nullptr);
    CHECK(armAt(loneArms, 1) == nullptr);

    // Live construction naming the unit opens the bay: both rigs enable together.
    // Retail builds the gate off a single per-unit construction row, so the
    // records share the same `enabled` byte delta on a single construction event.
    building.push_back(rm::sim::Construction{
        .position = {rm::sim::fxFromFloat(60.0f), rm::sim::Fx{}, rm::sim::fxFromFloat(40.0f)},
        .buildTimeRemaining = rm::sim::magFromFloat(50.0f),
        .totalBuildTime = rm::sim::magFromFloat(100.0f),
        .builder = twin,
    });
    rm::sim::syncBuilderArms(roster.store, roster.catalog, building);

    const auto& twinArmsWorking = armsOf(twin);
    REQUIRE(armAt(twinArmsWorking, 0) != nullptr);
    REQUIRE(armAt(twinArmsWorking, 1) != nullptr);
    CHECK(armAt(twinArmsWorking, 0)->enabled);
    CHECK(armAt(twinArmsWorking, 1)->enabled);

    // Work done, the bay closes: both rigs park.
    building.clear();
    rm::sim::syncBuilderArms(roster.store, roster.catalog, building);

    const auto& twinArmsIdle = armsOf(twin);
    CHECK_FALSE(armAt(twinArmsIdle, 0)->enabled);
    CHECK_FALSE(armAt(twinArmsIdle, 1)->enabled);
}
