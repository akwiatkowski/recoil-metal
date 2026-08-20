#include "core/sim/SlowUpdate.hpp"

namespace rm::sim {

std::size_t SlowUpdate::dueCount(std::size_t total, TickIndex tick) const noexcept {
    // Closed form rather than a loop: of `total` indices, those congruent to `tick` modulo the
    // period are `total / period`, plus one more when the residue falls inside the remainder.
    // Written out because a bench that counted this by looping would be measuring the counting.
    const auto period = static_cast<std::size_t>(period_);
    const std::size_t residue = static_cast<std::size_t>(tick % period_);
    return total / period + (residue < total % period ? 1 : 0);
}

} // namespace rm::sim
