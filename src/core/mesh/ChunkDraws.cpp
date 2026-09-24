#include "core/mesh/ChunkDraws.hpp"

#include <cmath>

namespace rm {

int lodForDistance(float distance, float chunkWidth) noexcept {
    // A degenerate chunk has no width to measure distance in. Nothing sensible
    // to divide by, and the finest level is always safe.
    if (!(chunkWidth > 0.0f)) {
        return 0;
    }

    const float inChunks = distance / chunkWidth;
    if (inChunks < kLodNearChunks) {
        return 0;
    }
    return inChunks < kLodFarChunks ? 1 : 2;
}

float lodMetric(float radius, float distance, float fovY, float viewportHeight,
                float lodScale) noexcept {
    // Retail: `lodDist * GLOBAL_LOD_FACTORS[objType]`
    //   = lodDist * (LODScale * lppScale)
    //   = lodDist * lodScale * (2 * tan(fovY/2)) * viewportHeight / distance
    //   = radius * lodScale * 2 * tan(fovY/2) * viewportHeight / distance
    // FAF `rts/Game/Camera.cpp:144` (lppScale), `rts/Rendering/LuaObjectDrawer.cpp:324`
    // (LODScale * lppScale), `rts/Lua/LuaObjectMaterial.h:159` (final product).
    //
    // The denominator guards: an eye at the chunk's centre or a zero-radius
    // chunk both divide by zero. `distance == 0` is the eye sitting on the
    // chunk — the chunk fills the entire view, so any sane answer is "the
    // finest level", which `lodForPixelScale` short-circuits to. Returning
    // +infinity would also be honest here; we return 0 because the call site
    // treats 0 as the "below every threshold" sentinel, and 0 from
    // `lodMetric` is never a real metric value (it's at most a degenerate
    // edge case). `viewportHeight == 0` is a zero-size framebuffer (the
    // headless screenshot path with a bad config); return zero so the caller
    // gets a sane "everything is one big pixel" result instead of NaN.
    if (!(distance > 0.0f)) {
        return 0.0f;
    }
    const float tanHalf = std::tan(fovY * 0.5f);
    return radius * lodScale * (2.0f * tanHalf) * viewportHeight / distance;
}

int lodForPixelScale(float radius, float distance, float fovY, float viewportHeight,
                     float lodScale) noexcept {
    // The eye is on the chunk's centre — the chunk fills the view. The
    // finest level is the only honest answer, and we cannot compute the metric
    // (it would divide by zero). `lodForDistance` does the same short-circuit
    // by guarding `chunkWidth > 0`; this is its pixel-scale twin.
    if (!(distance > 0.0f)) {
        return 0;
    }
    const float pixels = lodMetric(radius, distance, fovY, viewportHeight, lodScale);
    if (pixels >= kLodFarPixels) {
        return 0;
    }
    return pixels >= kLodNearPixels ? 1 : 2;
}

} // namespace rm
