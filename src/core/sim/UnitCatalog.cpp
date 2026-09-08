#include "core/sim/UnitCatalog.hpp"
#include "core/unit/BuildTree.hpp"

#include "core/map/Scmap.hpp"

#include <algorithm>
#include <cstdlib>
#include <optional>
#include <string_view>
#include <utility>

namespace rm::sim {
namespace {

/// The authored receiver-size row. `SIZE` categories are a content contract, not an
/// inference from skirt geometry: the latter only answers whether two units touch.
[[nodiscard]] std::optional<std::uint8_t> adjacencySizeIndex(const unitdef::UnitDef& def) noexcept {
    static constexpr std::string_view kSizes[] = {"SIZE4", "SIZE8", "SIZE12", "SIZE16",
                                                   "SIZE20"};
    std::optional<std::uint8_t> size;
    for (const std::string& category : def.categories) {
        if (category.starts_with("SIZE")) {
            bool valid = false;
            for (std::size_t i = 0; i < std::size(kSizes); ++i) {
            if (category == kSizes[i]) {
                    valid = true;
                    if (size.has_value()) {
                        return std::nullopt;
                    }
                    size = static_cast<std::uint8_t>(i);
                    break;
                }
            }
            if (!valid) {
                return std::nullopt;
            }
        }
    }
    return size;
}

} // namespace

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
        derived.buildReachElmos = fxFromFloat(def->buildDistanceElmos);
        derived.buildFootprintElmos = Fx::fromInt(
            std::min(def->footprintSquaresX, def->footprintSquaresZ) * kSquareSize);
        derived.buildSkirtElmos = fxFromFloat(
            std::max(def->skirtSquaresX, def->skirtSquaresZ) * scmap::kElmosPerOgrid);
        derived.regenPerTick = rate.magPerTick(def->regenPerSecond);
    }
    rates_.push_back(derived);
    auto& enhancements = enhancements_.emplace_back();
    if (def != nullptr) {
        for (const auto& spec : def->enhancements) {
            EnhancementEffects effects;
            if (const auto value = spec.parameters.numberAt("NewBuildRate")) {
                effects.buildPerTick = rate.magPerTick(static_cast<float>(*value));
            }
            if (const auto value = spec.parameters.numberAt("NewHealth")) {
                effects.healthAdd = magFromFloat(static_cast<float>(*value));
            }
            if (const auto value = spec.parameters.numberAt("NewRegenRate")) {
                effects.regenPerTickAdd = rate.magPerTick(static_cast<float>(*value));
            }
            if (const auto adds = spec.parameters.stringAt("BuildableCategoryAdds")) {
                effects.buildableAdds = unitdef::parseCategoryTerm(*adds);
            }
            enhancements.emplace(spec.name, std::move(effects));
        }
    }


    // Adjacency, out of the content's floats once (same boundary as everything above).
    // Mobile units and skirtless structures get the zero entry, which is also what keeps
    // them out of the pair scan.
    AdjacencyInfo adjacency{};
    if (def != nullptr && def->skirtSquaresX > 0.0f && def->skirtSquaresZ > 0.0f
        && !def->isMobile()) {
        adjacency.skirtHalfXElmos =
            fxFromFloat(0.5f * def->skirtSquaresX * scmap::kElmosPerOgrid);
        adjacency.skirtHalfZElmos =
            fxFromFloat(0.5f * def->skirtSquaresZ * scmap::kElmosPerOgrid);
        adjacency.skirtCentreOffsetXElmos =
            fxFromFloat(def->skirtCentreOffsetSquaresX * scmap::kElmosPerOgrid);
        adjacency.skirtCentreOffsetZElmos =
            fxFromFloat(def->skirtCentreOffsetSquaresZ * scmap::kElmosPerOgrid);
        const bool structure = std::find(def->categories.begin(), def->categories.end(), "STRUCTURE")
                               != def->categories.end();
        if (const std::optional<std::uint8_t> size = adjacencySizeIndex(*def); structure && size) {
            adjacency.sizeIndex = *size;
            adjacency.receives = true;
        }
        const unitdef::AdjacencyGrants& grants = unitdef::adjacencyGrants(
            unitdef::adjacencyClassFromName(def->adjacencyBuffs));
        for (std::size_t i = 0; i < unitdef::kAdjacencySizeSteps; ++i) {
            adjacency.givesMassProduction[i] = fxFromFloat(grants.massProduction[i]);
            adjacency.givesEnergyProduction[i] = fxFromFloat(grants.energyProduction[i]);
            adjacency.givesEnergyUpkeep[i] = fxFromFloat(grants.energyMaintenance[i]);
        }
    }
    adjacency_.push_back(adjacency);

    // The intel radii, converted out of the content's floats once (ADR-037). Same reason
    // the economy's rates are derived here: the alternative is a float conversion per
    // emitter per update, in a pass `tools/check_no_sim_floats.sh` forbids floats in.
    IntelRadii intel{};
    if (def != nullptr) {
        intel.vision = fxFromFloat(def->visionRadiusElmos);
        intel.radar = fxFromFloat(def->radarRadiusElmos);
        intel.sonar = fxFromFloat(def->sonarRadiusElmos);
        intel.omni = fxFromFloat(def->omniRadiusElmos);
        intel.eyeHeight = fxFromFloat(def->sizeYElmos);
        intel.radarStealth = def->radarStealth;
        intel.sonarStealth = def->sonarStealth;
        intel.cloak = def->cloak;
        intel.freeIntel = def->freeIntel;
        intel.radarStealthField = fxFromFloat(def->radarStealthFieldRadiusElmos);
        intel.sonarStealthField = fxFromFloat(def->sonarStealthFieldRadiusElmos);
        intel.jamRadius = fxFromFloat(def->jamRadiusElmos);
        intel.jammerBlips = def->jammerBlips;
    }
    intel_.push_back(intel);

    ShieldInfo shield{};
    if (def != nullptr && def->shield.exists()) {
        shield.maximum = def->shield.maximum;
        shield.shape = def->shield.shape;
        shield.radiusElmos = def->shield.radiusElmos;
        shield.verticalOffsetElmos = def->shield.verticalOffsetElmos;
        shield.boxHalfExtentsElmos = def->shield.boxHalfExtentsElmos;
        shield.collisionCenterElmos = def->shield.collisionCenterElmos;
        shield.boundingRadiusElmos = shield.shape == unitdef::ShieldShape::Sphere
                                       ? shield.radiusElmos
                                       : fxSqrt(shield.boxHalfExtentsElmos[0]
                                                    * shield.boxHalfExtentsElmos[0]
                                                + shield.boxHalfExtentsElmos[1]
                                                    * shield.boxHalfExtentsElmos[1]
                                                + shield.boxHalfExtentsElmos[2]
                                                    * shield.boxHalfExtentsElmos[2]);
        shield.regenPerTick = rate.magPerTick(def->shield.regenPerSecond);
        shield.regenDelay = rate.ticks(def->shield.regenDelay);
        shield.recharge = rate.ticks(def->shield.rechargeDelay);
        largestShieldRadius_ = std::max(largestShieldRadius_, shield.boundingRadiusElmos);
    }
    shields_.push_back(shield);

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
