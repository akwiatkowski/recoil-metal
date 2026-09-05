#pragma once

#include "core/sim/Economy.hpp"
#include "core/sim/UnitCatalog.hpp"
#include "core/sim/UnitStore.hpp"

#include <cstddef>
#include <span>
#include <vector>

namespace rm::sim {

// Lending a build arm: the Assist order's per-tick effect.
//
// THE MECHANIC. A builder with an `Assist` order standing within build reach of its
// target adds its own BuildRate to the target's UNFINISHED construction — the oldest one,
// which is the one the target is working on. The construction then advances and DRAINS at
// the combined rate (`Construction::effectiveBuildPerTick`), so help costs resources
// faster in exchange for time, exactly as a second engineer on a build does in the game.
//
// RECOMPUTED EVERY TICK, from orders and positions the hash already covers: a helper that
// walks away, dies, or is re-tasked stops contributing the same tick, with no
// subscription bookkeeping to forget. A target with an idle queue is simply waited
// beside — the order is a standing one (`advanceOrders` keeps it while the target lives).

/// Recomputes every construction's `assistPerTick` from who is currently helping.
/// Returns how many assisters contributed this tick — the outward sign the order works.
std::size_t applyAssistance(const UnitStore& store, const UnitCatalog& catalog,
                            std::vector<Construction>& building,
                            std::span<const Army> armies = {}, const Intel* intel = nullptr,
                            const PlayableRect* playableRect = nullptr);

} // namespace rm::sim
