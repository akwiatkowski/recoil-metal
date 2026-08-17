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

void OrbitCamera::frame(simd_float3 boxMin, simd_float3 boxMax) noexcept {
    target = (boxMin + boxMax) * 0.5f;

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
