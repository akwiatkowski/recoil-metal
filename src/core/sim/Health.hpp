#pragma once

#include <vector>

namespace rm::sim {

/// What a unit can still take. One per unit, parallel to the instances themselves —
/// same reason MoveState is: UnitInstance's layout is pinned and read by the shader.
///
/// Lives in its own header rather than in Combat.hpp because the unit store holds it and
/// Combat.hpp needs the store: the two would include each other. Splitting the type out is
/// the smaller fix, and it is where a reader would look for it anyway.
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

} // namespace rm::sim
