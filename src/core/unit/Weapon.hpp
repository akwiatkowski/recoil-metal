#pragma once

#include "core/sim/TickRate.hpp"

#include "core/lua/LuaValue.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rm::unitdef {

// What a unit shoots with.
//
// A blueprint's `Weapon` table is a Lua ARRAY, so a unit may have several — 247 of the
// 568 shipped units do, 494 weapons between them. Only the fields this engine can act
// on are read, on the same rule the rest of UnitDef follows.
//
// Spec read alongside the blueprints, cited rather than guessed at:
// `mohodata/lua/sim/defaultweapons.lua` for the firing cycle and
// `lua/sim/Unit.lua` for what damage does when it lands.

/// How a projectile travels, from `BallisticArc`.
///
/// Across the 393 weapons that state it: `RULEUBA_None` 294, `RULEUBA_LowArc` 83,
/// `RULEUBA_HighArc` 16. The distinction is not cosmetic — a flat shot needs line of
/// sight and an arced one does not, which is the whole of what artillery is for.
enum class BallisticArc : std::uint8_t {
    None,  ///< flat, straight at the target
    Low,   ///< a shallow lob
    High,  ///< artillery: up and over
};

[[nodiscard]] std::optional<BallisticArc> ballisticArcFromName(std::string_view name) noexcept;

/// What a weapon is FOR, from `WeaponCategory`.
///
/// Only enough of the twelve to decide whether this engine fires it. The important
/// one is `Death`: 99 of the 494 "weapons" are a unit's death explosion, which has no
/// target and no rate of fire and must never be aimed at anything. Reading it as a gun
/// would give a hundred units an unmissable, unreloadable cannon.
enum class WeaponRole : std::uint8_t {
    DirectFire,  ///< the ordinary case, and everything this milestone shoots
    AntiAir,     ///< read, not fired: nothing flies here yet
    Artillery,   ///< read, not fired: needs the high arc to mean something
    Death,       ///< the unit's own destruction. NOT a weapon that fires
    Other,       ///< bombs, torpedoes, kamikaze — read and ignored
};

[[nodiscard]] WeaponRole weaponRoleFromCategory(std::string_view category) noexcept;

/// Which of the sim's two movement layers a weapon may damage.
///
/// FA states richer Land/Water/Seabed/Air caps. This engine currently has one airborne layer
/// and one surface layer, so the three surface names deliberately collapse into one bit.
enum class TargetLayerMask : std::uint8_t {
    None = 0,
    Surface = 1,
    Air = 2,
    Both = 3,
};

/// The aiming cone a weapon gets when its blueprint states none.
///
/// Ten degrees: loose enough that an unstated tolerance never becomes a weapon that cannot
/// fire, tight enough that it is still an aim. The corpus's own values run 0 to 360 with a
/// mode of 2, so this is deliberately on the permissive side of typical — a missing field
/// should not be stricter than a stated one.
inline constexpr float kDefaultFiringToleranceDegrees = 10.0f;

/// Ten degrees in binary radians — `lround(10 * 65536 / 360)` — the value the default
/// converts to, kept as a constant so the field below needs no load-time work when the
/// blueprint states nothing.
inline constexpr std::int32_t kDefaultFiringToleranceBrads = 1820;

/// Degrees → binary radians, at LOAD time. A full turn is 65,536 brad, so a degree is
/// 65,536/360 = 182.04; rounded rather than truncated. This is the one place the conversion
/// runs: content converts once through the loader, and the sim compares integers — the same
/// rule every other authored angle follows.
[[nodiscard]] inline std::int32_t firingToleranceBradsFromDegrees(float degrees) noexcept {
    return static_cast<std::int32_t>(
        std::lround(std::max(0.0f, degrees) * (65536.0 / 360.0)));
}

struct Weapon {
    std::string label;  ///< the blueprint's own `Label`, for messages

    WeaponRole role = WeaponRole::Other;
    BallisticArc arc = BallisticArc::None;

    /// Unrestricted when content states no caps, preserving synthetic and Recoil weapons.
    TargetLayerMask targetLayers = TargetLayerMask::Both;

    [[nodiscard]] bool canTarget(bool airborne) const noexcept {
        const std::uint8_t wanted = static_cast<std::uint8_t>(
            airborne ? TargetLayerMask::Air : TargetLayerMask::Surface);
        return (static_cast<std::uint8_t>(targetLayers) & wanted) != 0;
    }

