#pragma once

#include "core/Types.hpp"
#include "core/sim/Fx.hpp"

namespace rm::sim {

// Where a unit is and which way it faces. **The sim's authority on both.**
//
// WHY THIS EXISTS. Until now the sim's position lived in `UnitInstance`, which is the GPU's
// layout — pinned by a `static_assert` and read verbatim by the vertex shader (PLAN2.md §1.2).
// That was workable while the sim was float. It stops being workable the moment position
// becomes fixed point, because the shader cannot read a `Q18.14` integer as a coordinate.
//
// So the two separate, and this is the authority: `UnitInstance` becomes a DRAW-TIME
// PROJECTION, built by the gather from a transform plus the type's scale, the army's colour
// and the walk-cycle phase. That is the split PLAN2 §7 P7 was going to make; P2.2 forces it
// early, the same way P1.4 forced the draw gather.
//
// The projection is one-way. Nothing reads a `UnitInstance` back into sim state — that would
// be a float round-trip mid-tick, which is precisely what `Fx.hpp` exists to prevent.
//
// ANGLES ARE `Brad`, not fixed-point radians. A turn is 65,536, so an angle wraps by unsigned
// overflow: exact, free, and impossible to get wrong. Fixed-point radians would need a modulo
// by 2π to stay in range, and 2π is irrational — the wrap itself would accumulate error over a
// match. See `core/Types.hpp`.
struct Transform {
    /// World position in elmos. `y` is the height the unit sits at, which the movement pass
    /// writes from the terrain rather than the caller.
    Fx x{};
    Fx y{};
    Fx z{};

    /// Yaw: which way the unit faces, measured **from +Z toward +X**.
    ///
    /// That is not the mathematical convention, and it is not negotiable — it is what the
    /// vertex shader does with the angle. `fxBearing(x, z)` takes its arguments in the same
    /// order for the same reason.
    Brad heading{};

    /// Slope alignment: rotation about +X and about +Z, which tilt the model onto the ground
    /// under its feet. Derived from the terrain normal and the yaw, so a unit facing any
    /// direction plants both feet on the same slope.
    ///
    /// Sim state rather than presentation, even though only the renderer reads them today:
    /// they are a function of position, and a pass that wanted to know whether a unit is on a
    /// slope steep enough to slow it down would read exactly these.
    Brad pitch{};
    Brad roll{};
};

} // namespace rm::sim
