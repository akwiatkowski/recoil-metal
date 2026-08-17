#pragma once

#include "core/model/Model.hpp"
#include "core/model/Sca.hpp"

#include <array>
#include <cstddef>
#include <span>
#include <vector>

namespace rm {

// What the vertex shader applies to a vertex, per bone.
//
// A rigid transform — a rotation and a translation, no scale — because that is
// all either content family expresses: .s3o carries translations only, and
// .sca's keys are a position and a quaternion. Storing it this way rather than
// as a 4x4 keeps the buffer at 32 bytes per bone and removes any chance of
// transposing a matrix between C++ and MSL, which is a bug that renders as
// *almost* right.
struct BoneTransform {
    std::array<float, 4> rotation{{1.0f, 0.0f, 0.0f, 0.0f}};  ///< quaternion (w, x, y, z)
    std::array<float, 3> translation{{0.0f, 0.0f, 0.0f}};
    float padding = 0.0f;  ///< keeps the stride at 32, matching the MSL struct
};

static_assert(sizeof(BoneTransform) == 32,
              "BoneTransform must stay tightly packed — the shader reads it as a "
              "packed_float4 and a packed_float3");

// The transform each bone contributes when nothing is animating.
//
// This is where the two families' vertex conventions are reconciled, and it is
// the only place that difference has to exist:
//
//   Recoil  vertices are bone-local, so the bone contributes its accumulated
//           offset — exactly what milestone 5 added to each vertex directly.
//   SupCom  vertices are already in model space, so the bone contributes
//           nothing. Identity, not "no transform": the same buffer is bound
//           either way and the shader has no branch.
[[nodiscard]] std::vector<BoneTransform> restPose(const Model& model);

/// For each of the model's bones, the animation bone driving it — or -1.
///
/// Matched by NAME, not by index. The two files agree in order in practice, but
/// neither states that they must, and a silently mismatched pair animates a
/// model into knots rather than failing.
[[nodiscard]] std::vector<int> mapBonesToAnimation(const Model& model,
                                                   const sca::Animation& animation);

// The pose at a time in seconds, as a transform per model bone.
//
// Time wraps at the animation's duration, so a caller can hand it a clock. Keys
// are interpolated: linearly for position, and for rotation by normalised lerp
// with a hemisphere fix rather than a true slerp — at 30 keys a second adjacent
// rotations are close enough that the two are indistinguishable, and nlerp
// cannot divide by a near-zero sine.
//
// A bone the animation does not drive falls back to the model's own local
// transform, so it reproduces its rest position while still following any
// ancestor that IS driven — a partially matched animation moves what it can
// instead of collapsing the model.
[[nodiscard]] std::vector<BoneTransform> poseAt(const Model& model,
                                                const sca::Animation& animation,
                                                std::span<const int> boneMap, float seconds);

} // namespace rm