    /// Damage a single shot does at the point of impact.
    /// FIXED POINT (`Mag`), converted at parse time — which is the legitimate float boundary
    /// (PLAN2.md §5.1: content converts exactly once, at load, and the sim never sees the
    /// float again). `Mag` rather than `Fx` because `Damage` reaches 15,000 in the corpus and
    /// the ring damages of a commander's death blast are larger still.
    sim::Mag damage{};

    /// WHAT KIND of damage, from `DamageType` — the key into `armordefinition.lua`'s matrix
    /// (PLAN2.md §7 P10.1, `ADR-033`).
    ///
    /// Distinct values in the corpus (`02 §9.6`): `Normal` 454, `DeathExplosion` 41, `Nuke` 12,
    /// `Overcharge` 10, `Deathnuke` 4, `EMP` 2, `TacticalMissile` 1, `FireBeetleExplosion` 1,
    /// `CzarBeam` 1. Only six of the twenty valid types appear in the multiplier table at all,
    /// and `Normal` is 1.0 against every class — which is why flattening leaves 478 of the 494
    /// weapons with the bare `damage` above and no overrides.
    ///
    /// **A NAME, resolved in `sim::UnitCatalog`**, for the reason `UnitDef::armorType` gives:
    /// `weaponsFrom` takes a Lua array and nothing else, and it should keep being callable that
    /// way. Empty is read as `Normal`, which is what a blueprint stating nothing means.
    ///
    /// BAR content has no equivalent field — a Recoil weapon def states absolute damage per
    /// armour class directly — so a BAR import leaves this empty and builds the profile from
    /// its own table instead of through the matrix.
    std::string damageType;

    /// Radius over which that damage is spread, in ELMOS, converted from the ogrids
    /// the file states. Zero means a point hit: 222 of the 494 weapons state no radius
    /// at all, and one states a negative one, which is read as zero rather than as an
    /// implosion.
    sim::Fx damageRadius{};

    /// How far the weapon reaches, in elmos. `MinRadius` is a dead zone inside which
    /// it cannot fire — 87 weapons have one, and without it a unit walks up to an
    /// artillery piece and stands in the one place it cannot be shot from.
    sim::Fx maxRange{};
    sim::Fx minRange{};

    /// Shots per second, as authored. Converted to ticks by `reloadTicks`.
    ///
    /// NOT put through `faWaitSeconds`, and that is deliberate rather than an omission:
    /// `11 §3.6` puts `RateOfFire` on the **engine** clock (`engine/Sim/UnitWeapon.lua:150`),
    /// not through a Lua coroutine, so it carries no `WaitSeconds` bias. Correcting it would
    /// make every weapon in the game 100 ms slow.
    float rateOfFire = 0.0f;

    // A BURST, which is how a third of the armed corpus actually delivers its damage.
    //
    // `MuzzleSalvoSize` shots leave `MuzzleSalvoDelay` apart, and only then does the weapon
    // reload. Read one shot per reload instead and a burst weapon does a fraction of its real
    // damage: `11 §5` puts the error at up to 50%.
    //
    // Measured over the 568 shipped blueprints: 402 weapons state both fields, 98 state a
    // delay above zero, and 106 state a salvo size above one. The delay's values are
    // 0.05, 0.1, 0.2, 0.25, 0.3, 0.33, 0.4, 0.5, 0.6, 0.8, 1.0, 1.5, 1.8 and 2.3 seconds.

    /// How many shots one trigger-pull delivers. One for an ordinary weapon.
    ///
    /// Clamped at one rather than trusted: a stated zero would mean a weapon that fires
    /// nothing, which is a table describing something else.
    int burstSize = 1;

    /// The gap between shots WITHIN a burst — **corrected** by `faWaitSeconds`, because this
    /// one is Lua-timed (`DefaultProjectileWeapon.lua:1134`).
    ///
    /// Zero means no gap and no burst, which is what the game means too: it guards the wait
    /// with `if salvoDelay > 0` at `:1130`, so a stated zero fires every muzzle inside one tick
    /// rather than waiting a quantum. That guard is why the correction is applied at the parse
    /// site and not inside `faWaitSeconds`.
    sim::Seconds burstDelay{};

    /// Whether this weapon delivers its shots as a burst rather than one at a time.
    ///
    /// BOTH fields are required. 106 weapons state a size above one while stating no delay —
    /// those fire their muzzles simultaneously (`11 §3.3`: `salvoDelay == 0` sets
    /// `numMuzzlesFiring = muzzleBoneCount`), which is a different mechanic from a burst spread
    /// over time and is not what this models.
    [[nodiscard]] bool bursts() const noexcept {
        return burstSize > 1 && burstDelay.value > 0.0f;
    }

