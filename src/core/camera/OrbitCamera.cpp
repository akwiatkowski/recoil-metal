#include "core/camera/OrbitCamera.hpp"

#include <algorithm>
#include <cmath>

namespace rm {

void OrbitCamera::orbit(float deltaYaw, float deltaPitch) noexcept {
    yaw += deltaYaw;
    pitch = std::clamp(pitch + deltaPitch, kMinPitch, kMaxPitch);
}

void OrbitCamera::zoom(float factor) noexcept {
    if (factor <= 0.0f) {
        return;
    }
    distance = std::clamp(distance * factor, kMinDistance, kMaxDistance);
}

void OrbitCamera::pan(float right, float forward) noexcept {
    // The view basis, flattened. viewMatrix's xAxis is cross(up, zAxis), which
    // is already horizontal for any pitch — so this IS the screen-right vector,
    // not an approximation of it. Forward is the horizontal part of the eye ->
    // target direction, which is the negated horizontal part of the offset in
    // eye(): dropping the y component is what keeps a pan level.
    const simd_float3 rightAxis{std::cos(yaw), 0.0f, -std::sin(yaw)};
    const simd_float3 forwardAxis{-std::sin(yaw), 0.0f, -std::cos(yaw)};

    target += rightAxis * right + forwardAxis * forward;

    // Clamped after the move rather than by rejecting it, so sliding along an
    // edge still works: a diagonal drag at the map's north border keeps its
    // east-west component instead of stopping dead.
    target.x = std::clamp(target.x, panMin.x, panMax.x);
    target.z = std::clamp(target.z, panMin.y, panMax.y);
}

float OrbitCamera::elmosPerPoint(float viewportHeightPoints) const noexcept {
    if (viewportHeightPoints <= 0.0f) {
        return 0.0f;
    }
    // The view frustum's height at the target's depth, divided over the points
    // that show it. tan(fovY/2) * distance is the half-height, hence the 2.
    return 2.0f * distance * std::tan(fovY * 0.5f) / viewportHeightPoints;
}

void OrbitCamera::frame(simd_float3 boxMin, simd_float3 boxMax) noexcept {
    target = (boxMin + boxMax) * 0.5f;

    // Whatever was framed is what panning may explore. Taking the limit from
    // here rather than from a map handed to the camera keeps core/camera free of
    // any notion of a map — the same reason `frame` takes a box and not a
    // HeightField.
    panMin = simd_make_float2(std::min(boxMin.x, boxMax.x), std::min(boxMin.z, boxMax.z));
    panMax = simd_make_float2(std::max(boxMin.x, boxMax.x), std::max(boxMin.z, boxMax.z));

    // Pull back far enough that the box's bounding sphere fits inside the
    // vertical field of view, with a little headroom so the map does not touch
    // the frame edges.
    constexpr float kHeadroom = 1.15f;

    const simd_float3 extent = boxMax - boxMin;
    const float radius = simd_length(extent) * 0.5f;
    const float fitDistance = radius / std::tan(fovY * 0.5f);

    distance = std::clamp(fitDistance * kHeadroom, kMinDistance, kMaxDistance);
}

simd_float3 OrbitCamera::eye() const noexcept {
    const float cosPitch = std::cos(pitch);
    const simd_float3 offset{
        cosPitch * std::sin(yaw),
        std::sin(pitch),
        cosPitch * std::cos(yaw),
    };
    return target + offset * distance;
}

simd_float4x4 OrbitCamera::viewMatrix() const noexcept {
    const simd_float3 up{0.0f, 1.0f, 0.0f};
    const simd_float3 position = eye();

    // Right-handed look-at: the camera looks down its own -Z, so zAxis points
    // back towards the eye.
    const simd_float3 zAxis = simd_normalize(position - target);
    const simd_float3 xAxis = simd_normalize(simd_cross(up, zAxis));
    const simd_float3 yAxis = simd_cross(zAxis, xAxis);

    // simd_float4x4 is column-major, so each columns[i] is a *column* of the
    // matrix — the basis vectors therefore appear transposed relative to how
    // the rotation is usually written on paper.
    return simd_matrix(
        simd_make_float4(xAxis.x, yAxis.x, zAxis.x, 0.0f),
        simd_make_float4(xAxis.y, yAxis.y, zAxis.y, 0.0f),
        simd_make_float4(xAxis.z, yAxis.z, zAxis.z, 0.0f),
        simd_make_float4(-simd_dot(xAxis, position),
                         -simd_dot(yAxis, position),
                         -simd_dot(zAxis, position),
                         1.0f));
}

simd_float4x4 OrbitCamera::projectionMatrix(float aspect) const noexcept {
    const float f = 1.0f / std::tan(fovY * 0.5f);
    const float zRange = nearZ - farZ;

    // Metal's clip space runs z from 0 at the near plane to 1 at the far plane
    // (unlike OpenGL's -1..1). These two terms are the whole difference.
    const float zScale = farZ / zRange;
    const float zBias = farZ * nearZ / zRange;

    return simd_matrix(
        simd_make_float4(f / aspect, 0.0f, 0.0f, 0.0f),
        simd_make_float4(0.0f, f, 0.0f, 0.0f),
        simd_make_float4(0.0f, 0.0f, zScale, -1.0f),
        simd_make_float4(0.0f, 0.0f, zBias, 0.0f));
}

simd_float4x4 OrbitCamera::viewProjection(float aspect) const noexcept {
    return simd_mul(projectionMatrix(aspect), viewMatrix());
}

} // namespace rm
