#pragma once

// Predicted order-queue times: the seconds a shift-held queue's labels wear at their
// nodes — "the waypoint in 0:12, the building done at 1:40".
//
// A FORECAST, not a promise: straight-line travel at rated speed, full build rate, no
// stalls, pauses or reroutes. The sim needs none of it — this is presentation math in
// floats, which is why it lives in `scene/` and not `sim/`: the tick is fixed point,
// and a number that only a label reads has no business being there.

#include "core/sim/CommandQueue.hpp"
#include "core/sim/Fx.hpp"
#include "core/unit/UnitDef.hpp"

#include <optional>
#include <vector>

namespace rm {
namespace sim { class UnitCatalog; }

/// The predicted completion time of each queued order, index-aligned with
/// `sim::CommandQueue::entries()`. Every order with a destination accumulates the
/// travel to its node from the previous one; a `Build` adds the product's own
/// `buildTime` divided by the mover's `buildRate`, which is what makes a row of
/// queued buildings read as cumulative "done at" times.
///
/// `std::nullopt` where the order has no node to hang the number on — a `Stop` draws
/// nothing, so it predicts nothing. A `Build` whose product or build rate is missing
/// keeps its travel time only: arrival is still a real answer.
[[nodiscard]] std::vector<std::optional<float>> predictedOrderTimes(
    const sim::UnitCatalog& catalog, const sim::CommandQueue& queue,
    const unitdef::UnitDef& mover, sim::Fx fromX, sim::Fx fromZ);

} // namespace rm
