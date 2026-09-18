#include "core/model/BuilderAim.hpp"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <limits>
#include <numbers>

namespace {

using Vec3 = std::array<float, 3>;

[[nodiscard]] Vec3 subtract(const Vec3& a, const Vec3& b) noexcept {
    return {{a[0] - b[0], a[1] - b[1], a[2] - b[2]}};
}

[[nodiscard]] Vec3 add(const Vec3& a, const Vec3& b) noexcept {
    return {{a[0] + b[0], a[1] + b[1], a[2] + b[2]}};
}

[[nodiscard]] Vec3 scale(const Vec3& v, float amount) noexcept {
    return {{v[0] * amount, v[1] * amount, v[2] * amount}};
}

[[nodiscard]] float dot(const Vec3& a, const Vec3& b) noexcept {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

[[nodiscard]] Vec3 cross(const Vec3& a, const Vec3& b) noexcept {
    return {{a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2],
             a[0] * b[1] - a[1] * b[0]}};
}

[[nodiscard]] Vec3 normalise(const Vec3& v) noexcept {
    const float length = std::sqrt(dot(v, v));
    return length > 0.000001f ? scale(v, 1.0f / length) : Vec3{};
}

[[nodiscard]] Vec3 rotateAxis(const Vec3& v, const Vec3& axis, float angle) noexcept {
    const Vec3 unit = normalise(axis);
    const float c = std::cos(angle);
    const float s = std::sin(angle);
    return add(add(scale(v, c), scale(cross(unit, v), s)),
               scale(unit, dot(unit, v) * (1.0f - c)));
}

[[nodiscard]] Vec3 rotateAround(const Vec3& point, const Vec3& pivot, const Vec3& axis,
                                float angle) noexcept {
    return add(pivot, rotateAxis(subtract(point, pivot), axis, angle));
}

[[nodiscard]] float signedAngle(const Vec3& from, const Vec3& to,
                                const Vec3& axis) noexcept {
    const Vec3 unitAxis = normalise(axis);
    const Vec3 flatFrom = normalise(subtract(from, scale(unitAxis, dot(from, unitAxis))));
    const Vec3 flatTo = normalise(subtract(to, scale(unitAxis, dot(to, unitAxis))));
    if (dot(flatFrom, flatFrom) == 0.0f || dot(flatTo, flatTo) == 0.0f) {
        return 0.0f;
    }
    return std::atan2(dot(unitAxis, cross(flatFrom, flatTo)), dot(flatFrom, flatTo));
}

[[nodiscard]] int resolve(const rm::Model& model, const rm::unitdef::BoneRef& ref) noexcept {
    if (!ref.name.empty()) {
        const auto found = std::ranges::find(model.bones, ref.name, &rm::ModelBone::name);
        return found == model.bones.end()
                   ? -1
                   : static_cast<int>(std::distance(model.bones.begin(), found));
    }
    return ref.index >= 0 && static_cast<std::size_t>(ref.index) < model.bones.size() ? ref.index
                                                                                      : -1;
}

[[nodiscard]] bool descendsFrom(const rm::Model& model, std::size_t bone,
                                int ancestor) noexcept {
    for (int at = static_cast<int>(bone); at >= 0;
         at = model.bones[static_cast<std::size_t>(at)].parent) {
        if (at == ancestor) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] float radians(float degrees) noexcept {
    return degrees * std::numbers::pi_v<float> / 180.0f;
}

[[nodiscard]] float step(float from, float to, float maximum) noexcept {
    const float delta = std::remainder(to - from, 2.0f * std::numbers::pi_v<float>);
    return std::abs(delta) <= maximum ? to : from + std::copysign(maximum, delta);
}

} // namespace

namespace rm {

BuilderAimRig resolveBuilderAim(const Model& model, const unitdef::BuilderArmSpec& spec,
                                std::span<const std::string> effectBones) {
    BuilderAimRig rig;
    if (!spec.exists() || model.bones.empty()) {
        return rig;
    }

    const int yaw = resolve(model, spec.yawBone);
    const int pitch = resolve(model, spec.pitchBone);
    int aim = resolve(model, spec.aimBone);

    // Retail blueprints commonly use numeric zero for "no separate aim marker" while naming
    // the actual tool muzzle in BuildEffectBones. That authored endpoint is the useful forward
    // reference; fall back to the pitch bone's +Z axis if neither form resolves.
    if (aim == 0) {
        for (const std::string& name : effectBones) {
            unitdef::BoneRef effect{.name = name};
            if (const int candidate = resolve(model, effect); candidate >= 0) {
                aim = candidate;
                break;
            }
        }
    }
    if (yaw < 0 || pitch < 0 || aim < 0) {
        return rig;
    }

    rig.boneFlags.resize(model.bones.size());
    for (std::size_t bone = 0; bone < model.bones.size(); ++bone) {
        if (descendsFrom(model, bone, yaw)) {
            rig.boneFlags[bone] |= kBuilderYawBone;
        }
        if (descendsFrom(model, bone, pitch)) {
            rig.boneFlags[bone] |= kBuilderPitchBone;
        }
    }

    const ModelBone& yawBone = model.bones[static_cast<std::size_t>(yaw)];
    const ModelBone& pitchBone = model.bones[static_cast<std::size_t>(pitch)];
    const ModelBone& aimBone = model.bones[static_cast<std::size_t>(aim)];
    rig.yawPivot = yawBone.globalOffset;
    rig.yawAxis = normalise(rotateByQuaternion(yawBone.globalRotation, {{0.0f, 1.0f, 0.0f}}));
    rig.pitchPivot = pitchBone.globalOffset;
    rig.pitchAxis =
        normalise(rotateByQuaternion(pitchBone.globalRotation, {{1.0f, 0.0f, 0.0f}}));
    rig.aimPoint = aimBone.globalOffset;
    if (dot(subtract(rig.aimPoint, rig.pitchPivot),
            subtract(rig.aimPoint, rig.pitchPivot)) < 0.000001f) {
        rig.aimPoint = add(rig.pitchPivot,
                           rotateByQuaternion(aimBone.globalRotation, {{0.0f, 0.0f, 1.0f}}));
    }
    rig.aimDir = subtract(rig.aimPoint, rig.pitchPivot);
    rig.yawMin = radians(spec.yawMinDegrees);
    rig.yawMax = radians(spec.yawMaxDegrees);
    rig.yawSlew = radians(spec.yawSlewDegreesPerSecond);
    rig.pitchMin = radians(spec.pitchMinDegrees);
    rig.pitchMax = radians(spec.pitchMaxDegrees);
    rig.pitchSlew = radians(spec.pitchSlewDegreesPerSecond);
    return rig;
}
BuilderAimRig resolveTurretAim(const Model& model, const TurretAimSpec& spec) {
    // Exact first, then case-insensitive: the corpus's bone spelling is not reliable
    // (see resolveMuzzleBones), and a turret lost to capitalisation would aim with
    // its hull while reporting a rig that exists.
    const auto turretResolve = [&model](const rm::unitdef::BoneRef& ref) {
        const int exact = resolve(model, ref);
        if (exact >= 0 || ref.name.empty()) {
            return exact;
        }
        for (std::size_t bone = 0; bone < model.bones.size(); ++bone) {
            const std::string& have = model.bones[bone].name;
            if (have.size() == ref.name.size()
                && std::equal(have.begin(), have.end(), ref.name.begin(),
                              [](unsigned char a, unsigned char b) {
                                  return std::tolower(a) == std::tolower(b);
                              })) {
                return static_cast<int>(bone);
            }
        }
        return -1;
    };
    BuilderAimRig rig;
    if (!spec.exists() || model.bones.empty()) {
        return rig;
    }
    const int yaw = turretResolve(spec.yawBone);
    const int pitch = turretResolve(spec.pitchBone);
    const int muzzle = turretResolve(spec.muzzleBone);
    // Coarse meshes merge small bones away — the Aurora's lod1 keeps the Turret
    // ring but loses Turret_Barrel. Retail's per-bone script rotations no-op on
    // a missing bone rather than freezing the turret, so resolve each degree of
    // freedom independently: only a mesh with NEITHER is a dead rig. The missing
    // side's arc clamps shut below, so its solve emits exactly rest.
    if (yaw < 0 && pitch < 0) {
        return rig;
    }
    rig.boneFlags.resize(model.bones.size());
    const int pitch2 = turretResolve(spec.pitch2Bone);
    const int muzzle2 = turretResolve(spec.muzzle2Bone);
    for (std::size_t bone = 0; bone < model.bones.size(); ++bone) {
        if (yaw >= 0 && descendsFrom(model, bone, yaw)) {
            rig.boneFlags[bone] |= kTurretYawBone;
        }
        if (pitch >= 0 && descendsFrom(model, bone, pitch)) {
            rig.boneFlags[bone] |= kTurretPitchBone;
        }
        if (pitch2 >= 0 && descendsFrom(model, bone, pitch2)) {
            rig.boneFlags[bone] |= kTurretPitch2Bone | kTurretYaw2Bone;
        }
    }
    if (yaw >= 0) {
        const ModelBone& yawBone = model.bones[static_cast<std::size_t>(yaw)];
        rig.yawPivot = yawBone.globalOffset;
        rig.yawAxis =
            normalise(rotateByQuaternion(yawBone.globalRotation, {{0.0f, 1.0f, 0.0f}}));
    }
    if (pitch >= 0) {
        const ModelBone& pitchBone = model.bones[static_cast<std::size_t>(pitch)];
        rig.pitchPivot = pitchBone.globalOffset;
        rig.pitchAxis =
            normalise(rotateByQuaternion(pitchBone.globalRotation, {{1.0f, 0.0f, 0.0f}}));
    }
    if (muzzle >= 0 && pitch >= 0) {
        rig.hasMuzzle = true;
        const ModelBone& muzzleBone = model.bones[static_cast<std::size_t>(muzzle)];
        // The aim reference is the barrel's DIRECTION, hung on the yaw pivot —
        // not the muzzle position. A centred barrel makes the two identical,
        // which is why the position reference ever worked; a side-mounted one
        // sits elmos off the ring's axis, and aiming its position stops the
        // traverse short (the Titan's torso halts ~28 degrees off its target).
        // The trunnion to muzzle line is the barrel.
        if (dot(subtract(muzzleBone.globalOffset, rig.pitchPivot),
                subtract(muzzleBone.globalOffset, rig.pitchPivot)) < 0.000001f) {
            // A muzzle coincident with its trunnion gives no forward reference —
            // same fallback as the builder's missing aim marker, the muzzle's
            rig.aimPoint = add(rig.pitchPivot,
                               rotateByQuaternion(muzzleBone.globalRotation,
                                                  {{0.0f, 0.0f, 1.0f}}));
            rig.aimDir = subtract(rig.aimPoint, rig.pitchPivot);
        } else {
            rig.aimPoint = add(rig.yawPivot,
                               normalise(subtract(muzzleBone.globalOffset, rig.pitchPivot)));
            rig.aimDir = subtract(muzzleBone.globalOffset, rig.pitchPivot);
        }
    } else {
        // No bore line — the mesh has no distinct muzzle, or no pitch bone to
        // hang one on (the yaw-only coarse case). Aim by the best surviving
        // bone's own rest forward through its pivot: direction without
        // position, which is all traverse needs. hasMuzzle stays false so
        // callers know flashes fall back to the fine offset.
        const int forward = pitch >= 0 ? pitch : (muzzle >= 0 ? muzzle : yaw);
        const ModelBone& forwardBone = model.bones[static_cast<std::size_t>(forward)];
        const Vec3 pivot = pitch >= 0 ? rig.pitchPivot : rig.yawPivot;
        rig.aimPoint =
            add(pivot,
                rotateByQuaternion(forwardBone.globalRotation, {{0.0f, 0.0f, 1.0f}}));
        rig.aimDir = subtract(rig.aimPoint, pivot);
    }
    if (pitch2 >= 0) {
        const ModelBone& pitch2Bone = model.bones[static_cast<std::size_t>(pitch2)];
        rig.pitch2Pivot = pitch2Bone.globalOffset;
        rig.pitch2Axis =
            normalise(rotateByQuaternion(pitch2Bone.globalRotation, {{1.0f, 0.0f, 0.0f}}));
        rig.hasPitch2 = true;
        // One scalar elevates BOTH arms. A mirrored bone's axis turns its own
        // barrel the other way, so flip the axis until the two barrels' rest
        // directions move together under the same rotation. Needs the primary
        // barrel's direction as reference — a pitch-less rig skips the check.
        if (muzzle2 >= 0 && pitch >= 0) {
            const Vec3 dir1 = rig.aimDir;
            const Vec3 dir2 = subtract(
                model.bones[static_cast<std::size_t>(muzzle2)].globalOffset, rig.pitch2Pivot);
            if (dot(cross(rig.pitch2Axis, dir2), cross(rig.pitchAxis, dir1)) < 0.0f) {
                rig.pitch2Axis = scale(rig.pitch2Axis, -1.0f);
            }
        }
    }
    // Already radians: the weapon states speeds that way and the caller converts the arc.
    // A degree of freedom the mesh does not have clamps SHUT — builderAimAt then
    // emits exactly rest for it instead of a solve around a pivot that is not there.
    rig.yawMin = yaw >= 0 ? spec.yawMin : 0.0f;
    rig.yawMax = yaw >= 0 ? spec.yawMax : 0.0f;
    rig.yawSlew = spec.yawSlew;
    rig.pitchMin = pitch >= 0 ? spec.pitchMin : 0.0f;
    rig.pitchMax = pitch >= 0 ? spec.pitchMax : 0.0f;
    rig.pitchSlew = spec.pitchSlew;
    return rig;
}

BuilderAimAngles builderAimAt(const BuilderAimRig& rig, const std::array<float, 3>& target) noexcept {
    if (!rig.exists()) {
        return {};
    }
    BuilderAimAngles answer;
    answer.yaw = std::clamp(signedAngle(subtract(rig.aimPoint, rig.yawPivot),
                                        subtract(target, rig.yawPivot), rig.yawAxis),
                            rig.yawMin, rig.yawMax);

    // Undo the chosen parent yaw before solving the child's pitch. Applying pitch and then yaw
    // in the shader is the exact inverse sequence.
    const Vec3 targetBeforeYaw =
        rotateAround(target, rig.yawPivot, rig.yawAxis, -answer.yaw);
    answer.pitch = std::clamp(signedAngle(rig.aimDir,
                                          subtract(targetBeforeYaw, rig.pitchPivot),
                                          rig.pitchAxis),
                              rig.pitchMin, rig.pitchMax);
    return answer;
}

BuilderAimAngles stepBuilderAim(BuilderAimAngles from, BuilderAimAngles to,
                                const BuilderAimRig& rig, float dtSeconds) noexcept {
    if (dtSeconds <= 0.0f) {
        return to;
    }
    from.yaw = step(from.yaw, to.yaw, std::max(0.0f, rig.yawSlew * dtSeconds));
    from.pitch = step(from.pitch, to.pitch, std::max(0.0f, rig.pitchSlew * dtSeconds));
    return from;
}

std::array<float, 3> applyBuilderAim(const std::array<float, 3>& point,
                                     std::uint32_t boneFlags, const BuilderAimRig& rig,
                                     BuilderAimAngles angles) noexcept {
    Vec3 aimed = point;
    // Pitch before yaw — the hierarchy the shader applies — and the dual arm's
    // trunnion takes its OWN two scalars: pitch2 about its axis, yaw2 about the
    // ring's axis at its pivot, both before the ring's yaw. Both bit families
    // map to the one rig passed in: whichever of builder/turret the bone
    // descends from is the rig the caller resolved.
    if ((boneFlags & kTurretPitch2Bone) != 0U) {
        aimed = rotateAround(aimed, rig.pitch2Pivot, rig.pitch2Axis, angles.pitch2);
    }
    if ((boneFlags & kTurretYaw2Bone) != 0U) {
        aimed = rotateAround(aimed, rig.pitch2Pivot, rig.yawAxis, angles.yaw2);
    }
    if ((boneFlags & (kBuilderPitchBone | kTurretPitchBone)) != 0U) {
        aimed = rotateAround(aimed, rig.pitchPivot, rig.pitchAxis, angles.pitch);
    }
    if ((boneFlags & (kBuilderYawBone | kTurretYawBone)) != 0U) {
        aimed = rotateAround(aimed, rig.yawPivot, rig.yawAxis, angles.yaw);
    }
    return aimed;
}

std::array<float, 3> builderTargetInModel(const std::array<float, 3>& world,
                                          const InstancePlacement& instance) noexcept {
    Vec3 point = subtract(world, instance.position);

    // Reverse unitOrient's yaw, pitch, roll sequence (Pose.cpp / Units.hpp).
    const float sinY = std::sin(-instance.rotationY);
    const float cosY = std::cos(-instance.rotationY);
    point = {{cosY * point[0] + sinY * point[2], point[1],
              -sinY * point[0] + cosY * point[2]}};

    const float sinP = std::sin(-instance.rotationX);
    const float cosP = std::cos(-instance.rotationX);
    point = {{point[0], cosP * point[1] - sinP * point[2],
              sinP * point[1] + cosP * point[2]}};

    const float sinR = std::sin(-instance.rotationZ);
    const float cosR = std::cos(-instance.rotationZ);
    point = {{cosR * point[0] - sinR * point[1],
              sinR * point[0] + cosR * point[1], point[2]}};

    return instance.scale != 0.0f ? scale(point, 1.0f / instance.scale) : Vec3{};
}

namespace {

/// The shared subtree walk behind resolveRecoilFlags/resolveTelescopeFlags:
/// the named bone and everything descending from it, flagged with `bit`.
/// Empty when the name resolves to nothing. Case-insensitive like the turret.
[[nodiscard]] std::vector<std::uint32_t> subtreeFlags(const Model& model,
                                                    std::string_view boneName,
                                                    std::uint32_t bit) {
    if (boneName.empty() || model.bones.empty()) {
        return {};
    }
    rm::unitdef::BoneRef ref{.name = std::string{boneName}};
    int root = resolve(model, ref);
    if (root < 0) {
        for (std::size_t bone = 0; bone < model.bones.size(); ++bone) {
            const std::string& have = model.bones[bone].name;
            if (have.size() == boneName.size()
                && std::equal(have.begin(), have.end(), boneName.begin(),
                              [](unsigned char a, unsigned char b) {
                                  return std::tolower(a) == std::tolower(b);
                              })) {
                root = static_cast<int>(bone);
                break;
            }
        }
    }
    if (root < 0) {
        return {};
    }
    std::vector<std::uint32_t> flags(model.bones.size(), 0U);
    for (std::size_t bone = 0; bone < model.bones.size(); ++bone) {
        if (descendsFrom(model, bone, root)) {
            flags[bone] |= bit;
        }
    }
    return flags;
}

} // namespace

std::vector<std::uint32_t> resolveRecoilFlags(const Model& model,
                                              std::string_view boneName) {
    return subtreeFlags(model, boneName, kBuilderRecoilBone);
}

std::vector<std::uint32_t> resolveTelescopeFlags(const Model& model,
                                                 std::string_view boneName) {
    return subtreeFlags(model, boneName, kBuilderTelescopeBone);
}

float stepRecoil(float amount, float returnPerStep) noexcept {
    return amount <= returnPerStep ? 0.0f : amount - returnPerStep;
}

RecoilSpec resolveRecoilSpec(const unitdef::Weapon& weapon, float meshToElmos,
                             float ticksPerSecond) noexcept {
    RecoilSpec spec;
    // The authored distance is SIGNED — negative is backwards along the rack,
    // positive forwards (UEL0203's +0.1) — so the elmos figure keeps the sign
    // and only the travel magnitude feeds the return-rate math. A `> 0` check
    // here once deleted every retail rack at once, because the corpus signs
    // backwards travel negative (the Titan's is -0.2).
    const float rackTravelMesh = std::abs(weapon.recoilDistanceMesh);
    spec.rackDistanceElmos = weapon.recoilDistanceMesh * meshToElmos;
    // `TelescopeRecoilDistance or RackRecoilDistance` (defaultweapons.lua:283):
    // absent falls back to the rack distance; an authored value — even zero —
    // stands. No telescope bone, no channel at all.
    const float telescopeMesh =
        weapon.telescopeBone.empty()
            ? 0.0f
            : weapon.telescopeDistanceMesh.value_or(weapon.recoilDistanceMesh);
    spec.telescopeDistanceElmos = telescopeMesh * meshToElmos;
    const float telescopeTravelMesh = std::abs(telescopeMesh);
    if (rackTravelMesh <= 0.0f) {
        // `bp.RackRecoilDistance != 0` gates the whole sequence in retail —
        // no kick, no sliders, no return.
        spec.rackDistanceElmos = 0.0f;
        spec.telescopeDistanceElmos = 0.0f;
        return spec;
    }

    // The return speed, authored or derived (defaultweapons.lua:54-62): the
    // LARGER travel over the firing interval minus the charge delay, padded
    // 25% so the slide is home before the next shot. Only 6 weapons state
    // `RackRecoilReturnSpeed`; the other ~200 ride this formula.
    float speedMeshPerSecond = weapon.recoilReturnSpeedMeshPerSecond;
    if (speedMeshPerSecond <= 0.0f) {
        const float dist = std::max(rackTravelMesh, telescopeTravelMesh);
        const float window = weapon.rateOfFire > 0.0f
            ? 1.0f / weapon.rateOfFire - weapon.muzzleChargeDelaySeconds
            : std::numeric_limits<float>::infinity();
        // Lua's arithmetic, edge cases included: a zero window divides to inf
        // (instant return), a negative one flips the sign and `math.abs`
        // recovers a large finite speed (a charge delay past the interval
        // returns FASTER), and a zero RateOfFire makes the whole expression
        // 0 (the slide never returns).
        speedMeshPerSecond = weapon.rateOfFire > 0.0f
            ? std::abs(dist / window) * 1.25f
            : 0.0f;
    }
    const float speedElmosPerTick =
        speedMeshPerSecond * meshToElmos / std::max(ticksPerSecond, 1.0f);
    const auto fractionPerTick = [speedElmosPerTick](float travelElmos) {
        if (travelElmos <= 0.0f) {
            return 0.0f;
        }
        const float perTick = speedElmosPerTick / travelElmos;
        // inf (or a speed past the whole travel) is a one-step return.
        return perTick > 1.0f ? 1.0f : perTick;
    };
    spec.rackReturnPerTick = fractionPerTick(std::abs(spec.rackDistanceElmos));
    spec.telescopeReturnPerTick =
        fractionPerTick(std::abs(spec.telescopeDistanceElmos));
    return spec;
}

} // namespace rm
