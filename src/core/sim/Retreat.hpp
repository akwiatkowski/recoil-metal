#pragma once

// Retreat-at-HP automation (#15785): Zero-K's mechanic that retail Supreme
// Commander never had — a unit whose hull falls under its setting breaks off for
// the nearest friendly builder, and walks back to where it flinched once it is
// whole. The setting lives on the unit (`UnitStore::retreatThreshold`); this is
// the per-tick pass that acts on it.
//
// DESIGN DEBTS, stated rather than hidden:
//
//   - The repair point is the NEAREST allied builder or factory, not a declared
//     retreat zone — the ticket's note ("Zero-K retreat zones") is the behaviour,
//     not zone placement UI, which does not exist here yet.
//   - "Repaired" means a full hull. A unit parked beside a builder nobody ordered
//     to repair it waits forever — which is also the honest answer, since nothing
//     here auto-assigns the mechanic.
//   - A player's order to a retreating unit is left alone. The flag still owes a
//     return trip when the hull is whole, but it will not stomp a live queue to
//     take it — the return move is only issued onto an empty queue or over the
//     retreat move this pass itself wrote.

#include "core/Types.hpp"
#include "core/sim/Army.hpp"
#include "core/sim/UnitCatalog.hpp"
#include "core/sim/UnitStore.hpp"

#include <span>

namespace rm::sim {

/// Applies the retreat thresholds to every unit, once a tick.
///
/// Placed after `updateAggressiveOrders` in the skirmish tick for the same reason
/// that pass sits where it does: it rewrites queues, and it wants the post-intel
/// world. A unit hurt below its line abandons its whole queue for the run home —
/// like an unshifted click, not a waypoint.
void updateRetreats(UnitStore& store, const UnitCatalog& catalog,
                    std::span<const Army> armies) noexcept;

} // namespace rm::sim
