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

    // The intel radii, converted out of the content's floats once (ADR-037). Same reason
    // the economy's rates are derived here: the alternative is a float conversion per
    // emitter per update, in a pass `tools/check_no_sim_floats.sh` forbids floats in.
    IntelRadii intel{};
    if (def != nullptr) {
        intel.vision = fxFromFloat(def->visionRadiusElmos);
        intel.radar = fxFromFloat(def->radarRadiusElmos);
        intel.sonar = fxFromFloat(def->sonarRadiusElmos);
        intel.omni = fxFromFloat(def->omniRadiusElmos);
        intel.radarStealth = def->radarStealth;
        intel.sonarStealth = def->sonarStealth;
        intel.cloak = def->cloak;
        intel.freeIntel = def->freeIntel;
    }
    intel_.push_back(intel);

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
                .damage = damageFor(weapon),
            });
        }
    }
    weapons_.push_back(std::move(weapons));

    // What the unit is made of, resolved from the name the blueprint states. A catalog with no
    // armour context resolves everything to `kDefaultArmor`, which is the pre-P10.1 engine.
    armor_.push_back(def != nullptr ? armor_names_.classFor(def->armorType) : kDefaultArmor);

    return type;
}

unitdef::DamageProfile UnitCatalog::profileFor(const unitdef::Weapon& weapon,
                                               Mag amount) const {
    if (armor_matrix_.empty()) {
        return unitdef::flatDamage(amount);
    }
    const std::string_view damageType =
        weapon.damageType.empty() ? std::string_view{"Normal"}
                                  : std::string_view{weapon.damageType};
    return unitdef::damageFromMatrix(amount, damageType, armor_matrix_);
}

unitdef::DamageProfile UnitCatalog::damageFor(const unitdef::Weapon& weapon) const {
    // NO MATRIX, NO TRANSPOSE. Two cases arrive here with an empty one and both are correct:
    // a catalog that was never given armour context (every existing test), and BAR content,
    // which states absolute damage per armour class in its own weapon defs rather than a
    // multiplier table. In both the profile is the scalar the weapon authored.
    if (armor_matrix_.empty()) {
        return unitdef::flatDamage(weapon.damage);
    }

    // An unstated `DamageType` is `Normal`. That is not a guess: `Normal` is what 454 of the
    // 494 shipped weapons say explicitly (`02 §9.6`), and its multiplier is 1.0 against every
    // class — so reading a missing field as anything else would give a handful of weapons a
    // silent bonus or penalty that no blueprint asked for.
    const std::string_view damageType =
        weapon.damageType.empty() ? std::string_view{"Normal"}
                                  : std::string_view{weapon.damageType};

    return unitdef::damageFromMatrix(weapon.damage, damageType, armor_matrix_);
}

void UnitCatalog::setArmor(unitdef::ArmorRegistry registry,
                           std::vector<unitdef::ArmorMultiplier> matrix) {
    armor_names_ = std::move(registry);
    armor_matrix_ = std::move(matrix);
}

} // namespace rm::sim
