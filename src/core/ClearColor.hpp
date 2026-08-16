#pragma once

namespace rm {

// Simple RGBA quadruplet, channels in [0, 1]. Matches MTL::ClearColor's
// layout by convention, not by type — core/ must not depend on Metal.
struct Rgba {
    float r;
    float g;
    float b;
    float a;
};

// HSV → RGB. h in [0, 1) (wraps), s and v in [0, 1]. Result alpha is 1.
// The classic six-sector algorithm (e.g. Foley & van Dam); chosen because it
// is branch-cheap and exact at the primaries, which the tests pin down.
[[nodiscard]] Rgba hsvToRgb(float h, float s, float v) noexcept;

// The milestone-1 background: a slow hue cycle, pure function of time so the
// unit tests can pin it without a GPU. seconds need not be monotonic-wrapped;
// fmod inside handles it.
[[nodiscard]] Rgba animatedClearColor(double seconds) noexcept;

} // namespace rm
