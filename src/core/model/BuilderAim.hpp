#pragma once

#include "core/model/Pose.hpp"
#include "core/unit/UnitDef.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace rm {

inline constexpr std::uint32_t kBuilderYawBone = 1U << 0U;
inline constexpr std::uint32_t kBuilderPitchBone = 1U << 1U;

/// A `BuilderArmManipulator` resolved from blueprint bone references onto one model.
struct BuilderAimRig {
    std::vector<std::uint32_t> boneFlags;
    std::array<float, 3> yawPivot{};
    std::array<float, 3> yawAxis{{0.0f, 1.0f, 0.0f}};
    std::array<float, 3> pitchPivot{};
    std::array<float, 3> pitchAxis{{1.0f, 0.0f, 0.0f}};
    std::array<float, 3> aimPoint{{0.0f, 0.0f, 1.0f}};
    float yawMin = 0.0f;
    float yawMax = 0.0f;
    float yawSlew = 0.0f;
    float pitchMin = 0.0f;
    float pitchMax = 0.0f;
    float pitchSlew = 0.0f;

    [[nodiscard]] bool exists() const noexcept { return !boneFlags.empty(); }
};

struct BuilderAimAngles {
    float yaw = 0.0f;
    float pitch = 0.0f;
};

/// Resolves names/indices, pivots, axes, limits and subtree membership once per model.
[[nodiscard]] BuilderAimRig resolveBuilderAim(
    const Model& model, const unitdef::BuilderArmSpec& spec,
    std::span<const std::string> effectBones = {});

/// What a weapon's turret needs, in RADIANS: the presentation layer converts once when
/// it resolves, because the weapon already states speeds in radians while the firing
/// arc arrives in degrees. Same two-axis shape as a builder arm — a yaw ring carrying
/// a pitching barrel — so the resolved rig answers the same builderAimAt/step/apply
/// calls and the unit shader needs no second aim path.
struct TurretAimSpec {
    unitdef::BoneRef yawBone;
    unitdef::BoneRef pitchBone;
    unitdef::BoneRef muzzleBone;
    float yawMin = -3.14159265f;
    float yawMax = 3.14159265f;
    float yawSlew = 6.2831853f;
    float pitchMin = -1.5707964f;
    float pitchMax = 1.5707964f;
    float pitchSlew = 6.2831853f;

    [[nodiscard]] bool exists() const noexcept {
        return yawBone.present() && pitchBone.present() && muzzleBone.present();
    }
};

/// The turret twin of resolveBuilderAim: flags mark the ring and barrel subtrees, the
/// aim point is the muzzle. An unresolvable bone yields a rig that does not exist.
[[nodiscard]] BuilderAimRig resolveTurretAim(const Model& model, const TurretAimSpec& spec);

/// The clamped two-axis pose that points the rig at a model-space target.
[[nodiscard]] BuilderAimAngles builderAimAt(const BuilderAimRig& rig,
                                            const std::array<float, 3>& target) noexcept;

/// Slews toward an aim pose at the blueprint's rates. A non-positive dt snaps, as deterministic
/// headless captures do elsewhere in presentation.
[[nodiscard]] BuilderAimAngles stepBuilderAim(BuilderAimAngles from, BuilderAimAngles to,
                                              const BuilderAimRig& rig,
                                              float dtSeconds) noexcept;

/// Applies the same pitch-then-yaw hierarchy operation used by the unit shader.
[[nodiscard]] std::array<float, 3> applyBuilderAim(const std::array<float, 3>& point,
                                                  std::uint32_t boneFlags,
                                                  const BuilderAimRig& rig,
                                                  BuilderAimAngles angles) noexcept;

/// Converts a world point into the model coordinates the rig is authored in, reversing the
/// instance's translate/scale/yaw/pitch/roll transform.
[[nodiscard]] std::array<float, 3> builderTargetInModel(
    const std::array<float, 3>& world, const InstancePlacement& instance) noexcept;

} // namespace rm
