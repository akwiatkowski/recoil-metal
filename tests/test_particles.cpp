// Dust behind moving units. The GPU decides where a particle IS — origin plus
// velocity times age — so what can be tested here is everything else: that the
// rate does not depend on the frame rate, that expired particles go, that a
// stationary unit makes none, and that the cap holds.
#include "core/scene/Particles.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

using Catch::Approx;

namespace {

[[nodiscard]] rm::HeightField flatField(int squares, std::uint16_t raw = 0) {
    rm::HeightField field;
    field.squaresX = squares;
    field.squaresZ = squares;
    field.baseHeight = 0.0f;
    field.heightScale = 0.0625f;
    const auto n = static_cast<std::size_t>(squares + 1);
    field.raw.assign(n * n, raw);
    return field;
}

[[nodiscard]] rm::DustEmitter movingAt(float speed) {
    return rm::DustEmitter{.position = {100.0f, 0.0f, 100.0f},
                           .speedElmosPerSecond = speed,
                           .radiusElmos = 10.0f};
}

/// One second of emission at 60 Hz, so the per-frame debt has to carry.
void emitForOneSecond(std::vector<rm::Particle>& particles,
                      std::span<const rm::DustEmitter> emitters, const rm::HeightField& field,
                      float& debt, std::uint32_t& seed) {
    for (int frame = 0; frame < 60; ++frame) {
        rm::emitDust(particles, emitters, field, 1.0f / 60.0f, debt, seed);
    }
}

} // namespace

TEST_CASE("a moving unit emits dust at its stated rate, whatever the frame rate") {
    // The reason emitDust carries a fractional debt at all. At 60 Hz and 12 puffs
    // a second each frame owes 0.2 of a puff; truncate that and the answer is
    // zero, forever.
    const rm::HeightField field = flatField(64);
    const std::vector<rm::DustEmitter> emitters{movingAt(20.0f)};

    std::vector<rm::Particle> fast;
    float fastDebt = 0.0f;
    std::uint32_t fastSeed = 1u;
    emitForOneSecond(fast, emitters, field, fastDebt, fastSeed);

    // The same second in four long frames instead of sixty short ones.
    std::vector<rm::Particle> slow;
    float slowDebt = 0.0f;
    std::uint32_t slowSeed = 1u;
    for (int frame = 0; frame < 4; ++frame) {
        rm::emitDust(slow, emitters, field, 0.25f, slowDebt, slowSeed);
    }

    CHECK(fast.size() == Approx(static_cast<double>(rm::kDustPerSecond)).margin(2.0));
    CHECK(slow.size() == Approx(static_cast<double>(rm::kDustPerSecond)).margin(2.0));
}

TEST_CASE("a unit barely moving raises no dust") {
    // A unit jostled by a crowd, or oscillating a fraction of an elmo against a
    // collision, would otherwise smoke as though it were driving.
    const rm::HeightField field = flatField(64);
    const std::vector<rm::DustEmitter> emitters{movingAt(rm::kDustSpeedThreshold - 0.1f)};

    std::vector<rm::Particle> particles;
    float debt = 0.0f;
    std::uint32_t seed = 1u;
    emitForOneSecond(particles, emitters, field, debt, seed);

    CHECK(particles.empty());
}

TEST_CASE("a squad that stops and starts does not exhale the pause") {
    // The debt is cleared while nothing moves rather than banked. Otherwise a
    // minute of standing still is a minute of dust in the frame it moves again.
    const rm::HeightField field = flatField(64);
    const std::vector<rm::DustEmitter> still{movingAt(0.0f)};
    const std::vector<rm::DustEmitter> moving{movingAt(20.0f)};

    std::vector<rm::Particle> particles;
    float debt = 0.0f;
    std::uint32_t seed = 1u;

    for (int frame = 0; frame < 600; ++frame) {  // ten seconds parked
        rm::emitDust(particles, still, field, 1.0f / 60.0f, debt, seed);
    }
    REQUIRE(particles.empty());

    rm::emitDust(particles, moving, field, 1.0f / 60.0f, debt, seed);
    CHECK(particles.size() <= 1);
}

TEST_CASE("dust is born on the ground under the point it comes from, not under the unit") {
    // A wide unit on a slope throws dust from under all of it, and a puff from its
    // downhill edge placed at the unit's own height would hang in the air.
    rm::HeightField field = flatField(64);
    const auto n = static_cast<std::size_t>(65);
    for (std::size_t z = 0; z < n; ++z) {
        for (std::size_t x = 0; x < n; ++x) {
            field.raw[z * n + x] = static_cast<std::uint16_t>(x * 300);
        }
    }

    std::vector<rm::Particle> particles;
    float debt = 0.0f;
    std::uint32_t seed = 7u;
    const std::vector<rm::DustEmitter> emitters{movingAt(20.0f)};
    emitForOneSecond(particles, emitters, field, debt, seed);
    REQUIRE(particles.size() > 4);

    for (const rm::Particle& particle : particles) {
        INFO("at x " << particle.origin[0]);
        CHECK(particle.origin[1]
              == Approx(field.heightAtWorld(particle.origin[0], particle.origin[2])
                        + rm::kDustLiftElmos));
    }

    // ...and the ramp is steep enough that they are not all at one height, which
    // is what makes the check above mean something.
    float lowest = particles.front().origin[1];
    float highest = lowest;
    for (const rm::Particle& particle : particles) {
        lowest = std::min(lowest, particle.origin[1]);
        highest = std::max(highest, particle.origin[1]);
    }
    CHECK(highest > lowest);
}

