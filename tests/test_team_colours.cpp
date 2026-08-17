#include "core/scene/TeamColours.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>

TEST_CASE("the team palette is non-empty and fully opaque") {
    REQUIRE(rm::kTeamColours.size() > 1);

    for (const rm::TeamColour& colour : rm::kTeamColours) {
        // Alpha is not a transparency here: Recoil multiplies it into the
        // model's one-bit alpha mask (ModelFragProgGL4.glsl:97), so anything
        // below 1 would make every unit vanish.
        REQUIRE(colour[3] == 1.0f);

        for (std::size_t channel = 0; channel < 3; ++channel) {
            REQUIRE(colour[channel] >= 0.0f);
            REQUIRE(colour[channel] <= 1.0f);
        }
    }
}

namespace {

/// Hue in degrees and saturation, both as the HSV cone defines them.
///
/// Distance in RGB is the wrong measure here: yellow and orange sit close in
/// every RGB metric and read as obviously different teams, while two colours a
/// long way apart in RGB can share a hue and be confusable at unit size on a
/// lit, textured model. Hue is what survives the shading.
struct HueSat {
    float hue;
    float saturation;
};

[[nodiscard]] HueSat hueSat(const rm::TeamColour& colour) {
    const float r = colour[0];
    const float g = colour[1];
    const float b = colour[2];
    const float hi = std::max({r, g, b});
    const float lo = std::min({r, g, b});
    const float span = hi - lo;

    if (span <= 0.0f) {
        return HueSat{0.0f, 0.0f};  // grey: no hue at all
    }

    float hue = 0.0f;
    if (hi == r) {
        hue = 60.0f * (g - b) / span;
    } else if (hi == g) {
        hue = 60.0f * (2.0f + (b - r) / span);
    } else {
        hue = 60.0f * (4.0f + (r - g) / span);
    }
    if (hue < 0.0f) {
        hue += 360.0f;
    }
    return HueSat{hue, hi > 0.0f ? span / hi : 0.0f};
}

/// Shortest way round the colour wheel, in degrees.
[[nodiscard]] float hueGap(float a, float b) {
    const float raw = std::abs(a - b);
    return std::min(raw, 360.0f - raw);
}

/// Below this a colour has no usable hue and counts as a neutral.
constexpr float kNeutralSaturation = 0.2f;

} // namespace

TEST_CASE("every team colour is distinguishable from every other") {
    // Two teams the eye cannot separate is a bug found instantly in play and
    // never by a test that only checks the values differ.
    for (std::size_t a = 0; a < rm::kTeamColours.size(); ++a) {
        for (std::size_t b = a + 1; b < rm::kTeamColours.size(); ++b) {
            const HueSat first = hueSat(rm::kTeamColours[a]);
            const HueSat second = hueSat(rm::kTeamColours[b]);

            const bool firstNeutral = first.saturation < kNeutralSaturation;
            const bool secondNeutral = second.saturation < kNeutralSaturation;

            if (firstNeutral || secondNeutral) {
                // At most one neutral in the palette; a second would be a
                // near-duplicate of the first however its RGB is written.
                REQUIRE_FALSE(firstNeutral == secondNeutral);
                continue;
            }

            // 20 degrees is about where adjacent hues stop being separable on a
            // shaded model a few dozen pixels across.
            REQUIRE(hueGap(first.hue, second.hue) > 20.0f);
        }
    }
}

TEST_CASE("team indices wrap around the palette") {
    REQUIRE(rm::teamColour(0) == rm::kTeamColours[0]);
    REQUIRE(rm::teamColour(rm::kTeamColours.size()) == rm::kTeamColours[0]);
    REQUIRE(rm::teamColour(rm::kTeamColours.size() + 2) == rm::kTeamColours[2]);
}
