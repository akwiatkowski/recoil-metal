#pragma once

#include "core/scene/UnitPlacement.hpp"
#include "core/sim/Army.hpp"
#include "core/sim/Movement.hpp"
#include "core/unit/UnitDef.hpp"

#include <array>
#include <optional>
#include <cstddef>
#include <span>
#include <vector>

namespace rm::sim {

// The first thing in this engine that can take something away.
//
// Everything here is grid- and vector arithmetic on the fixed tick, so it lives in
// core/ with the rest of the sim and is tested rather than watched. What it gets wrong
// is invisible in the good case: a falloff curve that is slightly wrong still kills
// things, just not the right ones, and a reload measured in the wrong unit reads as a
// balance complaint rather than as a bug.
//
// Read as the specification, cited rather than guessed at:
// `mohodata/lua/sim/defaultweapons.lua` for the firing cycle, `lua/sim/Unit.lua` for
// what damage does when it arrives.

/// A unit's place in the scene: which batch, and which instance of it.
///
/// The same shape as a SelectionEntry and deliberately not the same type — a selection
/// is what the mouse points at and this is what a gun points at, and conflating them
/// would let a click order a shot.
struct UnitRef {
    std::size_t batch = 0;
    std::size_t instance = 0;

    [[nodiscard]] friend bool operator==(const UnitRef&, const UnitRef&) noexcept = default;
};

/// What a unit can still take. One per instance, parallel to the instances themselves —
/// same reason MoveState is: UnitInstance's layout is pinned and read by the shader.
struct Health {
    float current = 0.0f;
    float maximum = 0.0f;

    /// Ticks until this weapon may fire again, one entry per weapon on the unit.
    ///
    /// Held with health rather than with the weapon definition because a DEFINITION is
    /// shared by every unit of that type — a hundred tanks have one blueprint and a
    /// hundred separate reloads. Getting this wrong makes a squad fire in perfect
    /// unison, which is the same class of mistake as a batch sharing one animation
    /// clock.
    std::vector<int> reloadRemaining;

    [[nodiscard]] bool alive() const noexcept { return current > 0.0f; }
};

/// One shot in flight.
struct Projectile {
    std::array<float, 3> position{};
    std::array<float, 3> velocity{};  ///< elmos per second

    float damage = 0.0f;
    float damageRadiusElmos = 0.0f;

    /// Who fired it, so a shot cannot kill its own side — checked at impact rather than
    /// at launch, because a unit may change hands between the two.
    int firedByArmy = kNoArmy;

    /// Whether it arcs. A flat shot travels in a straight line; an arced one is pulled
    /// down by gravity, which is what makes it clear a hill.
    unitdef::BallisticArc arc = unitdef::BallisticArc::None;

    /// Ticks before it gives up and expires.
    ///
    /// A projectile that misses must not live forever: nothing here despawns on leaving
    /// the map, and an accumulating list of shots that will never land is a leak with a
    /// frame-rate symptom.
    int ticksRemaining = 0;
};

/// Gravity applied to an arced shot, in elmos per second squared.
///
/// Supreme Commander's own, and stated rather than derived: the engine uses 4.9 ogrids
/// per second squared for its ballistics, which is Earth's 9.8 halved — a game constant
/// dressed as a physical one. Converted to elmos here like everything else.
inline constexpr float kProjectileGravityElmosPerSecond2 = 4.9f * 8.0f;

/// How far above a unit's own position its muzzle sits, in elmos.
///
/// A unit's position is at its FEET — that is what the terrain-hugging code puts there —
/// and a gun is not. Without this a flat shot fired across flat ground starts at exactly
/// ground height and detonates on the tick it is fired, at the muzzle, which reads as a
/// weapon that does no damage rather than as a missing offset.
///
/// Four elmos is half a heightmap square, about right for the corpus: a medium tank's
/// collision box is 0.55 ogrids tall, or 4.4 elmos. A per-unit muzzle bone would be
/// better and is what the blueprints' `RackBones` are for; that waits for turret aiming.
inline constexpr float kMuzzleHeightElmos = 4.0f;

/// How long a shot may live, in ticks, before it expires unspent.
///
/// Thirty seconds at 10 Hz. Long enough that the slowest projectile in the corpus (a
/// muzzle velocity of a few ogrids per second) crosses its own maximum range, and short
/// enough that a missed shot is forgotten within a plausible engagement.
inline constexpr int kProjectileLifetimeTicks = 300;

/// The distance from `from` to `to`, ignoring height.
///
/// Ground distance, because a weapon's range is a footprint on the map rather than a
/// sphere: a unit on a cliff is not out of range of the one below it, and using the
/// 3-D distance would make high ground a stealth field.
[[nodiscard]] float groundDistanceElmos(std::array<float, 3> from,
                                        std::array<float, 3> to) noexcept;

/// One batch's worth of units, for the combat pass.
///
/// Spans rather than a copy, because the pass writes health back and copying it out and
/// in again would be both slower and a chance to lose a kill.
struct CombatGroup {
    std::span<const UnitInstance> instances;
    std::span<const MoveState> motion;
    std::span<Health> health;

