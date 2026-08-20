#include "core/unit/Weapon.hpp"

#include "core/map/Scmap.hpp"
#include "core/sim/Movement.hpp"

#include <algorithm>
#include <cmath>

namespace rm::unitdef {
namespace {

[[nodiscard]] float numberOr(const lua::Value& table, std::string_view key,
                             float fallback) noexcept {
    const std::optional<double> value = table.numberAt(key);
    return value ? static_cast<float>(*value) : fallback;
}

} // namespace

std::optional<BallisticArc> ballisticArcFromName(std::string_view name) noexcept {
    if (name == "RULEUBA_None") {
        return BallisticArc::None;
    }
    if (name == "RULEUBA_LowArc") {
        return BallisticArc::Low;
    }
    if (name == "RULEUBA_HighArc") {
        return BallisticArc::High;
    }
    return std::nullopt;
}

WeaponRole weaponRoleFromCategory(std::string_view category) noexcept {
    // Matched on the whole string, because the categories overlap as prefixes:
    // "Direct Fire", "Direct Fire Naval" and "Direct Fire Experimental" are three
    // different things and a `starts_with` chain would read all three as the first.
    if (category == "Death") {
        return WeaponRole::Death;
    }
    if (category == "Direct Fire" || category == "Direct Fire Naval"
        || category == "Direct Fire Experimental" || category == "Defense") {
        return WeaponRole::DirectFire;
    }
    if (category == "Anti Air") {
        return WeaponRole::AntiAir;
    }
    if (category == "Artillery") {
        return WeaponRole::Artillery;
    }
    return WeaponRole::Other;
}

int Weapon::reloadTicks() const noexcept {
    if (rateOfFire <= 0.0f) {
        return 1;
    }
    const float ticks = static_cast<float>(sim::kTicksPerSecond) / rateOfFire;
    return std::max(1, static_cast<int>(std::lround(ticks)));
}

std::vector<Weapon> weaponsFrom(const lua::Value& weaponArray) {
    std::vector<Weapon> weapons;
    weapons.reserve(weaponArray.items.size());

    for (const lua::Value& entry : weaponArray.items) {
        Weapon weapon;

        weapon.label = std::string{entry.stringAt("Label").value_or("")};
        weapon.role =
            weaponRoleFromCategory(entry.stringAt("WeaponCategory").value_or("(none)"));
        if (const std::optional<std::string_view> arc = entry.stringAt("BallisticArc")) {
            weapon.arc = ballisticArcFromName(*arc).value_or(BallisticArc::None);
        }

        weapon.damage = numberOr(entry, "Damage", 0.0f);

        // Ogrids to elmos throughout, the same x8 everything else in this family takes.
        // A NEGATIVE radius appears once in the corpus and is read as a point hit: a
        // blast that pulls inward is not a thing, and clamping is honest where trusting
        // it would make the falloff divide by a negative and heal whatever it hit.
        weapon.damageRadiusElmos =
            std::max(0.0f, numberOr(entry, "DamageRadius", 0.0f)) * scmap::kElmosPerOgrid;

        weapon.maxRangeElmos = numberOr(entry, "MaxRadius", 0.0f) * scmap::kElmosPerOgrid;
        weapon.minRangeElmos =
            std::max(0.0f, numberOr(entry, "MinRadius", 0.0f)) * scmap::kElmosPerOgrid;

        // The rings, in the same ogrids everything else is stated in.
        weapon.innerRingDamage = numberOr(entry, "NukeInnerRingDamage", 0.0f);
        weapon.innerRingRadiusElmos =
            std::max(0.0f, numberOr(entry, "NukeInnerRingRadius", 0.0f)) * scmap::kElmosPerOgrid;
        weapon.outerRingDamage = numberOr(entry, "NukeOuterRingDamage", 0.0f);
        weapon.outerRingRadiusElmos =
            std::max(0.0f, numberOr(entry, "NukeOuterRingRadius", 0.0f)) * scmap::kElmosPerOgrid;

        weapon.rateOfFire = numberOr(entry, "RateOfFire", 0.0f);
        weapon.muzzleVelocityElmosPerSecond =
            numberOr(entry, "MuzzleVelocity", 0.0f) * scmap::kElmosPerOgrid;

        weapon.firingToleranceDegrees =
            numberOr(entry, "FiringTolerance", kDefaultFiringToleranceDegrees);

        if (const lua::Value* turreted = entry.find("Turreted")) {
            weapon.turreted = turreted->asBoolean().value_or(false);
        }

        weapons.push_back(std::move(weapon));
    }

    return weapons;
}

} // namespace rm::unitdef
