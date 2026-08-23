#pragma once

#include "core/map/HeightField.hpp"
#include "core/sim/Economy.hpp"
#include "core/sim/Events.hpp"
#include "core/sim/Army.hpp"
#include "core/sim/Health.hpp"
#include "core/sim/Intel.hpp"
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
    std::array<Fx, 3> position{};

    /// Elmos PER TICK, not per second (§5.1). The advance pass adds it straight to the
    /// position with no scaling, which is what "per tick" buys.
    std::array<Fx, 3> velocity{};

    /// The whole damage table, not one number (PLAN2.md §7 P10.1, `ADR-033`).
    ///
    /// CARRIED ON THE SHOT rather than looked up at impact, which is also what Recoil does — its
    /// projectiles hold a `DamageArray` by value. The alternative was a `(UnitTypeIndex, slot)`
    /// pair pointing back at the catalog, and it is smaller; it was rejected because a shot
    /// outlives its shooter routinely (a duel where both sides die on the same tick is ordinary),
    /// and a shot whose damage depends on a still-resolvable owner is a shot that changes value
    /// when the owner dies.
    ///
    /// It costs 72 bytes against the previous 8. Still fixed-size and trivially copyable, which
    /// is the property that matters: the state hash walks this.
    unitdef::DamageProfile damage{};
    Fx damageRadiusElmos{};
    unitdef::TargetLayerMask targetLayers = unitdef::TargetLayerMask::Both;

    /// WHICH UNIT fired it, for the `UnitDamaged`/`UnitDestroyed` events a hit produces
    /// (§7 P6.1). May be stale by the time the shot lands — a duel where both sides die on the
    /// same tick is ordinary — so it is a name for the shooter rather than a way back to one.
    UnitId firedBy{};

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
/// Authored per second squared. `constexpr float` is allowed in the sim precisely because a
/// constant cannot vary between platforms — see `tools/check_no_sim_floats.sh`, which bans
/// float variables, parameters and returns but not authored constants. Converted through the
/// tick rate at the point of use.
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
inline constexpr Fx kMuzzleHeight = Fx::fromInt(4);

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
[[nodiscard]] Fx groundDistanceElmos(std::array<Fx, 3> from, std::array<Fx, 3> to) noexcept;

/// A transform's position as a triple, for the geometry functions that take one.
///
/// A free function rather than a member of `Transform`, because a position is what several
/// things have — a shot, a construction site, a waypoint — and only one of them has a facing.
[[nodiscard]] std::array<Fx, 3> positionOf(const Transform& transform) noexcept;

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
///
/// AND ANYTHING THE SHOOTER'S SIDE CANNOT SEE (ADR-037). Until intel existed this pass
/// picked from the whole store filtered by hostility and range, so every unit in the match
/// shot at things across the map it had no way of knowing were there. `intel` may be null,
/// which restores exactly that behaviour and is what a scene with no fog of war gets.
///
/// SIGHT, NOT RADAR. A radar contact is a position without an identity, and what to do with
/// one is the contact rules rather than a target-selection question — see `IntelKind`. So a
/// unit lit only by radar is not auto-attacked, deliberately, and that is a choice this
/// engine is making rather than one it inherited.
[[nodiscard]] std::optional<UnitId> nearestTarget(std::array<Fx, 3> from, int fromArmy,
                                                  const unitdef::Weapon& weapon,
                                                  const UnitStore& store,
                                                  std::span<const Army> armies,
                                                  const Intel* intel = nullptr);

/// The bearing from `from` to `to`, in radians, measured the way a unit's yaw is.
///
/// `atan2(dx, dz)`, NOT `atan2(dz, dx)`: yaw here is measured from +Z toward +X because that
/// is what the vertex shader does with it (AGENT.md). Swapping the arguments compiles, runs,
/// and points every turret ninety degrees off.
[[nodiscard]] Brad bearingTo(std::array<Fx, 3> from, std::array<Fx, 3> to) noexcept;

