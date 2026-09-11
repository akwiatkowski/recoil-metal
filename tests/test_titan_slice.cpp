// One unit rendered correctly: the UEL0303 Titan as the vertical slice for
// turret aim, recoil and walk. Synthetic models prove the machinery; only the
// real blueprint, mesh and clips prove the content resolves onto it — a bone
// renamed in either file silently deletes the feature, which is exactly the
// failure this file exists to catch.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "app/SceneBuild.hpp"
#include "app/Match.hpp"
#include "core/data/MoveDef.hpp"
#include "core/map/HeightField.hpp"
#include "core/model/BuilderAim.hpp"
#include "core/model/Pose.hpp"
#include "core/model/Sca.hpp"
#include "core/model/Scm.hpp"
#include "core/unit/UnitBlueprint.hpp"

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

using Catch::Approx;

namespace {

[[nodiscard]] std::filesystem::path titanDir() {
    const char* home = std::getenv("HOME");
    return home ? std::filesystem::path{home} / "projects/llm/input/faf/units/UEL0303"
                : std::filesystem::path{};
}

} // namespace

TEST_CASE("the Titan's turret resolves from blueprint onto mesh", "[slice][turret]") {
    const auto dir = titanDir();
    if (!std::filesystem::is_directory(dir)) SKIP("retail corpus unavailable");
    const auto def = rm::unitbp::loadFile(dir / "UEL0303_unit.bp");
    REQUIRE(def);
    const auto model = rm::scm::loadFile(dir / "UEL0303_lod0.scm");
    REQUIRE(model.has_value());

    // The first turreted weapon is the twin cannon both barrels serve.
    const auto spec = rm::app::turretSpecFor(*def);
    REQUIRE(spec.has_value());
    const rm::unitdef::Weapon& gun = def->weapons[spec->second];
    CHECK(gun.turreted);
    CHECK_FALSE(gun.turretYawBone.empty());
    CHECK_FALSE(gun.turretPitchBone.empty());
    CHECK_FALSE(gun.muzzleBone.empty());

    // Every named bone exists in the mesh: a rename on either side would leave
    // the rig empty and the turret frozen, with no error anywhere.
    const rm::app::TurretRig rig = rm::app::resolveTurretRig(*model, &*def);
    CHECK(rig.rig.exists());
    CHECK(rig.weapon == spec->second);
}

TEST_CASE("the Titan's recoil resolves with authored travel", "[slice][recoil]") {
    const auto dir = titanDir();
    if (!std::filesystem::is_directory(dir)) SKIP("retail corpus unavailable");
    const auto def = rm::unitbp::loadFile(dir / "UEL0303_unit.bp");
    REQUIRE(def);
    const auto spec = rm::app::turretSpecFor(*def);
    REQUIRE(spec.has_value());
    const rm::unitdef::Weapon& gun = def->weapons[spec->second];
    CHECK(gun.recoilBone == "Barrel_R");
    CHECK(gun.recoilDistanceMesh == Catch::Approx(-0.2f).margin(1e-4));
    const auto model = rm::scm::loadFile(dir / "UEL0303_lod0.scm");
    REQUIRE(model.has_value());
    std::vector<std::string> boneNames;
    for (const auto& bone : model->bones) boneNames.push_back(bone.name);
    INFO("bones: " << boneNames.size());
    CHECK(std::ranges::find(boneNames, std::string{"Barrel_R"}) != boneNames.end());
    const rm::app::TurretRig rig = rm::app::resolveTurretRig(*model, &*def);
    REQUIRE_FALSE(rig.recoilFlags.empty());
    REQUIRE(rig.recoilFlags.size() == model->bones.size());
    // RackRecoilDistance -0.2 mesh units, converted once at resolve.
    CHECK(rig.recoilDistanceElmos
          == Approx(0.2f * def->meshToElmos).margin(1e-4));
    CHECK(rig.recoilReturnPerTick > 0.0f);
}

