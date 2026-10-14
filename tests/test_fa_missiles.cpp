// Player-perspective coverage for missiles claims (see docs/fa-exe-analysis-plan.md).
// Filled by the coverage-test wave; each TEST_CASE cites its claim IDs.
#include <catch2/catch_test_macros.hpp>

#include "core/unit/FaDuration.hpp"
#include "core/sim/TickRate.hpp"

TEST_CASE("a redirector waits one full Lua-timed rate cycle between redirects") {
    // C-088's Cybran MissileRedirect (ART-S007 `lua/sim/defaultantiprojectile.lua`,
    // `RedirectingState.Main`): after each redirect the entity waits
    // `WaitSeconds(1 / RedirectRateOfFire)` before returning to `WaitingState`.
    // That wait is Lua-timed, so it carries `simInit.lua`'s `n * 10 + 1` bias
    // (`core/unit/FaDuration.hpp`): URL0303's 1/sec is 11 ticks at 10 Hz, not 10.
    const rm::sim::TickRate rate{};
    CHECK(rm::unitdef::redirectCooldownTicks(rate, 1.0f) == 11);
    CHECK(rm::unitdef::redirectCooldownTicks(rate, 2.0f) == 6);
}