/// The smaller angle between two headings, in radians. Always 0..pi.
/// The shortest way round, as a MAGNITUDE in binary radians. Unsigned because every caller
/// wants "how far off", not "which way": the sign is what `shortestTurn` in Movement.cpp is
/// for.
[[nodiscard]] std::uint32_t headingError(Brad from, Brad to) noexcept;

/// Whether a weapon on a unit facing `yaw` may fire at something on `bearing`.
///
/// A TURRETED weapon always may: it aims independently of the hull, and 284 of the 399
/// weapons that say either way are turreted. This engine does not animate turrets, so
/// pretending otherwise would leave two thirds of the corpus unable to shoot.
///
/// An UNTURRETED one may only when the hull is pointing at the target, within the weapon's
/// own `FiringTolerance`. That is the fix for a tank shooting sideways out of its hull.
[[nodiscard]] bool canFireAt(const unitdef::Weapon& weapon, Brad yaw, Brad bearing) noexcept;

/// Turns idle units toward what they would shoot, at their own turn rate.
///
/// Only units that are NOT moving, and only those whose weapons need the hull pointed —
/// a turreted unit has no reason to turn and a moving one is already being turned by its
/// order. Without this an unturreted unit that happens to stop facing the wrong way can
/// never fire at all, so the facing gate above would read as a weapon that does not work.
///
/// Writes to the instances' yaw, which is why the groups are mutable here and const in
/// `fireWeapons`.
/// `intel` gates what may be aimed at, exactly as it gates what may be shot — a hull
/// swinging round to face something its side cannot see would give the position away.
std::size_t aimAtTargets(UnitStore& store, const UnitCatalog& catalog,
                         std::span<const Army> armies, const Intel* intel = nullptr);

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
                        std::vector<Projectile>& projectiles, TickRate rate,
                        EventQueue* events = nullptr, const Intel* intel = nullptr);

/// Fires every held OVERCHARGE whose moment has come: target alive, in the manual
/// weapon's range, reload ready, and the army's stored energy covering the shot's
/// `EnergyRequired` — drained the tick it fires, which is the mechanic (the blueprint's
/// 5000 against a store the player watches). One shot retires the order by FORGETTING
/// ITS TARGET on the head command, which `advanceOrders` then completes like any
/// arrival — one click, one shot, whether or not the target survives it.
///
/// A short energy bar HOLDS rather than fails: the unit stands at range until the store
/// fills, which is what the game's own queued overcharge does. Returns shots fired.
/// No facing gate: every ManualFire weapon in the corpus is turreted, and turrets aim
/// independently here (`fireWeapons`' rule, applied to the same hardware).
std::size_t fireOvercharge(UnitStore& store, const UnitCatalog& catalog,
                           std::span<const Army> armies,
                           std::vector<Projectile>& projectiles,
                           std::span<Economy> economies, TickRate rate,
                           EventQueue* events = nullptr);

/// Advances partial regeneration and damage-collapse recovery for ordinary bubbles.
void tickShields(UnitStore& store, const UnitCatalog& catalog,
                 EventQueue* events = nullptr);

/// Moves every projectile one tick, applies what lands, and removes what is spent.
///
/// A shot lands when it reaches its target's ground position or its height falls to the
/// ground — not when it collides with a model, because instances are points here and
/// their geometry is neither known nor cheap to test.
/// `catalog` is how a target's armour class is discovered; null means every target is ordinary
/// armour, which is the pre-P10.1 behaviour and what a scene with no types should get.
void advanceProjectiles(std::vector<Projectile>& projectiles, UnitStore& store,
                        std::span<const Army> armies, const Terrain& terrain, TickRate rate,
                        EventQueue* events = nullptr, const UnitCatalog* catalog = nullptr);

/// Gravity's pull on an arced shot, in elmos per tick per tick.
///
/// A rate squared, so it divides by the tick rate TWICE — the kind of conversion that is
/// silently wrong when written by hand, which is why it is one named function used by both the
/// launch solution and the per-tick advance rather than two expressions that must agree.
[[nodiscard]] Fx projectileGravityPerTickSquared(TickRate rate) noexcept;

