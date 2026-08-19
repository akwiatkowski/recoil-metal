#include "core/scene/PropBatch.hpp"

namespace rm {

std::size_t cullPropsByDistance(std::span<const UnitInstance> instances,
                                std::array<float, 3> eye, float drawDistanceElmos,
                                std::span<UnitInstance> out) noexcept {
    if (out.size() < instances.size()) {
        return 0;  // a caller mistake, and one that would otherwise overrun
    }

    // Squared, so the inner loop has no square root in it. At 47 000 props on the
    // busiest stock map this runs every frame, and it is the only per-prop CPU work
    // there is.
    const float limit = drawDistanceElmos;
    if (!(limit > 0.0f)) {
        return 0;
    }
    const bool unlimited = limit == std::numeric_limits<float>::infinity();
    const float limitSquared = unlimited ? 0.0f : limit * limit;

    std::size_t kept = 0;
    for (const UnitInstance& instance : instances) {
        if (!unlimited) {
            const float dx = instance.position[0] - eye[0];
            const float dy = instance.position[1] - eye[1];
            const float dz = instance.position[2] - eye[2];
            if (dx * dx + dy * dy + dz * dz > limitSquared) {
                continue;
            }
        }
        out[kept] = instance;
        ++kept;
    }
    return kept;
}

} // namespace rm
