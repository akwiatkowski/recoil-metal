// Pose evaluation tests. Pure maths over decoded data, so all of it runs
// without a GPU — which matters more here than anywhere else in the project: a
// wrong pose renders as a model that is subtly, plausibly deformed.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "core/model/Pose.hpp"

#include <cmath>
#include <numbers>
#include <vector>

using Catch::Approx;

namespace {

/// A quaternion for a rotation about +Y, which is the axis every test here uses
/// because its effect on a point is easy to state by hand.
[[nodiscard]] std::array<float, 4> aboutY(float radians) {
    return {{std::cos(radians / 2.0f), 0.0f, std::sin(radians / 2.0f), 0.0f}};
}

/// A two-bone model: a root, and a child offset along +X.
[[nodiscard]] rm::Model twoBoneModel(rm::Family family) {
    rm::Model model;
    model.family = family;

    rm::ModelBone root;
    root.name = "root";
    root.parent = -1;

    rm::ModelBone child;
    child.name = "child";
    child.parent = 0;
    child.offset = {{10.0f, 0.0f, 0.0f}};
    child.globalOffset = {{10.0f, 0.0f, 0.0f}};

    model.bones = {root, child};
    model.vertices.push_back(rm::ModelVertex{
        .position = {{1.0f, 0.0f, 0.0f}}, .normal = {{0, 1, 0}}, .uv = {{0, 0}}, .boneIndex = 1});
    model.indices = {0, 0, 0};
    return model;
}

/// An animation that holds every bone at rest for its whole length.
[[nodiscard]] rm::sca::Animation staticAnimation(const rm::Model& model, float duration) {
    rm::sca::Animation animation;
    animation.duration = duration;
    for (const rm::ModelBone& bone : model.bones) {
        animation.boneNames.push_back(bone.name);
        animation.boneParents.push_back(bone.parent);
    }

    for (float time : {0.0f, duration}) {
        rm::sca::Frame frame;
        frame.time = time;
        for (const rm::ModelBone& bone : model.bones) {
            frame.bones.push_back(rm::sca::Key{.position = bone.offset, .rotation = bone.rotation});
        }
        animation.frames.push_back(std::move(frame));
    }
    return animation;
}

} // namespace

TEST_CASE("a Recoil rest pose is each bone's accumulated offset") {
    const rm::Model model = twoBoneModel(rm::Family::Recoil);

    const auto pose = rm::restPose(model);

    REQUIRE(pose.size() == 2);
    REQUIRE(pose[1].translation[0] == Approx(10.0f));
    // Rotation stays identity — .s3o carries none at all.
    REQUIRE(pose[1].rotation[0] == Approx(1.0f));
}

TEST_CASE("a Supreme Commander rest pose is identity") {
    // Its vertices are already in model space, so a bone must contribute
    // nothing. Applying the offset would scatter the model along its hierarchy.
    const rm::Model model = twoBoneModel(rm::Family::SupremeCommander);

    const auto pose = rm::restPose(model);

    REQUIRE(pose.size() == 2);
    for (const rm::BoneTransform& transform : pose) {
        REQUIRE(transform.translation[0] == Approx(0.0f));
        REQUIRE(transform.translation[1] == Approx(0.0f));
        REQUIRE(transform.translation[2] == Approx(0.0f));
        REQUIRE(transform.rotation[0] == Approx(1.0f));
    }
}

TEST_CASE("bones are matched to an animation by name, not by index") {
    rm::Model model = twoBoneModel(rm::Family::SupremeCommander);

    rm::sca::Animation animation;
    animation.duration = 1.0f;
    // Deliberately the reverse order, plus a bone the model does not have.
    animation.boneNames = {"child", "elsewhere", "root"};
    animation.boneParents = {-1, -1, -1};

    const auto map = rm::mapBonesToAnimation(model, animation);

    REQUIRE(map.size() == 2);
    REQUIRE(map[0] == 2);  // "root"
    REQUIRE(map[1] == 0);  // "child"

    model.bones[1].name = "absent";
    REQUIRE(rm::mapBonesToAnimation(model, animation)[1] == -1);
}

