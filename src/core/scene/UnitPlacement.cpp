#include "core/scene/UnitPlacement.hpp"
#include "core/model/BuilderAim.hpp"

#include <cmath>
#include <numbers>
#include <random>

namespace {

/// Cap on resampling attempts per requested instance. A map that is almost
/// entirely underwater would otherwise spin here.
constexpr int kMaxAttemptsPerInstance = 32;

} // namespace

namespace rm {

std::optional<float> UnitInstance::barrelAlignmentErrorDegrees(
    const BuilderAimRig& rig, std::size_t barrelBone,
    const std::array<float, 3>& barrelBase, const std::array<float, 3>& barrelTip,
    const std::array<float, 3>& worldDirection) const noexcept {
    if (barrelBone >= rig.boneFlags.size() || !std::isfinite(scale) || scale <= 0.0f) {
        return std::nullopt;
    }
    const BuilderAimAngles angles{.yaw = turretYaw, .pitch = turretPitch,
                                  .yaw2 = turretYaw2, .pitch2 = turretPitch2};
    const auto base = applyBuilderAim(barrelBase, rig.boneFlags[barrelBone], rig, angles);
    const auto tip = applyBuilderAim(barrelTip, rig.boneFlags[barrelBone], rig, angles);
    // Translation and positive uniform scale cannot change an angle. Omit them
    // to avoid losing a short barrel's precision far from the world origin.
    const InstancePlacement orientation{
        .rotationX = rotationX, .rotationY = rotationY, .rotationZ = rotationZ};
    const auto axis = boneWorldPosition(
        {.translation = {tip[0] - base[0], tip[1] - base[1], tip[2] - base[2]}},
        orientation);
    std::array<double, 3> b{}, v{};
    for (std::size_t i = 0; i < 3; ++i) {
        if (!std::isfinite(axis[i]) || !std::isfinite(worldDirection[i])) {
            return std::nullopt;
        }
        b[i] = axis[i];
        v[i] = worldDirection[i];
    }
    const double barrelLength = std::hypot(b[0], b[1], b[2]);
    const double speed = std::hypot(v[0], v[1], v[2]);
    if (barrelLength == 0.0 || speed == 0.0) return std::nullopt;
    for (std::size_t i = 0; i < 3; ++i) {
        b[i] /= barrelLength;
        v[i] /= speed;
    }
    const double crossLength = std::hypot(b[1]*v[2] - b[2]*v[1],
                                         b[2]*v[0] - b[0]*v[2],
                                         b[0]*v[1] - b[1]*v[0]);
    const double dot = b[0]*v[0] + b[1]*v[1] + b[2]*v[2];
    return static_cast<float>(std::atan2(crossLength, dot) * 180.0 / std::numbers::pi);
}

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
    // Drawn from the same generator, so a scattered crowd is desynchronised
    // reproducibly rather than by wall time.
    std::uniform_real_distribution<float> phase{0.0f, 1.0f};

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
                .animationPhase = phase(rng),
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
            // Spread evenly around the cycle rather than left at zero. Facing
            // wants consistency; animation is the opposite — a row of bots
            // stepping in perfect time reads as one unit drawn several times.
            // Even spacing rather than random keeps this reproducible without
            // needing a generator here.
            .animationPhase = positions.empty()
                                  ? 0.0f
                                  : static_cast<float>(team)
                                        / static_cast<float>(positions.size()),
        });
        ++team;
    }

    return instances;
}

} // namespace rm
