// Shots in flight as particles: the vocabulary is the arc's, the sizes are floored in
// screen points, and artillery never disappears.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "core/scene/ProjectileFx.hpp"

#include "support/FxMatchers.hpp"

using Catch::Approx;
using rm::sim::Projectile;

namespace {

[[nodiscard]] Projectile aShot(rm::unitdef::BallisticArc arc, float x = 100.0f,
                               float vx = 8.0f) {
    Projectile shot;
    shot.position = {rm::test::fx(x), rm::test::fx(20.0f), rm::test::fx(100.0f)};
    shot.velocity = {rm::test::fx(vx), rm::test::fx(0.0f), rm::test::fx(0.0f)};
    shot.arc = arc;
    return shot;
}

} // namespace

TEST_CASE("the vocabulary is the arc's: a dash, a dot, and the amber artillery dot",
          "[fx][projectiles]") {
    std::vector<rm::Particle> out;
    const std::array shots{aShot(rm::unitdef::BallisticArc::None),
                           aShot(rm::unitdef::BallisticArc::Low),
                           aShot(rm::unitdef::BallisticArc::High)};
    rm::appendProjectiles(out, shots, 0.0f, 0.1f);

    // Three tracer samples, one lob dot, one artillery dot — all additive (alpha zero).
    REQUIRE(out.size() == 5);
    for (const rm::Particle& particle : out) {
        CHECK(particle.colour[3] == 0.0f);
    }

    // The artillery dot is the largest thing here, and amber rather than white.
    const rm::Particle& artillery = out.back();
    for (std::size_t i = 0; i + 1 < out.size(); ++i) {
        CHECK(artillery.size >= out[i].size);
    }
    CHECK(artillery.colour[1] < artillery.colour[0]);  // warmer than white
}

TEST_CASE("positions extrapolate by the frame's tick fraction", "[fx][projectiles]") {
    std::vector<rm::Particle> out;
    const std::array shots{aShot(rm::unitdef::BallisticArc::High, 100.0f, 8.0f)};
    rm::appendProjectiles(out, shots, 0.5f, 0.1f);
    REQUIRE(out.size() == 1);
    // Half a tick along an 8-elmo-per-tick flight: four elmos on.
    CHECK(out[0].origin[0] == Approx(104.0f));
}

TEST_CASE("the artillery floor holds at far zoom, in the icon convention's points",
          "[fx][projectiles]") {
    std::vector<rm::Particle> out;
    const std::array shots{aShot(rm::unitdef::BallisticArc::High)};

    // Far out: an elmo per point is a whole map in view; the dot holds its floor.
    rm::appendProjectiles(out, shots, 0.0f, 8.0f);
    REQUIRE(out.size() == 1);
    CHECK(out[0].size == Approx(rm::kArtilleryFloorPoints * 8.0f));

    // Close in, the physical size wins and the dot stops growing.
    out.clear();
    rm::appendProjectiles(out, shots, 0.0f, 0.01f);
    REQUIRE(out.size() == 1);
    CHECK(out[0].size == Approx(3.5f));
}

TEST_CASE("trails belong to the arcs, one puff per tick, and they age", "[fx][projectiles]") {
    std::vector<rm::Particle> out;
    const std::array shots{aShot(rm::unitdef::BallisticArc::None),
                           aShot(rm::unitdef::BallisticArc::High)};
    rm::emitProjectileTrails(out, shots);

    // The tracer smokes nothing; the shell leaves one puff, ordinary translucency, age zero
    // so it fades in and out on the particle shader's own curve.
    REQUIRE(out.size() == 1);
    CHECK(out[0].colour[3] > 0.0f);
    CHECK(out[0].age == 0.0f);
}