TEST_CASE("the Titan's walk clip drives its legs", "[slice][walk]") {
    const auto dir = titanDir();
    if (!std::filesystem::is_directory(dir)) SKIP("retail corpus unavailable");
    const auto model = rm::scm::loadFile(dir / "UEL0303_lod0.scm");
    REQUIRE(model.has_value());
    const auto walk = rm::sca::loadFile(dir / "UEL0303_Awalk.sca");
    REQUIRE(walk.has_value());
    CHECK(walk->duration > 0.0f);

    const std::vector<int> mapping = rm::mapBonesToAnimation(*model, *walk);
    REQUIRE(mapping.size() == model->bones.size());
    std::size_t driven = 0;
    for (const int bone : mapping) driven += (bone >= 0) ? 1 : 0;
    CHECK(driven > 0);

    // The cycle moves something: two poses a quarter-cycle apart differ.
    const auto a = rm::poseAt(*model, *walk, mapping, 0.0f);
    const auto b = rm::poseAt(*model, *walk, mapping, walk->duration * 0.25f);
    REQUIRE(a.size() == b.size());
    float travel = 0.0f;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const float dx = a[i].translation[0] - b[i].translation[0];
        const float dy = a[i].translation[1] - b[i].translation[1];
        const float dz = a[i].translation[2] - b[i].translation[2];
        travel = std::max(travel, std::sqrt(dx * dx + dy * dy + dz * dz));
    }
    CHECK(travel > 0.0f);
}

TEST_CASE("walk discovery finds the Titan's cycle by mesh convention", "[slice][walk]") {
    const char* home = std::getenv("HOME");
    const std::filesystem::path root =
        home ? std::filesystem::path{home} / "projects/llm/input/faf" : std::filesystem::path{};
    if (!std::filesystem::is_directory(root / "units/UEL0303")) SKIP("retail corpus unavailable");
    rm::vfs::Vfs content;
    content.mountDirectory(root);
    rm::app::UnitScene scene;
    scene.armies = rm::sim::freeForAll(2);
    const rm::sca::Animation* walk = rm::app::loadWalkAnimation(
        scene, content, "/units/UEL0303/UEL0303_lod0.scm");
    REQUIRE(walk != nullptr);
    CHECK(walk->duration > 0.0f);
    // A factory's directory holds an upgrade clip but no walk: statics keep
    // the rest pose, and discovery must say so rather than animating one.
    CHECK(rm::app::loadWalkAnimation(scene, content, "/units/UEB0101/UEB0101_LOD0.scm")
          == nullptr);
}

