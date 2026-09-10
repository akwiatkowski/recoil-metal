#include "core/scene/CombatEffects.hpp"

#include "core/sim/Combat.hpp"

#include <cmath>
#include <algorithm>

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
/// The beam: a chain of additive particles along the muzzle-to-strike line, dense enough
/// to read as one bar of light at battle zoom, alive for a quarter second — a pulse, not a
/// lamp. Spacing in elmos decides the particle count, so a long-range beam costs more
/// particles exactly when it is the most visible thing on screen.
inline constexpr float kBeamLifetime = 0.25f;
inline constexpr float kBeamSize = 3.5f;
inline constexpr float kBeamSpacingElmos = 4.0f;

inline constexpr float kPuffLifetime = 0.55f;
inline constexpr float kPuffSize = 5.0f;
inline constexpr float kSparkLifetime = 0.12f;
inline constexpr float kSparkSize = 4.0f;

/// The death burst: a white flash, an orange fireball, two smoke puffs and three
/// sparks, all scaled by the corpse's collision radius. A death with no other visual
/// is the common case (most units have no authored death weapon), and silence there
/// reads as a bug — every kill should bloom on screen the way retail's do.
///
/// Sizes in radii, clamped so a scout pops and an experimental does not fill the map.
inline constexpr float kDeathFallbackRadius = 2.0f;
inline constexpr float kDeathFlashLifetime = 0.15f;
inline constexpr float kDeathFireLifetime = 0.45f;
inline constexpr float kDeathSmokeLifetime = 2.5f;
[[nodiscard]] std::array<float, 3> atOf(const sim::Event& event) {
    return {sim::fxToFloat(event.at[0]), sim::fxToFloat(event.at[1]),
            sim::fxToFloat(event.at[2])};
}

} // namespace

