// The tick's combat as particles: what each event kind earns, and what it carries.
#include <catch2/catch_test_macros.hpp>

#include "core/scene/CombatEffects.hpp"

#include <catch2/catch_approx.hpp>
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

TEST_CASE("shield absorption flashes blue and ripples outward", "[effects]") {
    std::vector<rm::Particle> out;
    const Event absorbed{.kind = EventKind::ShieldDamaged,
                         .at = {rm::test::fx(64.0f), rm::test::fx(20.0f), rm::test::fx(96.0f)}};
    rm::emitCombatEffects(out, {&absorbed, 1});

    REQUIRE(out.size() == 7);
    CHECK(out[0].origin == std::array{64.0f, 20.0f, 96.0f});
    CHECK(out[0].colour[2] > out[0].colour[0]);
    CHECK(out[0].colour[3] == 0.0f);
    // The ripple: six sparks in a horizontal hexagon, all additive blue, all
    // leaving the impact point — an expanding ring, deterministic by construction.
    float vx = 0.0f;
    float vz = 0.0f;
    for (std::size_t i = 1; i < 7; ++i) {
        CHECK(out[i].colour == out[0].colour);
        CHECK(out[i].velocity[1] == 0.0f);
        CHECK(out[i].origin == out[0].origin);
        vx += out[i].velocity[0];
        vz += out[i].velocity[2];
    }
    CHECK(std::abs(vx) < 0.01f);
    CHECK(std::abs(vz) < 0.01f);
}

TEST_CASE("a death earns a flash, a fireball, smoke and sparks scaled by size", "[effects]") {
    std::vector<rm::Particle> out;
    const Event death{.kind = EventKind::UnitDestroyed,
                      .at = {rm::test::fx(10.0f), rm::test::fx(0.0f), rm::test::fx(20.0f)}};
    // A 10-elmo radius (an experimental): the caller reads it from the corpse's
    // slot, which outlives the unit the same way the kill ledger's type does.
    rm::emitCombatEffects(out, {&death, 1}, nullptr, nullptr, 0.0f,
                          [](rm::sim::UnitId) { return 10.0f; });

    REQUIRE(out.size() == 7);
    // The flash is the brightest and briefest; the fireball shorter-lived than
    // the smoke; the smoke is the only one that blends rather than adds.
    CHECK(out[0].colour == std::array{1.0f, 0.95f, 0.8f, 0.0f});
    CHECK(out[1].colour == std::array{1.0f, 0.55f, 0.2f, 0.0f});
    CHECK(out[2].colour[3] > 0.0f);
    CHECK(out[2].velocity[1] > 0.0f);
    CHECK(out[0].lifetime < out[1].lifetime);
    CHECK(out[1].lifetime < out[2].lifetime);
    // Sizes follow the corpse: flash the widest, all far beyond infantry scale.
    CHECK(out[0].size == Catch::Approx(30.0f));
    CHECK(out[1].size == Catch::Approx(20.0f));
    for (std::size_t i = 4; i < 7; ++i) {
        CHECK(out[i].colour[3] == 0.0f);
    }
}

TEST_CASE("a death of unknown size still earns a tank-scale burst", "[effects]") {
    std::vector<rm::Particle> out;
    const Event death{.kind = EventKind::UnitDestroyed};
    rm::emitCombatEffects(out, {&death, 1});
    CHECK(out.size() == 7);
    CHECK(out[0].size < 30.0f);
}

TEST_CASE("everything else earns nothing here", "[effects]") {
    std::vector<rm::Particle> out;
    const Event built{.kind = EventKind::ConstructionFinished};
    rm::emitCombatEffects(out, {&built, 1});
    CHECK(out.empty());
}

TEST_CASE("a beam earns a hot core and a soft halo per node", "[effects]") {
    std::vector<rm::Particle> out;
    const Event beam{.kind = EventKind::BeamFired,
                     .at = {rm::test::fx(8.0f), rm::test::fx(0.0f), rm::test::fx(0.0f)},
                     .at2 = {rm::test::fx(0.0f), rm::test::fx(0.0f), rm::test::fx(0.0f)}};
    rm::emitCombatEffects(out, {&beam, 1});

    // Eight elmos at four-elmo spacing: three nodes, each a core plus a halo,
    // then the strike spark where it lands.
    REQUIRE(out.size() == 7);
    for (std::size_t node = 0; node < 3; ++node) {
        const rm::Particle& core = out[2 * node];
        const rm::Particle& halo = out[2 * node + 1];
        CHECK(core.colour == std::array{0.55f, 0.75f, 1.0f, 0.0f});
        CHECK(halo.colour[3] == 0.0f);
        CHECK(halo.size > core.size);
        CHECK(halo.colour[0] < core.colour[0]);
        CHECK(halo.origin == core.origin);
        CHECK(halo.lifetime == core.lifetime);
    }
    CHECK(out[6].colour == std::array{0.7f, 0.85f, 1.0f, 0.0f});
}

TEST_CASE("a death leaves a smoke plume that outlives the flash", "[effects]") {
    std::vector<rm::Particle> out;
    const Event death{.kind = EventKind::UnitDestroyed,
                      .at = {rm::test::fx(10.0f), rm::test::fx(0.0f), rm::test::fx(20.0f)}};
    rm::CombatEffectState state;
    rm::emitCombatEffects(out, {&death, 1}, nullptr, &state, 0.1f,
                          [](rm::sim::UnitId) { return 10.0f; });
    const std::size_t burst = out.size();
    // Seven instant particles plus the column's first puff, exhaled the same tick.
    REQUIRE(burst == 8);
    // Two seconds of ticks: the column keeps building after the flash is gone.
    for (int i = 0; i < 20; ++i) {
        rm::emitCombatEffects(out, {}, nullptr, &state, 0.1f);
    }
    CHECK(out.size() > burst + 10);
    // And then it stops: two more seconds add nothing, the sky clears.
    const std::size_t settled = out.size();
    for (int i = 0; i < 20; ++i) {
        rm::emitCombatEffects(out, {}, nullptr, &state, 0.1f);
    }
    CHECK(out.size() == settled);
}
