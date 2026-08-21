#pragma once

#include "core/Types.hpp"
#include "core/sim/Snapshot.hpp"

#include <array>
#include <cstddef>
#include <vector>

namespace rm {

// Where a unit is DRAWN, which is not where the sim says it is.
//
// WHY THIS EXISTS (PLAN2.md §4.1, §7 P7.2). The sim steps 10 times a second and the screen
// refreshes 60 to 120, so drawing sim state directly means every unit teleports six to twelve
// times per step and then waits. That is visible, and `--tick-rate 5` makes it four times
// worse. Interpolating between the last two snapshots is what turns a stepping sim into
// continuous motion, and it is the whole reason `sim::Snapshot` exists.
//
// **THIS IS THE UNSYNCED SIDE OF THE SEAM.** Everything here is float, everything here is
// derived, and nothing here is ever read back into the sim — a float round-trip through the
// middle of a match is exactly what fixed point was adopted to prevent (D1). The compiler does
// not enforce that yet (§4's four libraries are still one target), so it is written down here
// and checked by `tools/check_no_sim_floats.sh` from the other side.
//
// WHY THE INTERPOLATION IS BY ID AND NOT BY INDEX. Two snapshots taken a tick apart do not hold
// the same units in the same places: something died, something spawned, and a spawn can reuse a
// dead unit's slot. Matching by position in the array would smear a dead tank into the newborn
// engineer that took its slot — a unit visibly sliding across the map from where something else
// died. Both snapshots are in slot order, so the match is a two-pointer merge rather than a
// lookup per unit.

/// One unit, ready to draw.
///
/// Deliberately NOT `UnitInstance`. That struct's layout is pinned by the vertex shader and
/// carries a team colour and a scale, both of which come from the type rather than from the
/// tick; this is the per-tick part, and the caller assembles the two. Keeping them apart is
/// what lets the interpolation be tested without a GPU.
struct DrawUnit {
    sim::UnitId id{};
    UnitTypeIndex type = 0;
    int armyIndex = -1;

    std::array<float, 3> position{};
    float rotationX = 0.0f;
    float rotationY = 0.0f;
    float rotationZ = 0.0f;

    /// Ground covered, for the walk cycle. Interpolated like everything else — a leg that
    /// stepped at 10 Hz would slide as badly as a body that did.
    float distanceTravelledElmos = 0.0f;

    /// Elmos per tick, carried through so the caller can work out the stride. Not interpolated:
    /// it is a constant per unit, and blending a constant with itself is the same constant.
    float speedPerTick = 0.0f;

    /// Health as a fraction of full, 0..1. Precomputed because every consumer wants the ratio
    /// and none wants the two `Mag`s, and because the division belongs on this side of the
    /// seam.
    float healthFraction = 1.0f;
};

/// Interpolates two snapshots into what to draw.
///
/// `alpha` is how far from `from` to `to`: 0 draws the older, 1 the newer. Clamped, because a
/// frame that arrives late would otherwise extrapolate — and extrapolating a unit that has
/// stopped walks it through a wall.
///
/// **ALPHA 0 AND 1 REPRODUCE THE ENDPOINTS EXACTLY**, which is §7 P7.2's stated test, and it is
/// the reason the arithmetic is `(1 - t) * a + t * b` rather than the more obvious
/// `a + (b - a) * t`. The second is not exact at `t = 1`: `a + (b - a)` re-rounds twice and
/// lands a bit or two off `b`. The first is `0 * a + 1 * b` at the endpoint, which is `b`.
///
/// UNITS PRESENT IN `to` ONLY. A unit that has died since `from` is gone — there is nothing to
/// fade, because fading is a decision about what the game looks like and this is the geometry.
/// A unit that spawned since `from` is drawn at its `to` position with no interpolation, which
/// is right: it did not travel there.
void interpolate(const sim::Snapshot& from, const sim::Snapshot& to, float alpha,
                 std::vector<DrawUnit>& out);

/// The same for one snapshot, with no interpolation at all.
///
/// What `--no-interpolate` uses, and what a headless capture wants: a screenshot of tick N
/// should be tick N and not a blend of two ticks, or every golden image would depend on when
/// the frame happened to land.
void project(const sim::Snapshot& state, std::vector<DrawUnit>& out);

} // namespace rm
