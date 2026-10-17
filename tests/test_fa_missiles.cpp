#include <catch2/catch_test_macros.hpp>

#include "core/sim/Combat.hpp"

#include "support/FxMatchers.hpp"
#include "support/TestRoster.hpp"

#include "core/unit/FaDuration.hpp"
#include "core/sim/TickRate.hpp"

namespace {

[[nodiscard]] rm::HeightField levelField() {
    rm::HeightField field;
    field.squaresX = 100;
    field.squaresZ = 100;
    field.baseHeight = 0.0f;
    field.heightScale = 1.0f;
    field.raw.assign(field.sampleCount(), std::uint16_t{0});
    return field;
}

} // namespace

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

TEST_CASE("an interceptor flies through a wreck instead of bursting on it") {
    // C-086 (ART-E001 `0x006a3c60`): the interception sweep issues ONE query
    // with mask `0xD00` — units, projectiles, beams — with props deliberately
    // excluded. A wreck on the flight line is not a contact: the interceptor
    // still reaches the hostile missile behind it, and the wreck keeps
    // its health.
    using rm::sim::Fx;
    using rm::sim::Mag;
    const std::vector<rm::sim::Army> armies = rm::sim::freeForAll(2);
    rm::test::Roster roster;
    rm::sim::FeatureStore features;
    const rm::sim::FeatureId wreck = features.add(
        {.at = {Fx::fromInt(0), Fx::fromInt(4), Fx::fromInt(10)},
         .radiusElmos = Fx::fromInt(2),
         .health = Mag::fromInt(100),
         .maximumHealth = Mag::fromInt(100)});
    rm::sim::Projectile hostile{.position = rm::test::at(0, 4, 18),
                                .firedByArmy = 1,
                                .ticksRemaining = 2};
    rm::sim::Projectile interceptor{.position = rm::test::at(0, 4, 0),
                                    .velocity = rm::test::at(0, 0, 20),
                                    .damage = rm::unitdef::flatDamage(rm::test::mag(10.0f)),
                                    .firedByArmy = 0,
                                    .interceptor = true,
                                    .ticksRemaining = 2};
    std::vector<rm::sim::Projectile> shots{hostile, interceptor};
    rm::sim::advanceProjectiles(shots, roster.store, armies,
                                rm::sim::Terrain{levelField()}, roster.rate, nullptr,
                                &roster.catalog, {}, &features);
    CHECK(shots.empty());  // the mark fell and the interceptor was spent on it
    REQUIRE(features.find(wreck) != nullptr);
    CHECK(features.find(wreck)->health == Mag::fromInt(100));
}