    /// The definition every unit in this group shares. Null for a group with no
    /// definition — a decorative batch — which therefore never fires and never dies.
    const unitdef::UnitDef* def = nullptr;
};

/// The nearest unit `shooter` may shoot, or nothing.
///
/// NEAREST rather than weakest or most dangerous: a target priority list is a game
/// design decision and this milestone does not make one. Ties break on the lower batch
/// then the lower instance, so the same scene always picks the same target and a
/// screenshot proves something twice.
///
/// Excludes the shooter itself, its allies, anything already dead, and anything a
/// defeated army owns (see `hostile`). Range is checked against the WEAPON, so a unit
/// with a long gun and a short one may find a target for the first and not the second.
[[nodiscard]] std::optional<UnitRef> nearestTarget(std::array<float, 3> from, int fromArmy,
                                                   const unitdef::Weapon& weapon,
                                                   std::span<const CombatGroup> groups,
                                                   std::span<const Army> armies);

/// Advances reloads, picks targets, and appends the shots fired this tick.
///
/// Returns how many shots were fired, which is what a caller reports and a test asserts
/// without reaching into the projectile list.
///
/// Firing is deliberately NOT gated on facing: a turreted weapon may fire while its
/// hull points elsewhere (284 of the 399 weapons that say so are turreted), and this
/// engine does not aim turrets yet, so gating on the hull's yaw would leave two thirds
/// of the corpus unable to shoot at all. The consequence is that an unturreted weapon
/// also fires sideways, which is wrong and visible, and is the next thing to fix.
std::size_t fireWeapons(std::span<const CombatGroup> groups, std::span<const Army> armies,
                        std::vector<Projectile>& projectiles);

/// Moves every projectile one tick, applies what lands, and removes what is spent.
///
/// A shot lands when it reaches its target's ground position or its height falls to the
/// ground — not when it collides with a model, because instances are points here and
/// their geometry is neither known nor cheap to test.
void advanceProjectiles(std::vector<Projectile>& projectiles, std::span<CombatGroup> groups,
                        std::span<const Army> armies, const HeightField& field);

/// Builds the shot a weapon fires from `from` at `to`.
///
/// An ARCED shot's vertical velocity is solved in closed form rather than iterated: the
/// flight time follows from the horizontal distance and the muzzle velocity, so the `vy`
/// that lands it on the target is one line of algebra. A muzzle velocity of zero — 111 of
/// the 494 weapons — is given a speed that crosses its own range in a tick, so an
/// instantaneous weapon needs no separate code path and nothing divides by zero.
[[nodiscard]] Projectile launch(std::array<float, 3> from, std::array<float, 3> to,
                                const unitdef::Weapon& weapon, int byArmy);

/// Spreads `damage` over everything within `radiusElmos` of `centre`, and returns how
/// much was dealt in total.
///
/// LINEAR falloff from full at the centre to nothing at the rim, which is what
/// `lua/sim/Unit.lua`'s damage model does. A radius of zero damages only what is at the
/// centre, at full strength — the 222 weapons that state no radius are point hits
/// rather than weapons that cannot hurt anything.
float damageArea(std::array<float, 3> centre, float radiusElmos, float damage, int byArmy,
                 std::span<CombatGroup> groups, std::span<const Army> armies);

/// Which units died this tick, so a caller can leave wreckage and check for a defeat.
[[nodiscard]] std::vector<UnitRef> deadUnits(std::span<const CombatGroup> groups);

} // namespace rm::sim