TEST_CASE("dust is born premultiplied, so one blend state serves it and a spark") {
    const rm::HeightField field = flatField(64);
    std::vector<rm::Particle> particles;
    float debt = 0.0f;
    std::uint32_t seed = 3u;
    const std::vector<rm::DustEmitter> emitters{movingAt(20.0f)};
    emitForOneSecond(particles, emitters, field, debt, seed);
    REQUIRE_FALSE(particles.empty());

    const rm::Particle& particle = particles.front();
    // Premultiplied means no channel exceeds alpha. A straight-alpha colour of
    // (0.78, 0.73, 0.64, 0.22) would fail this on every channel, and would render
    // as a bright grey card rather than as haze.
    CHECK(particle.colour[0] <= particle.colour[3]);
    CHECK(particle.colour[1] <= particle.colour[3]);
    CHECK(particle.colour[2] <= particle.colour[3]);
    CHECK(particle.colour[3] > 0.0f);
}

TEST_CASE("particles age and then go") {
    std::vector<rm::Particle> particles{
        rm::Particle{.origin = {0.0f, 0.0f, 0.0f}, .age = 0.0f, .velocity = {}, .lifetime = 1.0f,
                     .colour = {}, .size = 1.0f, .growth = 0.0f},
        rm::Particle{.origin = {1.0f, 0.0f, 0.0f}, .age = 0.0f, .velocity = {}, .lifetime = 3.0f,
                     .colour = {}, .size = 1.0f, .growth = 0.0f},
    };

    rm::advanceParticles(particles, 0.5f);
    REQUIRE(particles.size() == 2);
    CHECK(particles[0].age == Approx(0.5f));

    // The first one's time is up; the second's is not.
    rm::advanceParticles(particles, 0.6f);
    REQUIRE(particles.size() == 1);
    CHECK(particles.front().lifetime == Approx(3.0f));

    rm::advanceParticles(particles, 10.0f);
    CHECK(particles.empty());
}

TEST_CASE("the particle cap drops new dust rather than growing the buffer") {
    // The buffer cannot be resized while the GPU may be reading it, so the cap is
    // a real limit rather than a hint.
    const rm::HeightField field = flatField(256);
    std::vector<rm::DustEmitter> emitters;
    for (int i = 0; i < 400; ++i) {
        emitters.push_back(movingAt(40.0f));
    }

    std::vector<rm::Particle> particles;
    float debt = 0.0f;
    std::uint32_t seed = 5u;
    for (int frame = 0; frame < 600; ++frame) {
        rm::emitDust(particles, emitters, field, 1.0f / 60.0f, debt, seed);
    }

    CHECK(particles.size() == rm::kMaxParticles);
}

TEST_CASE("emission is deterministic for a seed") {
    // Same rule as the unit scatter: a screenshot that changes between runs is
    // hard to compare, and a benchmark whose scene changes is not a benchmark.
    const rm::HeightField field = flatField(64);
    const std::vector<rm::DustEmitter> emitters{movingAt(20.0f)};

    const auto run = [&] {
        std::vector<rm::Particle> particles;
        float debt = 0.0f;
        std::uint32_t seed = 99u;
        emitForOneSecond(particles, emitters, field, debt, seed);
        return particles;
    };

    const std::vector<rm::Particle> first = run();
    const std::vector<rm::Particle> second = run();

    REQUIRE(first.size() == second.size());
    for (std::size_t i = 0; i < first.size(); ++i) {
        CHECK(first[i].origin[0] == Approx(second[i].origin[0]));
        CHECK(first[i].velocity[1] == Approx(second[i].velocity[1]));
    }
}

TEST_CASE("dust is born just clear of the ground, so it can win a depth test") {
    // The lift is load-bearing, and its absence is the most silent failure in this
    // whole system: a puff born exactly on the surface is coplanar with the terrain
    // already drawn there, the particle pass is depth-tested, and it loses. The
    // draw is still issued, the count is still right, and nothing appears — which
    // reads as the feature not working rather than as a depth-test result.
    const rm::HeightField field = flatField(64, 1000);
    std::vector<rm::Particle> particles;
    float debt = 0.0f;
    std::uint32_t seed = 11u;
    const std::vector<rm::DustEmitter> emitters{movingAt(20.0f)};
    emitForOneSecond(particles, emitters, field, debt, seed);
    REQUIRE_FALSE(particles.empty());

    const float ground = 1000.0f * 0.0625f;
    for (const rm::Particle& particle : particles) {
        CHECK(particle.origin[1] > ground);
        // ...and not by enough to read as hovering: well under a heightmap square.
        CHECK(particle.origin[1] - ground < 8.0f);
    }
}
