#include "core/scene/UnitIcons.hpp"

namespace rm {

float apparentPoints(float radiusElmos, float elmosPerPoint) noexcept {
    if (elmosPerPoint <= 0.0f) {
        return 0.0f;
    }
    return radiusElmos * 2.0f / elmosPerPoint;  // the diameter is what one sees
}

std::size_t appendUnitIcons(std::vector<Particle>& into,
                            std::span<const UnitInstance> instances,
                            std::span<const float> radiiElmos, float elmosPerPoint) {
    if (elmosPerPoint <= 0.0f) {
        return 0;
    }

    // A size in points, converted once rather than per unit: every icon is the same size on
    // screen, which is the whole point of an icon.
    const float sizeElmos = kIconSizePoints * elmosPerPoint;

    std::size_t added = 0;
    for (std::size_t i = 0; i < instances.size(); ++i) {
        const float radius = i < radiiElmos.size() ? radiiElmos[i] : 0.0f;
        if (radius <= 0.0f) {
            continue;  // a retired corpse, or something that opted out of collision
        }
        if (apparentPoints(radius, elmosPerPoint) >= kIconThresholdPoints) {
            continue;  // the mesh still reads; an icon over it would be a square on a tank
        }

        const UnitInstance& unit = instances[i];

        // The army's colour, PREMULTIPLIED, because that is what the particle pipeline
        // takes (ADR-025) and an icon wants ordinary translucency rather than an additive
        // spark. Alpha is deliberately below one: a solid block hides the ground it stands
        // on, and a crowded battle line becomes one shape.
        constexpr float kIconAlpha = 0.85f;
        const std::array<float, 4>& c = unit.teamColour;

        into.push_back(Particle{
            .origin = {unit.position[0], unit.position[1] + kIconLiftElmos, unit.position[2]},
            // NOT zero: the shader would fade it to nothing. See kIconBirthAgeFraction.
            .age = kIconLifetimeSeconds * kIconBirthAgeFraction,
            .velocity = {0.0f, 0.0f, 0.0f},
            // Rebuilt every frame from the units' live positions, so a lifetime only has to
            // outlast the frame it is drawn in. One second is far more than that and keeps
            // the fade the pipeline applies over a lifetime from ever showing.
            .lifetime = kIconLifetimeSeconds,
            .colour = {c[0] * kIconAlpha, c[1] * kIconAlpha, c[2] * kIconAlpha, kIconAlpha},
            .size = sizeElmos,
            .growth = 0.0f,
        });
        ++added;
    }

    return added;
}

} // namespace rm