void emitCombatEffects(std::vector<Particle>& into, std::span<const sim::Event> events,
    const WeaponVisuals* visuals, CombatEffectState* state, float seconds,
    std::function<float(sim::UnitId)> unitRadius) {
    CombatEffectState immediate;
    if (visuals && !state) state = &immediate;
    const auto burst = [&](const std::string& key, std::array<float,3> position,
                           std::array<float,3> direction, sim::UnitId owner = {}, const std::string& weapon = "") {
        if (!visuals || !visuals->contains(key)) return false;
        for (const auto id : visuals->find(key)) {
            state->bursts.push_back({id, position, 0, ++state->seed, visuals->scale(key), owner, weapon, direction});
        }
        return true;
    };
    for (const sim::Event& event : events) {
        const std::array<float,3> direction{sim::fxToFloat(event.visualDirection[0]),
            sim::fxToFloat(event.visualDirection[1]), sim::fxToFloat(event.visualDirection[2])};
        switch (event.kind) {
        case sim::EventKind::WeaponFired: {
            const std::array<float,3> muzzle{sim::fxToFloat(event.at2[0]),
                sim::fxToFloat(event.at2[1]), sim::fxToFloat(event.at2[2])};
            if (burst(event.visualId + "#FxMuzzleFlash", muzzle, direction, event.unit, event.visualId)) break;
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
        case sim::EventKind::BeamFired: {
            // The line itself, muzzle to strike. Both ends ride the event because by the
            // time anything draws, either unit's slot may already be someone else.
            const std::array<float, 3> to = atOf(event);
            const std::array<float, 3> from = {sim::fxToFloat(event.at2[0]),
                                               sim::fxToFloat(event.at2[1]),
                                               sim::fxToFloat(event.at2[2])};
            (void)burst(event.visualId + "#FxMuzzleFlash", from, direction, event.unit, event.visualId);
            if (visuals && state && !visuals->find(event.visualId).empty()) {
                // A weapon fires one beam at a time: a new shot replaces the live one rather
                // than stacking a second strip on top of it. The strips themselves are drawn
                // below, one per tick, so the endpoints can move while the beam lasts.
                std::erase_if(state->beams, [&](const CombatBeam& live) {
                    return live.owner == event.unit && live.weapon == event.visualId;
                });
                state->beams.push_back(CombatBeam{.weapon = event.visualId, .owner = event.unit,
                    .target = event.instigator, .from = from, .to = to,
                    .remaining = std::max(sim::fxToFloat(event.visualDuration), seconds)});
            } else {
                const float dx = to[0] - from[0];
                const float dy = to[1] - from[1];
                const float dz = to[2] - from[2];
                const float length = std::sqrt(dx * dx + dy * dy + dz * dz);
                const int steps = std::max(2, static_cast<int>(length / kBeamSpacingElmos));
                for (int i = 0; i <= steps; ++i) {
                    const float t = static_cast<float>(i) / static_cast<float>(steps);
                    const std::array<float, 3> node{from[0] + dx * t, from[1] + dy * t,
                                                    from[2] + dz * t};
                    into.push_back(Particle{
                        .origin = node,
                        .age = 0.0f,
                        .velocity = {0.0f, 0.0f, 0.0f},
                        .lifetime = kBeamLifetime,
                        // Hot blue-white, additive — a laser is light and nothing else.
                        .colour = {0.55f, 0.75f, 1.0f, 0.0f},
                        .size = kBeamSize,
                    });
                    // The halo: the same node two and a half times wider at a third
                    // the brightness. A bare chain reads as beads at battle zoom;
                    // the halo merges them into one bar of light.
                    into.push_back(Particle{
                        .origin = node,
                        .age = 0.0f,
                        .velocity = {0.0f, 0.0f, 0.0f},
                        .lifetime = kBeamLifetime,
                        .colour = {0.18f, 0.25f, 0.33f, 0.0f},
                        .size = kBeamSize * 2.5f,
                    });
                }
            }
            if (burst(event.visualId + "#FxImpactUnit", to, direction)) break;
            // And the spark where it lands, so the strike point reads even when the line
            // is foreshortened to nothing by the camera.
            into.push_back(Particle{
                .origin = to,
                .age = 0.0f,
                .velocity = {0.0f, 2.0f, 0.0f},
                .lifetime = kSparkLifetime,
                .colour = {0.7f, 0.85f, 1.0f, 0.0f},
                .size = kSparkSize,
            });
            break;
        }
        case sim::EventKind::ProjectileImpact: {
            const std::array<float, 3> at = atOf(event);
            const char* field = "FxImpactNone";
            switch (event.impactType) {
            case sim::ImpactType::Terrain: field="FxImpactLand"; break;
            case sim::ImpactType::Water: field="FxImpactWater"; break;
            case sim::ImpactType::Underwater:
            case sim::ImpactType::UnitUnderwater: field="FxImpactUnderWater"; break;
            case sim::ImpactType::Projectile: field="FxImpactProjectile"; break;
            case sim::ImpactType::ProjectileUnderwater: field="FxImpactProjectileUnderWater"; break;
            case sim::ImpactType::Prop: field="FxImpactProp"; break;
            case sim::ImpactType::Shield: field="FxImpactShield"; break;
            case sim::ImpactType::Unit: field="FxImpactUnit"; break;
            case sim::ImpactType::UnitAir: field="FxImpactAirUnit"; break;
            default: break;
            }
            if (burst(event.visualId + "#" + field, at, direction)) break;
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
        case sim::EventKind::ShieldDamaged: {
            into.push_back(Particle{
                .origin = atOf(event),
                .age = 0.0f,
                .velocity = {0.0f, 2.0f, 0.0f},
                .lifetime = kBeamLifetime,
                .colour = {0.25f, 0.75f, 1.0f, 0.0f},
                .size = kFlashSize,
            });
            break;
        }
        case sim::EventKind::UnitDestroyed: {
            const std::array<float, 3> at = atOf(event);
            const float radius =
                unitRadius ? std::max(0.5f, unitRadius(event.unit)) : kDeathFallbackRadius;
            const float flash = std::clamp(3.0f * radius, 6.0f, 36.0f);
            const float fire = std::clamp(2.0f * radius, 4.0f, 24.0f);
            const float smoke = std::clamp(2.5f * radius, 5.0f, 30.0f);
            into.push_back(Particle{
                .origin = at,
                .age = 0.0f,
                .velocity = {0.0f, 0.0f, 0.0f},
                .lifetime = kDeathFlashLifetime,
                .colour = {1.0f, 0.95f, 0.8f, 0.0f},
                .size = flash,
            });
            into.push_back(Particle{
                .origin = at,
                .age = 0.0f,
                .velocity = {0.0f, 3.0f, 0.0f},
                .lifetime = kDeathFireLifetime,
                .colour = {1.0f, 0.55f, 0.2f, 0.0f},
                .size = fire,
            });
            for (int i = 0; i < 2; ++i) {
                into.push_back(Particle{
                    .origin = {at[0], at[1] + fire * 0.25f * static_cast<float>(i), at[2]},
                    .age = 0.0f,
                    .velocity = {0.0f, 9.0f, 0.0f},
                    .lifetime = kDeathSmokeLifetime,
                    .colour = {0.22f, 0.20f, 0.18f, 0.55f},
                    .size = smoke,
                });
            }
            // Fixed fan of sparks: deterministic — no RNG state threads through here,
            // and a death that rolled differently every replay would be a lie.
            const std::array<std::array<float, 3>, 3> fan{{{6.0f, 8.0f, 0.0f},
                                                           {-5.0f, 10.0f, 3.0f},
                                                           {1.0f, 7.0f, -6.0f}}};
            for (const auto& velocity : fan) {
                into.push_back(Particle{
                    .origin = at,
                    .age = 0.0f,
                    .velocity = velocity,
                    .lifetime = kSparkLifetime,
                    .colour = {1.0f, 0.6f, 0.3f, 0.0f},
                    .size = kSparkSize,
                });
            }
            break;
        }
        default:
            break;
        }
        }
    if (state && visuals) {
        for (auto& emitter : state->bursts) {
            const auto& material = visuals->materials[emitter.material];
            const auto first = into.size();
            const float end = emitter.started ? emitter.age+seconds : emitter.age;
            emitWeaponParticles(into, material, emitter.material, emitter.position, emitter.position,
                emitter.age, end, emitter.seed, emitter.direction, !emitter.started);
            for (auto i=first; i<into.size(); ++i) {
                auto& particle = into[i];
                particle.size *= emitter.scale;
                particle.growth *= emitter.scale;
                for (std::size_t axis=0; axis<3; ++axis) {
                    particle.origin[axis] = emitter.position[axis]
                        + (particle.origin[axis]-emitter.position[axis])*emitter.scale;
                    particle.velocity[axis] *= emitter.scale;
                    particle.acceleration[axis] *= emitter.scale;
                }
            }
            emitter.age = end;
            emitter.started = true;
        }
        std::erase_if(state->bursts, [&](const auto& emitter) {
            const auto& material = visuals->materials[emitter.material];
            return material.emitterLifetime >= 0 && emitter.age >= material.emitterLifetime;
        });
        for (auto& beam : state->beams) {
            // One strip that lives exactly until the next tick redraws it. Its age starts
            // where the previous strip's ended, which is what keeps texture scrolling smooth.
            const auto first = into.size();
            appendWeaponVisual(into, *visuals, beam.weapon, beam.from, beam.to, 0.0f, true,
                beam.age + seconds);
            for (auto i = first; i < into.size(); ++i) into[i].age = beam.age;
            beam.age += seconds;
            beam.remaining -= seconds;
        }
        // Half a tick of slack so 0.3 s at 0.1 s ticks draws three strips, not a fourth
        // from floating-point residue.
        std::erase_if(state->beams, [&](const CombatBeam& beam) {
            return beam.remaining < seconds * 0.5f;
        });
    }
}

} // namespace rm
