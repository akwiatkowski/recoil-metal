#include "core/map/ProceduralField.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace rm {

HeightField makeSineHills(int squaresX, int squaresZ, float minHeight, float maxHeight) {
    HeightField field;
    if (squaresX <= 0 || squaresZ <= 0) {
        return field;
    }

    field.squaresX = squaresX;
    field.squaresZ = squaresZ;
    field.baseHeight = minHeight;
    // Same quantisation as SMF: the domain is 2^16 steps wide, not 2^16 - 1.
    field.heightScale = (maxHeight - minHeight) / 65536.0f;

    const auto nx = static_cast<std::size_t>(field.verticesX());
    const auto nz = static_cast<std::size_t>(field.verticesZ());
    field.raw.resize(nx * nz);

    // Two superposed sine waves per axis. The low frequency gives broad hills,
    // the high one adds enough small-scale relief that shading errors become
    // visible instead of hiding on a smooth surface.
    constexpr float kTwoPi = 2.0f * std::numbers::pi_v<float>;
    constexpr float kBroadCycles = 3.0f;
    constexpr float kDetailCycles = 11.0f;
    constexpr float kDetailWeight = 0.25f;

    for (std::size_t z = 0; z < nz; ++z) {
        const float v = static_cast<float>(z) / static_cast<float>(nz - 1);
        for (std::size_t x = 0; x < nx; ++x) {
            const float u = static_cast<float>(x) / static_cast<float>(nx - 1);

            const float broad = std::sin(u * kTwoPi * kBroadCycles)
                              * std::sin(v * kTwoPi * kBroadCycles);
            const float detail = std::sin(u * kTwoPi * kDetailCycles)
                               * std::sin(v * kTwoPi * kDetailCycles);

            // Combine, then map [-1, 1] onto the full uint16 domain.
            const float mixed = (broad + kDetailWeight * detail) / (1.0f + kDetailWeight);
            const float unit = std::clamp((mixed + 1.0f) * 0.5f, 0.0f, 1.0f);

            field.raw[z * nx + x] = static_cast<std::uint16_t>(unit * 65535.0f);
        }
    }

    return field;
}

} // namespace rm
