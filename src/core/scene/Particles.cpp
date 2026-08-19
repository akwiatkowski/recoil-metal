#include "core/scene/Particles.hpp"

#include <algorithm>
#include <cmath>

namespace rm {
namespace {

/// The dust's own colours and shape. Named rather than inline so the numbers are
/// somewhere a change can be reasoned about, and so it is clear how few there are.
///
/// Dust is pale, translucent and short-lived. It rises slightly — the puff is
/// disturbed air as much as thrown grit — and swells as it disperses, which is what
/// makes it read as a cloud rather than a moving dot.
constexpr float kDustLifetimeSeconds = 1.1f;
constexpr float kDustRiseElmosPerSecond = 5.0f;
constexpr float kDustSpreadElmosPerSecond = 3.5f;
constexpr float kDustGrowthElmosPerSecond = 12.0f;

/// Peak opacity. Low, because dust is many overlapping puffs and each one carrying
/// much alpha turns a moving squad into fog.
constexpr float kDustAlpha = 0.30f;

/// A dry, slightly warm grey. Not the ground's own colour, which would need
/// sampling the terrain texture from the CPU; a neutral pale tone reads as dust
/// over every stratum in both games' palettes.
constexpr std::array<float, 3> kDustColour{{0.80f, 0.75f, 0.66f}};

/// xorshift32. A named generator rather than std::mt19937 because this needs a
/// dozen bits of jitter per particle, not statistical quality, and because the
/// state is one uint32 the caller can hold and hand back — which is what makes the
/// whole system deterministic without a global.
[[nodiscard]] std::uint32_t nextRandom(std::uint32_t& state) noexcept {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

/// A float in [-1, 1).
[[nodiscard]] float jitter(std::uint32_t& state) noexcept {
    // 24 bits, which is the mantissa a float actually has.
    const auto bits = static_cast<float>(nextRandom(state) >> 8);
    return bits / 8388608.0f - 1.0f;
}

} // namespace

void advanceParticles(std::vector<Particle>& particles, float seconds) {
    if (seconds < 0.0f) {
        return;  // a clock that went backwards is not this function's business
    }

    for (Particle& particle : particles) {
        particle.age += seconds;
    }

    std::erase_if(particles, [](const Particle& particle) {
        return particle.age >= particle.lifetime;
    });
}

void emitDust(std::vector<Particle>& particles, std::span<const DustEmitter> emitters,
              const HeightField& field, float seconds, float& debt, std::uint32_t& seed) {
    if (seconds <= 0.0f || emitters.empty()) {
        return;
    }

    // How many puffs this frame owes, across all emitters, carried between frames
    // as a fraction. A 60 Hz frame at 12 puffs a second is 0.2 of a puff per
    // moving unit — truncating that to zero every frame would emit nothing at all.
    debt += seconds * kDustPerSecond;
    if (debt < 1.0f) {
        return;
    }

    // Emitters that are actually under way, since the debt is spent among those.
    const auto raisesDust = [](const DustEmitter& emitter) {
        return emitter.moving && emitter.topSpeedElmosPerSecond >= kDustSpeedThreshold;
    };

    std::size_t moving = 0;
    for (const DustEmitter& emitter : emitters) {
        if (raisesDust(emitter)) {
            ++moving;
        }
    }
    if (moving == 0) {
        // Nobody is moving, so nothing is owed. Cleared rather than banked: a
        // squad that stops for a minute and starts again should not exhale a
        // minute's worth of dust in one frame.
        debt = 0.0f;
        return;
    }

    const auto puffsEach = static_cast<int>(debt);
    debt -= static_cast<float>(puffsEach);

    for (const DustEmitter& emitter : emitters) {
        if (!raisesDust(emitter)) {
            continue;
        }

        for (int i = 0; i < puffsEach; ++i) {
            if (particles.size() >= kMaxParticles) {
                return;  // dropped rather than grown — see kMaxParticles
            }

            // Born at ground level under the unit, scattered across its footprint
            // rather than at its centre: a trail from one point is a dotted line,
            // and a wide unit throws dust from under all of it.
            const float spread = emitter.radiusElmos * 0.8f;
            const float x = emitter.position[0] + jitter(seed) * spread;
            const float z = emitter.position[2] + jitter(seed) * spread;

            // Height from the GROUND, not from the unit. A unit on a slope has its
            // origin at the surface under its centre, and dust thrown from the
            // downhill edge of a wide one would otherwise hang in the air.
            const float y = field.heightAtWorld(x, z) + kDustLiftElmos;

            // Faster units throw dust harder, up to a point. Scaled against the
            // threshold rather than an absolute speed, so it does not need to know
            // what a fast unit is.
            const float vigour =
                std::min(2.0f, emitter.topSpeedElmosPerSecond / kDustSpeedThreshold);

            particles.push_back(Particle{
                .origin = {x, y, z},
                .age = 0.0f,
                .velocity = {jitter(seed) * kDustSpreadElmosPerSecond,
                             kDustRiseElmosPerSecond * vigour,
                             jitter(seed) * kDustSpreadElmosPerSecond},
                .lifetime = kDustLifetimeSeconds,
                // Premultiplied: the shader blends these straight over the scene,
                // and an additive particle would carry alpha 0 instead.
                .colour = {kDustColour[0] * kDustAlpha, kDustColour[1] * kDustAlpha,
                           kDustColour[2] * kDustAlpha, kDustAlpha},
                .size = emitter.radiusElmos * 1.3f,
                .growth = kDustGrowthElmosPerSecond,
            });
        }
    }
}

} // namespace rm
