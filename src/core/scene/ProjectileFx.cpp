#include "core/scene/ProjectileFx.hpp"

#include "core/scene/UnitIcons.hpp"

#include <algorithm>

namespace rm {
namespace {

/// A rebuilt-every-frame particle is born already grown up, exactly as the strategic icons
/// are and for the icon file's documented reason: the shader fades a particle IN over the
/// first 0.15 of its lifetime, and a particle reborn each frame at age zero is a particle
/// that is never visible at all.
inline constexpr float kDotLifetime = 1.0f;
inline constexpr float kDotAge = kDotLifetime * 0.2f;

/// The trail's own clock: long enough that ten-per-second emission (one per tick) reads as
/// a line, short enough that the sky is not striped with history a minute old.
inline constexpr float kTrailLifetime = 0.45f;

/// One dot. Additive — colour with zero alpha, the Particle contract's "light, not paint" —
/// because ordnance in flight is the one thing here that should brighten whatever it
/// crosses and hide nothing.
void dot(std::vector<Particle>& into, std::array<float, 3> at, std::array<float, 4> colour,
         float sizeElmos) {
    into.push_back(Particle{
        .origin = at,
        .age = kDotAge,
        .velocity = {0.0f, 0.0f, 0.0f},
        .lifetime = kDotLifetime,
        .colour = colour,
        .size = sizeElmos,
    });
}

} // namespace

void appendProjectiles(std::vector<Particle>& into, std::span<const sim::Projectile> shots,
                       float alpha, float elmosPerPoint) {
    if (!(elmosPerPoint > 0.0f)) {
        return;
    }
    const float blend = std::clamp(alpha, 0.0f, 1.0f);

    for (const sim::Projectile& shot : shots) {
        // Extrapolated by the frame's fraction of a tick — at most one tick of flight,
        // along the velocity the sim will apply anyway. The float boundary, crossed in the
        // draw direction only.
        const std::array<float, 3> at{
            sim::fxToFloat(shot.position[0]) + sim::fxToFloat(shot.velocity[0]) * blend,
            sim::fxToFloat(shot.position[1]) + sim::fxToFloat(shot.velocity[1]) * blend,
            sim::fxToFloat(shot.position[2]) + sim::fxToFloat(shot.velocity[2]) * blend,
        };

        switch (shot.arc) {
        case unitdef::BallisticArc::None: {
            // The tracer: a short dash, drawn as three dots sampled back along the flight —
            // the particle is a round glow and the streak is made of them, the way the
            // trail is made of puffs. Warm white, small, floored at a glint.
            const float size =
                std::max(2.0f, kTracerFloorPoints * elmosPerPoint);
            constexpr std::array<float, 4> kTracer{1.0f, 0.95f, 0.75f, 0.0f};
            for (int sample = 0; sample < 3; ++sample) {
                const float back = 0.35f * static_cast<float>(sample);
                dot(into,
                    {at[0] - sim::fxToFloat(shot.velocity[0]) * back,
                     at[1] - sim::fxToFloat(shot.velocity[1]) * back,
                     at[2] - sim::fxToFloat(shot.velocity[2]) * back},
                    {kTracer[0], kTracer[1] * (1.0f - 0.15f * static_cast<float>(sample)),
                     kTracer[2] * (1.0f - 0.25f * static_cast<float>(sample)), 0.0f},
                    size * (1.0f - 0.2f * static_cast<float>(sample)));
            }
            break;
        }
        case unitdef::BallisticArc::Low:
            dot(into, at, {1.0f, 0.72f, 0.35f, 0.0f},
                std::max(2.5f, kLobFloorPoints * elmosPerPoint));
            break;
        case unitdef::BallisticArc::High:
            // THE YELLOW DOT. Bright amber, and its floor is the largest here — artillery
            // is the ordnance a commander reroutes an army around, and Supreme Commander
            // keeps it visible from orbit for exactly that reason.
            dot(into, at, {1.0f, 0.85f, 0.3f, 0.0f},
                std::max(3.5f, kArtilleryFloorPoints * elmosPerPoint));
            break;
        }
    }
}

void emitProjectileTrails(std::vector<Particle>& into,
                          std::span<const sim::Projectile> shots) {
    for (const sim::Projectile& shot : shots) {
        if (shot.arc == unitdef::BallisticArc::None) {
            continue;  // a tracer's shape is its streak; smoke belongs to the arcs
        }
        // Ordinary premultiplied smoke, one puff per tick at the shell's own position — the
        // arc paints itself out of its history, and the spacing is a tick of travel by
        // construction. A whisper of lift so the line softens as it fades.
        into.push_back(Particle{
            .origin = {sim::fxToFloat(shot.position[0]), sim::fxToFloat(shot.position[1]),
                       sim::fxToFloat(shot.position[2])},
            .age = 0.0f,
            .velocity = {0.0f, 3.0f, 0.0f},
            .lifetime = kTrailLifetime,
            .colour = {0.14f, 0.13f, 0.12f, 0.4f},
            .size = 2.5f,
        });
    }
}

} // namespace rm
