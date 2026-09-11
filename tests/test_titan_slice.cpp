// One unit rendered correctly: the UEL0303 Titan as the vertical slice for
// turret aim, recoil and walk. Synthetic models prove the machinery; only the
// real blueprint, mesh and clips prove the content resolves onto it — a bone
// renamed in either file silently deletes the feature, which is exactly the
// failure this file exists to catch.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "app/SceneBuild.hpp"
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
