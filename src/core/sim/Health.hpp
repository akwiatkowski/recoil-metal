#pragma once

#include "core/sim/Fx.hpp"
#include "core/sim/IdPool.hpp"
#include "core/sim/Veterancy.hpp"

#include <vector>

namespace rm::sim {

struct ShieldState {
    Mag current{};
    Mag maximum{};
    TickCount regenDelayRemaining = 0;
    TickCount rechargeRemaining = 0;

    [[nodiscard]] bool active() const noexcept {
        return maximum > Mag{} && current > Mag{} && rechargeRemaining == 0;
    }
};

/// What a unit can still take. One per unit, parallel to the instances themselves —
/// same reason MoveState is: UnitInstance's layout is pinned and read by the shader.
///
/// Lives in its own header rather than in Combat.hpp because the unit store holds it and
/// Combat.hpp needs the store: the two would include each other. Splitting the type out is
/// the smaller fix, and it is where a reader would look for it anyway.
struct Health {
    /// FIXED POINT (`Mag`, Q50.14), not float — PLAN2.md §5.2, D1.
    ///
    /// `Mag` rather than `Fx` because health does not fit the geometric type: `MaxHealth`
    /// reaches 5,000,000 in the corpus (XSC9010) against `Fx`'s ceiling of 131,072. Measured
    /// across all 568 unit blueprints; see `core/Types.hpp`.
    ///
    /// The fraction matters as much as the range, though no longer for the reason first given
    /// here — that argument was blast falloff, which `C-061` established retail does not have.
    /// It still holds for armour multipliers (0.25 against an Overcharge, 0.032 against a
    /// Deathnuke), for shield absorption, and for regeneration accrued per tick, all of which
    /// produce fractional health that rounding to whole points would visibly distort.
    Mag current{};
    Mag maximum{};
    ShieldState shield;

    /// Ticks until this weapon may fire again, one entry per weapon on the unit.
    ///
    /// Held with health rather than with the weapon definition because a DEFINITION is
    /// shared by every unit of that type — a hundred tanks have one blueprint and a
    /// hundred separate reloads. Getting this wrong makes a squad fire in perfect
    /// unison, which is the same class of mistake as a batch sharing one animation
    /// clock.
    std::vector<int> reloadRemaining;

    /// Shots still owed by the burst this weapon is in the middle of, one entry per weapon.
    ///
    /// Zero means "not in a burst" — the next shot starts a fresh one. Per UNIT for the same
    /// reason the reload is: a squad of burst weapons must not be locked into one shared
    /// rhythm, and where each of them is within its burst is exactly the state that keeps them
    /// apart (PLAN2.md §7 P3.5).
    std::vector<int> burstRemaining;

    /// The unit that last took health off this one, or an unset handle.
    ///
    /// THE INSTIGATOR a `UnitDestroyed` event names (§7 P6.1). Recoil passes an attacker triple
    /// with its own `UnitDestroyed` (`04 §4.2`); this is the same fact in one handle, since the
    /// army and the definition are both reachable from it.
    ///
    /// May be STALE by the time it is read — a shot's killer can die on the same tick — and that
    /// is correct rather than a defect: it is a name for who did it, not a way back to a live
    /// unit, and a generational handle says so at the point of use. Left in place across a death
    /// so the death event can name the killer; cleared when the slot is reused, like the order
    /// queue.
    UnitId lastHitBy{};

    /// What this unit has earned by killing things (`Veterancy.hpp`).
    ///
    /// HERE rather than in a fourth parallel array in the store, because it is per-unit
    /// mutable combat state and that is what this struct has become — it already holds the
    /// reload clocks and the instigator, neither of which is health either. A separate
    /// array would mean another span to keep index-locked for no gain.
    Veterancy veterancy;

    [[nodiscard]] bool alive() const noexcept { return current > Mag{}; }
};

/// Fresh unit survivability. Shield.lua starts ordinary bubbles full (`:206-215`).
[[nodiscard]] inline Health initialHealth(Mag hull, Mag shield = {}) {
    Health health;
    health.current = hull;
    health.maximum = hull;
    health.shield.current = shield;
    health.shield.maximum = shield;
    return health;
}

} // namespace rm::sim
