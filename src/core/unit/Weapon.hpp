#pragma once

#include "core/lua/LuaValue.hpp"

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

struct Weapon {
    std::string label;  ///< the blueprint's own `Label`, for messages

    WeaponRole role = WeaponRole::Other;
    BallisticArc arc = BallisticArc::None;

    /// Damage a single shot does at the point of impact.
    float damage = 0.0f;

    /// Radius over which that damage is spread, in ELMOS, converted from the ogrids
    /// the file states. Zero means a point hit: 222 of the 494 weapons state no radius
    /// at all, and one states a negative one, which is read as zero rather than as an
    /// implosion.
    float damageRadiusElmos = 0.0f;

    /// How far the weapon reaches, in elmos. `MinRadius` is a dead zone inside which
    /// it cannot fire — 87 weapons have one, and without it a unit walks up to an
    /// artillery piece and stands in the one place it cannot be shot from.
    float maxRangeElmos = 0.0f;
    float minRangeElmos = 0.0f;

    /// Shots per second, as authored. Converted to ticks by `reloadTicks`.
    float rateOfFire = 0.0f;

    /// How fast a projectile leaves, in elmos per second. Zero for a weapon whose
    /// projectile is instant.
    float muzzleVelocityElmosPerSecond = 0.0f;

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
    float innerRingDamage = 0.0f;
    float innerRingRadiusElmos = 0.0f;
    float outerRingDamage = 0.0f;
    float outerRingRadiusElmos = 0.0f;

    /// Whether this weapon does its damage in rings rather than as a single blast.
    [[nodiscard]] bool hasRings() const noexcept {
        return innerRingDamage > 0.0f || outerRingDamage > 0.0f;
    }

    /// Whether the weapon has a turret. A turreted weapon may fire without the hull
    /// turning; 284 of the 399 that say so are turreted.
    bool turreted = false;

    /// Whether this weapon is one this engine fires at a target.
    ///
    /// The `Death` exclusion is the load-bearing one. A weapon with no range or no
    /// rate of fire is also excluded: both are things a gun must have, and a "weapon"
    /// lacking them is a table describing something else.
    [[nodiscard]] bool fires() const noexcept {
        return role != WeaponRole::Death && maxRangeElmos > 0.0f && rateOfFire > 0.0f
            && damage > 0.0f;
    }

    /// Whether this weapon does any damage at all, by either scheme. What a death explosion
    /// is asked, since a death explosion has no range and no rate of fire to have.
    [[nodiscard]] bool harmful() const noexcept { return damage > 0.0f || hasRings(); }

    /// Ticks between shots, never less than one.
    ///
    /// The game quantises this the same way and for the same reason: a sim with a tick
    /// cannot fire between two of them, so a rate faster than the tick rate becomes one
    /// shot per tick rather than a fractional shot. At 10 Hz the fastest weapon in the
    /// corpus (10 shots/second) is exactly one shot per tick, which is presumably why
    /// that is the fastest weapon in the corpus.
    [[nodiscard]] int reloadTicks() const noexcept;
};

/// Reads a blueprint's `Weapon` array — a Lua array, so its entries are positional.
/// Empty for the 321 units that carry none.
[[nodiscard]] std::vector<Weapon> weaponsFrom(const lua::Value& weaponArray);

} // namespace rm::unitdef
