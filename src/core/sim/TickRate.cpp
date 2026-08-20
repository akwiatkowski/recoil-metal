#include "core/sim/TickRate.hpp"

#include <cmath>
#include <stdexcept>
#include <string>

namespace rm::sim {

TickRate::TickRate(std::uint32_t ticksPerSecond) : ticksPerSecond_(ticksPerSecond) {
    if (ticksPerSecond < kMinTicksPerSecond || ticksPerSecond > kMaxTicksPerSecond) {
        // Named at the moment it is introduced, with both the value and the range, because the
        // symptom of a silently clamped rate is "the game feels wrong" and there is nothing to
        // grep for.
        throw std::invalid_argument("tick rate must be between "
                                    + std::to_string(kMinTicksPerSecond) + " and "
                                    + std::to_string(kMaxTicksPerSecond)
                                    + " Hz (PLAN2.md §5.1); got "
                                    + std::to_string(ticksPerSecond));
    }
}

TickCount TickRate::ticks(Seconds duration) const noexcept {
    const float exact = duration.value * static_cast<float>(ticksPerSecond_);
    if (exact <= 0.0f) {
        return 0;  // a zero or negative duration is genuinely no ticks, not one
    }
    // Round to nearest, then floor at one. See the header on why a positive duration must
    // never become zero ticks.
    const auto rounded = static_cast<TickCount>(std::lround(exact));
    return rounded == 0 ? TickCount{1} : rounded;
}

Fx TickRate::perTick(float perSecond) const noexcept {
    return fxFromFloat(perSecond / static_cast<float>(ticksPerSecond_));
}

Mag TickRate::magPerTick(float perSecond) const noexcept {
    return magFromFloat(perSecond / static_cast<float>(ticksPerSecond_));
}

std::int32_t TickRate::bradPerTick(float radiansPerSecond) const noexcept {
    // Signed and NOT a `Brad`: a turn rate is a delta that gets added to an angle, and a rate
    // large enough to wrap a full turn in one tick would be indistinguishable from standing
    // still if it were stored in the wrapping type. `int32` keeps "three turns a tick" as
    // three turns, so a clamp against the remaining error still behaves.
    constexpr double kTwoPi = 6.283185307179586476925286766559;
    const double turnsPerTick = static_cast<double>(radiansPerSecond) / kTwoPi
                                / static_cast<double>(ticksPerSecond_);
    return static_cast<std::int32_t>(std::lround(turnsPerTick * 65536.0));
}

float TickRate::secondsPerTick() const noexcept {
    return 1.0f / static_cast<float>(ticksPerSecond_);
}

} // namespace rm::sim