TEST_CASE("an animation holding the rest pose changes nothing") {
    // The strongest single check on the whole pipeline: keys equal to the
    // model's own rest transforms must reproduce restPose exactly, for either
    // family. If the rest-pose inverse were composed the wrong way round, this
    // is what would catch it.
    for (const rm::Family family : {rm::Family::Recoil, rm::Family::SupremeCommander}) {
        const rm::Model model = twoBoneModel(family);
        const auto animation = staticAnimation(model, 2.0f);
        const auto map = rm::mapBonesToAnimation(model, animation);

        const auto rest = rm::restPose(model);
        const auto posed = rm::poseAt(model, animation, map, 0.7f);

        REQUIRE(posed.size() == rest.size());
        for (std::size_t i = 0; i < posed.size(); ++i) {
            for (std::size_t axis = 0; axis < 3; ++axis) {
                REQUIRE(posed[i].translation[axis] == Approx(rest[i].translation[axis]).margin(1e-5));
            }
            REQUIRE(std::abs(posed[i].rotation[0]) == Approx(std::abs(rest[i].rotation[0])).margin(1e-5));
        }
    }
}

TEST_CASE("a rotating parent carries its child around with it") {
    const rm::Model model = twoBoneModel(rm::Family::Recoil);
    auto animation = staticAnimation(model, 2.0f);

    // Turn the root a quarter turn about +Y by the end. The child sits 10 along
    // +X at rest, so a quarter turn about +Y should put it near -Z or +Z — the
    // point is that it MOVES with the parent rather than staying put.
    animation.frames[1].bones[0].rotation = aboutY(std::numbers::pi_v<float> / 2.0f);

    const auto map = rm::mapBonesToAnimation(model, animation);
    // Just short of the end: the duration itself wraps to the loop's start.
    const auto posed = rm::poseAt(model, animation, map, 1.999f);

    REQUIRE(posed[1].translation[0] == Approx(0.0f).margin(0.01f));
    REQUIRE(std::abs(posed[1].translation[2]) == Approx(10.0f).margin(0.01f));
}

TEST_CASE("time interpolates between keyframes and wraps at the duration") {
    const rm::Model model = twoBoneModel(rm::Family::Recoil);
    auto animation = staticAnimation(model, 2.0f);
    animation.frames[1].bones[1].position = {{20.0f, 0.0f, 0.0f}};

    const auto map = rm::mapBonesToAnimation(model, animation);

    REQUIRE(rm::poseAt(model, animation, map, 0.0f)[1].translation[0] == Approx(10.0f));
    REQUIRE(rm::poseAt(model, animation, map, 1.0f)[1].translation[0] == Approx(15.0f));
    REQUIRE(rm::poseAt(model, animation, map, 1.999f)[1].translation[0]
            == Approx(20.0f).margin(0.01f));

    // Wrapping means a caller can hand this a clock that only ever grows — and
    // that the duration itself is the loop's start, not its end.
    REQUIRE(rm::poseAt(model, animation, map, 2.0f)[1].translation[0] == Approx(10.0f));
    REQUIRE(rm::poseAt(model, animation, map, 3.0f)[1].translation[0] == Approx(15.0f));
    REQUIRE(rm::poseAt(model, animation, map, -1.0f)[1].translation[0] == Approx(15.0f));
}

TEST_CASE("interpolated rotations stay unit length") {
    const rm::Model model = twoBoneModel(rm::Family::Recoil);
    auto animation = staticAnimation(model, 1.0f);
    animation.frames[1].bones[0].rotation = aboutY(std::numbers::pi_v<float> * 0.9f);

    const auto map = rm::mapBonesToAnimation(model, animation);

    // A lerp between two quaternions is not unit length in the middle. Left
    // unnormalised it would scale the model as it turns — a wobble that reads
    // as "the animation is a bit off" rather than as a bug.
    for (float t = 0.0f; t <= 1.0f; t += 0.1f) {
        const auto pose = rm::poseAt(model, animation, map, t);
        const auto& q = pose[0].rotation;
        const float length = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
        REQUIRE(length == Approx(1.0f).margin(1e-5));
    }
}

TEST_CASE("an empty or mismatched animation falls back to the rest pose") {
    const rm::Model model = twoBoneModel(rm::Family::Recoil);
    const rm::sca::Animation empty;

    const auto pose = rm::poseAt(model, empty, std::vector<int>{0, 1}, 0.5f);

    REQUIRE(pose.size() == model.bones.size());
    REQUIRE(pose[1].translation[0] == Approx(10.0f));

    // A bone map of the wrong length is a caller error, not a reason to index
    // out of bounds.
    const auto wrongMap = rm::poseAt(model, staticAnimation(model, 1.0f), std::vector<int>{0}, 0.5f);
    REQUIRE(wrongMap.size() == model.bones.size());
}
