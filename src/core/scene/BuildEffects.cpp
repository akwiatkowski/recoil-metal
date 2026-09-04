#include "core/scene/BuildEffects.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace rm {

BuildBeamEnds uefBuildBeamEnds(std::array<float, 3> site,
                               std::array<float, 3> builder, float extentX,
                               float extentY, float extentZ, float progress,
                               float seconds) noexcept {
    const float halfX = std::max(0.0f, extentX) * 0.5f;
    const float halfZ = std::max(0.0f, extentZ) * 0.5f;
    const float y = site[1] + std::max(0.0f, extentY)
                                 * (1.0f - std::clamp(progress, 0.0f, 1.0f));
    const std::array<std::array<float, 3>, 4> corners{{
        {{site[0] + halfX, y, site[2] + halfZ}},
        {{site[0] + halfX, y, site[2] - halfZ}},
        {{site[0] - halfX, y, site[2] + halfZ}},
        {{site[0] - halfX, y, site[2] - halfZ}},
    }};

    std::array<std::size_t, 2> nearest{};
    std::array<bool, 4> used{};
    for (std::size_t pick = 0; pick < nearest.size(); ++pick) {
        float bestDistance = std::numeric_limits<float>::max();
        for (std::size_t i = 0; i < corners.size(); ++i) {
            if (used[i]) {
                continue;
            }
            const float dx = corners[i][0] - builder[0];
            const float dy = corners[i][1] - builder[1];
            const float dz = corners[i][2] - builder[2];
            const float distance = dx * dx + dy * dy + dz * dz;
            if (distance < bestDistance) {
                bestDistance = distance;
                nearest[pick] = i;
            }
        }
        used[nearest[pick]] = true;
    }

    constexpr float kSweepSeconds = 0.6f;
    float phase =
        std::fmod(std::max(0.0f, seconds), kSweepSeconds * 2.0f) / kSweepSeconds;
    if (phase > 1.0f) {
        phase = 2.0f - phase;
    }
    const auto blend = [phase](const std::array<float, 3>& from,
                               const std::array<float, 3>& to) {
        return std::array<float, 3>{{from[0] + (to[0] - from[0]) * phase,
                                     from[1] + (to[1] - from[1]) * phase,
                                     from[2] + (to[2] - from[2]) * phase}};
    };
    return {.first = blend(corners[nearest[0]], corners[nearest[1]]),
            .second = blend(corners[nearest[1]], corners[nearest[0]])};
}

}  // namespace rm
