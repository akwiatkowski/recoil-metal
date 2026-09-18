#pragma once

#include "core/sim/Fx.hpp"
#include "core/sim/UnitCatalog.hpp"
#include "core/sim/UnitStore.hpp"

#include <vector>

namespace rm::sim {

// Who is standing beside whom, and what it is worth (`core/unit/Adjacency.hpp` for the
// tables and their citations).
//
// RECOMPUTED EVERY TICK from positions and types the state hash already covers, so it
// carries no hash entry of its own — the same reasoning as `Economy::requestedLastTick`.
// The scan is quadratic in STRUCTURES, not in units: a long match carries a few dozen
// skirted structures against a couple thousand tanks, and the participation flag filters
// the tanks out before any arithmetic.

/// One unit's multipliers this tick. `kFxOne` everywhere for the unbuffed —
/// multiplying by these is free of branches at the application site.
struct AdjacencyEffects {
    Fx massProduction = kFxOne;
    Fx energyProduction = kFxOne;
    Fx energyUpkeep = kFxOne;
    /// `MassActive`/`EnergyActive`: the build-drain discount a generator or extractor
    /// gives a structure that is actively building — multiplied into the builder's
    /// construction demand (`C-051`(a) reproduces retail's dropped-digit Size20 row).
    Fx massBuild = kFxOne;
    Fx energyBuild = kFxOne;
    /// `RateOfFire` adjacency: a PENALTY in retail despite the "Bonus" name — the
    /// multiplier lands on the weapon's rate, so `Add < 0` fires slower (`C-051`(b)).
    /// Only SIZE4 artillery receivers ever see it move off one (`C-051`(c)).
    Fx rateOfFire = kFxOne;
};

/// How far apart two skirts may stand and still count as touching, in elmos.
///
/// OURS, and the reason is stated rather than hidden: the game SNAPS structures to the
/// build grid, so its skirts abut exactly and its engine can test equality
/// (`EffectUtilities.lua:509-556` does, literally `==`). This engine places freely, so
/// exactness would make adjacency a pixel-hunt. Half an ogrid of slack keeps the
/// decision readable — park the storage beside the extractor and it counts — without
/// letting a bonus jump a lane a tank drives through.
inline constexpr Fx kAdjacencyGapElmos = Fx::fromInt(4);

/// Whether two skirt rectangles share an edge: gap within tolerance on ONE axis, and
/// POSITIVE overlap on the other. Corner-to-corner contact is deliberately no — the
/// original's own beam code handles only the two edge cases, and a corner shares no
/// concrete to run a cable across.
[[nodiscard]] bool skirtsShareEdge(Fx ax, Fx az, Fx aHalfX, Fx aHalfZ, Fx bx, Fx bz,
                                   Fx bHalfX, Fx bHalfZ,
                                   Fx tolerance = kAdjacencyGapElmos) noexcept;

/// Every unit's multipliers this tick, indexed by slot. `out` is resized and reset —
/// caller-owned so the per-tick call reuses one allocation.
///
/// Additive stacking, the original's: each adjacent giver ADDS its grant for the
/// receiver's size row, the sum lands on 1, and nothing caps it but geometry
/// (`Buff.lua:140-190`, `Stacks = 'ALWAYS'`). Same-army pairs only, both alive.
///
/// `tolerance` is the edge-contact slack: `kAdjacencyGapElmos` under free placement, zero
/// under grid placement where skirts meet exactly (`Terrain::placement`).
void adjacencyEffects(const UnitStore& store, const UnitCatalog& catalog,
                      std::vector<AdjacencyEffects>& out, Fx tolerance = kAdjacencyGapElmos);

// --- The ghost's preview -----------------------------------------------------
//
// What placing a structure WOULD pay, answered before it exists — the build ghost's
// question. The grant arithmetic is the pair scan's own, run for one hypothetical
// participant: the same skirt test, the same tolerance, the same army gate.

/// One direction of a link: what one side would add to the other's multipliers.
/// Deltas, not totals — a positive production grant is a bonus, a negative upkeep
/// grant is the discount a generator gives a factory.
struct AdjacencyFlow {
    Fx massProduction{};
    Fx energyProduction{};
    Fx energyUpkeep{};
    Fx massBuild{};
    Fx energyBuild{};
    Fx rateOfFire{};

    /// True when a grant crosses in this direction. Every authored grant is a bonus —
    /// production adds, maintenance and build discounts subtract — so a link is always
    /// good news, just of a different size and sign. (`RateOfFire` is the exception:
    /// retail's penalty-by-bug, `C-051`(b), still counts as a link.)
    [[nodiscard]] bool any() const noexcept {
        return massProduction != Fx{} || energyProduction != Fx{} || energyUpkeep != Fx{}
            || massBuild != Fx{} || energyBuild != Fx{} || rateOfFire != Fx{};
    }
};

/// One touching neighbour, and the grant in each direction. A link exists only when a
/// bonus actually crosses — two skirted buildings can share an edge and pay nothing.
struct AdjacencyLink {
    UnitIndex slot;
    AdjacencyFlow toGhost;    ///< the standing neighbour's grant onto the ghost
    AdjacencyFlow fromGhost;  ///< the ghost's grant onto it
};

/// The answer for a ghosted structure: the multipliers it would receive (summed like
/// `adjacencyEffects`, ones for a non-receiver) and a link per neighbour a grant
/// would cross to or from.
struct AdjacencyPreview {
    AdjacencyEffects received;
    std::vector<AdjacencyLink> links;
};

/// Evaluates `ghost` — a catalogue adjacency row — as if placed at `x`,`z` (the unit's
/// position; the skirt offset is applied inside, matching `adjacencyEffects`). An empty
/// `links` and all-ones `received` for a non-participant or an army of `kNoArmy`.
[[nodiscard]] AdjacencyPreview adjacencyPreview(
    const UnitStore& store, const UnitCatalog& catalog, int army,
    const UnitCatalog::AdjacencyInfo& ghost, Fx x, Fx z,
    Fx tolerance = kAdjacencyGapElmos);

} // namespace rm::sim
