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
            weapons.push_back(WeaponRates{
                .muzzlePerTick = rate.perTick(weapon.muzzleVelocityElmosPerSecond),
                .reloadTicks = static_cast<TickCount>(weapon.reloadTicks(rate)),
            });
        }
    }
    weapons_.push_back(std::move(weapons));

    return type;
}

} // namespace rm::sim
