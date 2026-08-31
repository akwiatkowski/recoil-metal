#pragma once

// One whole beat of building, for tests that drive the economy without a whole match.
//
// WHY THIS EXISTS. Construction is settled in TWO stages, and the split is retail's, not a
// convenience: the builder's own task materialises its target in the command-dispatch stage at
// the head of the beat, and the ratio that work is multiplied by is written at the foot of it
// (`C-112`, `C-142`, `C-162`, `C-188`). A test that calls `tickEconomy` alone therefore bills
// for work that never happened, which is not a smaller version of a beat — it is a different
// one. Rather than let every such test hand-roll the pairing (and get the order wrong in a way
// that reads as a real result), the order lives here once.

#include "core/sim/Economy.hpp"

#include <span>

namespace rm::test {

/// Advances every construction and then charges for it, in the order a real tick does.
inline void tickBuild(sim::Economy& economy, std::span<sim::Construction> building) {
    for (sim::Construction& work : building) {
        sim::advanceConstruction(work);
    }
    sim::tickEconomy(economy, building);
}

} // namespace rm::test
