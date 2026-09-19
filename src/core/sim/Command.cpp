// THE COMMAND VOCABULARY — the names, equality, and predicates every stage of the pipeline
// shares, now that the pipeline itself is split.
//
// The stages live beside this file: `CommandBuffer.cpp` (intake), `CommandApply.cpp`
// (authority), `CommandAdvance.cpp` (what the queue does next), `CommandLog.cpp` (the record).
// What is HERE is what no stage owns: the spellings (`commandKindName`), the equality a log
// diff compares, and the validity predicates asked twice — once when an order is issued and
// again when the queue head reaches it, in different translation units now.
//
// The cross-file declarations are `CommandInternal.hpp`; only the family includes it.
#include "core/sim/Command.hpp"
#include "core/sim/CommandInternal.hpp"

#include "core/sim/Combat.hpp"
#include "core/sim/Movement.hpp"
#include "core/sim/UnitStore.hpp"

#include <algorithm>

namespace rm::sim {

// --- Names, equality and the intake canonical form ----------------------------

void canonicalizeUnits(std::vector<UnitId>& units) {
    std::ranges::sort(units, [](UnitId a, UnitId b) {
        return a.index < b.index || (a.index == b.index && a.generation < b.generation);
    });
    units.erase(std::unique(units.begin(), units.end()), units.end());
}

[[nodiscard]] bool validCancellation(const CommandIssue& issue) noexcept {
    return (issue.kind == CommandKind::CancelFactoryBuild)
        == (issue.cancelCommandId != kInvalidCommandId);
}
const char* commandKindName(CommandKind kind) noexcept {
    switch (kind) {
    case CommandKind::Dive: return "dive";
    case CommandKind::Move:
        return "move";
    case CommandKind::AttackMove:
        return "attack-move";
    case CommandKind::Patrol:
        return "patrol";
    case CommandKind::Stop:
        return "stop";
    case CommandKind::Attack:
        return "attack";
    case CommandKind::Build:
        return "build";
    case CommandKind::Reclaim:
        return "reclaim";
    case CommandKind::ReclaimUnit:
        return "reclaim-unit";
    case CommandKind::Capture:
        return "capture";
    case CommandKind::Overcharge:
        return "overcharge";
    case CommandKind::Assist:
        return "assist";
    case CommandKind::Guard:
        return "guard";
    case CommandKind::ToggleFactoryRepeat:
        return "toggle-factory-repeat";
    case CommandKind::ToggleProduction:
        return "toggle-production";
    case CommandKind::CycleBuildPriority:
        return "cycle-build-priority";
    case CommandKind::CycleRetreatThreshold:
        return "cycle-retreat-threshold";
    case CommandKind::CycleTargetFocus:
        return "cycle-target-focus";
    case CommandKind::SetBuildPriority:
        return "set-build-priority";
    case CommandKind::Repair:
        return "repair";
    case CommandKind::Script:
        return "script";
    case CommandKind::CancelFactoryBuild:
        return "cancel-factory-build";
    case CommandKind::MissileLaunch:
        return "missile-launch";
    case CommandKind::LoadTransport:
        return "load-transport";
    case CommandKind::UnloadTransport:
        return "unload-transport";
    case CommandKind::Ferry:
        return "ferry";
    case CommandKind::SiloBuildTactical:
        return "silo-build-tactical";
    case CommandKind::SiloBuildNuke:
        return "silo-build-nuke";
    case CommandKind::ToggleSiloAuto:
        return "toggle-silo-auto";
    case CommandKind::SelfDestruct:
        return "self-destruct";
    case CommandKind::ToggleScriptBit:
        return "toggle-script-bit";
    case CommandKind::OfferDraw:
        return "offer-draw";
    case CommandKind::Sacrifice:
        return "sacrifice";
    case CommandKind::Gift:
        return "gift";
    }
    return "stop";
}

bool operator==(const Command& a, const Command& b) noexcept {
    return a.tick == b.tick && a.player == b.player && a.kind == b.kind && a.queued == b.queued
           && a.unit == b.unit
           && a.targetX == b.targetX && a.targetZ == b.targetZ && a.target == b.target
           && a.buildType == b.buildType;
}

bool operator==(const CommandIssue& a, const CommandIssue& b) noexcept {
    return a.tick == b.tick && a.phase == b.phase && a.source == b.source && a.id == b.id
           && a.player == b.player && a.kind == b.kind && a.queued == b.queued
           && a.units == b.units && a.targetX == b.targetX && a.targetZ == b.targetZ
           && a.target == b.target && a.buildType == b.buildType && a.count == b.count
           && a.scriptTask == b.scriptTask && a.scriptData == b.scriptData
           && a.cancelCommandId == b.cancelCommandId && a.priority == b.priority
           && a.scriptBit == b.scriptBit;
}

// --- The shared predicates ------------------------------------------------

// Retail's mobile-build range test (`CUnitMobileBuildTask` state 1): compare centre distance
// after subtracting the builder's smaller footprint side and the product's larger skirt side.
// Declared in the header because the assist pass judges an engineering station's reach to a
// construction with the very same rule.
Fx constructionReach(const UnitCatalog& catalog, UnitTypeIndex builder,
                     UnitTypeIndex product) noexcept {
    const UnitCatalog::Rates& builderRates = catalog.rates(builder);
    return builderRates.buildReachElmos + builderRates.buildFootprintElmos
         + catalog.rates(product).buildSkirtElmos;
}

std::optional<std::pair<Fx, Fx>> buildSiteFor(
    UnitTypeIndex buildType, Fx targetX, Fx targetZ, UnitIndex slot,
    const UnitStore& store, const UnitCatalog& catalog) noexcept {
    const unitdef::UnitDef* builder = catalog.def(store.typeAt(slot));
    const unitdef::UnitDef* product = catalog.def(buildType);
    if (builder == nullptr || product == nullptr) {
        return std::nullopt;
    }
    // The same rule `startCommand`'s Build case applies: an upgrade and a factory's
    // mobile product happen on the builder's own pad; anything else stands where ordered.
    const bool pad = (!builder->upgradesTo.empty() && builder->upgradesTo == product->name)
                     || (builder->hasCategory("FACTORY") && product->isMobile());
    if (pad) {
        const Transform& at = store.transforms()[slot];
        return std::pair{at.x, at.z};
    }
    return std::pair{targetX, targetZ};
}

Construction* constructionAtSite(std::vector<Construction>& building,
                                 UnitTypeIndex blueprint, Fx x, Fx z) noexcept {
    for (Construction& work : building) {
        if (work.blueprintIndex == blueprint && work.position[0] == x && work.position[2] == z) {
            return &work;
        }
    }
    return nullptr;
}

bool constructionWorkedOn(const Construction& work, const UnitStore& store,
                          const UnitCatalog& catalog) noexcept {
    if (!store.alive(work.builder)) {
        return false;
    }
    const CommandQueue& queue = store.orders()[work.builder.index];
    if (work.retainedCommandId != kInvalidCommandId) {
        // A factory's own build retained behind its Guard order: the Guard dispatch is
        // what advances it, so it is worked while the retained entry is still queued.
        return std::ranges::any_of(queue.entries(), [&](const QueuedCommand& entry) {
            return entry.kind() == CommandKind::Build
                   && entry.payload().id == work.retainedCommandId;
        });
    }
    const QueuedCommand* active = queue.activeEntry();
    if (active == nullptr || active->kind() != CommandKind::Build) {
        return false;
    }
    const auto site = buildSiteFor(active->buildType(), active->targetX(), active->targetZ(),
                                   work.builder.index, store, catalog);
    return site.has_value() && work.blueprintIndex == active->buildType()
           && work.position[0] == site->first && work.position[2] == site->second;
}

bool constructionArmyAllied(const Construction& work, UnitIndex slot,
                            const UnitStore& store,
                            std::span<const Army> armies) noexcept {
    if (armies.empty()) {
        return true;
    }
    const int mine = store.motion()[slot].armyIndex;
    const auto a = std::ranges::find_if(armies, [mine](const Army& army) {
        return army.index == mine;
    });
    const auto b = std::ranges::find_if(armies, [theirs = work.armyIndex](const Army& army) {
        return army.index == theirs;
    });
    return a != armies.end() && b != armies.end() && allied(*a, *b);
}

bool joinableConstructionAt(std::vector<Construction>& building, const Command& command,
                            const UnitStore& store, const UnitCatalog& catalog,
                            std::span<const Army> armies) noexcept {
    if (command.kind != CommandKind::Build) {
        return false;
    }
    const auto site = buildSiteFor(command.buildType, command.targetX, command.targetZ,
                                   command.unit.index, store, catalog);
    if (!site) {
        return false;
    }
    const Construction* row = constructionAtSite(building, command.buildType,
                                                 site->first, site->second);
    return row != nullptr && !row->finished()
           && (row->builder == command.unit
               || constructionArmyAllied(*row, command.unit.index, store, armies));
}

bool canPauseProduction(const unitdef::UnitDef& def) noexcept {
    if (def.isBuilder() || def.hasCategory("FACTORY")
        || def.hasToggleCap("RULEUTC_ProductionToggle") || def.producesMassPerSecond > 0.0f
        || def.producesEnergyPerSecond > 0.0f || def.upkeepEnergyPerSecond > 0.0f) {
        return true;
    }
    return std::ranges::any_of(
        def.weapons, [](const unitdef::Weapon& weapon) { return weapon.countedProjectile; });
}

bool canSetBuildPriority(const unitdef::UnitDef& def) noexcept {
    // The tier is bucketed by the PRODUCER — a unit whose work the allocator can
    // serve early: builders and factories for construction/repair/capture/enhance,
    // counted-projectile silos for their ammo builds (`tickEconomy`'s tierAt calls).
    return def.isBuilder() || def.hasCategory("FACTORY")
           || std::ranges::any_of(
               def.weapons, [](const unitdef::Weapon& w) { return w.countedProjectile; });
}

bool buildSitePlaceable(const PassabilityGrid& grid, Fx x, Fx z, Fx radiusElmos,
                        const UnitStore& store, const UnitCatalog& catalog,
                        std::span<const Construction> building) noexcept {
    if (!sitePlaceable(grid, x, z, radiusElmos)) {
        return false;
    }

    const std::array<Fx, 3> site{x, Fx{}, z};
    for (UnitIndex slot = 0; slot < store.slotCount(); ++slot) {
        if (!store.slotAlive(slot) || !store.health()[slot].alive()
            || store.motion()[slot].airborne) {
            continue;
        }
        const Fx occupied = store.motion()[slot].radiusElmos;
        if (occupied > Fx{}
            && groundDistanceElmos(site, positionOf(store.transforms()[slot]))
                   < radiusElmos + occupied) {
            return false;
        }
    }

    for (const Construction& work : building) {
        if (work.finished()) {
            continue;  // history is retained, but only unfinished work occupies a site
        }
        const unitdef::UnitDef* def =
            catalog.def(static_cast<UnitTypeIndex>(work.blueprintIndex));
        if (def == nullptr) {
            continue;
        }
        const Fx occupied = fxFromFloat(def->collisionRadiusElmos);
        if (groundDistanceElmos(site, work.position) < radiusElmos + occupied) {
            return false;
        }
    }
    return true;
}
/// Validation shared by immediate orders, queued promises, and their eventual start.
/// Routing is deliberately absent: a queued helper should not move until this reaches the head.
[[nodiscard]] bool validAssist(const Command& command, const UnitStore& store,
                               const UnitCatalog& catalog) noexcept {
    if (!store.alive(command.target) || command.target == command.unit) {
        return false;
    }
    if (store.motion()[command.unit.index].armyIndex
        != store.motion()[command.target.index].armyIndex) {
        return false;
    }
    const unitdef::UnitDef* assister = catalog.def(store.typeAt(command.unit.index));
    const unitdef::UnitDef* target = catalog.def(store.typeAt(command.target.index));
    if (assister == nullptr || target == nullptr || !target->isBuilder()) {
        return false;
    }
    return assister->isBuilder() || assister->hasCategory("COMMAND");
}

[[nodiscard]] bool validGuard(const Command& command, const UnitStore& store,
                              const UnitCatalog& catalog) noexcept {
    if (!store.alive(command.target) || command.target == command.unit
        || !store.health()[command.target.index].alive()) return false;
    const auto* def = catalog.def(store.typeAt(command.unit.index));
    return def != nullptr && (def->isMobile() || def->isBuilder())
        && (!def->commandCapsDeclared || def->hasCommandCap("RULEUCC_Guard"));
}
[[nodiscard]] bool repairStillAllied(UnitIndex builder, UnitId target, const UnitStore& store,
                                     std::span<const Army> armies) noexcept {
    if (!store.alive(target)) {
        return false;
    }
    if (armies.empty()) {
        return true;  // the direct-dispatch compatibility seam has no alliance state to judge
    }
    const int owner = store.motion()[builder].armyIndex;
    const int targetOwner = store.motion()[target.index].armyIndex;
    const auto mine = std::ranges::find_if(armies, [owner](const Army& army) {
        return army.index == owner;
    });
    const auto theirs = std::ranges::find_if(armies, [targetOwner](const Army& army) {
        return army.index == targetOwner;
    });
    return mine != armies.end() && theirs != armies.end() && allied(*mine, *theirs);
}

std::optional<SacrificeWork> sacrificeWork(
    const Command& command, const UnitStore& store,
    std::span<const Construction> building,
    std::span<const EnhancementWork> enhancements) noexcept {
    // `C-192`, `0x00601c50`: retail's task points at the unit being built —
    // `IsBeingBuilt` or state `38 Enhancing`. Here a rising structure is a
    // `Construction` row, not an entity, so the command addresses the work two
    // ways: `target` names a unit being UPGRADED (the row's `upgradeOf`) or
    // ENHANCED (the row's `owner`), and `targetX`/`targetZ` name a scaffold's
    // site. Alliance is the caller's check — this only finds the work.
    if (command.target.generation != 0) {
        if (!store.alive(command.target) || command.target == command.unit) {
            return std::nullopt;
        }
        for (std::size_t i = 0; i < building.size(); ++i) {
            const Construction& work = building[i];
            if (!work.finished() && work.upgradeOf == command.target) {
                return SacrificeWork{.index = i,
                                     .unitTarget = true,
                                     .at = positionOf(
                                         store.transforms()[command.target.index]),
                                     .armyIndex = work.armyIndex,
                                     .productType =
                                         static_cast<UnitTypeIndex>(work.blueprintIndex),
                                     .cost = work.cost,
                                     .totalBuildTime = work.totalBuildTime};
            }
        }
        for (std::size_t i = 0; i < enhancements.size(); ++i) {
            const EnhancementWork& work = enhancements[i];
            if (!work.finished() && work.owner == command.target) {
                return SacrificeWork{.index = i,
                                     .enhancement = true,
                                     .unitTarget = true,
                                     .at = positionOf(
                                         store.transforms()[command.target.index]),
                                     .armyIndex =
                                         store.motion()[command.target.index].armyIndex,
                                     .cost = work.cost,
                                     .totalBuildTime = work.totalBuildTime};
            }
        }
        return std::nullopt;
    }
    for (std::size_t i = 0; i < building.size(); ++i) {
        const Construction& work = building[i];
        if (!work.finished() && work.position[0] == command.targetX
            && work.position[2] == command.targetZ) {
            return SacrificeWork{.index = i,
                                 .at = work.position,
                                 .armyIndex = work.armyIndex,
                                 .productType =
                                     static_cast<UnitTypeIndex>(work.blueprintIndex),
                                 .cost = work.cost,
                                 .totalBuildTime = work.totalBuildTime};
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<std::array<Fx, 3>> guardReturnPosition(
    UnitIndex slot, UnitId guardee, const UnitStore& store, const UnitCatalog& catalog,
    std::span<const Construction> building) {
    if (!store.alive(guardee)) return {};
    const auto* guard = catalog.def(store.typeAt(slot));
    const auto* guarded = catalog.def(store.typeAt(guardee.index));
    if (guard == nullptr || guarded == nullptr || !guard->isMobile()
        || guard->guardScanRadiusElmos <= Fx{}) return {};
    auto anchor = positionOf(store.transforms()[guardee.index]);
    const auto work = std::ranges::find_if(building, [&](const Construction& item) {
        return !item.finished() && item.builder == guardee;
    });
    if (work != building.end()) anchor = work->position;
    // C-183: full guarded-unit width plus half the scan radius; distance is three-dimensional.
    const Fx leash = fxFromFloat(guarded->collisionRadiusElmos * 2.0f)
        + guard->guardScanRadiusElmos / Fx::fromInt(2);
    const auto from = positionOf(store.transforms()[slot]);
    const Fx distance = fxHypot(fxHypot(anchor[0] - from[0], anchor[2] - from[2]),
                               anchor[1] - from[1]);
    return distance > leash ? std::optional{anchor} : std::nullopt;
}

[[nodiscard]] std::optional<UnitId> guardAttackTarget(
    UnitIndex slot, const UnitStore& store, const UnitCatalog& catalog,
    std::span<const Army> armies, const Intel* intel, const PlayableRect* playableRect,
    TickIndex tick, TickRate rate) {
    const auto* guard = catalog.def(store.typeAt(slot));
    if (armies.empty() || guard == nullptr || !guard->isMobile()
        || guard->guardScanRadiusElmos <= Fx{}) return {};
    const auto& at = store.transforms()[slot];
    std::optional<UnitId> prey;
    Fx preyDistance{};
    for (std::size_t w = 0; w < guard->weapons.size(); ++w) {
        const auto& weapon = guard->weapons[w];
        if (!weapon.fires() || weapon.manuallyFired() || weapon.targetsProjectiles) continue;
        auto ranged = weapon;
        ranged.maxRange = guard->guardScanRadiusElmos;
        const auto& cache = store.health()[slot].automaticTargets;
        const auto incumbent = w < cache.size() ? std::optional{cache[w]} : std::nullopt;
        const auto found = nearestTarget(positionOf(at), store.motion()[slot].armyIndex, ranged,
            store, armies, intel, &catalog, at.heading, incumbent, playableRect,
            {}, std::nullopt, tick, rate, store.targetFocuses()[slot]);
        if (!found) continue;
        const Fx distance = groundDistanceElmos(positionOf(at),
            positionOf(store.transforms()[found->index]));
        if (!prey || distance < preyDistance
            || (distance == preyDistance && found->index < prey->index)) {
            prey = found;
            preyDistance = distance;
        }
    }
    return prey;
}

bool guardAllowsBuildAssistance(UnitIndex slot, const UnitStore& store,
    const UnitCatalog& catalog, std::span<const Construction> building,
    std::span<const Army> armies, const Intel* intel, const PlayableRect* playableRect,
    TickIndex tick, TickRate rate) {
    if (!store.slotAlive(slot)) return false;
    const auto* head = store.orders()[slot].active();
    if (head == nullptr || !isGuardCommand(head->kind()) || !store.alive(head->target())
        || !repairStillAllied(slot, head->target(), store, armies)) return false;
    if (head->kind() == CommandKind::Guard && !validGuard(head->asCommand(), store, catalog)) return false;
    // The economy prepass and dispatch use the same C-183 return/attack decisions. A helper
    // cannot lend build power before dispatch chooses a higher-priority activity for it.
    return !guardReturnPosition(slot, head->target(), store, catalog, building)
        && !guardAttackTarget(slot, store, catalog, armies, intel, playableRect, tick, rate);
}

/// The unfinished work this builder founded, or null. The OLDEST such, which is the only one
/// there can be: `startCommand` refuses a second build while the first is running.
[[nodiscard]] Construction* activeConstruction(std::vector<Construction>& building,
                                               UnitId builder) noexcept {
    const auto found = std::ranges::find_if(building, [builder](const Construction& work) {
        return !work.finished() && work.builder == builder;
    });
    return found == building.end() ? nullptr : &*found;
}

void teardownMovement(MoveState& motion) {
    motion.moving = false;
    motion.path.clear();
    motion.pathIndex = 0;
    motion.pathPhaseCellsX = 0;
}

} // namespace rm::sim
