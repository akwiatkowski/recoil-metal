#include "core/scene/UnitPlacement.hpp"

#include <cmath>
#include <numbers>
#include <random>

namespace {

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
    // Scattered units are spread across every team rather than all wearing one
    // colour: the point of the scatter is to show what a populated map looks
    // like, and a real one is not single-army. Drawn from the same generator so
    // the whole scene stays reproducible from the seed alone.
    std::uniform_int_distribution<std::size_t> team{0, kTeamColours.size() - 1};

    for (std::size_t placed = 0; placed < count; ++placed) {
        for (int attempt = 0; attempt < kMaxAttemptsPerInstance; ++attempt) {
            const float x = alongX(rng);
            const float z = alongZ(rng);
            const float y = field.heightAtWorld(x, z);

            if (y < minHeight) {
                continue;  // underwater; try elsewhere
            }

            instances.push_back(UnitInstance{
                .position = {{x, y, z}},
                .rotationY = yaw(rng),
                .scale = scale,
                .teamColour = teamColour(team(rng)),
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

    // The nth start position belongs to the nth team, which is exactly how both
    // engines read the list, so the index is the team index.
    std::size_t team = 0;
    for (const mapinfo::StartPosition& start : positions) {
        instances.push_back(UnitInstance{
            .position = {{start.x, field.heightAtWorld(start.x, start.z), start.z}},
            // Unrotated: a spawn marker facing a consistent direction reads more
            // clearly than a random one, and there is no facing data to honour.
            .rotationY = 0.0f,
            .scale = scale,
            .teamColour = teamColour(team),
        });
        ++team;
    }

    return instances;
}

} // namespace rm
