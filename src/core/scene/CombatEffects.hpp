#pragma once

// The tick's combat, as particles: a muzzle flash for every WeaponFired, an impact puff for
// every ProjectileImpact.
//
// EVENT-DRIVEN, which is what P6.3 asked for and the whole reason it is this small: the sim
// already says what happened and where, so an effect is a translation rather than a second
// simulation. The events are a per-tick notification (Events.hpp) — a consumer reads them
// during the tick or misses them — so the caller invokes this once per advanced tick, right
// where it advanced it.
//
// Pure over its inputs and tested without a renderer, like every other particle source: how
// many particles an event earns and what they carry is arithmetic; whether they LOOK like a
// shot landing is the screenshot's job.

#include "core/scene/Particles.hpp"
#include "core/scene/WeaponVisuals.hpp"
#include "core/sim/Events.hpp"

#include <functional>
#include <span>
#include <vector>

namespace rm {

struct CombatEffectBurst {
    std::uint32_t material = 0;
    std::array<float,3> position{};
    float age = 0;
    std::uint32_t seed = 1;
    float scale = 1;
    sim::UnitId owner{};
    std::string weapon;
    std::array<float,3> direction{};
    bool started = false;
};
/// A beam that is still being drawn. The firing event fixes its lifetime; the app moves
/// `from` to the shooter's muzzle and `to` to the target each tick, and every tick draws
/// one fresh strip between them. The strip's age keeps counting from the shot so a
/// scrolling beam texture runs continuously rather than restarting each tick.
struct CombatBeam {
    std::string weapon;          ///< UNIT:WeaponLabel, the visual definition key
    sim::UnitId owner{};
    sim::UnitId target{};
    std::array<float,3> from{};
    std::array<float,3> to{};
    float age = 0;               ///< seconds since the shot
    float remaining = 0;         ///< seconds still to draw
};
struct CombatEffectState {
    std::vector<CombatEffectBurst> bursts;
    std::vector<CombatBeam> beams;
    std::uint32_t seed = 1;
};

/// Appends the particles `events` earn: muzzle flashes and beams, impact smoke and
/// sparks, shield flashes, and death bursts scaled by the corpse's size. Kinds with
/// no visual (construction and the rest) earn nothing.
///
/// `unitRadius` answers a corpse's collision radius in elmos for UnitDestroyed; the
/// caller reads it from the dead slot, which outlives the unit. Absent, deaths fall
/// back to tank scale rather than skipping the burst — a silent death reads as a bug.
void emitCombatEffects(std::vector<Particle>& into, std::span<const sim::Event> events,
                       const WeaponVisuals* visuals = nullptr,
                       CombatEffectState* state = nullptr, float seconds = 0.1f,
                       std::function<float(sim::UnitId)> unitRadius = {});

} // namespace rm
