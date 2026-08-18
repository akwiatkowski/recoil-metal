#include "core/scene/Picking.hpp"

#include <cmath>
#include <limits>

namespace {

/// Marching step, in elmos. Half a heightmap square: fine enough that the
/// surface cannot change slope between samples in a way that hides a crossing,
/// coarse enough that a whole-map ray is a few thousand samples rather than
/// tens of thousands.
constexpr float kMarchStepElmos = static_cast<float>(rm::kSquareSize) * 0.5f;

/// Bisections applied once a crossing is bracketed. Twenty halvings take the
/// 4-elmo bracket to under a millionth of an elmo — far past what anything
/// downstream can tell apart, and still only twenty height samples.
constexpr int kRefinementSteps = 20;

/// How far a ray is followed, as a multiple of the map diagonal. Two covers a
/// ray entering at one corner and leaving at the other with room for a camera
/// standing well outside the map.
constexpr float kMaxMarchInDiagonals = 2.0f;

[[nodiscard]] bool insideMap(const rm::HeightField& field, simd_float3 point) noexcept {
    return point.x >= 0.0f && point.x <= field.widthElmos()
        && point.z >= 0.0f && point.z <= field.depthElmos();
}

/// Height of the ray above the terrain — negative once it is underground. This
/// is the function whose zero the march is looking for.
[[nodiscard]] float clearance(const rm::HeightField& field, simd_float3 point) noexcept {
    return point.y - field.heightAtWorld(point.x, point.z);
}

} // namespace

namespace rm {

Ray screenRay(const OrbitCamera& camera, float pointX, float pointY, float widthPoints,
              float heightPoints) noexcept {
    const simd_float3 eye = camera.eye();
    const simd_float3 forward = simd_normalize(camera.target - eye);

    // A window mid-resize, or a zero-sized offscreen target. There is no
    // meaningful ray through a viewport with no area, and the centre one is a
    // defined answer rather than a NaN propagating into the sim.
    if (!(widthPoints > 0.0f) || !(heightPoints > 0.0f)) {
        return Ray{.origin = eye, .direction = forward};
    }

    const float aspect = widthPoints / heightPoints;
    const simd_float4x4 inverse = simd_inverse(camera.viewProjection(aspect));

    // View coordinates to Metal's normalised device coordinates. Both axes run
    // -1..1 with +Y up — the same sense as AppKit's bottom-left origin, so Y
    // needs no flip here even though the framebuffer's origin is top-left.
    const float ndcX = 2.0f * (pointX / widthPoints) - 1.0f;
    const float ndcY = 2.0f * (pointY / heightPoints) - 1.0f;

    // Unprojecting the FAR plane (z = 1 in Metal's [0, 1] depth range) rather
    // than the near one: the difference from the eye is the ray direction, and
    // the far point maximises the numerical distance being normalised.
    const simd_float4 farClip = simd_make_float4(ndcX, ndcY, 1.0f, 1.0f);
    const simd_float4 farWorld = simd_mul(inverse, farClip);

    // A perspective divide by a zero w means the point is on the eye plane,
    // which the far plane never is — but guard rather than emit a NaN into the
    // frame loop.
    if (farWorld.w == 0.0f) {
        return Ray{.origin = eye, .direction = forward};
    }

    const simd_float3 target =
        simd_make_float3(farWorld.x, farWorld.y, farWorld.z) / farWorld.w;

    return Ray{.origin = eye, .direction = simd_normalize(target - eye)};
}

std::optional<simd_float3> pickGround(const Ray& ray, const HeightField& field) noexcept {
    if (field.squaresX <= 0 || field.squaresZ <= 0) {
        return std::nullopt;
    }

    const float maxDistance =
        std::hypot(field.widthElmos(), field.depthElmos()) * kMaxMarchInDiagonals;

    // A camera dipped below the terrain — which this one is free to do, since
    // the terrain is drawn from underneath rather than culled — starts the ray
    // underground. There is no crossing to find, and the honest answer is the
    // ground directly at the origin.
    if (insideMap(field, ray.origin) && clearance(field, ray.origin) <= 0.0f) {
        return simd_make_float3(ray.origin.x,
                                field.heightAtWorld(ray.origin.x, ray.origin.z),
                                ray.origin.z);
    }

    float previousT = 0.0f;
    bool previousInside = insideMap(field, ray.origin);

    for (float t = kMarchStepElmos; t <= maxDistance; t += kMarchStepElmos) {
        const simd_float3 here = ray.origin + ray.direction * t;
        const bool inside = insideMap(field, here);

        // A crossing only counts with both ends over the map. Without the
        // previousInside half of this, a ray that re-enters the map already
        // underground reports a hit at the border it never actually touched.
        if (inside && previousInside && clearance(field, here) <= 0.0f) {
            float low = previousT;   // above the terrain
            float high = t;          // at or below it

            for (int i = 0; i < kRefinementSteps; ++i) {
                const float middle = (low + high) * 0.5f;
                if (clearance(field, ray.origin + ray.direction * middle) > 0.0f) {
                    low = middle;
                } else {
                    high = middle;
                }
            }

            const simd_float3 hit = ray.origin + ray.direction * high;
            // Snapped onto the surface: bisection leaves the point within a
            // millionth of an elmo of it, and a caller comparing the Y against
            // the field should get equality rather than nearly.
            return simd_make_float3(hit.x, field.heightAtWorld(hit.x, hit.z), hit.z);
        }

        previousT = t;
        previousInside = inside;
    }

    return std::nullopt;
}

float distanceToRay(const Ray& ray, simd_float3 point) noexcept {
    const simd_float3 toPoint = point - ray.origin;

    // Distance along the ray. Negative means the point is behind the viewer,
    // where the perpendicular distance is still small but selecting it would
    // mean clicking the sky and grabbing something at your back. Infinity
    // rather than a flag: it loses every comparison without a special case.
    const float along = simd_dot(toPoint, ray.direction);
    if (along <= 0.0f) {
        return std::numeric_limits<float>::infinity();
    }

    return simd_length(toPoint - ray.direction * along);
}

std::optional<std::size_t> pickUnit(const Ray& ray, std::span<const UnitInstance> units,
                                    float radiusElmos) noexcept {
    std::optional<std::size_t> best;
    float bestDistance = radiusElmos;

    for (std::size_t i = 0; i < units.size(); ++i) {
        const simd_float3 position = simd_make_float3(
            units[i].position[0], units[i].position[1], units[i].position[2]);

        const float distance = distanceToRay(ray, position);
        if (distance <= bestDistance) {
            bestDistance = distance;
            best = i;
        }
    }

    return best;
}

} // namespace rm
