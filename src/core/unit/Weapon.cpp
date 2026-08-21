#include "core/unit/Weapon.hpp"

#include "core/unit/FaDuration.hpp"

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

int Weapon::reloadTicks(sim::TickRate rate) const noexcept {
    if (rateOfFire <= 0.0f) {
        return 1;
    }
    // Shots per second inverted into seconds between shots, then converted by the rate.
    // `TickRate::ticks` already floors at one tick, for exactly the reason this function used
    // to do so itself: a weapon whose interval rounds to zero fires every tick, which is a
    // different mechanic rather than a fast one.
    return static_cast<int>(rate.ticks(sim::seconds(1.0f / rateOfFire)));
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

        weapon.damage = sim::magFromFloat(numberOr(entry, "Damage", 0.0f));
        weapon.damageType = std::string{entry.stringAt("DamageType").value_or("")};

        // Ogrids to elmos throughout, the same x8 everything else in this family takes.
        // A NEGATIVE radius appears once in the corpus and is read as a point hit: a
        // blast that pulls inward is not a thing, and clamping is honest where trusting
        // it would make the falloff divide by a negative and heal whatever it hit.
        weapon.damageRadius = sim::fxFromFloat(
            std::max(0.0f, numberOr(entry, "DamageRadius", 0.0f)) * scmap::kElmosPerOgrid);

        weapon.maxRange = sim::fxFromFloat(numberOr(entry, "MaxRadius", 0.0f)
                                           * scmap::kElmosPerOgrid);
        weapon.minRange = sim::fxFromFloat(
            std::max(0.0f, numberOr(entry, "MinRadius", 0.0f)) * scmap::kElmosPerOgrid);

        // The rings, in the same ogrids everything else is stated in.
        weapon.innerRingDamage = sim::magFromFloat(numberOr(entry, "NukeInnerRingDamage", 0.0f));
        weapon.innerRingRadius = sim::fxFromFloat(
            std::max(0.0f, numberOr(entry, "NukeInnerRingRadius", 0.0f))
            * scmap::kElmosPerOgrid);
        weapon.outerRingDamage = sim::magFromFloat(numberOr(entry, "NukeOuterRingDamage", 0.0f));
        weapon.outerRingRadius = sim::fxFromFloat(
            std::max(0.0f, numberOr(entry, "NukeOuterRingRadius", 0.0f))
            * scmap::kElmosPerOgrid);

        weapon.rateOfFire = numberOr(entry, "RateOfFire", 0.0f);

        // The burst. `MuzzleSalvoSize` is a Lua LOOP COUNT (`:1036`), not a duration, so it is
        // taken as stated; `MuzzleSalvoDelay` is a `WaitSeconds` argument and is corrected.
        //
        // The `> 0` guard mirrors the game's own at `DefaultProjectileWeapon.lua:1130`. Without
        // it every one of the 304 weapons stating a delay of exactly zero would acquire a
        // 100 ms gap between muzzles that the game does not give them.
        weapon.burstSize = std::max(1, static_cast<int>(numberOr(entry, "MuzzleSalvoSize", 1.0f)));
        const float authoredDelay = numberOr(entry, "MuzzleSalvoDelay", 0.0f);
        if (authoredDelay > 0.0f) {
            weapon.burstDelay = faWaitSeconds(sim::seconds(authoredDelay));
        }

        weapon.muzzleVelocityElmosPerSecond =
            numberOr(entry, "MuzzleVelocity", 0.0f) * scmap::kElmosPerOgrid;

        weapon.firingToleranceDegrees =
            numberOr(entry, "FiringTolerance", kDefaultFiringToleranceDegrees);

        if (const lua::Value* turreted = entry.find("Turreted")) {
            weapon.turreted = turreted->asBoolean().value_or(false);
        }
        if (const lua::Value* manual = entry.find("ManualFire")) {
            weapon.manualFire = manual->asBoolean().value_or(false);
        }
        // Any stated enhancement gates the weapon: which one it is does not matter
        // to an engine that builds none of them.
        weapon.enabledByEnhancement = entry.stringAt("EnabledByEnhancement").has_value();

        weapons.push_back(std::move(weapon));
    }

    return weapons;
}

} // namespace rm::unitdef
