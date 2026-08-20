#pragma once

#include "core/scene/UnitPlacement.hpp"
#include "core/sim/Army.hpp"
#include "core/sim/Health.hpp"
#include "core/sim/TickRate.hpp"
#include "core/sim/UnitCatalog.hpp"
#include "core/sim/UnitStore.hpp"
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

// A unit is named by its handle now, not by where it is drawn. `UnitId` comes from
// IdPool.hpp; `UnitRef{batch, instance}` used to live here and named a draw call and a slot
// in its instance buffer, which is what PLAN2.md §1.1 called out as the root problem.

/// One shot in flight.
struct Projectile {
    std::array<float, 3> position{};
    std::array<float, 3> velocity{};  ///< elmos per second

    /// Fixed point, carried straight from the weapon that fired it.
    Mag damage{};
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

/// How long a shot may live before it expires unspent — AS A DURATION.
///
/// Long enough that the slowest projectile in the corpus (a muzzle velocity of a few ogrids
/// per second) crosses its own maximum range, and short enough that a missed shot is
/// forgotten within a plausible engagement.
///
/// This was `kProjectileLifetimeTicks = 300`, with "thirty seconds at 10 Hz" in a comment —
/// the constant PLAN2 §5.1 names first. At 20 Hz it would silently have become a
/// fifteen-second lifetime: nothing fails, no test goes red, the balance just changes.
/// Converted through a `TickRate` at launch, thirty seconds is thirty seconds.
inline constexpr Seconds kProjectileLifetime = Seconds{30.0f};

/// The distance from `from` to `to`, ignoring height.
///
/// Ground distance, because a weapon's range is a footprint on the map rather than a
/// sphere: a unit on a cliff is not out of range of the one below it, and using the
/// 3-D distance would make high ground a stealth field.
[[nodiscard]] float groundDistanceElmos(std::array<float, 3> from,
                                        std::array<float, 3> to) noexcept;

// `CombatGroup` used to live here: one batch's spans plus the one def its whole batch
// shared. That sharing is the only reason the passes below were ever per-batch — move the def
// behind a per-unit type (`UnitCatalog`) and the group has nothing left to be. The passes take
// the store and loop once over slots (PLAN2.md §7 P1.4).
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
[[nodiscard]] std::optional<UnitId> nearestTarget(std::array<float, 3> from, int fromArmy,
                                                  const unitdef::Weapon& weapon,
                                                  const UnitStore& store,
                                                  std::span<const Army> armies);

/// The bearing from `from` to `to`, in radians, measured the way a unit's yaw is.
///
/// `atan2(dx, dz)`, NOT `atan2(dz, dx)`: yaw here is measured from +Z toward +X because that
/// is what the vertex shader does with it (AGENT.md). Swapping the arguments compiles, runs,
/// and points every turret ninety degrees off.
[[nodiscard]] float bearingTo(std::array<float, 3> from, std::array<float, 3> to) noexcept;

/// The smaller angle between two headings, in radians. Always 0..pi.
[[nodiscard]] float headingError(float from, float to) noexcept;

/// Whether a weapon on a unit facing `yaw` may fire at something on `bearing`.
///
/// A TURRETED weapon always may: it aims independently of the hull, and 284 of the 399
/// weapons that say either way are turreted. This engine does not animate turrets, so
/// pretending otherwise would leave two thirds of the corpus unable to shoot.
///
/// An UNTURRETED one may only when the hull is pointing at the target, within the weapon's
/// own `FiringTolerance`. That is the fix for a tank shooting sideways out of its hull.
[[nodiscard]] bool canFireAt(const unitdef::Weapon& weapon, float yaw, float bearing) noexcept;

/// Turns idle units toward what they would shoot, at their own turn rate.
///
/// Only units that are NOT moving, and only those whose weapons need the hull pointed —
/// a turreted unit has no reason to turn and a moving one is already being turned by its
/// order. Without this an unturreted unit that happens to stop facing the wrong way can
/// never fire at all, so the facing gate above would read as a weapon that does not work.
///
/// Writes to the instances' yaw, which is why the groups are mutable here and const in
/// `fireWeapons`.
std::size_t aimAtTargets(UnitStore& store, const UnitCatalog& catalog,
                         std::span<const Army> armies);

/// Advances reloads, picks targets, and appends the shots fired this tick.
///
/// Returns how many shots were fired, which is what a caller reports and a test asserts
/// without reaching into the projectile list.
///
/// Firing IS gated on facing now, but only for the weapons that need it — see `canFireAt`.
/// A turreted weapon fires whatever the hull is doing, because it aims independently and
/// this engine does not animate turrets; an unturreted one has to be pointed at its target,
/// which is what stops a tank firing out of its side armour.
/// The rate is passed rather than read from a constant, because the shot it creates carries
/// a lifetime in ticks and that number is only meaningful against a rate (§5.1).
std::size_t fireWeapons(UnitStore& store, const UnitCatalog& catalog,
                        std::span<const Army> armies,
                        std::vector<Projectile>& projectiles, TickRate rate);

/// Moves every projectile one tick, applies what lands, and removes what is spent.
///
/// A shot lands when it reaches its target's ground position or its height falls to the
/// ground — not when it collides with a model, because instances are points here and
/// their geometry is neither known nor cheap to test.
void advanceProjectiles(std::vector<Projectile>& projectiles, UnitStore& store,
                        std::span<const Army> armies, const HeightField& field);

/// Builds the shot a weapon fires from `from` at `to`.
///
/// An ARCED shot's vertical velocity is solved in closed form rather than iterated: the
/// flight time follows from the horizontal distance and the muzzle velocity, so the `vy`
/// that lands it on the target is one line of algebra. A muzzle velocity of zero — 111 of
/// the 494 weapons — is given a speed that crosses its own range in a tick, so an
/// instantaneous weapon needs no separate code path and nothing divides by zero.
[[nodiscard]] Projectile launch(std::array<float, 3> from, std::array<float, 3> to,
                                const unitdef::Weapon& weapon, int byArmy, TickRate rate);

/// Spreads `damage` over everything within `radiusElmos` of `centre`, and returns how
/// much was dealt in total.
///
/// LINEAR falloff from full at the centre to nothing at the rim, which is what
/// `lua/sim/Unit.lua`'s damage model does. A radius of zero damages only what is at the
/// centre, at full strength — the 222 weapons that state no radius are point hits
/// rather than weapons that cannot hurt anything.
/// `damage` is a `Mag` because that is what health is; the FALLOFF is computed in `Fx`,
/// because a fraction of a blast radius is geometry. The two meet in one multiply, which is
/// the only place the types mix — and it is exact rather than a rescale, since both use the
/// same number of fractional bits.
Mag damageArea(std::array<float, 3> centre, float radiusElmos, Mag damage, int byArmy,
               UnitStore& store, std::span<const Army> armies);

/// The unit's own destruction, if its definition describes one.
///
/// 99 of the 494 shipped weapons are `WeaponCategory = 'Death'`: a blast with no target, no
/// range and no rate of fire, which `Weapon::fires()` correctly refuses to aim at anything.
/// This is where it finally goes off — and an ACU's is enormous, which is the single most
/// characteristic thing about Supreme Commander.
///
/// Returns null when the definition has none. Most units do have one.
[[nodiscard]] const unitdef::Weapon* deathWeapon(const unitdef::UnitDef& def) noexcept;

/// Sets off `def`'s death explosion at `at`, and returns the damage dealt.
///
/// Attributed to the DYING unit's own army, which has one consequence worth stating: it
/// hurts that army's enemies and not its allies. The game's death explosions hurt everything
/// nearby including friends, and this does not — `damageArea` takes an attacker and asks
/// `hostile`, so friendly fire would need a mode of its own rather than a different
/// argument. Noted rather than hidden: a commander detonating in a friendly crowd should be
/// a catastrophe and here it is merely an inconvenience.
Mag explodeOnDeath(const unitdef::UnitDef& def, std::array<float, 3> at, int byArmy,
                     UnitStore& store, std::span<const Army> armies);

/// Which units died this tick, so a caller can leave wreckage and check for a defeat.
[[nodiscard]] std::vector<UnitId> deadUnits(const UnitStore& store);

} // namespace rm::sim
