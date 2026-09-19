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
    if (const auto valid = def->validateEnhancements(slots, sequence); !valid) return valid;
    // Faction CreateEnhancement handlers: engineering is a replacement-rate buff,
    // additive health/regen, and an additional build category (retail ACU scripts).
    for (const auto& name : sequence) if (name != "AdvancedEngineering" && name != "T3Engineering"
        && name != "AdvancedEngineeringRemove" && name != "T3EngineeringRemove") {
        return std::unexpected("enhancement effect handler not implemented: " + std::string{name});
    }
    return {};
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
