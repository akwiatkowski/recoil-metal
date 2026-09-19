#include "core/sim/Enhancement.hpp"
#include "core/sim/CommandInternal.hpp"
#include "core/unit/BuildTree.hpp"
#include <algorithm>
#include <array>

namespace rm::sim {
Mag effectiveBuildPerTick(const UnitStore& store, const UnitCatalog& catalog, UnitIndex slot) noexcept {
    Mag result = catalog.rates(store.typeAt(slot)).buildPerTick;
    for (const auto& [position, name] : store.enhancements()[slot]) {
        const auto* effects = catalog.enhancementEffects(store.typeAt(slot), name);
        if (effects && effects->buildPerTick) result = *effects->buildPerTick;
    }
    // C-360's `CheatBuildRate` (`CheatBuffs.lua`: BuildRate Mult 2.0, Stacks='ALWAYS',
    // Duration=-1): retail's buff multiplies the unit's `GetBuildRate`, which is what
    // every consumer of this figure — construction, assistance, reclaim, repair,
    // capture, enhancement work — reads. Doubling HERE rather than at each call site is
    // the buff's own shape: one multiplier on the unit, not a rule each consumer
    // remembers to apply.
    if (store.cheatBuffedAt(slot)) result += result;
    return result;
}
Mag enhancementHealthAdd(const UnitStore& store, const UnitCatalog& catalog, UnitIndex slot) noexcept {
    Mag result{};
    for (const auto& [position, name] : store.enhancements()[slot]) {
        if (const auto* effects = catalog.enhancementEffects(store.typeAt(slot), name)) result += effects->healthAdd;
    }
    return result;
}
Mag enhancementRegenPerTick(const UnitStore& store, const UnitCatalog& catalog, UnitIndex slot) noexcept {
    Mag result{};
    for (const auto& [position, name] : store.enhancements()[slot]) {
        if (const auto* effects = catalog.enhancementEffects(store.typeAt(slot), name)) result += effects->regenPerTickAdd;
    }
    return result;
}
bool canBuild(const UnitStore& store, const UnitCatalog& catalog, UnitIndex slot,
              const unitdef::UnitDef& product) {
    const auto* def = catalog.def(store.typeAt(slot));
    if (!def) return false;
    // ACU OnCreate restricts the advanced commander categories present in the base BP.
    const bool engineeringRestricted = def->hasCategory("COMMAND")
        && (product.hasCategory("BUILTBYTIER2COMMANDER") || product.hasCategory("BUILTBYTIER3COMMANDER"));
    if (!engineeringRestricted && unitdef::matchesExpression(def->buildableCategory, product)) return true;
    for (const auto& [position, installed] : store.enhancements()[slot]) {
        const auto* spec = def->enhancement(installed);
        // T3 replaces the T2 buff, but does not restore the T2 build restriction.
        for (std::size_t visited = 0; spec && visited < def->enhancements.size(); ++visited) {
            const auto* effects = catalog.enhancementEffects(store.typeAt(slot), spec->name);
            if (effects && !effects->buildableAdds.empty()
                && unitdef::matchesExpression({effects->buildableAdds}, product)) return true;
            spec = def->enhancement(spec->prerequisite);
        }
    }
    return false;
}
std::expected<void, std::string> validateEnhancementSequence(
    const UnitStore& store, const UnitCatalog& catalog, UnitId unit, std::span<const std::string> sequence) {
    if (store.resolve(unit).state != UnitStore::HandleState::Alive) return std::unexpected("enhancement owner is dead");
    const auto* def = catalog.def(store.typeAt(unit.index));
    if (!def) return std::unexpected("enhancement owner has no blueprint");
    const auto& slots = store.enhancements()[unit.index];
    if (sequence.empty()) return std::unexpected("empty enhancement sequence");
    // C-255: every authored enhancement installs — the slot/prerequisite chain
    // is the whole gate (retail's `OnWorkBegin`), and the per-name effects are
    // the script table below plus the blueprint's own parameters. Names whose
    // effect has no sim home yet install and record like the rest.
    return def->validateEnhancements(slots, sequence);
}
namespace {

// `C-255`/`C-379`: the per-name effect table standing in for each unit script's
// `CreateEnhancement`/`XxxRemove` elseif chain. Only the effects the sim can
// honour are listed; weapon-label enables, intel-radius writes, shield/pod
// creation and weapon stat mods are script-side too but have no per-unit home
// in the sim yet, so their names carry an empty record — they still install
// and record, which is the registry half of the claim.
//
// The cap lists are the literal `AddCommandCap`/`RemoveCommandCap` and
// `AddToggleCap`/`RemoveToggleCap` arguments from the eight shipped scripts —
// including retail's asymmetries, like `CloakingGenerator` adding no toggle cap
// while its Remove takes one away (URL0001/URL0301).
constexpr std::string_view kCapTeleport[]{"RULEUCC_Teleport"};
constexpr std::string_view kCapSacrifice[]{"RULEUCC_Sacrifice"};
constexpr std::string_view kCapOvercharge[]{"RULEUCC_Overcharge"};
constexpr std::string_view kCapTacticalPair[]{"RULEUCC_Tactical", "RULEUCC_SiloBuildTactical"};
constexpr std::string_view kCapNukePair[]{"RULEUCC_Nuke", "RULEUCC_SiloBuildNuke"};
constexpr std::string_view kCapAllSilo[]{
    "RULEUCC_Nuke", "RULEUCC_SiloBuildNuke", "RULEUCC_Tactical", "RULEUCC_SiloBuildTactical"};
constexpr std::string_view kToggleShield[]{"RULEUTC_ShieldToggle"};
constexpr std::string_view kToggleJamming[]{"RULEUTC_JammingToggle"};
constexpr std::string_view kToggleCloak[]{"RULEUTC_CloakToggle"};

struct ScriptEffectRow {
    std::string_view name;
    EnhancementScriptEffects effects;
};

constexpr std::string_view kWeaponChrono[]{"ChronoDampener"};
constexpr std::string_view kWeaponMissile[]{"Missile"};
constexpr std::string_view kWeaponNMissile[]{"NMissile"};
constexpr std::string_view kWeaponTacMissile[]{"TacMissile"};
constexpr std::string_view kWeaponTacNuke[]{"TacNukeMissile"};
constexpr std::string_view kWeaponTacBoth[]{"TacMissile", "TacNukeMissile"};
constexpr std::string_view kWeaponOvercharge[]{"OverCharge"};

constexpr ScriptEffectRow kScriptEffects[] = {
    // Teleporter — all eight scripts: AddCommandCap / RemoveCommandCap.
    {"Teleporter", {.commandCapAdds = kCapTeleport}},
    {"TeleporterRemove", {.commandCapRemoves = kCapTeleport}},
    // UAL0301 Sacrifice.
    {"Sacrifice", {.commandCapAdds = kCapSacrifice}},
    {"SacrificeRemove", {.commandCapRemoves = kCapSacrifice}},
    // XSL0301 Overcharge: cap plus the weapon-label enable.
    {"Overcharge", {.commandCapAdds = kCapOvercharge,
                    .weaponEnables = kWeaponOvercharge}},
    {"OverchargeRemove", {.commandCapRemoves = kCapOvercharge,
                          .weaponDisables = kWeaponOvercharge}},
    // XSL0001/XSL0301 Missile and UEL0001's TacticalMissile/TacticalNukeMissile
    // pair — the nuke swap drops the tactical caps, disables TacMissile and
    // enables TacNukeMissile; both Removes silence the pair.
    {"Missile", {.commandCapAdds = kCapTacticalPair,
                 .weaponEnables = kWeaponMissile}},
    {"MissileRemove", {.commandCapRemoves = kCapTacticalPair,
                       .weaponDisables = kWeaponMissile}},
    {"TacticalMissile", {.commandCapAdds = kCapTacticalPair,
                         .weaponEnables = kWeaponTacMissile}},
    {"TacticalNukeMissile",
     {.commandCapAdds = kCapNukePair, .commandCapRemoves = kCapTacticalPair,
      .weaponEnables = kWeaponTacNuke, .weaponDisables = kWeaponTacMissile}},
    {"TacticalMissileRemove", {.commandCapRemoves = kCapAllSilo,
                               .weaponDisables = kWeaponTacBoth}},
    {"TacticalNukeMissileRemove", {.commandCapRemoves = kCapAllSilo,
                                   .weaponDisables = kWeaponTacBoth}},
    // UAL0001's ChronoDampener: the enhancement IS the weapon's label.
    {"ChronoDampener", {.weaponEnables = kWeaponChrono}},
    {"ChronoDampenerRemove", {.weaponDisables = kWeaponChrono}},
    // URL0301's NaniteMissileSystem enables the NMissile label.
    {"NaniteMissileSystem", {.weaponEnables = kWeaponNMissile}},
    {"NaniteMissileSystemRemove", {.weaponDisables = kWeaponNMissile}},
    // Shields: `Shield` adds the toggle cap everywhere; `ShieldHeavy` and
    // `ShieldGeneratorField` do not — the Remove branches still take it away.
    {"Shield", {.toggleCapAdds = kToggleShield}},
    {"ShieldRemove", {.toggleCapRemoves = kToggleShield}},
    {"ShieldHeavyRemove", {.toggleCapRemoves = kToggleShield}},
    {"ShieldGeneratorFieldRemove", {.toggleCapRemoves = kToggleShield}},
    // UEL0301's RadarJammer adds the jamming toggle; the stealth/cloak
    // generators ride the cloak toggle (URL0001/URL0301).
    {"RadarJammer", {.toggleCapAdds = kToggleJamming}},
    {"RadarJammerRemove", {.toggleCapRemoves = kToggleJamming}},
    {"StealthGenerator", {.toggleCapAdds = kToggleCloak}},
    {"StealthGeneratorRemove", {.toggleCapRemoves = kToggleCloak}},
    {"CloakingGeneratorRemove", {.toggleCapRemoves = kToggleCloak}},
    // `C-258`: the three `SetRegenRate` writes — absolute, not adds. UEL0001's
    // name is retail's own typo (`DamageStablization`); the Seraphim
    // `DamageStabilization` is a Regen buff and is NOT in this list.
    {"DamageStablization", {.regenRateOverride = true}},
    {"SelfRepairSystem", {.regenRateOverride = true}},
    {"SystemIntegrityCompensator", {.regenRateOverride = true}},
};

[[nodiscard]] bool capListHas(std::span<const std::string_view> list,
                              std::string_view cap) noexcept {
    return std::ranges::find(list, cap) != list.end();
}

} // namespace

EnhancementScriptEffects enhancementScriptEffects(std::string_view name) noexcept {
    for (const ScriptEffectRow& row : kScriptEffects) {
        if (row.name == name) return row.effects;
    }
    return {};
}

bool enhancementRegenIsOverride(std::string_view name) noexcept {
    return enhancementScriptEffects(name).regenRateOverride;
}

std::vector<std::string> enhancementOrderSequence(
    const UnitStore& store, const UnitCatalog& catalog, UnitId unit,
    std::string_view name) {
    const unitdef::UnitDef* def = catalog.def(store.typeAt(unit.index));
    const unitdef::EnhancementSpec* spec = def != nullptr ? def->enhancement(name) : nullptr;
    if (spec == nullptr) return {};
    const auto& slots = store.enhancements()[unit.index];
    const auto occupant = slots.find(spec->slot);
    if (occupant == slots.end() || occupant->second == spec->prerequisite) {
        // Empty slot, or the chain's own prerequisite — the install replaces
        // its occupant directly, no Remove needed (construction.lua:975-981).
        return {std::string{name}};
    }
    if (occupant->second == name) {
        return {};  // already installed: retail's click is a no-op
    }
    // Occupied by something else: `<occupant>Remove` then the new id, both
    // clear-queue — the two-command protocol (construction.lua:955-968).
    return {occupant->second + "Remove", std::string{name}};
}

bool unitHasCommandCap(const UnitStore& store, const UnitCatalog& catalog,
                       UnitIndex slot, std::string_view cap) noexcept {
    const unitdef::UnitDef* def = catalog.def(store.typeAt(slot));
    bool granted = def == nullptr || !def->commandCapsDeclared || def->hasCommandCap(cap);
    for (const auto& [position, installed] : store.enhancements()[slot]) {
        const EnhancementScriptEffects effects = enhancementScriptEffects(installed);
        if (capListHas(effects.commandCapAdds, cap)) granted = true;
        if (capListHas(effects.commandCapRemoves, cap)) granted = false;
    }
    return granted;
}

bool unitHasToggleCap(const UnitStore& store, const UnitCatalog& catalog,
                      UnitIndex slot, std::string_view cap) noexcept {
    const unitdef::UnitDef* def = catalog.def(store.typeAt(slot));
    bool granted = def != nullptr && def->toggleCapsDeclared && def->hasToggleCap(cap);
    for (const auto& [position, installed] : store.enhancements()[slot]) {
        const EnhancementScriptEffects effects = enhancementScriptEffects(installed);
        if (capListHas(effects.toggleCapAdds, cap)) granted = true;
        if (capListHas(effects.toggleCapRemoves, cap)) granted = false;
    }
    return granted;
}
Mag enhancementMaintenancePerTick(const UnitStore& store,
                                  const UnitCatalog& catalog, UnitIndex slot) noexcept {
    Mag result{};
    for (const auto& [position, name] : store.enhancements()[slot]) {
        if (const auto* effects = catalog.enhancementEffects(store.typeAt(slot), name)) {
            result += effects->maintenanceEnergyPerTick;
        }
    }
    return result;
}

bool weaponGatedByEnhancement(const UnitStore& store, const UnitCatalog& catalog,
                              UnitIndex slot, std::string_view label) noexcept {
    if (slot >= store.enhancements().size()) return false;
    const unitdef::UnitDef* def = catalog.def(store.typeAt(slot));
    if (def == nullptr) return false;
    for (const unitdef::EnhancementSpec& spec : def->enhancements) {
        const EnhancementScriptEffects effects = enhancementScriptEffects(spec.name);
        if (capListHas(effects.weaponEnables, label)
            || capListHas(effects.weaponDisables, label)) {
            return true;
        }
    }
    return false;
}

bool weaponEnabledForUnit(const UnitStore& store, const UnitCatalog& catalog,
                          UnitIndex slot, const unitdef::Weapon& weapon) noexcept {
    // Ungated weapons are always enabled; a gated one needs an installed
    // branch that enables its label and none that disables it — the
    // `SetWeaponEnabledByLabel` net effect (`C-255`/`C-379`).
    if (!weapon.enabledByEnhancement
        && !weaponGatedByEnhancement(store, catalog, slot, weapon.label)) {
        return true;
    }
    if (slot >= store.enhancements().size()) return false;
    bool enabled = false;
    for (const auto& [position, installed] : store.enhancements()[slot]) {
        const EnhancementScriptEffects effects = enhancementScriptEffects(installed);
        if (capListHas(effects.weaponEnables, weapon.label)) enabled = true;
        if (capListHas(effects.weaponDisables, weapon.label)) enabled = false;
    }
    return enabled;
}
Resources enhancementProductionPerTick(const UnitStore& store,
                                       const UnitCatalog& catalog, UnitIndex slot) noexcept {
    Resources result{};
    for (const auto& [position, name] : store.enhancements()[slot]) {
        if (const auto* effects = catalog.enhancementEffects(store.typeAt(slot), name)) {
            result.mass += effects->producesMassPerTick;
            result.energy += effects->producesEnergyPerTick;
        }
    }
    return result;
}

std::optional<Mag> enhancementRegenOverride(const UnitStore& store,
                                          const UnitCatalog& catalog, UnitIndex slot) noexcept {
    for (const auto& [position, name] : store.enhancements()[slot]) {
        if (const auto* effects = catalog.enhancementEffects(store.typeAt(slot), name);
            effects != nullptr && effects->regenPerTickOverride.has_value()) {
            return effects->regenPerTickOverride;
        }
    }
    return std::nullopt;
}

std::expected<void, std::string> canInstallEnhancement(
    const UnitStore& store, const UnitCatalog& catalog, UnitId unit, std::string_view name) {
    const std::array sequence{std::string{name}};
    return validateEnhancementSequence(store, catalog, unit, sequence);
}
std::expected<void, std::string> installEnhancement(
    UnitStore& store, const UnitCatalog& catalog, UnitId unit, std::string_view name) {
    if (const auto valid = canInstallEnhancement(store, catalog, unit, name); !valid) return valid;
    const auto* def = catalog.def(store.typeAt(unit.index));
    const auto* spec = def->enhancement(name);
    auto& slots = store.enhancements()[unit.index];
    slots[spec->slot] = name;
    std::erase_if(slots, [spec](const auto& entry) {
        return std::find(spec->removes.begin(), spec->removes.end(), entry.second) != spec->removes.end();
    });
    auto& health = store.health()[unit.index];
    // `C-258`'s last-writer-wins regen, replicated: a `SetRegenRate`
    // enhancement writes its absolute rate; a `XxxRemove` whose `removes` list
    // names one runs `RevertRegenRate`, which erases the regen buffs too; and
    // any other regen-affecting install — a buff add landing or leaving — is a
    // recompute that erases the direct write. Installs that touch no regen
    // leave the standing write alone.
    const EnhancementScriptEffects own = enhancementScriptEffects(name);
    const bool removesOverride = std::ranges::any_of(spec->removes, [](std::string_view removed) {
        return enhancementRegenIsOverride(removed);
    });
    const bool touchesRegenBuff =
        (catalog.enhancementEffects(store.typeAt(unit.index), name) != nullptr
         && catalog.enhancementEffects(store.typeAt(unit.index), name)->regenPerTickAdd > Mag{})
        || std::ranges::any_of(spec->removes, [&](std::string_view removed) {
               const auto* effects =
                   catalog.enhancementEffects(store.typeAt(unit.index), removed);
               return effects != nullptr && effects->regenPerTickAdd > Mag{};
           });
    if (own.regenRateOverride) {
        health.regenWrite = Health::RegenWrite::Overridden;
    } else if (removesOverride) {
        health.regenWrite = Health::RegenWrite::Reverted;
    } else if (touchesRegenBuff) {
        health.regenWrite = Health::RegenWrite::None;
    }
    const Mag previous = health.maximum;
    health.maximum = veterancyMaxHealth(def->health + enhancementHealthAdd(store, catalog, unit.index), health.veterancy.level);
    // Buff.lua preserves absolute damage when max HP grows, and clamps when it shrinks.
    health.current = health.maximum > previous ? health.current + health.maximum - previous
                                               : std::min(health.current, health.maximum);
    return {};
}
namespace {
/// `OnWorkBegin`, the row creation half: validates the slot/prerequisite chain and
/// pushes the funded-work row the economy allocator drains. Called only once the
/// unit is stationary — see the `Stopping` gate in `taskTick` (`C-376`).
[[nodiscard]] bool beginEnhancementWork(UnitStore& store, const UnitCatalog& catalog,
    std::vector<EnhancementWork>& work, UnitId unit, const std::string& name) {
    if (!canInstallEnhancement(store, catalog, unit, name)) return false;
    if (std::any_of(work.begin(), work.end(),
                    [unit](const auto& entry) { return entry.owner == unit; })) return false;
    const auto* spec = catalog.def(store.typeAt(unit.index))->enhancement(name);
    const auto pace = effectiveBuildPerTick(store, catalog, unit.index);
    if (pace <= Mag{}) return false;
    // C-253: retail's `OnWorkBegin` copies `BuildCostEnergy` into
    // `WorkItemBuildCostMass` (`Unit.lua:2006`), so the mass drain equals the
    // ENERGY cost, not the blueprint's mass cost. Replicated verbatim.
    work.push_back({.owner=unit, .name=name,
        .cost={spec->buildCostEnergy,spec->buildCostEnergy},
        .totalBuildTime=Mag::fromFx(spec->buildTime), .buildTimeRemaining=Mag::fromFx(spec->buildTime),
        .buildPerTick=pace});
    return true;
}
} // namespace
void EnhancementTasks::onCreate(UnitId, std::string_view,
    std::span<const std::uint8_t>, ScriptTaskState&) {
    // Retail's EnhanceTask has no OnCreate work: the task opens in `Stopping` and
    // `OnWorkBegin` fires only after the unit stands still. Both live in
    // `taskTick` below, so creation is a no-op here (`C-376`).
}
std::int32_t EnhancementTasks::taskTick(UnitId unit, std::string_view task,
    std::span<const std::uint8_t> data, ScriptTaskState&) {
    if (task != "EnhanceTask"
        || store_.resolve(unit).state != UnitStore::HandleState::Alive) {
        return static_cast<std::int32_t>(ScriptTaskStatus::Abort);
    }
    const std::string name(data.begin(), data.end());
    const auto work = std::find_if(work_.begin(), work_.end(), [&](const auto& entry) {
        return entry.owner == unit && entry.name == name;
    });
    if (work == work_.end()) {
        // `Stopping` (`C-376`): a mobile unit still under way is halted first —
        // retail's `Navigator:AbortMove()` — and the work row appears only once
        // it stands still. Our abort is instantaneous, so the gate costs one
        // beat: this tick stops the unit, the next creates the row.
        const unitdef::UnitDef* def = catalog_.def(store_.typeAt(unit.index));
        if (def != nullptr && def->isMobile() && store_.motion()[unit.index].moving) {
            teardownMovement(store_.motion()[unit.index]);
            return static_cast<std::int32_t>(ScriptTaskStatus::NextBeat);
        }
        if (!beginEnhancementWork(store_, catalog_, work_, unit, name)) {
            return static_cast<std::int32_t>(ScriptTaskStatus::Abort);
        }
        return static_cast<std::int32_t>(ScriptTaskStatus::NextBeat);
    }
    advanceEnhancement(*work);
    if (!work->finished()) return static_cast<std::int32_t>(ScriptTaskStatus::NextBeat);
    return static_cast<std::int32_t>(installEnhancement(store_, catalog_, unit, name)
        ? ScriptTaskStatus::Done : ScriptTaskStatus::Abort);
}
void EnhancementTasks::onDestroy(UnitId unit, std::string_view task,
    std::span<const std::uint8_t> data, ScriptTaskState&) {
    if (task != "EnhanceTask") return;
    const std::string name(data.begin(), data.end());
    // Already consumed resources are not refunded, whether finished, cancelled, or killed.
    std::erase_if(work_, [&](const auto& entry) { return entry.owner == unit && entry.name == name; });
}
} // namespace rm::sim
