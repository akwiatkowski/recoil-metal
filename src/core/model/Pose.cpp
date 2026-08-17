#include "core/model/Pose.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace {

using rm::BoneTransform;

/// Conjugate — the inverse of a unit quaternion.
[[nodiscard]] std::array<float, 4> conjugate(const std::array<float, 4>& q) noexcept {
    return {{q[0], -q[1], -q[2], -q[3]}};
}

[[nodiscard]] std::array<float, 4> normalise(std::array<float, 4> q) noexcept {
    const float length =
        std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (length <= 0.0f) {
        return {{1.0f, 0.0f, 0.0f, 0.0f}};
    }
    for (float& component : q) {
        component /= length;
    }
    return q;
}

/// Normalised lerp between two rotations, taking the short way round.
///
/// A quaternion and its negation are the same rotation, so without the
/// hemisphere check an interpolation can travel the long way and spin a bone
/// most of a turn between two adjacent keys.
[[nodiscard]] std::array<float, 4> blend(const std::array<float, 4>& a,
                                         const std::array<float, 4>& b, float t) noexcept {
    const float dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
    const float sign = dot < 0.0f ? -1.0f : 1.0f;

    return normalise({{a[0] + (sign * b[0] - a[0]) * t, a[1] + (sign * b[1] - a[1]) * t,
                       a[2] + (sign * b[2] - a[2]) * t, a[3] + (sign * b[3] - a[3]) * t}});
}

[[nodiscard]] std::array<float, 3> blend(const std::array<float, 3>& a,
                                         const std::array<float, 3>& b, float t) noexcept {
    return {{a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t}};
}

/// Composes two rigid transforms: `outer` applied after `inner`.
[[nodiscard]] BoneTransform compose(const BoneTransform& outer,
                                    const BoneTransform& inner) noexcept {
    const std::array<float, 3> rotated =
        rm::rotateByQuaternion(outer.rotation, inner.translation);

    BoneTransform out;
    out.rotation = rm::multiplyQuaternions(outer.rotation, inner.rotation);
    out.translation = {{outer.translation[0] + rotated[0], outer.translation[1] + rotated[1],
                        outer.translation[2] + rotated[2]}};
    return out;
}

/// The inverse of a rigid transform.
[[nodiscard]] BoneTransform invert(const BoneTransform& transform) noexcept {
    BoneTransform out;
    out.rotation = conjugate(transform.rotation);
    const std::array<float, 3> back = rm::rotateByQuaternion(out.rotation, transform.translation);
    out.translation = {{-back[0], -back[1], -back[2]}};
    return out;
}

} // namespace

namespace rm {

std::vector<BoneTransform> restPose(const Model& model) {
    std::vector<BoneTransform> pose;
    pose.reserve(model.bones.size());

    for (const ModelBone& bone : model.bones) {
        BoneTransform transform;
        if (model.family == Family::Recoil) {
            transform.translation = bone.globalOffset;
        }
        // SupremeCommander leaves it identity: its vertices are already posed.
        pose.push_back(transform);
    }

    return pose;
}

std::vector<int> mapBonesToAnimation(const Model& model, const sca::Animation& animation) {
    std::unordered_map<std::string, int> byName;
    byName.reserve(animation.boneNames.size());
    for (std::size_t i = 0; i < animation.boneNames.size(); ++i) {
        byName.emplace(animation.boneNames[i], static_cast<int>(i));
    }

    std::vector<int> map;
    map.reserve(model.bones.size());
    for (const ModelBone& bone : model.bones) {
        const auto found = byName.find(bone.name);
        map.push_back(found == byName.end() ? -1 : found->second);
    }

    return map;
}

std::vector<BoneTransform> poseAt(const Model& model, const sca::Animation& animation,
                                  std::span<const int> boneMap, float seconds) {
    if (animation.empty() || boneMap.size() != model.bones.size()) {
        return restPose(model);
    }

    // --- Which two keyframes surround this instant -------------------------
    float time = seconds;
    if (animation.duration > 0.0f) {
        time = std::fmod(seconds, animation.duration);
        if (time < 0.0f) {
            time += animation.duration;
        }
    }

    std::size_t next = animation.frames.size() - 1;
    for (std::size_t i = 0; i < animation.frames.size(); ++i) {
        if (animation.frames[i].time >= time) {
            next = i;
            break;
        }
    }
    const std::size_t previous = next == 0 ? 0 : next - 1;

    const float span = animation.frames[next].time - animation.frames[previous].time;
    const float t = span > 0.0f ? (time - animation.frames[previous].time) / span : 0.0f;

    // --- Local transforms, then accumulated down the MODEL's hierarchy -----
    // The model's hierarchy, not the animation's: the model is what the
    // vertices reference, and the animation supplies only the local motion.
    std::vector<BoneTransform> global;
    global.reserve(model.bones.size());

    std::vector<BoneTransform> pose;
    pose.reserve(model.bones.size());

    for (std::size_t i = 0; i < model.bones.size(); ++i) {
        const ModelBone& bone = model.bones[i];

        BoneTransform local;
        local.rotation = bone.rotation;
        local.translation = bone.offset;

        const int animationBone = boneMap[i];
        if (animationBone >= 0) {
            const auto index = static_cast<std::size_t>(animationBone);
            const sca::Key& from = animation.frames[previous].bones[index];
            const sca::Key& to = animation.frames[next].bones[index];
            local.rotation = blend(from.rotation, to.rotation, t);
            local.translation = blend(from.position, to.position, t);
        }

        // Parents always precede their children — both loaders check it — so one
        // pass suffices and global[parent] is already final here.
        if (bone.parent < 0) {
            global.push_back(local);
        } else {
            global.push_back(compose(global[static_cast<std::size_t>(bone.parent)], local));
        }

        if (model.family == Family::Recoil) {
            // Recoil vertices are bone-local, so the animated global transform
            // IS what to apply — there is no bind pose to undo.
            pose.push_back(global.back());
        } else {
            // Supreme Commander vertices are in model space, posed at rest. To
            // move them, first undo the rest pose, then apply the animated one:
            //   v' = animatedGlobal * restGlobal⁻¹ * v
            BoneTransform restGlobal;
            restGlobal.rotation = bone.globalRotation;
            restGlobal.translation = bone.globalOffset;
            pose.push_back(compose(global.back(), invert(restGlobal)));
        }
    }

    // A bone the animation does not drive needs no special case: its local
    // transform is the model's own, so the accumulation reproduces its rest
    // transform exactly — while still following any ancestor that IS driven,
    // which is what a partially matched animation should do.
    return pose;
}

} // namespace rm