TEST_CASE("the Titan aims, kicks and strides in a live tick", "[slice][behavior]") {
    const auto dir = titanDir();
    if (!std::filesystem::is_directory(dir)) SKIP("retail corpus unavailable");
    const auto def = rm::unitbp::loadFile(dir / "UEL0303_unit.bp");
    REQUIRE(def);
    const auto model = rm::scm::loadFile(dir / "UEL0303_lod0.scm");
    REQUIRE(model.has_value());
    const auto walk = rm::sca::loadFile(dir / "UEL0303_Awalk.sca");
    REQUIRE(walk.has_value());

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
    scene.economies[0].stored = {.mass = rm::sim::Mag::fromInt(100000),
                                 .energy = rm::sim::Mag::fromInt(100000)};
    scene.definitions.push_back(*def);
    const rm::UnitTypeIndex type =
        scene.catalog.add(&scene.definitions.back(), rm::app::gAppTickRate);
    scene.setTypeTraits(type, rm::data::moveDefFor(*def), def->meshToElmos);
    scene.models.push_back(*model);
    scene.animations.push_back(*walk);
    const rm::app::TurretRig rig = rm::app::resolveTurretRig(scene.models.back(), &*def);
    REQUIRE(rig.rig.exists());
    scene.batches.push_back(rm::UnitBatch{
        .model = &scene.models.back(),
        .animation = &scene.animations.back(),
        .turretAim = rig.rig,
        .turretWeapon = rig.weapon,
        .recoilFlags = rig.recoilFlags,
        .recoilDistanceElmos = rig.recoilDistanceElmos,
        .recoilReturnPerTick = rig.recoilReturnPerTick,
        .animationDrivenByInstance = true,
    });
    scene.setBatchForType(type, 0);

    // Inside the cannon's 20-elmo reach, so the sim fires from the first beats.
    const rm::sim::UnitId shooter = scene.store.spawn(rm::sim::UnitStore::Spawn{
        .type = type,
        .transform = {.x = rm::sim::fxFromFloat(200.0f), .z = rm::sim::fxFromFloat(200.0f)},
        .motion = rm::app::motionFor(*def, 0),
        .health = rm::sim::initialHealth(def->health),
    });
    const rm::sim::UnitId tgt = scene.store.spawn(rm::sim::UnitStore::Spawn{
        .type = type,
        .transform = {.x = rm::sim::fxFromFloat(300.0f), .z = rm::sim::fxFromFloat(200.0f)},
        .motion = rm::app::motionFor(*def, 1),
        .health = rm::sim::initialHealth(def->health),
    });
    (void)tgt;
    rm::app::PassabilitySet passability{field, false, 0.0f};
    rm::vfs::Vfs content;
    rm::app::MatchRunner runner =
        rm::app::makeMatchRunner(scene, field, passability, content, {}, {});
    runner.scripts.clear();
    // Sample the slide every tick: it springs home between shots, so a single
    // end-of-run read races the decay. Any nonzero sample proves the kick.
    bool kicked = false;
    for (int tick = 0; tick < 30; ++tick) {
        (void)rm::app::advanceMatch(runner, tick, 0.0f);
        const auto shown = scene.recoilShown.find(shooter.index);
        kicked = kicked || (shown != scene.recoilShown.end() && shown->second > 0.0f);
    }
    // The gun acquired its target: recoil is presentation of a sim fact.
    CHECK(scene.store.health()[shooter.index].automaticTargets.size() == 1);
    INFO("target hp: " << rm::sim::magToFloat(scene.store.health()[tgt.index].current));
    CHECK(scene.store.health()[tgt.index].current < rm::sim::Mag::fromInt(1200));
    // The shooter's own yaw, by instance: drawSlotOf maps instances to slots.
    // The shooter's own aim state, by instance: drawSlotOf maps instances to slots.
    // Values hoisted out — INFO inside the lambda dies with its scope.
    float gotYaw = 0.0f;
    float gotPitch = 0.0f;
    float gotX = 0.0f;
    float gotZ = 0.0f;
    float gotRotY = 0.0f;
    const auto shooterAim = [&]() {
        scene.publish(29);
        scene.publish(29);
        scene.gatherForDrawing(1.0f, nullptr, {}, 0.0f);
        REQUIRE(scene.batches[0].instances.size() == 2);
        for (std::size_t i = 0; i < scene.batches[0].instances.size(); ++i) {
            if (scene.unitDrawnAt(0, i) == shooter) {
                const auto& in = scene.batches[0].instances[i];
                gotYaw = in.builderYaw;
                gotPitch = in.builderPitch;
                gotX = in.position[0];
                gotZ = in.position[2];
                gotRotY = in.rotationY;
                return;
            }
        }
        FAIL("shooter has no drawn instance");
    };
    // Facing +Z with the target 100 elmos up +X: the turret shows ~+90 degrees,
    // minus the barrel's few degrees of rest skew. At this range the pivots'
    // model-space offsets are sub-degree geometry, so mirrored bearings give
    // mirrored yaws — the convention-free proof of direction.
    shooterAim();
    const float yawEast = gotYaw;
    CHECK(std::abs(yawEast) == Catch::Approx(1.5708f).margin(0.2f));
    // The same target stepped across to -X.
    scene.store.transforms()[tgt.index].x = rm::sim::fxFromFloat(100.0f);
    for (int tick = 30; tick < 35; ++tick) {
        (void)rm::app::advanceMatch(runner, tick, 0.0f);
    }
    shooterAim();
    const float yawWest = gotYaw;
    INFO("aim state yaw " << gotYaw << " pitch " << gotPitch << " at (" << gotX << ","
                          << gotZ << ") rotY " << gotRotY);
    CHECK((yawEast - yawWest) == Catch::Approx(3.14159f).margin(0.05f));

    // March orders: ground covered becomes walk phase.
    REQUIRE(rm::app::issueMove(scene, shooter, 0, 35, rm::sim::fxFromFloat(400.0f),
                               rm::sim::fxFromFloat(200.0f)));
    for (int tick = 35; tick < 65; ++tick) {
        (void)rm::app::advanceMatch(runner, tick, 0.0f);
    }
    scene.publish(64);
    scene.publish(64);
    scene.gatherForDrawing(1.0f, nullptr, {}, 0.0f);
    bool striding = false;
    for (const rm::UnitInstance& instance : scene.batches[0].instances) {
        striding = striding || std::abs(instance.animationPhase) > 1e-6f;
    }
    CHECK(striding);
}
