#pragma once

#include <array>
#include <simd/simd.h>

namespace rm {

// The six planes of a view frustum, for deciding what not to draw.
//
// Pure arithmetic on a matrix, which is why it lives in core/ with a test
// rather than inside the renderer: culling is invisible when it works and
// looks like missing geometry when it does not, and "does this box survive"
// is far easier to assert than to see.
struct Frustum {
    // Each plane as (nx, ny, nz, d), normalised, with the inside on the
    // positive side — so a point is inside when dot(plane.xyz, p) + d >= 0.
    std::array<simd_float4, 6> planes{};

    /// Whether an axis-aligned box is at all inside.
    ///
    /// Conservative: a box that straddles a plane is kept. Culling only what is
    /// WHOLLY outside is the point — the opposite test clips geometry at the
    /// screen edge, which shows up as terrain disappearing as the camera turns.
    [[nodiscard]] bool intersectsBox(simd_float3 minimum, simd_float3 maximum) const noexcept;
};

/// The frustum of a view-projection matrix, by Gribb-Hartmann plane extraction:
/// each plane is a sum or difference of two rows of the matrix.
///
/// Written for Metal's clip space, whose depth runs 0..1 — so the near plane is
/// row 2 alone rather than row 3 + row 2 as it would be for OpenGL's -1..1.
/// Getting that wrong culls everything close to the camera.
[[nodiscard]] Frustum frustumOf(simd_float4x4 viewProjection) noexcept;

} // namespace rm
