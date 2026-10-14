#include "core/unit/FaDuration.hpp"

#include <cmath>

namespace rm::unitdef {

sim::Seconds faWaitSeconds(sim::Seconds authored) noexcept {
    // The `n <= 0.1` branch, which also swallows zero and anything negative: FA's own guard is
    // `<=`, so a tenth of a second is the floor rather than the first quantum. Negative
    // durations do not appear in the corpus, but `n <= 0.1` is what the game would do with one
    // and diverging here would be inventing a rule.
    if (authored.value <= 0.1f) {
        return sim::seconds(0.1f);
    }

    // `WaitTicks(n * 10 + 1)` ticks, each 100 ms. `floor` rather than a rounding conversion
    // because Lua's `n * 10 + 1` is a float count and `WaitTicks` consumes it as a loop bound —
    // 2.5 ticks yields twice and then the +1, which is 3 ticks. Two corpus values are not
    // multiples of 0.1 and exercise exactly this: 0.25 becomes 0.3 and 0.33 becomes 0.4.
    const float tenths = std::floor(authored.value * 10.0f) + 1.0f;
    return sim::seconds(tenths / 10.0f);
}
int redirectCooldownTicks(sim::TickRate rate, float shotsPerSecond) noexcept {
    // Lua-timed: `WaitSeconds(1 / rate)` carries simInit.lua's `n * 10 + 1` bias, applied in
    // seconds at import so the correction is rate-independent. A non-positive rate is no wait
    // at all — the spawn sites already refuse to build a redirector for one.
    if (shotsPerSecond <= 0.0f) {
        return 0;
    }
    return static_cast<int>(rate.ticks(faWaitSeconds(sim::seconds(1.0f / shotsPerSecond))));
}

} // namespace rm::unitdef
