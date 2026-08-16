#include "core/ClearColor.hpp"

#include <cmath>

namespace rm {

Rgba hsvToRgb(float h, float s, float v) noexcept {
    // Wrap h into [0, 1) so callers can pass arbitrary hue drifts.
    h = std::fmod(h, 1.0f);
    if (h < 0.0f) {
        h += 1.0f;
    }

    const float sector = h * 6.0f; // 6 hue sectors per revolution
    const int i = static_cast<int>(sector);
    const float f = sector - static_cast<float>(i); // fractional position in sector

    const float p = v * (1.0f - s);
    const float q = v * (1.0f - s * f);
    const float t = v * (1.0f - s * (1.0f - f));

    switch (i % 6) {
        case 0: return {v, t, p, 1.0f};
        case 1: return {q, v, p, 1.0f};
        case 2: return {p, v, t, 1.0f};
        case 3: return {p, q, v, 1.0f};
        case 4: return {t, p, v, 1.0f};
        default: return {v, p, q, 1.0f};
    }
}

Rgba animatedClearColor(double seconds) noexcept {
    // Full hue revolution every 12 s. Arbitrary aesthetic constant: fast
    // enough to notice drift when eyeballing the window, slow enough to not
    // strobe.
    constexpr double kCycleSeconds = 12.0;

    const float hue = static_cast<float>(std::fmod(seconds, kCycleSeconds) / kCycleSeconds);

    // Saturation 0.55 / value 0.85: saturated enough that banding or gamma
    // mistakes in the render path stay visible, dull enough to stare at.
    return hsvToRgb(hue, 0.55f, 0.85f);
}

} // namespace rm