    /// How fast a projectile leaves, in elmos per second. Zero for a weapon whose
    /// projectile is instant.
    float muzzleVelocityElmosPerSecond = 0.0f;

    /// The bone the shot leaves from — `TurretBoneMuzzle`, or the rack's first muzzle
    /// bone when no turret names one. A NAME, because the blueprint knows the name and
    /// only the MODEL knows where that bone is; the app resolves it at load.
    std::string muzzleBone;

    /// The resolved muzzle height in elmos, filled by the app from the model's skeleton
    /// (bone global offset × mesh scale). Zero means unresolved, and the sim falls back
    /// to its old constant — every unit firing from four elmos up, which is what this
    /// field exists to retire.
    sim::Fx muzzleHeight{};

    /// A BEAM: damage arrives the instant the weapon fires, nothing flies. From
    /// `BeamLifetime` (a pulse that exists for a fraction of a second) or `ContinuousBeam`
    /// — either way the corpus's beam weapons never spawn a projectile, and modelling them
    /// as fast bullets both misses (the sim would integrate a flight) and reads wrong (a
    /// laser is a LINE). One pulse per trigger-pull at the weapon's own rate of fire.
    bool beam = false;

    // A TWO-RING BLAST, for the weapons that state one instead of a plain damage figure.
    //
    // Five of the 494 do, and they are the ones that matter most: the four commanders' death
    // explosions, which state `NukeInnerRingDamage = 45000` over 30 ogrids and 5000 over 40
    // rather than `Damage` at all. Read only `Damage` and an ACU detonates for nothing — and
    // an ACU detonating for 45000 over 240 elmos, enough to take any other commander with
    // it, is the single most characteristic event in the game.
    //
    // Held as two rings rather than flattened into one radius and one number because they
    // are genuinely two: a near-total kill zone and a wide fringe, and averaging them would
    // both spare what should die and spread damage where the game puts none.
    sim::Mag innerRingDamage{};
    sim::Fx innerRingRadius{};
    sim::Mag outerRingDamage{};
    sim::Fx outerRingRadius{};

    /// Whether this weapon does its damage in rings rather than as a single blast.
    [[nodiscard]] bool hasRings() const noexcept {
        return innerRingDamage > sim::Mag{} || outerRingDamage > sim::Mag{};
    }

    /// The firing arc, in DEGREES, and its centre — retail's `HeadingArcCenter`/`Range`.
    ///
    /// `arcRangeDegrees >= 180` means unrestricted, and the engine skips the test entirely in
    /// that case rather than comparing against a half-turn (`C-167`); 180 is also the schema's
    /// documented default, so a weapon stating nothing is unrestricted. The value is a HALF
    /// angle either side of the centre.
    float arcCentreDegrees = 0.0f;
    float arcRangeDegrees = 180.0f;

    /// The largest height difference the weapon can reach across, in elmos.
    ///
    /// Retail folds this into the same "cannot reach" class as being out of range, so a
    /// weapon that cannot elevate treats a target above it as unreachable rather than as
    /// merely distant (`C-167`). Zero means unlimited: no shipped weapon states 0, and a
    /// literal zero would make every weapon unable to shoot anything not exactly level.
    sim::Fx maxHeightDifference{};

    /// What this weapon prefers to shoot, most-wanted first — retail's `TargetPriorities`.
    ///
    /// **An EMPTY list means acquire NOTHING**, not "anything" (`C-156`). That is verified
    /// against all 494 shipped weapons: the 157 without the key are every one of them on a
    /// path that does not use priority acquisition — death weapons, anti-projectile, manual
    /// fire, explicit-order missiles. So the restrictive reading is safe only because those
    /// paths exist, and a weapon that reaches here with no priorities genuinely stays silent.
    ///
    /// The INDEX is the rank and it sorts lexicographically ahead of the score (`C-157`): a
    /// row-0 match beats a row-1 match however far away it is. That is the one term of
    /// retail's ordering that is a true sort key rather than a score adjustment.
    /// Spelled out rather than using `CategoryExpression`, because that alias lives in
    /// `BuildTree.hpp`, which needs `UnitDef`, which includes this file. The type is
    /// identical — `vector<vector<string>>`, outer index the priority rank, inner a term
    /// whose tags are ANDed — and `matchesExpression` accepts it directly.
    std::vector<std::vector<std::string>> targetPriorities;

