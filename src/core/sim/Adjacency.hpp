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
                                   Fx bHalfX, Fx bHalfZ) noexcept;

/// Every unit's multipliers this tick, indexed by slot. `out` is resized and reset —
/// caller-owned so the per-tick call reuses one allocation.
///
/// Additive stacking, the original's: each adjacent giver ADDS its grant for the
/// receiver's size row, the sum lands on 1, and nothing caps it but geometry
/// (`Buff.lua:140-190`, `Stacks = 'ALWAYS'`). Same-army pairs only, both alive.
void adjacencyEffects(const UnitStore& store, const UnitCatalog& catalog,
                      std::vector<AdjacencyEffects>& out);

} // namespace rm::sim
