#pragma once

#include "core/camera/OrbitCamera.hpp"
#include "core/map/HeightField.hpp"
#include "core/scene/UnitPlacement.hpp"

#include <cstddef>
#include <optional>
#include <span>

namespace rm {

// Turning a click into a place on the map.
//
// All of it is pure: a ray is arithmetic on the camera, and the terrain
// intersection is arithmetic on the heightfield. Nothing here touches the GPU,
// so "does clicking there select that unit" is a test rather than a thing to
// squint at on screen. The alternative — reading back a depth or ID buffer —
// would be more exact, cost a GPU round-trip, and be untestable without a
// device.
struct Ray {
    simd_float3 origin;
    simd_float3 direction;  ///< normalised, so the ray parameter is elmos
};

/// The world ray through a point in the viewport.
///
/// `pointX`/`pointY` are in view coordinates with the origin at the
/// BOTTOM-LEFT, which is what AppKit hands an unflipped NSView. The viewport
/// size is in the same units — points, not pixels. Passing backing pixels for
/// one and points for the other is the classic Retina bug and shows up as
/// picking that is correct only in the lower-left quarter of the window.
[[nodiscard]] Ray screenRay(const OrbitCamera& camera, float pointX, float pointY,
                            float widthPoints, float heightPoints) noexcept;

/// Where a ray first meets the terrain, or nothing if it never does.
///
/// Marched at half-square steps and then bisected, rather than solved per
/// triangle: the heightfield has no triangles until the mesh builder makes
/// some, and a march is indifferent to how the surface was triangulated. The
/// cost is a ray grazing a thin ridge can tunnel through it — acceptable for
/// something that runs once per click, not once per frame.
///
/// Only the inside of the map counts as terrain. heightAtWorld clamps beyond
/// the border, which would otherwise let a ray heading out to sea "hit" an
/// infinite plane extruded from the map edge.
[[nodiscard]] std::optional<simd_float3> pickGround(const Ray& ray,
                                                    const HeightField& field) noexcept;

/// Perpendicular distance from a point to the ray, or infinity if the point is
/// behind the origin.
///
/// The measure pickUnit ranks candidates by, exposed because a caller holding
/// several separate instance arrays — one per model — has to compare winners
/// across them, and recomputing this at the call site would be the same four
/// lines with a different chance of being wrong.
[[nodiscard]] float distanceToRay(const Ray& ray, simd_float3 point) noexcept;

/// The instance nearest the ray within `radiusElmos` of it, or nothing.
///
/// Distance to the ray's line, not to a model's actual geometry: instances are
/// points here, and their models are neither known nor cheap to test against.
/// A radius stands in for the model's size — generous enough to click a unit
/// without hitting the pixel, small enough not to grab a neighbour.
///
/// Ties break toward the unit closest to the ray, not the closest to the
/// camera, so clicking directly at a unit standing behind another still picks
/// the one under the cursor.
[[nodiscard]] std::optional<std::size_t> pickUnit(const Ray& ray,
                                                  std::span<const UnitInstance> units,
                                                  float radiusElmos) noexcept;

/// Default click radius, in elmos.
///
/// 40 elmos is five heightmap squares — roughly the footprint of a mid-sized
/// BAR unit, and about a centimetre on screen at a normal working zoom.
inline constexpr float kDefaultPickRadiusElmos = 40.0f;

} // namespace rm
