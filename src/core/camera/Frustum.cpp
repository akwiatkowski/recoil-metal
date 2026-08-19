#include "core/camera/Frustum.hpp"

#include <cmath>

namespace {

/// Scales a plane so its normal is unit length. Not needed for a sign test, but
/// it makes the plane's `d` a real distance, which is what any later use — a
/// sphere test, a margin — will expect.
[[nodiscard]] simd_float4 normalisePlane(simd_float4 plane) noexcept {
    const float length = simd_length(plane.xyz);
    return length > 0.0f ? plane / length : plane;
}

} // namespace

namespace rm {

Frustum frustumOf(simd_float4x4 viewProjection) noexcept {
    // simd matrices are column-major, so the ROWS the extraction wants are
    // gathered across the columns. Reading these as columns is the standard way
    // to get a frustum that is subtly rotated and culls the wrong things.
    const simd_float4 row0 = simd_make_float4(viewProjection.columns[0].x,
                                              viewProjection.columns[1].x,
                                              viewProjection.columns[2].x,
                                              viewProjection.columns[3].x);
    const simd_float4 row1 = simd_make_float4(viewProjection.columns[0].y,
                                              viewProjection.columns[1].y,
                                              viewProjection.columns[2].y,
                                              viewProjection.columns[3].y);
    const simd_float4 row2 = simd_make_float4(viewProjection.columns[0].z,
                                              viewProjection.columns[1].z,
                                              viewProjection.columns[2].z,
                                              viewProjection.columns[3].z);
    const simd_float4 row3 = simd_make_float4(viewProjection.columns[0].w,
                                              viewProjection.columns[1].w,
                                              viewProjection.columns[2].w,
                                              viewProjection.columns[3].w);

    Frustum frustum;
    frustum.planes[0] = normalisePlane(row3 + row0);  // left
    frustum.planes[1] = normalisePlane(row3 - row0);  // right
    frustum.planes[2] = normalisePlane(row3 + row1);  // bottom
    frustum.planes[3] = normalisePlane(row3 - row1);  // top
    // Metal's depth range is 0..1, so near is row2 by itself. For OpenGL's
    // -1..1 it would be row3 + row2, and using that here culls everything
    // near the camera.
    frustum.planes[4] = normalisePlane(row2);         // near
    frustum.planes[5] = normalisePlane(row3 - row2);  // far
    return frustum;
}

bool Frustum::intersectsBox(simd_float3 minimum, simd_float3 maximum) const noexcept {
    for (const simd_float4 plane : planes) {
        // The box's corner furthest along the plane's normal. If even that one
        // is behind the plane, every corner is, and the box is wholly outside.
        // Any other corner choice would cull boxes that merely straddle.
        const simd_float3 furthest = simd_make_float3(plane.x >= 0.0f ? maximum.x : minimum.x,
                                                      plane.y >= 0.0f ? maximum.y : minimum.y,
                                                      plane.z >= 0.0f ? maximum.z : minimum.z);

        if (simd_dot(plane.xyz, furthest) + plane.w < 0.0f) {
            return false;
        }
    }
    return true;
}

} // namespace rm
