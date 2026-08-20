#include "core/sim/BuildOrder.hpp"

#include <cmath>

namespace rm::sim {

namespace {

/// How far a structure sits from the start position, in elmos, and how far the
/// slots fan apart. 40 out and ±28 across clears the factory's footprint (UEB0101
/// is the largest thing the script places) with room for the commander to walk
/// between; both are OUR numbers, chosen for the base to read as a base in a
/// screenshot rather than a pile.
constexpr float kSiteDistanceElmos = 40.0f;
constexpr float kSiteFanElmos = 28.0f;

/// How far past the factory a fresh tank drives before waiting, in elmos.
/// Past the arrival radius (8.7) and the factory's own footprint, so the line
/// of waiting tanks forms outside the buildings rather than inside them.
constexpr float kRolloffDistanceElmos = 32.0f;

/// The unit vector from `fromX/fromZ` toward `toX/toZ`, or +x when the two
/// coincide — a start position ON the map centre has no inward direction, and
/// any fixed one beats dividing by zero.
[[nodiscard]] std::array<float, 2> towards(float fromX, float fromZ, float toX,
                                           float toZ) noexcept {
    const float dx = toX - fromX;
    const float dz = toZ - fromZ;
    const float length = std::sqrt(dx * dx + dz * dz);
    if (length <= 0.0f) {
        return {1.0f, 0.0f};
    }
    return {dx / length, dz / length};
}

} // namespace

StructureOrder nextStructure(const ArmyView& view) noexcept {
    if (!view.commanderAlive || view.commanderBusy) {
        return StructureOrder::None;
    }
    // Wait for the spawn-ordered extractor: starting the power generator beside a
    // quarter-funded extractor stalls both, and the extractor is what unstalls
    // everything else.
    if (view.extractorsStanding == 0) {
        return StructureOrder::None;
    }
    if (view.powerGeneratorsStanding == 0) {
        return StructureOrder::PowerGenerator;
    }
    if (view.extractorsStanding < 2) {
        return StructureOrder::Extractor;
    }
    if (view.factoriesStanding == 0) {
        return StructureOrder::Factory;
    }
    return StructureOrder::None;
}

bool wantsTank(const ArmyView& view) noexcept {
    return view.commanderAlive && view.factoriesStanding > 0 && !view.factoryBusy;
}

bool launchesAttack(const Opponent& script, const ArmyView& view) noexcept {
    return !script.attackLaunched && view.commanderAlive
           && view.tanksAlive >= kAttackWaveTanks;
}

std::array<float, 3> structureSite(const std::array<float, 3>& start, float centreX,
                                   float centreZ, int slot) noexcept {
    const std::array<float, 2> in = towards(start[0], start[2], centreX, centreZ);
    // Perpendicular to the inward direction, so the slots fan across it.
    const std::array<float, 2> across{-in[1], in[0]};
    // Slots 0, 1, 2... sit at fan offsets +1, -1, +2, -2... so two structures
    // straddle the inward line rather than queueing along it.
    const int step = slot / 2 + 1;
    const float fan = (slot % 2 == 0 ? 1.0f : -1.0f) * static_cast<float>(step);
    return {
        start[0] + in[0] * kSiteDistanceElmos + across[0] * kSiteFanElmos * fan,
        start[1],
        start[2] + in[1] * kSiteDistanceElmos + across[1] * kSiteFanElmos * fan,
    };
}

std::array<float, 2> rolloffPoint(const std::array<float, 3>& factory, float centreX,
                                  float centreZ) noexcept {
    const std::array<float, 2> in = towards(factory[0], factory[2], centreX, centreZ);
    return {factory[0] + in[0] * kRolloffDistanceElmos,
            factory[2] + in[1] * kRolloffDistanceElmos};
}

} // namespace rm::sim
