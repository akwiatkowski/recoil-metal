// The tick's combat as particles: what each event kind earns, and what it carries.
#include <catch2/catch_test_macros.hpp>

#include "core/scene/CombatEffects.hpp"

#include "core/sim/Combat.hpp"

#include "support/FxMatchers.hpp"

using rm::sim::Event;
using rm::sim::EventKind;

TEST_CASE("a shot earns an additive flash at the muzzle's own height", "[effects]") {
    std::vector<rm::Particle> out;
    const Event fired{.kind = EventKind::WeaponFired,
                      .at = {rm::test::fx(100.0f), rm::test::fx(20.0f), rm::test::fx(300.0f)}};
    rm::emitCombatEffects(out, {&fired, 1});

    REQUIRE(out.size() == 1);
    // Additive: colour with zero alpha, per the Particle contract — light, not paint.
    CHECK(out[0].colour[3] == 0.0f);
    // At the muzzle's height, the same constant the projectile spawns at.
    CHECK(out[0].origin[1] == 20.0f + rm::sim::fxToFloat(rm::sim::kMuzzleHeight));
}

TEST_CASE("an impact earns smoke that drifts and a spark that adds", "[effects]") {
    std::vector<rm::Particle> out;
    const Event impact{.kind = EventKind::ProjectileImpact,
                       .at = {rm::test::fx(64.0f), rm::test::fx(0.0f), rm::test::fx(64.0f)}};
    rm::emitCombatEffects(out, {&impact, 1});

    REQUIRE(out.size() == 2);
    CHECK(out[0].colour[3] > 0.0f);      // the smoke blends
    CHECK(out[0].velocity[1] > 0.0f);    // and rises
    CHECK(out[1].colour[3] == 0.0f);     // the spark adds
    CHECK(out[1].lifetime < out[0].lifetime);
}

TEST_CASE("shield absorption flashes blue at the intercepted impact", "[effects]") {
    std::vector<rm::Particle> out;
    const Event absorbed{.kind = EventKind::ShieldDamaged,
                         .at = {rm::test::fx(64.0f), rm::test::fx(20.0f), rm::test::fx(96.0f)}};
    rm::emitCombatEffects(out, {&absorbed, 1});

    REQUIRE(out.size() == 1);
    CHECK(out[0].origin == std::array{64.0f, 20.0f, 96.0f});
    CHECK(out[0].colour[2] > out[0].colour[0]);
    CHECK(out[0].colour[3] == 0.0f);
}

TEST_CASE("everything else earns nothing here", "[effects]") {
    std::vector<rm::Particle> out;
    const Event death{.kind = EventKind::UnitDestroyed};
    const Event built{.kind = EventKind::ConstructionFinished};
    rm::emitCombatEffects(out, {&death, 1});
    rm::emitCombatEffects(out, {&built, 1});
    CHECK(out.empty());
}
