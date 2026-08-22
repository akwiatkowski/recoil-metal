#include "core/scene/CombatEffects.hpp"

#include "core/sim/Combat.hpp"

namespace rm {
namespace {

/// The flash: one additive particle at the muzzle. Additive — colour with zero alpha, per
/// the Particle contract — because a flash is LIGHT: it brightens what is behind it and
/// obscures nothing, which is also what lets it share the dust's pipeline and draw.
///
/// Short. A muzzle flash that outlives two frames reads as a lamp; at 0.1 seconds it is a
/// glint the eye catches without ever seeing the shape of.
inline constexpr float kFlashLifetime = 0.1f;
inline constexpr float kFlashSize = 7.0f;

/// The impact: a puff of smoke and a spark. The smoke is ordinary premultiplied grey that
/// drifts up and fades; the spark is the flash's additive cousin, smaller and hotter.
inline constexpr float kPuffLifetime = 0.55f;
inline constexpr float kPuffSize = 5.0f;
inline constexpr float kSparkLifetime = 0.12f;
inline constexpr float kSparkSize = 4.0f;

[[nodiscard]] std::array<float, 3> atOf(const sim::Event& event) {
    return {sim::fxToFloat(event.at[0]), sim::fxToFloat(event.at[1]),
            sim::fxToFloat(event.at[2])};
}

} // namespace

void emitCombatEffects(std::vector<Particle>& into, std::span<const sim::Event> events) {
    for (const sim::Event& event : events) {
        switch (event.kind) {
        case sim::EventKind::WeaponFired: {
            std::array<float, 3> at = atOf(event);
            // The muzzle's own height, the same constant the projectile spawns at
            // (Combat.hpp) — the flash must sit where the shot comes from or the two read
            // as unrelated.
            at[1] += sim::fxToFloat(sim::kMuzzleHeight);
            into.push_back(Particle{
                .origin = at,
                .age = 0.0f,
                .velocity = {0.0f, 0.0f, 0.0f},
                .lifetime = kFlashLifetime,
                // Warm white-yellow, additive: (rgb, 0) adds light and hides nothing.
                .colour = {1.0f, 0.85f, 0.45f, 0.0f},
                .size = kFlashSize,
            });
            break;
        }
        case sim::EventKind::ProjectileImpact: {
            const std::array<float, 3> at = atOf(event);
            // The smoke: premultiplied grey, drifting up, gone in half a second. One puff
            // per impact rather than a burst — a battle is many impacts, and the burst is
            // the battle.
            into.push_back(Particle{
                .origin = at,
                .age = 0.0f,
                .velocity = {0.0f, 9.0f, 0.0f},
                .lifetime = kPuffLifetime,
                .colour = {0.22f, 0.20f, 0.18f, 0.55f},
                .size = kPuffSize,
            });
            into.push_back(Particle{
                .origin = at,
                .age = 0.0f,
                .velocity = {0.0f, 2.0f, 0.0f},
                .lifetime = kSparkLifetime,
                .colour = {1.0f, 0.6f, 0.3f, 0.0f},
                .size = kSparkSize,
            });
            break;
        }
        default:
            break;
        }
    }
}

} // namespace rm
