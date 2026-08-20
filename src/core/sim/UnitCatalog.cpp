#include "core/sim/UnitCatalog.hpp"

#include <utility>

namespace rm::sim {

UnitTypeIndex UnitCatalog::add(const unitdef::UnitDef* def, TickRate rate) {
    const auto type = static_cast<UnitTypeIndex>(defs_.size());
    defs_.push_back(def);

    // The one place a per-second rate becomes a per-tick amount. Null defs get zeroes rather
    // than being rejected: a decorative crowd has no definition, earns nothing, and "no def"
    // has to be representable.
    Rates derived{};
    if (def != nullptr) {
        derived.massPerTick = rate.magPerTick(def->producesMassPerSecond);
        derived.energyPerTick = rate.magPerTick(def->producesEnergyPerSecond);
        derived.upkeepEnergyPerTick = rate.magPerTick(def->upkeepEnergyPerSecond);
        derived.buildPerTick = rate.magPerTick(def->buildRate);
    }
    rates_.push_back(derived);

    std::vector<WeaponRates> weapons;
    if (def != nullptr) {
        weapons.reserve(def->weapons.size());
        for (const unitdef::Weapon& weapon : def->weapons) {
            const auto reload = static_cast<TickCount>(weapon.reloadTicks(rate));

            // A non-bursting weapon gets its own reload as its burst gap and a size of one, so
            // the firing pass reads the same two fields either way. The alternative — zero or a
            // sentinel — puts a branch in the inner loop to mean "actually use the other one".
            weapons.push_back(WeaponRates{
                .muzzlePerTick = rate.perTick(weapon.muzzleVelocityElmosPerSecond),
                .reloadTicks = reload,
                .burstDelayTicks =
                    weapon.bursts() ? rate.ticks(weapon.burstDelay) : reload,
                .burstSize = weapon.bursts() ? weapon.burstSize : 1,
            });
        }
    }
    weapons_.push_back(std::move(weapons));

    return type;
}

} // namespace rm::sim