/// Builds the shot a weapon fires from `from` at `to`.
///
/// An ARCED shot's vertical velocity is solved in closed form rather than iterated: the
/// flight time follows from the horizontal distance and the muzzle velocity, so the `vy`
/// that lands it on the target is one line of algebra. A muzzle velocity of zero — 111 of
/// the 494 weapons — is given a speed that crosses its own range in a tick, so an
/// instantaneous weapon needs no separate code path and nothing divides by zero.
/// `muzzlePerTick` comes from the catalog rather than from the weapon: `MuzzleVelocity` is
/// authored per second, and only a clock turns that into a distance a shot covers in a tick.
/// `damage` is the weapon's resolved table. Passed in rather than read from `weapon.damage`
/// for the same reason `muzzlePerTick` is: the authored figure becomes a usable one only once
/// the match's armour classes are known, and that resolution belongs to `UnitCatalog`.
[[nodiscard]] Projectile launch(std::array<Fx, 3> from, std::array<Fx, 3> to,
                                const unitdef::Weapon& weapon, int byArmy, TickRate rate,
                                Fx muzzlePerTick, const unitdef::DamageProfile& damage,
                                UnitId firedBy = {});

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
/// `by` and `events` are OPTIONAL and trail the signature deliberately: they are how a hit
/// becomes a `UnitDamaged` event naming its instigator (§7 P6.1), and defaulting them keeps the
/// two dozen existing call sites — most of them tests asserting the falloff curve — unchanged.
/// A caller that passes neither gets exactly the old behaviour and no events.
Mag damageArea(std::array<Fx, 3> centre, Fx radiusElmos, Mag damage, int byArmy,
               UnitStore& store, std::span<const Army> armies, UnitId by = {},
               EventQueue* events = nullptr, const UnitCatalog* catalog = nullptr);

/// The same, with a weapon's full damage table rather than one number (PLAN2.md §7 P10.1,
/// `ADR-033`, D12).
///
/// **AN OVERLOAD RATHER THAN A REPLACEMENT**, and the scalar version above is not deprecated.
/// A blast's falloff is geometry and has nothing to say about armour, so the two dozen tests
/// that assert the curve are better off passing a number — and `flatDamage` makes the two
/// provably the same call, which is what keeps `make verify` a strict check across this change.
///
/// `catalog` is how a TARGET's armour class is discovered, and it may be null: a scene with no
/// catalog has no types, so every target is `kDefaultArmor` and a flat profile answers `base`
/// for it. That is the pre-P10.1 engine reproduced exactly rather than approximated.
///
/// The lookup happens per target inside the loop, because it genuinely varies within one blast
/// — a shell between a tank and a bunker hits two armour classes, which is the point.
Mag damageArea(std::array<Fx, 3> centre, Fx radiusElmos, const unitdef::DamageProfile& damage,
               int byArmy, UnitStore& store, std::span<const Army> armies,
               const UnitCatalog* catalog = nullptr, UnitId by = {},
               EventQueue* events = nullptr,
               unitdef::TargetLayerMask targetLayers = unitdef::TargetLayerMask::Both);

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
/// `catalog` supplies both halves: the target's armour class, and the death weapon's own damage
/// table, which it resolves on demand rather than from `weaponRates`. That split is deliberate —
/// firing is per-shot and hot, so it reads a profile computed once at load; a death blast happens
/// a few times a match, so recomputing costs nothing and saves threading a weapon index through
/// the death report.
Mag explodeOnDeath(const unitdef::UnitDef& def, std::array<Fx, 3> at, int byArmy,
                     UnitStore& store, std::span<const Army> armies, UnitId by = {},
                     EventQueue* events = nullptr, const UnitCatalog* catalog = nullptr);

/// Which units died this tick, so a caller can leave wreckage and check for a defeat.
[[nodiscard]] std::vector<UnitId> deadUnits(const UnitStore& store);

} // namespace rm::sim
