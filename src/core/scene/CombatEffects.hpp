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
#include "core/sim/Events.hpp"

#include <span>
#include <vector>

namespace rm {

/// Appends the particles `events` earn. Kinds other than WeaponFired and ProjectileImpact
/// earn nothing here — a death already has its wreck decal, and construction its ghost.
void emitCombatEffects(std::vector<Particle>& into, std::span<const sim::Event> events);

} // namespace rm
