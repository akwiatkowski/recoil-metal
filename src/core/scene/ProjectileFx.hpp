#pragma once

// Shots in flight, made visible.
//
// The sim integrates every projectile honestly and nothing drew them — a fight read as
// muzzle flashes answering impact puffs with nothing crossing the gap. This is the gap's
// renderer, and the whole vocabulary is derived from what the shot already carries:
// `BallisticArc` says what it IS (High is artillery — the yellow dot Supreme Commander keeps
// visible from orbit), the damage profile says how much it matters, and neither needed a new
// sim field, which is why the state hash and the golden log never noticed this file.
//
// TWO KINDS OF GEOMETRY, on two clocks:
//
//   DOTS AND TRACERS are rebuilt every frame from the live projectile list, extrapolated by
//   the frame's tick fraction so a 10 Hz sim glides at display rate. They ride the same
//   scratch the strategic icons do and never age.
//
//   TRAILS persist and fade, so they are EMITTED — once per TICK, not per frame, which makes
//   the emission rate the sim's own and the spacing one tick of travel. An artillery arc
//   paints itself out of its own history; no per-shot identity is needed, which matters
//   because the projectile list compacts on impact and has none to offer.

#include "core/scene/Particles.hpp"
#include "core/scene/WeaponVisuals.hpp"
#include "core/sim/Combat.hpp"

#include <span>
#include <vector>

namespace rm {

/// The screen-size floors, in points against `kIconReferenceHeightPoints` — the strategic
/// icons' own convention. A tracer may thin to a glint; ARTILLERY NEVER DISAPPEARS: the
/// high-arc floor is what keeps incoming fire readable at full-map zoom, which is the
/// Supreme Commander rule this file exists to copy.
inline constexpr float kTracerFloorPoints = 1.5f;
inline constexpr float kLobFloorPoints = 2.0f;
inline constexpr float kArtilleryFloorPoints = 3.5f;

/// Appends this frame's shots: a velocity-aligned tracer dash for flat fire, a warm dot for
/// a low lob, the bright amber dot for artillery. `alpha` is the frame's fraction into the
/// next tick (TickClock::alpha) — positions are extrapolated by `velocity * alpha`, which is
/// at most one tick of flight and what turns bead-stepping into glide. `elmosPerPoint` is
/// the camera's, measured against the icon reference height.
void appendProjectiles(std::vector<Particle>& into, std::span<const sim::Projectile> shots,
                       float alpha, float elmosPerPoint, const WeaponVisuals* visuals = nullptr);

/// Emits one trail puff per ARCED shot — called once per advanced tick, from the tick loop.
/// Flat fire gets no trail: a tracer's shape is its streak, and a thousand rifle rounds
/// smoking would bury the arcs the trail exists to show.
void emitProjectileTrails(std::vector<Particle>& into, std::span<const sim::Projectile> shots,
    const WeaponVisuals* visuals = nullptr, float secondsPerTick = 0.1f);

} // namespace rm
