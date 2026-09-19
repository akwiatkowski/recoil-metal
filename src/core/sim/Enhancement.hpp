#pragma once
#include "core/sim/UnitStore.hpp"
#include "core/sim/Economy.hpp"
#include "core/sim/UnitCatalog.hpp"
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace rm::sim {
[[nodiscard]] Mag effectiveBuildPerTick(const UnitStore&, const UnitCatalog&, UnitIndex) noexcept;
[[nodiscard]] Mag enhancementHealthAdd(const UnitStore&, const UnitCatalog&, UnitIndex) noexcept;
[[nodiscard]] Mag enhancementRegenPerTick(const UnitStore&, const UnitCatalog&, UnitIndex) noexcept;
[[nodiscard]] bool canBuild(const UnitStore&, const UnitCatalog&, UnitIndex,
                            const unitdef::UnitDef& product);
[[nodiscard]] std::expected<void, std::string> validateEnhancementSequence(
    const UnitStore&, const UnitCatalog&, UnitId, std::span<const std::string>);
[[nodiscard]] std::expected<void, std::string> canInstallEnhancement(
    const UnitStore&, const UnitCatalog&, UnitId, std::string_view name);
/// Called only by completed, funded enhancement work. Unsupported handlers fail explicitly.
[[nodiscard]] std::expected<void, std::string> installEnhancement(
    UnitStore&, const UnitCatalog&, UnitId, std::string_view name);
/// What one shipped `CreateEnhancement`/`XxxRemove` script branch does, for the
/// branches whose effect is a native setter the sim can honour (`C-255`,
/// `C-379`). The NUMBERS come from the blueprint's `Enhancements` table
/// (`EnhancementSpec::parameters`, folded into `UnitCatalog::EnhancementEffects`);
/// this table carries only the parts the blueprint cannot express — which
/// command/toggle caps the branch adds or removes, and whether `NewRegenRate`
/// is a `SetRegenRate` absolute write rather than a `Regen` buff add. The same
/// field name means both in the corpus (`UEL0001`'s `DamageStablization` —
/// retail's own typo — writes 200 outright while `AdvancedEngineering` adds
/// 20), so the distinction is name-keyed exactly the way the scripts are.
struct EnhancementScriptEffects {
    /// `SetRegenRate(NewRegenRate)` — an absolute write, not an add.
    bool regenRateOverride = false;
    std::span<const std::string_view> commandCapAdds;
    std::span<const std::string_view> commandCapRemoves;
    std::span<const std::string_view> toggleCapAdds;
    std::span<const std::string_view> toggleCapRemoves;
    /// `SetWeaponEnabledByLabel(label, true/false)` — the weapon LABELS the
    /// branch enables or disables (`C-255`/`C-379`). Labels, not enhancement
    /// names: `TacticalMissile` enables `TacMissile`, `TacticalNukeMissile`
    /// enables `TacNukeMissile` while disabling `TacMissile`.
    std::span<const std::string_view> weaponEnables;
    std::span<const std::string_view> weaponDisables;
};
/// The script branch for one enhancement name; an empty record for names whose
/// whole effect is blueprint data (the engineering suites, the buff carriers).
[[nodiscard]] EnhancementScriptEffects enhancementScriptEffects(std::string_view name) noexcept;
/// Whether `name`'s `NewRegenRate` is a `SetRegenRate` write. `UnitCatalog`
/// consults this while folding parameters, so the override never also lands in
/// `regenPerTickAdd` — which would resurrect the erased value as an add after
/// the next buff event (`C-258`).
[[nodiscard]] bool enhancementRegenIsOverride(std::string_view name) noexcept;

/// The UI's replacement protocol for an occupied slot (`C-251`, `C-378`:
/// `construction.lua:947-981`). Returns the enhancement names to issue as
/// `UNITCOMMAND_Script` `EnhanceTask` orders, in order, each with the
/// clear-queue flag: `{occupant + "Remove", name}` when the slot holds an
/// enhancement that is neither `name` nor `name`'s own prerequisite chain
/// entry — retail asks the player first, then issues the pair 0.5 s apart;
/// `{name}` when the slot is empty or holds `name`'s prerequisite (the chain
/// install replaces its occupant directly); `{}` when `name` is already
/// installed, since retail's click is a no-op there. The 0.5 s spacing is
/// presentation — the sim contract is the sequence.
[[nodiscard]] std::vector<std::string> enhancementOrderSequence(
    const UnitStore& store, const UnitCatalog& catalog, UnitId unit,
    std::string_view name);

/// The unit's effective command cap: the blueprint's declared set plus the
/// caps its installed enhancements' script branches add, minus the ones they
/// remove (`C-379`'s `AddCommandCap`/`RemoveCommandCap` mutation). Undeclared
/// blueprint tables mean "everything", matching `CommandPanel`'s `permits`.
[[nodiscard]] bool unitHasCommandCap(
    const UnitStore& store, const UnitCatalog& catalog, UnitIndex slot,
    std::string_view cap) noexcept;
/// The same for `RULEUTC_*` toggle caps (`AddToggleCap`/`RemoveToggleCap`).
[[nodiscard]] bool unitHasToggleCap(
    const UnitStore& store, const UnitCatalog& catalog, UnitIndex slot,
    std::string_view cap) noexcept;

/// The production an installed enhancement adds to the unit's own output —
/// `ResourceAllocation`'s `SetProductionPerSecond*(bp + base)` (`C-255`). The
/// base half is already the unit's rate, so only the enhancement's fields add.
[[nodiscard]] Resources enhancementProductionPerTick(
    const UnitStore&, const UnitCatalog&, UnitIndex slot) noexcept;

/// Whether the unit's installed enhancements gate this weapon's label at all —
/// the script-table counterpart of `Weapon::enabledByEnhancement`, which only
/// catches the label==name cases (`C-255`). A weapon whose label appears in
/// any enhancement's enable/disable list is dead until installed state says
/// otherwise.
[[nodiscard]] bool weaponGatedByEnhancement(
    const UnitStore& store, const UnitCatalog& catalog, UnitIndex slot,
    std::string_view label) noexcept;
/// Whether the weapon is enabled FOR THIS UNIT: ungated weapons are always
/// enabled; a gated one is enabled iff some installed enhancement's branch
/// enables its label and none disables it (`SetWeaponEnabledByLabel`,
/// `C-255`/`C-379`).
[[nodiscard]] bool weaponEnabledForUnit(
    const UnitStore& store, const UnitCatalog& catalog, UnitIndex slot,
    const unitdef::Weapon& weapon) noexcept;

/// The unit-context weapon predicates: the `*IgnoringEnhancement` shape gated
/// by this unit's installed enhancements (`C-255`). Every callsite that asks
/// "can THIS unit fire/launch this weapon" uses these; the bare `fires()`/
/// `manuallyFired()`/`siloLaunched()` remain the def-level questions.
[[nodiscard]] inline bool weaponFiresFor(
    const UnitStore& store, const UnitCatalog& catalog, UnitIndex slot,
    const unitdef::Weapon& weapon) noexcept {
    return weapon.firesIgnoringEnhancement() && !weapon.targetsProjectiles
        && weaponEnabledForUnit(store, catalog, slot, weapon);
}
[[nodiscard]] inline bool weaponFiresAtProjectilesFor(
    const UnitStore& store, const UnitCatalog& catalog, UnitIndex slot,
    const unitdef::Weapon& weapon) noexcept {
    return weapon.firesIgnoringEnhancement() && weapon.targetsProjectiles
        && weaponEnabledForUnit(store, catalog, slot, weapon);
}
[[nodiscard]] inline bool weaponManuallyFiredFor(
    const UnitStore& store, const UnitCatalog& catalog, UnitIndex slot,
    const unitdef::Weapon& weapon) noexcept {
    return weapon.manuallyFiredIgnoringEnhancement()
        && weaponEnabledForUnit(store, catalog, slot, weapon);
}
[[nodiscard]] inline bool weaponSiloLaunchedFor(
    const UnitStore& store, const UnitCatalog& catalog, UnitIndex slot,
    const unitdef::Weapon& weapon) noexcept {
    return weapon.siloLaunchedIgnoringEnhancement()
        && weaponEnabledForUnit(store, catalog, slot, weapon);
}

/// The upkeep an installed enhancement adds — `MaintenanceConsumptionPerSecondEnergy`
/// (`SetEnergyMaintenanceConsumptionOverride`), per tick (`C-255`). Drains
/// while the enhancement stands; install/remove is the active switch.
[[nodiscard]] Mag enhancementMaintenancePerTick(
    const UnitStore&, const UnitCatalog&, UnitIndex slot) noexcept;

/// The absolute regen a `SetRegenRate` enhancement wrote, when one is
/// installed — `std::nullopt` otherwise. Read by `tickRegeneration` only while
/// the unit's `regenWrite` says the write still stands (`C-258`).
[[nodiscard]] std::optional<Mag> enhancementRegenOverride(
    const UnitStore& store, const UnitCatalog& catalog, UnitIndex slot) noexcept;
/// Native counterpart of retail EnhanceTask.lua. The host must outlive bound command queues.
class EnhancementTasks final : public ScriptTaskHost {
public:
    EnhancementTasks(UnitStore& store, const UnitCatalog& catalog, std::vector<EnhancementWork>& work)
        : store_(store), catalog_(catalog), work_(work) {}
    ~EnhancementTasks() override = default;
    EnhancementTasks(const EnhancementTasks&) = delete;
    EnhancementTasks& operator=(const EnhancementTasks&) = delete;
    EnhancementTasks(EnhancementTasks&&) = delete;
    EnhancementTasks& operator=(EnhancementTasks&&) = delete;
    void onCreate(UnitId, std::string_view, std::span<const std::uint8_t>, ScriptTaskState&) override;
    std::int32_t taskTick(UnitId, std::string_view, std::span<const std::uint8_t>, ScriptTaskState&) override;
    void onDestroy(UnitId, std::string_view, std::span<const std::uint8_t>, ScriptTaskState&) override;
private:
    UnitStore& store_;
    const UnitCatalog& catalog_;
    std::vector<EnhancementWork>& work_;
};
} // namespace rm::sim
