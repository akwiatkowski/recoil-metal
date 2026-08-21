#pragma once

#include "core/Types.hpp"
#include "core/sim/Fx.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace rm::sim {

class UnitStore;

// Which units are near a place — built once a tick, asked many times.
//
// WHY THIS EXISTS (PLAN2.md §6.5, §7 P5.1). Three passes ask the same question by scanning
// every unit: `nearestTarget` for every shooter, `nearestStruck` for every projectile, and
// `damageArea` for every blast. `aimAtTargets` asks it a fourth time, for every unturreted
// hull, every tick. That is O(n²) several times over, and §1.3's thousands of units are not
// reachable through it at any constant factor.
//
// WHY NOT RECOIL'S `QuadField` (`Sim/Misc/QuadField.h`). It allocates a result vector per
// query, served from a `QueryVectorCache` pool that `assert(false)`s when exhausted — a fixed
// pool of vectors, handed out and returned, with a hard failure when a frame asks for too many
// at once. Ours sorts every unit into a uniform grid ONCE per tick and answers a query by
// scanning the cells that overlap it, writing into one buffer this object owns.
//
// **The reason we can do that and Recoil cannot** is not cleverness: its objects move
// continuously *within* a frame and it queries mid-update, so a grid built at one point would
// be stale by the next reader. Our tick has a declared pass order (`Skirmish.cpp`), so the grid
// can be rebuilt at one named point and be authoritative for the rest of the tick.
//
// TWO PROPERTIES THAT ARE PART OF THE CONTRACT, not implementation details:
//
//   **Results come back in ascending SLOT order**, not in the order the cells were scanned.
//   That costs a sort of a handful of elements per query and buys three things: the answer is
//   byte-identical to the brute-force scan it replaces *including its order*, so every
//   existing tie-break (`nearestTarget` keeps the first minimum it meets) survives untouched;
//   the oracle test can compare sequences rather than sets; and the adaptive path below is
//   invisible to callers.
//
//   **A query is never worse than brute force.** When the cell range to scan is larger than
//   the number of units in the grid, the whole array is scanned instead. So a weapon with a
//   2,048-elmo range on a grid of 32-elmo cells does not walk 4,096 cells to find three units
//   — and the cell size stops being a number that has to be right.
//
// THE GRID INDEXES EVERY SLOT, live or dead, and callers filter. Deliberate: it is an index
// over positions, not a liveness oracle, and its callers disagree about what live means —
// `shootable` asks about HEALTH (a unit at zero health is dead to the sim before its handle is
// taken away) while a pass walking slots asks the id pool. A grid that picked one would be
// wrong for the other.

/// Units bucketed by position.
///
/// A VALUE the caller owns and hands to the tick, like the projectile list: rebuilt every tick
/// by `tickSkirmish`, kept between ticks so its buffers do not reallocate. Not a global — two
/// sims must be able to exist in one process (§5.4).
class SpatialGrid {
public:
    /// The smallest cell the grid will use, in elmos.
    ///
    /// A floor rather than a validated argument because a caller derives the cell size from
    /// content — twice the largest collision radius — and a scene of markers with no radius at
    /// all would otherwise ask for zero and divide by it. One elmo is finer than any useful
    /// cell and cannot be reached by accident.
    static constexpr Fx kMinCellSize = Fx::fromInt(1);

    /// Sorts every slot in the store into cells. Clears whatever was there.
    ///
    /// `cellSize` is clamped up to `kMinCellSize`. Called once per tick, before anything reads
    /// the grid — `tickSkirmish` owns that ordering, and `check_sim_boundary.sh` keeps the
    /// ordering in one place.
    void rebuild(const UnitStore& store, Fx cellSize);

    [[nodiscard]] Fx cellSize() const noexcept { return cellSize_; }

    /// How many slots are indexed. Equal to the store's slot count after a rebuild.
    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }

    /// Slots within `radius` of (x, z), measured on the GROUND — the same distance
    /// `groundDistanceElmos` measures, so range is a footprint on the map and not a sphere.
    ///
    /// Ascending slot order. **The span points into this object and is invalidated by the next
    /// query**, which is why this is non-const: a caller that needs two results at once must
    /// copy the first. That is the same bargain Recoil's vector pool makes, with the failure
    /// mode moved from "the pool ran out" to "you kept a span too long" — the second is a
    /// compile-time-visible mistake in a way the first never was.
    [[nodiscard]] std::span<const UnitIndex> within(Fx x, Fx z, Fx radius);

    /// The same, WITHOUT the distance test: every slot in a cell that overlaps the square
    /// `[x±radius, z±radius]`.
    ///
    /// For a caller whose test is not a fixed radius — collision separation compares against
    /// the sum of two units' radii, which differs per pair, so it wants the neighbourhood and
    /// does its own arithmetic.
    [[nodiscard]] std::span<const UnitIndex> candidates(Fx x, Fx z, Fx radius);

private:
    /// One unit's cell and slot, sorted by cell then slot.
    ///
    /// A single array rather than two parallel ones so that sorting moves both together and
    /// they cannot come apart — the bug that a parallel-array sort invites.
    struct Entry {
        std::uint64_t cell = 0;
        UnitIndex slot = 0;
        Fx x{};
        Fx z{};
    };

    /// Fills `result_` with the cells overlapping the square, then optionally distance-filters.
    void gather(Fx x, Fx z, Fx radius, bool exact);

    std::vector<Entry> entries_;

    /// The last query's answer. A member so that repeated queries in one pass reuse the
    /// capacity — which is what "no per-query allocation" means in practice.
    std::vector<UnitIndex> result_;

    Fx cellSize_ = kMinCellSize;
};

} // namespace rm::sim
