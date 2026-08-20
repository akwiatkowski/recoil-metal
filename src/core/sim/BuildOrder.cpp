#include "core/sim/BuildOrder.hpp"

#include <cmath>

namespace rm::sim {

namespace {

/// How far a structure sits from the start position, in elmos, and how far the
/// slots fan apart. 40 out and ±28 across clears the factory's footprint (UEB0101
/// is the largest thing the script places) with room for the commander to walk
/// between; both are OUR numbers, chosen for the base to read as a base in a
/// screenshot rather than a pile.
constexpr rm::sim::Fx kSiteDistance = rm::sim::Fx::fromInt(40);
constexpr rm::sim::Fx kSiteFan = rm::sim::Fx::fromInt(28);

/// How far past the factory a fresh tank drives before waiting, in elmos.
/// Past the arrival radius (8.7) and the factory's own footprint, so the line
/// of waiting tanks forms outside the buildings rather than inside them.
constexpr rm::sim::Fx kRolloffDistance = rm::sim::Fx::fromInt(32);

/// The unit vector from `fromX/fromZ` toward `toX/toZ`, or +x when the two
/// coincide — a start position ON the map centre has no inward direction, and
/// any fixed one beats dividing by zero.
[[nodiscard]] std::array<rm::sim::Fx, 2> towards(rm::sim::Fx fromX, rm::sim::Fx fromZ,
                                                 rm::sim::Fx toX, rm::sim::Fx toZ) noexcept {
    const rm::sim::Fx dx = toX - fromX;
    const rm::sim::Fx dz = toZ - fromZ;
    const rm::sim::Fx length = rm::sim::fxHypot(dx, dz);
    if (length <= rm::sim::Fx{}) {
        // No direction to point in — the two positions coincide. +X is as good as any, and
        // being deterministic about which matters more than which it is.
        return {rm::sim::kFxOne, rm::sim::Fx{}};
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

bool launchesAttack(const Opponent& script, const ArmyView& view,
                    std::size_t waveSize) noexcept {
    return !script.attackLaunched && view.commanderAlive
           && view.tanksAlive >= waveSize;
}

std::array<Fx, 3> structureSite(const std::array<Fx, 3>& start, Fx centreX, Fx centreZ,
                                int slot) noexcept {
    const std::array<Fx, 2> in = towards(start[0], start[2], centreX, centreZ);
    // Perpendicular to the inward direction, so the slots fan across it.
    const std::array<Fx, 2> across{-in[1], in[0]};
    // Slots 0, 1, 2... sit at fan offsets +1, -1, +2, -2... so two structures
    // straddle the inward line rather than queueing along it.
    const int step = slot / 2 + 1;
    const Fx fan = Fx::fromInt(slot % 2 == 0 ? step : -step);
    return {
        start[0] + in[0] * kSiteDistance + across[0] * kSiteFan * fan,
        start[1],
        start[2] + in[1] * kSiteDistance + across[1] * kSiteFan * fan,
    };
}

std::array<Fx, 2> rolloffPoint(const std::array<Fx, 3>& factory, Fx centreX,
                               Fx centreZ) noexcept {
    const std::array<Fx, 2> in = towards(factory[0], factory[2], centreX, centreZ);
    return {factory[0] + in[0] * kRolloffDistance,
            factory[2] + in[1] * kRolloffDistance};
}

} // namespace rm::sim
