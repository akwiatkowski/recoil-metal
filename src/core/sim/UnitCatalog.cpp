#include "core/sim/UnitCatalog.hpp"

#include "core/map/Scmap.hpp"

#include <algorithm>
#include <cstdlib>
#include <utility>

namespace rm::sim {
namespace {

/// The receiver-size row for a definition: the authored `SIZE<n>` category when stated,
/// else `SkirtSizeX + SkirtSizeZ` rounded to the nearest step — the same arithmetic the
/// corpus's own authoring follows (a 2×2 skirt is SIZE4, the factory's 8×8 is SIZE16;
/// the one hand-authored outlier, UEB0103's 12×14 skirt marked SIZE16, is why the
/// category wins when present).
[[nodiscard]] std::uint8_t adjacencySizeIndex(const unitdef::UnitDef& def) noexcept {
    static constexpr std::string_view kSizes[] = {"SIZE4", "SIZE8", "SIZE12", "SIZE16",
                                                  "SIZE20"};
    for (std::size_t i = 0; i < std::size(kSizes); ++i) {
        for (const std::string& category : def.categories) {
            if (category == kSizes[i]) {
                return static_cast<std::uint8_t>(i);
            }
        }
    }
    const float sum = def.skirtSquaresX + def.skirtSquaresZ;
    const auto step = static_cast<int>((sum + 2.0f) / 4.0f);  // nearest of 4,8,12,16,20
    return static_cast<std::uint8_t>(std::clamp(step - 1, 0, 4));
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
    }
    rates_.push_back(derived);

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
        adjacency.sizeIndex = adjacencySizeIndex(*def);
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
        shield.radiusElmos = def->shield.radiusElmos;
        shield.verticalOffsetElmos = def->shield.verticalOffsetElmos;
        shield.regenPerTick = rate.magPerTick(def->shield.regenPerSecond);
        shield.regenDelay = rate.ticks(def->shield.regenDelay);
        shield.recharge = rate.ticks(def->shield.rechargeDelay);
        largestShieldRadius_ = std::max(largestShieldRadius_, shield.radiusElmos);
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
