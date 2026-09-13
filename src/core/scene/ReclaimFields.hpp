#pragma once

#include "core/sim/FeatureStore.hpp"
#include "core/sim/Fx.hpp"

#include <vector>

namespace rm {

/// One labelled patch of reclaimable ground — a cluster of wrecks close enough that
/// FAF would draw one number over them rather than one each.
struct ReclaimField {
    /// Mass-weighted centroid of the cluster's wrecks, world elmos.
    sim::Fx x{};
    sim::Fx z{};
    /// What reclaiming everything in the cluster yields.
    sim::Mag mass{};
    sim::Mag energy{};
    /// How many wrecks merged into this label — a scatter of ten tanks reads
    /// differently from one experiment's corpse at the same total.
    int wrecks = 0;
};

/// The cell size the clusters are bucketed by, in elmos. Wide enough that one
/// artillery barrage's worth of corpses reads as one field; narrow enough that two
/// separate battle sites keep their own labels.
inline constexpr float kReclaimFieldCellElmos = 48.0f;

/// Clusters every live feature with something left to reclaim into labelled fields.
/// Grid-bucketed — wrecks sharing a cell merge, wrecks a cell apart do not — and
/// emitted in cell order so the label layout is stable frame to frame. Empty
/// scorches (nothing left to reclaim) produce no label; a wreck is only worth a
/// number while a number is worth having.
[[nodiscard]] std::vector<ReclaimField> reclaimFields(
    const sim::FeatureStore& features);

} // namespace rm
