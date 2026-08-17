#include "core/scene/UnitPlacement.hpp"

#include <cmath>
#include <numbers>
#include <random>

namespace {

/// Samples the terrain at a world position by nearest grid corner.
///
/// Nearest-corner rather than bilinear: at 8 elmos per square a unit-sized
/// object spans several corners anyway, and an interpolating sampler is a
/// separate thing worth having once units need to sit flush on slopes.
[[nodiscard]] float groundHeight(const rm::HeightField& field, float x, float z) {
    const auto gx = static_cast<int>(std::lround(x / static_cast<float>(rm::kSquareSize)));
    const auto gz = static_cast<int>(std::lround(z / static_cast<float>(rm::kSquareSize)));
    return field.heightAt(gx, gz);  // heightAt clamps, so edges are safe
}

/// Cap on resampling attempts per requested instance. A map that is almost
/// entirely underwater would otherwise spin here.
constexpr int kMaxAttemptsPerInstance = 32;

} // namespace

namespace rm {

std::vector<UnitInstance> scatterOnLand(const HeightField& field, std::size_t count,
                                        std::uint32_t seed, float scale, float minHeight) {
    std::vector<UnitInstance> instances;
    if (field.squaresX <= 0 || field.squaresZ <= 0 || count == 0) {
        return instances;
    }

    instances.reserve(count);

    // mt19937 seeded explicitly: reproducible across runs and machines, which
    // matters because these instances feed both screenshots and benchmarks.
    std::mt19937 rng{seed};
    std::uniform_real_distribution<float> alongX{0.0f, field.widthElmos()};
    std::uniform_real_distribution<float> alongZ{0.0f, field.depthElmos()};
    std::uniform_real_distribution<float> yaw{0.0f, 2.0f * std::numbers::pi_v<float>};

    for (std::size_t placed = 0; placed < count; ++placed) {
        for (int attempt = 0; attempt < kMaxAttemptsPerInstance; ++attempt) {
            const float x = alongX(rng);
            const float z = alongZ(rng);
            const float y = groundHeight(field, x, z);

            if (y < minHeight) {
                continue;  // underwater; try elsewhere
            }

            instances.push_back(UnitInstance{
                .position = {{x, y, z}},
                .rotationY = yaw(rng),
                .scale = scale,
            });
            break;
        }
    }

    return instances;
}

std::vector<UnitInstance> atStartPositions(const HeightField& field,
                                           std::span<const mapinfo::StartPosition> positions,
                                           float scale) {
    std::vector<UnitInstance> instances;
    instances.reserve(positions.size());

    for (const mapinfo::StartPosition& start : positions) {
        instances.push_back(UnitInstance{
            .position = {{start.x, groundHeight(field, start.x, start.z), start.z}},
            // Unrotated: a spawn marker facing a consistent direction reads more
            // clearly than a random one, and there is no facing data to honour.
            .rotationY = 0.0f,
            .scale = scale,
        });
    }

    return instances;
}

} // namespace rm