    /// Whether the weapon has a turret. A turreted weapon may fire without the hull
    /// turning; 284 of the 399 that say so are turreted.
    bool turreted = false;

    /// How far off the aim may be and still fire, in BINARY RADIANS (a full turn is
    /// 65,536). 457 weapons state one, and 279 of those say 2 degrees — a tight cone.
    ///
    /// This is what stops an unturreted weapon shooting sideways: with no turret the hull's
    /// own facing IS the aim, so the unit has to be pointing at what it shoots. A weapon that
    /// states none gets a generous default rather than zero, since zero would be a weapon
    /// that can never fire at all. Converted once at load by
    /// `firingToleranceBradsFromDegrees`, because the tick has no business running float math
    /// per shot to re-derive a number that never changes.
    std::int32_t firingToleranceBrads = kDefaultFiringToleranceBrads;

    /// `ManualFire = true`: the game fires this only on an explicit order — the
    /// commander's 12000-damage OverCharge is the reason the flag exists. The order
    /// exists now: `CommandKind::Overcharge`, one shot per click, gated on the energy
    /// below. 12 of the 494 state it.
    bool manualFire = false;

    /// `EnergyRequired`: what one manual shot costs, drained from the army's store the
    /// tick it fires — 5000 for every faction's OverCharge. Zero for the ordinary guns,
    /// whose blueprint states the field only on charged weapons.
    sim::Mag energyRequired{};

    /// `EnabledByEnhancement = '...'`: the weapon belongs to an upgrade the unit does
    /// not have until built. Enhancements do not exist in this engine, so neither does
    /// the weapon — the commander's TacMissile states a 2048-elmo range, and reading it
    /// as a gun had the commander sniping whole tank columns from a fifth of the map
    /// away before anything reached it.
    bool enabledByEnhancement = false;

    /// Whether this weapon is one this engine fires at a target.
    ///
    /// The `Death` exclusion is the load-bearing one; manual and enhancement-gated
    /// weapons wait for orders and upgrades that never come here. A weapon with no
    /// range or no rate of fire is also excluded: both are things a gun must have, and
    /// a "weapon" lacking them is a table describing something else.
    [[nodiscard]] bool fires() const noexcept {
        return role != WeaponRole::Death && !manualFire && !enabledByEnhancement
            && maxRange > sim::Fx{} && rateOfFire > 0.0f && damage > sim::Mag{};
    }

    /// Whether this weapon does any damage at all, by either scheme. What a death explosion
    /// is asked, since a death explosion has no range and no rate of fire to have.
    [[nodiscard]] bool harmful() const noexcept {
        return damage > sim::Mag{} || hasRings();
    }

    /// Whether this is a weapon an explicit order fires — OverCharge, in practice.
    ///
    /// `fires()`'s deliberate complement: manual, NOT enhancement-gated (the commander's
    /// TacMissile is both flags at once and belongs to an upgrade that does not exist
    /// here), and otherwise a real gun by the same tests.
    [[nodiscard]] bool manuallyFired() const noexcept {
        return role != WeaponRole::Death && manualFire && !enabledByEnhancement
            && maxRange > sim::Fx{} && damage > sim::Mag{};
    }

    /// Ticks between shots at a given rate, never less than one.
    ///
    /// The rate is a PARAMETER. `rateOfFire` is authored in shots per second, which is a fact
    /// about the weapon; how many ticks that is depends on the clock, which is a fact about
    /// the sim (PLAN2.md §5.1). This function was already the correct shape — the plan cites
    /// it as the pattern to generalise — it just used to read the rate from a constant.
    ///
    /// The game quantises this the same way and for the same reason: a sim with a tick
    /// cannot fire between two of them, so a rate faster than the tick rate becomes one
    /// shot per tick rather than a fractional shot. At 10 Hz the fastest weapon in the
    /// corpus (10 shots/second) is exactly one shot per tick, which is presumably why
    /// that is the fastest weapon in the corpus.
    [[nodiscard]] int reloadTicks(sim::TickRate rate = sim::TickRate{}) const noexcept;
};

/// Reads a blueprint's `Weapon` array — a Lua array, so its entries are positional.
/// Empty for the 321 units that carry none.
[[nodiscard]] std::vector<Weapon> weaponsFrom(const lua::Value& weaponArray,
                                               bool airborneSource = false);

} // namespace rm::unitdef
