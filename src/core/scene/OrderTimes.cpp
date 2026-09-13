#include "core/scene/OrderTimes.hpp"

#include "core/sim/UnitCatalog.hpp"

#include <cmath>

namespace rm {

std::vector<std::optional<float>> predictedOrderTimes(
    const sim::UnitCatalog& catalog, const sim::CommandQueue& queue,
    const unitdef::UnitDef& mover, sim::Fx fromX, sim::Fx fromZ) {
    std::vector<std::optional<float>> times;
    times.reserve(queue.entries().size());
    float elapsed = 0.0f;
    float cursorX = sim::fxToFloat(fromX);
    float cursorZ = sim::fxToFloat(fromZ);
    for (const sim::QueuedCommand& order : queue.entries()) {
        if (order.kind() == sim::CommandKind::Stop) {
            times.push_back(std::nullopt);
            continue;
        }
        const float toX = sim::fxToFloat(order.targetX());
        const float toZ = sim::fxToFloat(order.targetZ());
        if (mover.speedElmosPerSecond > 0.0f) {
            elapsed += std::hypot(toX - cursorX, toZ - cursorZ)
                       / mover.speedElmosPerSecond;
        }
        cursorX = toX;
        cursorZ = toZ;
        if (order.kind() == sim::CommandKind::Build && mover.buildRate > 0.0f) {
            if (const unitdef::UnitDef* product = catalog.def(order.buildType())) {
                // A queued batch is ONE entry carrying `remainingCount` — five shift-
                // clicked tanks are a single node whose completion is five build times.
                elapsed += sim::magToFloat(product->buildTime)
                           * static_cast<float>(order.payload().remainingCount)
                           / mover.buildRate;
            }
        }
        times.push_back(elapsed);
    }
    return times;
}

} // namespace rm
