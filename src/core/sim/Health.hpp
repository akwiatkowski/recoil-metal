#pragma once

#include "core/sim/Fx.hpp"

#include <vector>

namespace rm::sim {

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
    /// The fraction matters as much as the range. Damage is spread with a linear falloff, so a
    /// unit at the rim of a blast takes an arbitrary fraction of the weapon's damage; rounding
    /// that to whole points would make a large blast deal visibly different totals depending
    /// on how the survivors happened to be arranged.
    Mag current{};
    Mag maximum{};

    /// Ticks until this weapon may fire again, one entry per weapon on the unit.
    ///
    /// Held with health rather than with the weapon definition because a DEFINITION is
    /// shared by every unit of that type — a hundred tanks have one blueprint and a
    /// hundred separate reloads. Getting this wrong makes a squad fire in perfect
    /// unison, which is the same class of mistake as a batch sharing one animation
    /// clock.
    std::vector<int> reloadRemaining;

    [[nodiscard]] bool alive() const noexcept { return current > Mag{}; }
};

} // namespace rm::sim
