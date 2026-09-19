#include "core/sim/Skirmish.hpp"

#include "core/sim/Adjacency.hpp"
#include "core/sim/Assist.hpp"
#include "core/sim/Reclaim.hpp"
#include "core/sim/Retreat.hpp"
#include "core/sim/Transport.hpp"
#include "core/sim/Veterancy.hpp"

namespace rm::sim {
namespace {

/// Takes the destroyed out of the fight, and returns who newly fell.
///
/// A dead unit stops moving and stops being DRAWN, by way of a zero scale — which
/// collapses its mesh to a point rather than removing it from the batch. Removing it
/// properly would mean erasing from the instance array, and the health, motion and
/// instance arrays are parallel: an erase from one has to be an erase from all three,
/// and every span the combat and collision passes hold would have to be rebuilt
/// mid-tick.
///
/// NEWLY dead is what a radius still being positive means: retiring a unit is what
/// zeroes it, so a corpse is reported exactly once however many ticks it then sits
/// there. Without that a dead unit would set off its death explosion every tick
/// forever, which is both a wrong answer and an unbounded one.
void retireDead(UnitStore& store, const UnitCatalog& catalog, TickReport& report,
                EventQueue* events, FeatureStore* features,
                std::vector<ArmyStats>* armyStats) {
    const std::span<Transform> transforms = store.transforms();
    const std::span<MoveState> motion = store.motion();
    const std::span<const Health> healths = store.health();

    for (UnitIndex slot = 0; slot < healths.size(); ++slot) {
        if (healths[slot].alive()) {
            continue;
        }
        if (slot >= motion.size() || slot >= transforms.size()) {
            continue;
        }
        if (motion[slot].radiusElmos <= Fx{}) {
            continue;  // already retired on an earlier tick
        }

        report.died.push_back(Death{
            .ref = store.idAt(slot),
            .at = positionOf(transforms[slot]),
            .radiusElmos = motion[slot].radiusElmos,
        });

        // EXACTLY ONCE PER DEATH, which is the property §7 P6.1's test asserts — and it is this
        // loop's `radiusElmos > 0` guard that provides it, not anything about events. A corpse
        // sits in its slot for the rest of the match; without the guard it would be reported
        // every tick forever, which is both a wrong answer and an unbounded one.
        emit(events, Event{
                         .kind = EventKind::UnitDestroyed,
                         .unit = store.idAt(slot),
                         .instigator = healths[slot].lastHitBy,
                         .army = motion[slot].armyIndex,
                         .at = positionOf(transforms[slot]),
                     });

        // KILL CREDIT, here because this is retail's own placement: `Unit.lua` calls
        // `instigator:OnKilledUnit(self)` from the victim's death handling, before the death
        // weapon fires and before the wreck exists. Putting it anywhere later would credit a
        // chain kill to the wrong tick; putting it in `damageArea` would credit every hit.
        //
        // The same `radiusElmos > 0` guard makes this once per death too, which matters more
        // here than for the event: a kill counted twice is a unit promoted at half the cost.
        (void)creditKill(store, catalog, healths[slot].lastHitBy, events);

        // `CArmyStats` (`C-227`): the victim's army loses a unit, the killer's army
        // gains a kill — the two counters `aibrain.lua` reads for score and taunts.
        // Same once-per-death guard as the credit above, and the same live-killer
        // rule: a dead instigator earns nothing. The per-blueprint rows key on the
        // blueprint id, which is what `GetBlueprintStat` asks for.
        if (armyStats != nullptr) {
            const int victimArmy = motion[slot].armyIndex;
            const unitdef::UnitDef* victimDef = catalog.def(store.typeAt(slot));
            if (victimArmy >= 0
                && static_cast<std::size_t>(victimArmy) < armyStats->size()) {
                ArmyStats& victim = (*armyStats)[static_cast<std::size_t>(victimArmy)];
                addArmyStat(victim, "Units_Killed", Mag::fromInt(1));
                if (victimDef != nullptr) {
                    addArmyBlueprintStat(victim, "Units_Killed", victimDef->name,
                                         Mag::fromInt(1));
                }
            }
            const UnitId killer = healths[slot].lastHitBy;
            if (store.alive(killer) && killer.index < motion.size()) {
                const int killerArmy = motion[killer.index].armyIndex;
                if (killerArmy >= 0
                    && static_cast<std::size_t>(killerArmy) < armyStats->size()) {
                    ArmyStats& scorer = (*armyStats)[static_cast<std::size_t>(killerArmy)];
                    addArmyStat(scorer, "Enemies_Killed", Mag::fromInt(1));
                    if (victimDef != nullptr) {
                        addArmyBlueprintStat(scorer, "Enemies_Killed", victimDef->name,
                                             Mag::fromInt(1));
                        if (isCommanderId(victimDef->name)) {
                            addArmyStat(scorer, "Enemies_Commanders_Destroyed",
                                        Mag::fromInt(1));
                        }
                    }
                }
            }
        }

        // THE WRECK, after the victim's owner has awarded kill credit. `Unit.lua` runs
        // `instigator:OnKilledUnit(self)` before it creates the wreck; keep the event ordering
        // above unchanged while matching that gameplay ordering.
        if (features != nullptr) {
            const unitdef::UnitDef* def = catalog.def(store.typeAt(slot));
            (void)features->add(Feature{
                .at = positionOf(transforms[slot]),
                .radiusElmos = motion[slot].radiusElmos,
                .fromType = store.typeAt(slot),
                .armyIndex = motion[slot].armyIndex,
                .health = def != nullptr ? def->wreckHealth : Mag{},
                .maximumHealth = def != nullptr ? def->health : Mag{},
                .maximumMassReclaim = def != nullptr ? def->wreckMass : Mag{},
                .maximumEnergyReclaim = def != nullptr ? def->wreckEnergy : Mag{},
                .massRemaining = def != nullptr ? def->wreckMass : Mag{},
                .energyRemaining = def != nullptr ? def->wreckEnergy : Mag{},
                .reclaimWorkRemaining = def != nullptr
                    ? std::max(def->wreckMass, def->wreckEnergy) : Mag{},
                .reclaimWorkTotal = def != nullptr
                    ? std::max(def->wreckMass, def->wreckEnergy) : Mag{},
                .reclaimFraction = kFxOne,
                .damageRatio = kFxOne,
                .maximumReclaimPerBuildRate =
                    def != nullptr ? def->reclaimPerBuildRate : Fx{},
                .reclaimPerBuildRate = def != nullptr ? def->reclaimPerBuildRate : Fx{},
            });
        }

        // The scale that used to be zeroed here belonged to `UnitInstance`, which the store no
        // longer holds — a corpse is left out of the draw gather instead, which is both
        // cheaper and less of a lie than drawing a collapsed mesh.
        motion[slot].moving = false;
        motion[slot].speedPerTick = Fx{};
        motion[slot].radiusElmos = Fx{};  // and stops shoving the living
    }

    // The handles go stale HERE, after the report has been built from them — a caller reads
    // `Death::ref` to mark a wreck, and a handle killed a line earlier would already be
    // unresolvable. Retirement is the moment a unit stops existing, so it is the moment its
    // handle should stop naming it: anything still holding one from an earlier tick now
    // fails cleanly instead of finding whoever inherits the slot.
    for (const Death& death : report.died) {
        store.kill(death.ref);
    }
}

/// Carries out C-210's delayed OnDefeat cleanup. Health is set to zero rather than retiring a
/// unit here: retirement is the start of the next tick, preserving the ordinary death path.
void clearDefeatedArmy(UnitStore& store, const UnitCatalog& catalog, int armyIndex) {
    std::span<Health> healths = store.health();
    const std::span<const MoveState> motion = store.motion();

    for (UnitIndex slot = 0; slot < healths.size() && slot < motion.size(); ++slot) {
        if (!healths[slot].alive() || motion[slot].armyIndex != armyIndex) {
            continue;
        }

        const unitdef::UnitDef* def = catalog.def(store.typeAt(slot));
        if (def != nullptr && def->hasCategory("WALL")) {
            continue;
        }
        healths[slot].current = Mag{};
    }
}

/// Advances OnDefeat timers that were pending before this tick. A newly defeated army is
/// scheduled below and first loses a tick on the following call, making the delay twenty full
/// seconds after the poll that observed its commander missing.
void advanceDefeatCleanup(UnitStore& store, const UnitCatalog& catalog, Match& match) {
    if (match.defeatCleanupRemainingTicks.size() != match.armies.size()) {
        match.defeatCleanupRemainingTicks.resize(match.armies.size());
    }

    for (std::size_t i = 0; i < match.defeatCleanupRemainingTicks.size(); ++i) {
        TickCount& remaining = match.defeatCleanupRemainingTicks[i];
        if (remaining == 0) {
            continue;
        }
        --remaining;
        if (remaining == 0) {
            clearDefeatedArmy(store, catalog, match.armies[i].index);
        }
    }
}

/// Sums what every LIVING unit produces, costs to run, and stores, into its owner's
/// economy.
///
/// Recomputed from scratch each tick rather than adjusted when a build finishes or a
/// unit dies: an adjustment has to be applied exactly once at both ends, and the failure
/// mode of getting that wrong is an economy that drifts over a long match with nothing
/// pointing at when it started.
void recomputeIncome(const UnitStore& store, const UnitCatalog& catalog, Match& match,
                     TickRate rate, const Terrain& terrain,
                     std::vector<AdjacencyEffects>& adjacency) {
    // The commander's trickle, per tick. Computed once for the whole pass rather than per
    // commander: it is the same number for all of them.
    const Resources trickle{
        .mass = rate.magPerTick(kCommanderTrickleMassPerSecond),
        .energy = rate.magPerTick(kCommanderTrickleEnergyPerSecond),
    };

    // Who stands beside whom, this tick (`core/sim/Adjacency.hpp`): a storage feeding the
    // extractor it touches, a generator discounting its neighbours' upkeep. Derived state,
    // recomputed like the income itself — a structure that died in step 3 takes its
    // bonuses with it in the same tick its production stops. Written into the caller's
    // vector so `tickEconomy` can read the build-drain rows (`C-051`) without a second
    // scan.
    // Grid placement meets exactly; free placement keeps the half-ogrid slack (C-074).
    adjacencyEffects(store, catalog, adjacency,
                     terrain.placement() == PlacementMode::Grid ? Fx{} : kAdjacencyGapElmos);
    if (match.resourceFlows) match.resourceFlows->assign(store.slotCount(), {});

    for (Economy& economy : match.economies) {
        economy.incomePerTick = {};
        economy.upkeepPerTick = {};
        economy.storage = match.baseStorage;
    }

    const std::span<const MoveState> motion = store.motion();
    const std::span<const Health> healths = store.health();

    for (UnitIndex slot = 0; slot < motion.size(); ++slot) {
        const unitdef::UnitDef* def = catalog.def(store.typeAt(slot));
        if (def == nullptr) {
            continue;  // a decorative unit earns nothing
        }
        const int owner = motion[slot].armyIndex;
        if (owner < 0 || static_cast<std::size_t>(owner) >= match.economies.size()) {
            continue;
        }
        if (slot >= healths.size() || !healths[slot].alive()) {
            continue;
        }

        Economy& economy = match.economies[static_cast<std::size_t>(owner)];
        const Resources incomeBefore = economy.incomePerTick;
        const Resources upkeepBefore = economy.upkeepPerTick;

        // The per-tick rates come from the CATALOG, which derived them once when it learned
        // the type (§5.1). This loop runs over every unit every tick, so a conversion here
        // would be a divide per unit per tick — and would keep the per-second value in reach
        // of a later reader, which is the part that matters.
        const UnitCatalog::Rates& rates = catalog.rates(store.typeAt(slot));

        if (isCommanderId(def->name)) {
            // The commander is the trickle and the starting storage, both OURS (see
            // kCommanderTrickle*) — not its blueprint's fields, which the spawn does not
            // read either. Converted through the rate here rather than in the catalog
            // because it is not a property of any type: every commander gets the same
            // trickle whatever its blueprint says.
            economy.incomePerTick.mass += trickle.mass;
            economy.incomePerTick.energy += trickle.energy;
        } else {
            // Production and upkeep through this unit's adjacency multipliers — one for
            // the unbuffed, which is everything that stands alone. A production-paused
            // unit does neither: the fab stops fabricating AND stops drawing power, the
            // generator stops generating. Storage it offers still counts — capacity is
            // not production.
            const AdjacencyEffects& beside = adjacency[slot];
            if (!store.productionPaused(store.idAt(slot))) {
                economy.incomePerTick.mass += rates.massPerTick * beside.massProduction;
                economy.incomePerTick.energy += rates.energyPerTick * beside.energyProduction;
                // `SetMaintenanceConsumption{Active,Inactive}` gates upkeep only —
                // the script-bit toggles cut a unit's draw without touching what
                // it produces (`Unit.lua`'s `OnScriptBitSet`/`OnScriptBitClear`).
                if (store.maintenanceActive(store.idAt(slot))) {
                    economy.upkeepPerTick.energy +=
                        rates.upkeepEnergyPerTick * beside.energyUpkeep;
                }
            }
            // TRUNCATED PER STRUCTURE, and this is the only place the economy rounds
            // (`C-069`, `C-104`(e), `C-160`). Retail keeps its capacity as a `uint64` and
            // adds each contribution through `CEconStorage::Apply`, whose `__ftol2`
            // truncates toward zero — so a structure offering 105.6 mass of storage
            // contributes 105, and three of them contribute 315 rather than 316.8.
            // Truncating the SUM instead would agree for whole-numbered blueprints and
            // diverge for every other, which is the case that decides it.
            economy.storage.mass += Mag::fromInt(def->storageMass.floorToInt());
            economy.storage.energy += Mag::fromInt(def->storageEnergy.floorToInt());
        }
        if (match.resourceFlows) {
            (*match.resourceFlows)[slot] = {
                .unit = store.idAt(slot), .armyIndex = owner,
                .incomePerTick = {.mass = economy.incomePerTick.mass - incomeBefore.mass,
                                  .energy = economy.incomePerTick.energy - incomeBefore.energy},
                .upkeepPerTick = {.mass = economy.upkeepPerTick.mass - upkeepBefore.mass,
                                  .energy = economy.upkeepPerTick.energy - upkeepBefore.energy},
            };
        }
    }
}

/// C-210's Supremacy predicate: structures and engineers count, but walls do not.
[[nodiscard]] std::vector<int> countSupremacyUnits(const UnitStore& store,
                                                    const UnitCatalog& catalog,
                                                    std::size_t armyCount) {
    std::vector<int> alive(armyCount, 0);
    const std::span<const MoveState> motion = store.motion();
    const std::span<const Health> healths = store.health();

    for (UnitIndex slot = 0; slot < motion.size(); ++slot) {
        const unitdef::UnitDef* def = catalog.def(store.typeAt(slot));
        if (def == nullptr || def->hasCategory("WALL")
            || (!def->hasCategory("STRUCTURE") && !def->hasCategory("ENGINEER"))) {
            continue;
        }
        const int army = motion[slot].armyIndex;
        if (army < 0 || static_cast<std::size_t>(army) >= alive.size()) {
            continue;
        }
        if (slot < healths.size() && healths[slot].alive()) {
            ++alive[static_cast<std::size_t>(army)];
        }
    }
    return alive;
}


} // namespace
namespace {

/// Case-insensitive compare, like faction names: the lobby writes lowercase and
/// a scenario author has no reason to agree about case.
[[nodiscard]] bool equalsNoCase(std::string_view a, std::string_view b) noexcept {
    return std::ranges::equal(a, b, [](char x, char y) {
        const auto lower = [](char c) {
            return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c;
        };
        return lower(x) == lower(y);
    });
}

} // namespace

VictoryMode victoryModeFromName(std::string_view name) noexcept {
    if (equalsNoCase(name, "supremacy") || equalsNoCase(name, "domination")) {
        return VictoryMode::Supremacy;
    }
    if (equalsNoCase(name, "annihilation") || equalsNoCase(name, "eradication")) {
        return VictoryMode::Annihilation;
    }
    if (equalsNoCase(name, "sandbox")) {
        return VictoryMode::Sandbox;
    }
    // 'demoralization' lands here too — it IS Assassination, the default.
    return VictoryMode::Assassination;
}

VictoryMode victoryModeFromScenarioKey(std::string_view key) noexcept {
    // victory.lua's own chain, verbatim: three named keys and an else that
    // returns before checking anything — which is exactly what Sandbox does.
    if (equalsNoCase(key, "demoralization")) {
        return VictoryMode::Assassination;
    }
    if (equalsNoCase(key, "domination")) {
        return VictoryMode::Supremacy;
    }
    if (equalsNoCase(key, "eradication")) {
        return VictoryMode::Annihilation;
    }
    return VictoryMode::Sandbox;
}

/// C-210's Annihilation predicate: everything counts but walls.
[[nodiscard]] std::vector<int> countAnnihilationUnits(const UnitStore& store,
                                                      const UnitCatalog& catalog,
                                                      std::size_t armyCount) {
    std::vector<int> alive(armyCount, 0);
    const std::span<const MoveState> motion = store.motion();
    const std::span<const Health> healths = store.health();

    for (UnitIndex slot = 0; slot < motion.size(); ++slot) {
        const unitdef::UnitDef* def = catalog.def(store.typeAt(slot));
        if (def == nullptr || def->hasCategory("WALL")) {
            continue;
        }
        const int army = motion[slot].armyIndex;
        if (army < 0 || static_cast<std::size_t>(army) >= alive.size()) {
            continue;
        }
        if (slot < healths.size() && healths[slot].alive()) {
            ++alive[static_cast<std::size_t>(army)];
        }
    }
    return alive;
}

std::vector<int> countCommanders(const UnitStore& store, const UnitCatalog& catalog,
                                 std::size_t armyCount) {
    std::vector<int> alive(armyCount, 0);
    const std::span<const MoveState> motion = store.motion();
    const std::span<const Health> healths = store.health();

    for (UnitIndex slot = 0; slot < motion.size(); ++slot) {
        const unitdef::UnitDef* def = catalog.def(store.typeAt(slot));
        if (def == nullptr || !isCommanderId(def->name)) {
            continue;
        }
        const int army = motion[slot].armyIndex;
        if (army < 0 || static_cast<std::size_t>(army) >= alive.size()) {
            continue;
        }
        if (slot < healths.size() && healths[slot].alive()) {
            ++alive[static_cast<std::size_t>(army)];
        }
    }
    return alive;
}

std::vector<std::uint8_t> blockingCells(const UnitStore& store,
                                      const PassabilityGrid& grid) {
    std::vector<std::uint8_t> cells(
        static_cast<std::size_t>(grid.cellsX) * static_cast<std::size_t>(grid.cellsZ), 0);
    if (grid.cellsX <= 0 || grid.cellsZ <= 0) {
        return cells;
    }
    // Half a cell past the footprint, so a building claims the cell it sits in
    // even when centred on the boundary — a radius alone would let a small
    // structure on a corner claim nothing.
    const Fx halfCell = grid.elmosPerCell / Fx::fromInt(2);
    const std::span<const Transform> transforms = store.transforms();
    const std::span<const MoveState> motion = store.motion();
    for (UnitIndex slot = 0; slot < motion.size() && slot < transforms.size(); ++slot) {
        const MoveState& state = motion[slot];
        if (!store.slotAlive(slot) || state.airborne || state.attached
            || state.speedPerTick > Fx{}) {
            continue;
        }
        const Transform& at = transforms[slot];
        const Fx reach = state.radiusElmos + halfCell;
        const int x0 = grid.cellAtWorld(at.x - reach);
        const int x1 = grid.cellAtWorld(at.x + reach);
        const int z0 = grid.cellAtWorld(at.z - reach);
        const int z1 = grid.cellAtWorld(at.z + reach);
        for (int z = z0; z <= z1; ++z) {
            for (int x = x0; x <= x1; ++x) {
                const Fx dx = grid.worldAtCellCentre(x) - at.x;
                const Fx dz = grid.worldAtCellCentre(z) - at.z;
                if (fxHypot(dx, dz) <= reach) {
                    cells[static_cast<std::size_t>(z) * static_cast<std::size_t>(grid.cellsX)
                          + static_cast<std::size_t>(x)] = 1;
                }
            }
        }
    }
    return cells;
}

namespace {

/// The cell size the spatial index uses, in elmos.
///
/// TWO QUERY SCALES SHARE ONE GRID, and the cell has to serve both. Collision separation asks
/// about twice the largest collision radius — tens of elmos — while targeting asks about a
/// weapon's range, which reaches 2,048 in the corpus. A cell sized for the first makes a
/// targeting query walk thousands of cells; a cell sized for the second makes a collision query
/// fetch a whole neighbourhood to find one pair.
///
/// The floor is the collision reach, because a cell smaller than the smallest query is wasted
/// work with no benefit. Above that, the grid's own fallback covers the tail: a query whose
/// cell range exceeds the unit count scans the array instead, so a long-ranged weapon degrades
/// to the brute force it replaced rather than to something worse.
///
/// 128 ELMOS IS MEASURED, NOT CHOSEN. Sixty seconds of headless match at 2,008 units, user
/// time, two runs each — against 12.12 s for the full-scan version this replaces:
///
///     cell  32    2.99  3.01
///     cell  64    1.91  2.04
///     cell 128    1.78  1.81      <- and the curve is flat here
///     cell 256    1.95  1.96
///
/// The shape is what a uniform grid always does: too fine and a query pays a binary search per
/// cell for cells that hold nothing; too coarse and it fetches a neighbourhood to find one
/// pair. The minimum is broad, which is the useful part — being a factor of two out costs
/// about 10%, so this is a number that does not have to be re-tuned per map.
[[nodiscard]] Fx spatialCellSize(const UnitStore& store) noexcept {
    constexpr Fx kPreferredCell = Fx::fromInt(128);

    Fx largest{};
    for (const MoveState& state : store.motion()) {
        largest = std::max(largest, state.radiusElmos);
    }
    return std::max(kPreferredCell, largest * 2);
}

} // namespace

TickReport tickSkirmish(UnitStore& store, const UnitCatalog& catalog, Match& match,
                          const Terrain& terrain, TickRate rate, TickIndex tickIndex) {
    TickReport report;

    // This presentation-facing stamp survives the completed tick so the frame can tell a
    // working beam from a stalled site. Clear it at the next tick boundary; an active build
    // stamps it again below when its command actually advances the construction.
    if (match.building != nullptr) {
        for (Construction& work : *match.building) {
            work.advancedLastTick = false;
        }
    }

    // `GetArmyUnitCostTotal` (`CArmyImpl` vfunc `0x4c`): each army's live `CapCost`
    // sum, refreshed before the order queues run so this tick's creation gate
    // (`0x0074fda0`, `unitCapBlocks`) reads this tick's headcount. Rising works
    // are NOT here — they reserve their `CapCost` at the gate itself.
    for (Army& army : match.armies) {
        army.unitCostTotal = {};
    }
    for (UnitIndex slot = 0; slot < store.slotCount(); ++slot) {
        if (!store.slotAlive(slot)) {
            continue;
        }
        const int owner = store.motion()[slot].armyIndex;
        if (owner < 0 || static_cast<std::size_t>(owner) >= match.armies.size()) {
            continue;
        }
        if (const unitdef::UnitDef* def = catalog.def(store.typeAt(slot))) {
            match.armies[static_cast<std::size_t>(owner)].unitCostTotal =
                match.armies[static_cast<std::size_t>(owner)].unitCostTotal + def->capCost;
        }
    }


    // THE PRODUCTION PAUSE, mirrored once a tick. `UnitStore::productionPaused` is the one
    // authoritative flag; the work records carry a copy because the economy pass and the
    // script-task host never see the store. Enhancements and silo builds take theirs here;
    // constructions take it again at their advance sites in `advanceOrders`, so a pause
    // ordered mid-tick is honoured the same beat.
    if (match.enhancements != nullptr) {
        for (EnhancementWork& work : *match.enhancements) {
            work.paused = store.productionPaused(work.owner);
        }
    }
    if (match.siloAmmo != nullptr) {
        for (SiloAmmo& ammo : *match.siloAmmo) {
            ammo.paused = store.productionPaused(ammo.owner);
        }
    }

    // 0a. WHO IS HELPING, read from where everyone stood at the end of the last tick. It
    //     belongs with the dispatch stage below and immediately before it, because that is
    //     when retail's assisting builders do their work: each keeps its own build task in the
    //     same command-dispatch stage, judging its own reach from the previous beat's motion
    //     output (`C-187`, `C-142`). Recomputed every tick from orders and positions, so a
    //     helper that walked away, died or was re-tasked stops contributing at once
    //     (`core/sim/Assist.hpp`).
    if (match.building != nullptr) {
        (void)applyAssistance(store, catalog, *match.building, match.armies, match.intel,
            match.playableRect ? &*match.playableRect : nullptr, tickIndex, rate,
            match.assistLinks);
    }

    // 0. THE ORDER QUEUES, before anything moves (§7 P4.1). A unit that finished its order last
    //    tick starts the next one now, so a shift-queued route runs waypoint to waypoint
    //    without a gap the player can see. First in the tick for the same reason the scripted
    //    opponents decide first: an order started this tick should move this tick.
    //    CONSTRUCTION ADVANCES INSIDE IT, because that is the stage retail advances it in
    //    (`C-112`, `C-142`, `C-188`): the builder's own task materialises the target and then
    //    retires its own order, both in command dispatch. The bill for that work is still
    //    settled by the economy pass at the foot of the tick, which is also where retail
    //    writes the ratio this stage will multiply by next beat.
    if (match.pathService != nullptr) {
        // ADR-035 layer 3: tell the path service which cells standing structures
        // now claim, per movement domain. One tick stale — the layer describes
        // where units ENDED last tick, like the sight stamp below.
        for (const PassabilityGrid* grid : match.passability) {
            if (grid != nullptr) {
                match.pathService->setBlocking(*grid, blockingCells(store, *grid));
            }
        }
        for (const PathResult& result : match.pathService->service()) {
            (void)publishPathResult(result, store, catalog);
        }
    }
    std::vector<GuardWork> guardWork;
    report.ordersStarted = advanceOrders(store, catalog, terrain, match.passability, rate,
                                          match.building, match.events, match.features,
                                          &report.finished, match.pathService, match.armies,
                                          match.intel,
                                          match.playableRect ? &*match.playableRect
                                                             : nullptr,
                                          match.scriptTasks, &guardWork, &match.random,
                                          tickIndex, match.passabilitySubmerged);

    // 0b. TRANSPORTS. Between dispatch and movement so a route issued here —
    //     a carrier coming to its cargo, a ferry turning for the drop — moves
    //     this tick like any other order's. Attach/detach land here too: the
    //     propagation pass below publishes the new hierarchy the same tick.
    updateTransports(store, catalog, terrain, match.passability);

    // 1. MOVEMENT, then collisions. Everything downstream reads where a unit has got to
    //    this tick rather than where it started it.
    //
    //    One call each now, over the whole store. It used to be a loop per batch plus a
    //    view built to hand every batch to the collision pass at once — because two units
    //    of different models had to be able to see each other. With one flat array that
    //    problem does not arise.
    tick(store.transforms(), store.motion(), terrain, match.passability, store.types(),
         match.passabilitySubmerged);
    store.propagateAttachments();

    //    THE SPATIAL INDEX IS REBUILT TWICE, and both points are load-bearing (§7 P5.2).
    //    Here, because collisions ask which units are near each other and `tick` has just
    //    moved all of them; and again below, because collisions move them too and combat must
    //    not aim at where a unit was before it was shoved.
    //
    //    Each rebuild is one pass over the slots and a sort — cheap against what it replaces,
    //    which was a scan over every unit for every shooter, every projectile and every blast.
    store.reindex(spatialCellSize(store));
    resolveCollisions(store, terrain, match.passability, match.passabilitySubmerged);
    // Collision resolution can move either member independently. Reapply attachment-local
    // transforms before publishing positions to combat, so children never lag a parent by a tick.
    store.propagateAttachments();
    store.reindex(spatialCellSize(store));

    //    The last movement pass: a unit that spent the wait leaning on a friendly
    //    blocker gets it to step aside; a hard blocker gets a waypoint around it.
    //    Runs on the post-push positions the second reindex just published, and
    //    before the match-only early return so a crowd un-jams itself too.
    resolveCongestion(store, terrain, match.passability, match.armies,
                      match.passabilitySubmerged);

    // Everything below is a MATCH, and a scene with no armies is not one — a `--units`
    // crowd scattered for a screenshot has nothing to shoot at and nobody to pay.
    if (match.armies.empty()) {
        return report;
    }

    // 1b. WHAT EACH ALLIANCE CAN SEE (ADR-037). After movement and collisions, because
    //     sight is stamped from where a unit ENDED the tick; before aiming, because a
    //     shooter may only choose a target its side can see. Those two constraints are what
    //     fix this pass here rather than anywhere else in the order.
    if (match.intel != nullptr) {
        match.intel->update(store, catalog, match.armies, &terrain, rate, match.economies);
    }

    // Recovery precedes fire: a bubble whose timer reaches zero can intercept this tick,
    // while a hit later in the tick restarts its authored delay.
    tickShields(store, catalog, match.events);

    // Hulls heal alongside bubbles, and before the guns: a unit that regenerates back above
    // zero this tick was never dead, and one that does not is retired below. Doing it after
    // the damage would let a unit spend a tick at zero health and still survive, which is a
    // different game.
    tickRegeneration(store, catalog, rate);

    // Match owns the configuration; these addresses exist only for this tick's pure combat and
    // command passes, so neither pass can retain a borrowed scenario object between ticks.
    const PlayableRect* const playableRect =
        match.playableRect ? &*match.playableRect : nullptr;

    // Attack-move and patrol acquire only from the post-movement, post-intel world. Their
    // temporary target then feeds the ordinary aiming and firing passes below.
    updateAggressiveOrders(store, catalog, match.armies, terrain, match.passability, rate,
                            match.intel, playableRect, tickIndex, match.passabilitySubmerged);

    // Same class of per-unit automation: a hull under its retreat threshold abandons
    // its queue for the nearest mechanic before the aim pass picks its next target.
    updateRetreats(store, catalog, match.armies);

    // 2. AIM, then fire. An unturreted weapon may only shoot along the hull, so a unit
    //    that has stopped facing the wrong way has to be brought round first; otherwise
    //    the facing gate reads as a weapon that does not work.
    // What own-side engineers are taking apart this tick, so no gun of theirs shoots it
    // (C-157). Derived from the queue heads after dispatch, consumed by aim and fire only.
    // Capture claims join the reclaim ones: own guns spare what own engineers are taking,
    // whichever order kind is doing the taking.
    std::vector<WorkClaim> claims = collectUnitWorkClaims(store);
    for (const WorkClaim& claim : collectCaptureClaims(store)) {
        claims.push_back(claim);
    }
    (void)aimAtTargets(store, catalog, match.armies, match.intel, match.projectiles,
                       playableRect, tickIndex, rate, claims);

    // Who stands beside whom for the GUNS, computed once here: the RateOfFire row
    // (`C-051`(b) — a penalty, not a bonus) lands on this tick's reloads. The economy
    // recomputes its own inside `recomputeIncome` after the dead are retired, because a
    // giver that dies in step 3 stops granting the same tick its production stops —
    // sharing this earlier scan would let a corpse discount one more beat.
    std::vector<AdjacencyEffects> fireAdjacency;
    adjacencyEffects(store, catalog, fireAdjacency,
                     terrain.placement() == PlacementMode::Grid ? Fx{} : kAdjacencyGapElmos);

    // 3. FIRE, fly, land.
    if (match.projectiles != nullptr) {
        report.shotsFired =
            fireWeapons(store, catalog, match.armies, *match.projectiles, rate, match.events,
                             match.intel, playableRect, tickIndex,
                             match.siloAmmo != nullptr ? std::span<SiloAmmo>{*match.siloAmmo}
                                                       : std::span<SiloAmmo>{}, match.features,
                             claims, fireAdjacency);
        // The held overcharges, after the guns and before the flight: a shot authorised
        // this tick flies this tick, and the energy it burned is gone before the economy
        // pass reads the store.
        report.shotsFired += fireOvercharge(store, catalog, match.armies, *match.projectiles,
                                            match.economies, rate, match.events);
        // The silo's round, same slot in the beat: a launch authorised this tick flies
        // this tick, and the round it burned leaves the stockpile before the economy
        // pass reads it.
        report.shotsFired += fireMissiles(store, catalog, match.armies, *match.projectiles,
                                          match.siloAmmo != nullptr
                                              ? std::span<SiloAmmo>{*match.siloAmmo}
                                              : std::span<SiloAmmo>{},
                                          terrain, rate, match.events);
        advanceProjectiles(*match.projectiles, store, match.armies, terrain, rate,
                           match.events, &catalog,
                           match.redirects != nullptr ? std::span<MissileRedirect>{*match.redirects}
                                                      : std::span<MissileRedirect>{}, match.features,
                           &match.random);
    }

    // 4. The dead, then their explosions, then C-210's defeat poll. A commander that died to a
    //    shot this tick is visible to the next three-second poll, and an ACU's detonation is
    //    still resolved before that poll samples the surviving commanders.
    // Self-destruct countdowns (`C-345`): `selfdestruct.lua`'s five-second
    // `StartCountdown`, then `unit:Kill()` — which here is the ordinary death path:
    // health to zero and `retireDead` below reports it, wrecks it and scores it like
    // any other kill. A dead unit's countdown dies with it.
    if (match.selfDestructs != nullptr) {
        std::span<Health> healths = store.health();
        for (std::size_t i = 0; i < match.selfDestructs->size();) {
            SelfDestructWork& work = (*match.selfDestructs)[i];
            if (!store.alive(work.unit) || work.unit.index >= healths.size()
                || !healths[work.unit.index].alive()) {
                match.selfDestructs->erase(match.selfDestructs->begin()
                                           + static_cast<std::ptrdiff_t>(i));
                continue;
            }
            if (work.remainingTicks > 0) {
                --work.remainingTicks;
            }
            if (work.remainingTicks > 0) {
                ++i;
                continue;
            }
            healths[work.unit.index].current = Mag{};
            match.selfDestructs->erase(match.selfDestructs->begin()
                                       + static_cast<std::ptrdiff_t>(i));
        }
    }

    retireDead(store, catalog, report, match.events, match.features, match.armyStats);

    // 99 of the 494 shipped weapons are `WeaponCategory = 'Death'` — a blast with no
    // target and no rate of fire. This is where they finally go off.
    //
    // Anything a blast kills is retired on the NEXT tick rather than this one, so a
    // chain of detonations propagates one link per tick instead of recursing here. That
    // is both the cheaper answer and the deterministic one: recursion would make the
    // result depend on the order the units happen to sit in.
    for (const Death& death : report.died) {
        // Read by SLOT, not by resolving the handle: `retireDead` has already killed it, and
        // a corpse's arrays are deliberately left intact for exactly this (UnitStore.hpp,
        // "death is a tombstone").
        const UnitIndex slot = death.ref.index;
        const unitdef::UnitDef* def = catalog.def(store.typeAt(slot));
        if (def == nullptr || deathWeapon(*def) == nullptr) {
            continue;
        }
        // ATTRIBUTED TO THE CORPSE, by handle as well as by army — so a unit killed by a
        // commander's detonation names the commander, and the chain of a base going up reads
        // as a chain rather than as a crowd dying of nothing.
        report.deathBlastDamage += explodeOnDeath(*def, death.at,
                                                 store.motion()[slot].armyIndex, store,
                                                 match.armies, death.ref, match.events,
                                                 &catalog, match.features);
        ++report.deathBlasts;
    }

    advanceDefeatCleanup(store, catalog, match);

    // C-210: the retail commander check is a three-second poll. Its phase is match state, not
    // a caller-local counter, so a match remains deterministic when its tick rate changes.
    const TickCount defeatPollTicks = rate.ticks(seconds(3.0f));
    ++match.defeatPollElapsedTicks;
    if (match.defeatPollElapsedTicks >= defeatPollTicks) {
        match.defeatPollElapsedTicks = 0;
        // Sandbox never ends: CheckVictory returns immediately (C-210), so the
        // poll counts nothing and defeats nothing — but the timer still resets,
        // keeping the phase deterministic across tick rates.
        if (match.victoryMode != VictoryMode::Sandbox) {
            const std::vector<int> alive =
                match.victoryMode == VictoryMode::Supremacy
                    ? countSupremacyUnits(store, catalog, match.armies.size())
                : match.victoryMode == VictoryMode::Annihilation
                    ? countAnnihilationUnits(store, catalog, match.armies.size())
                    : countCommanders(store, catalog, match.armies.size());
            const std::vector<bool> defeatedBefore = [&match] {
                std::vector<bool> before;
                before.reserve(match.armies.size());
                for (const Army& army : match.armies) {
                    before.push_back(army.defeated);
                }
                return before;
            }();
            report.defeated = applyDefeats(match.armies, alive, match.commandersEver,
                                           match.victoryMode == VictoryMode::Assassination);
        // WHICH armies fell, not just how many. `applyDefeats` returns a count, which is all the
        // report ever needed; an event has to name the army, so the flags are compared either side
        // of the call rather than by changing a function four tests assert the return value of.
        if (match.defeatCleanupRemainingTicks.size() != match.armies.size()) {
            match.defeatCleanupRemainingTicks.resize(match.armies.size());
        }
        const TickCount cleanupTicks = rate.ticks(seconds(20.0f));
        for (std::size_t i = 0; i < match.armies.size() && i < defeatedBefore.size(); ++i) {
            if (match.armies[i].defeated && !defeatedBefore[i]) {
                emit(match.events, Event{.kind = EventKind::TeamDefeated,
                                         .army = match.armies[i].index});
                match.defeatCleanupRemainingTicks[i] = cleanupTicks;
            }
        }
        }
    }

    if (!match.over) {
        const std::optional<int> winner = winningAlliance(match.armies);
        const std::size_t survivors = survivorCount(match.armies);
        // `victory.lua`'s two immediate ends, checked before the stability
        // window: nobody left is a draw on the spot (`CallEndGame(true,
        // false)`), and so is every surviving army offering one
        // (`OfferingDraw` — `SimUtils.SetOfferDraw`). A sole surviving
        // alliance still wins through the ordinary window even with offers
        // on the table, matching retail's win-before-draw order.
        const bool mutualDraw =
            !winner && survivors > 0
            && std::ranges::all_of(match.armies, [](const Army& army) {
                   return army.defeated || army.offeringDraw;
               });
        if (match.victoryMode != VictoryMode::Sandbox
            && (survivors == 0 || mutualDraw)) {
            match.over = true;
            report.matchEnded = true;
            report.winner = std::nullopt;
            emit(match.events, Event{.kind = EventKind::GameOver,
                                     .army = kNoArmy});
        } else {
            // A team wins when it is the only ALLIANCE left, even if several
            // allied armies survived. `survivorCount <= 1` left a successful
            // 2v2 running forever.
            const bool terminal = match.victoryMode != VictoryMode::Sandbox
                && winner.has_value();
            if (!terminal) {
                match.winnerPending = false;
                match.pendingWinner.reset();
                match.pendingSurvivorMask = 0;
                match.winnerStableTicks = 0;
            } else {
                // `victory.lua` compares `stillAlive` to `potentialWinners`
                // with `table.equal`: the fifteen seconds restarts when the
                // survivor SET changes — a survivor dying inside the winning
                // alliance counts — not only when the verdict does.
                std::uint64_t survivorMask = 0;
                for (const Army& army : match.armies) {
                    if (!army.defeated && army.index < 64) {
                        survivorMask |= std::uint64_t{1} << army.index;
                    }
                }
                if (!match.winnerPending || match.pendingWinner != winner
                    || match.pendingSurvivorMask != survivorMask) {
                    match.winnerPending = true;
                    match.pendingWinner = winner;
                    match.pendingSurvivorMask = survivorMask;
                    match.winnerStableTicks = 0;
                }

                // C-210: the retail win condition waits for fifteen seconds of the same winner.
                // Derive the duration from this tick's rate so changing the simulation clock does
                // not change the wall-clock confirmation time.
                const TickCount confirmationTicks = rate.ticks(seconds(15.0f));
                ++match.winnerStableTicks;
                if (match.winnerStableTicks >= confirmationTicks) {
                    match.over = true;
                    report.matchEnded = true;
                    report.winner = winner;
                    // A draw is an ordinary outcome — every commander dying at once — so the event
                    // carries `kNoArmy` rather than being suppressed.
                    emit(match.events, Event{.kind = EventKind::GameOver,
                                             .army = report.winner.value_or(kNoArmy)});
                }
            }
        }
    }

    // 5. THE ECONOMY, last, so a producer destroyed in step 3 stops paying in the same
    //    tick it died rather than funding one more.
    //
    //    Recompute capacity first, then harvest INSIDE the economy step: reclaim credits the
    //    store directly,
    //    and running before `tickEconomy` means this tick's haul meets this tick's storage
    //    cap — reclaiming over a full mass bar overflows and is lost, the same rule as
    //    every other income (`core/sim/Reclaim.hpp`).
    std::vector<AdjacencyEffects> adjacency;
    recomputeIncome(store, catalog, match, rate, terrain, adjacency);
    if (match.features != nullptr) {
        (void)harvestReclaim(store, catalog, *match.features, match.economies);
        (void)applyGuardReclaim(store, catalog, *match.features, match.economies, guardWork);
    }
    // Units being un-built, in the same economy step and for the same reason as wrecks.
    (void)reclaimUnits(store, catalog, match.economies, match.events);
    // Manual reclaim has priority over autonomous patrol service when both reach the same
    // final scrap. Patrol helpers also use the freshly recomputed storage cap to avoid waste.
    (void)servicePatrolBuilders(store, catalog, match.armies, match.features, match.economies);

    // Explicit repair is an economy consumer, not a pre-allocation debit. Its requests enter
    // the same pass as upkeep and construction; their awarded ratios are applied below.
    std::vector<RepairWork> repairs;
    collectRepairWork(store, catalog, match.armies, repairs, guardWork,
                      match.building != nullptr ? std::span<const Construction>{*match.building}
                                                : std::span<const Construction>{});

    // Funded unit captures reconcile against this tick's active Capture heads before
    // the per-army partition below, so new tasks enter with computed budgets and
    // retired orders leave with their progress rather than lingering.
    if (match.captures != nullptr) {
        syncCaptureWork(store, catalog, match.armies, *match.captures, rate);
    }

    // Components are keyed by a generational UnitId. Remove before partitioning so a dead silo
    // cannot pay, and a subsequently recycled slot cannot inherit its ammunition (`C-081`).
    if (match.siloAmmo != nullptr) {
        std::erase_if(*match.siloAmmo, [&store](const SiloAmmo& ammo) {
            return !store.alive(ammo.owner);
        });
    }
    // The queue is the same component: a dead owner's pending builds die with the silo.
    if (match.siloQueue != nullptr) {
        std::erase_if(*match.siloQueue, [&store](const SiloBuild& entry) {
            return !store.alive(entry.owner);
        });
    }
    // Redirectors die with their owners for the same reason (`C-088`).
    if (match.redirects != nullptr) {
        std::erase_if(*match.redirects, [&store](const MissileRedirect& redirect) {
            return !store.alive(redirect.owner);
        });
    }

    // An upgrade whose unit died is CANCELLED, not completed: the work was that unit
    // becoming something, and there is no longer anything to become it. Before the economy
    // pass, so a cancelled upgrade stops drawing resources the same tick its factory fell.
    if (match.building != nullptr) {
        std::erase_if(*match.building, [&store](const Construction& work) {
            return work.isUpgrade() && !work.finished() && !store.alive(work.upgradeOf);
        });
    }

    if (match.building != nullptr || match.siloAmmo != nullptr || match.enhancements != nullptr || !repairs.empty()
        || (match.captures != nullptr && !match.captures->empty())) {
        for (std::size_t army = 0; army < match.economies.size(); ++army) {
            // Partitioned per army because `tickEconomy` is documented to be given one
            // army's work, and charging the wrong one is a caller's mistake to avoid.
            //
            // The empty case still goes through: a base with nothing under construction
            // still runs its structures, and skipping the call would make upkeep free
            // whenever the build queue happened to be empty.
            std::vector<Construction> mine;
            if (match.building != nullptr) {
                for (const Construction& work : *match.building) {
                    if (work.armyIndex == static_cast<int>(army)) {
                        mine.push_back(work);
                    }
                }
            }
            std::vector<RepairWork> repairMine;
            for (const RepairWork& repair : repairs) {
                if (repair.armyIndex == static_cast<int>(army)) {
                    repairMine.push_back(repair);
                }
            }
            std::vector<CaptureWork> captureMine;
            if (match.captures != nullptr) {
                for (const CaptureWork& work : *match.captures) {
                    if (work.armyIndex == static_cast<int>(army)) {
                        captureMine.push_back(work);
                    }
                }
            }
            std::vector<SiloAmmo> siloMine;
            if (match.siloAmmo != nullptr) {
                for (const SiloAmmo& ammo : *match.siloAmmo) {
                    if (store.alive(ammo.owner)
                        && store.motion()[ammo.owner.index].armyIndex == static_cast<int>(army)) {
                        siloMine.push_back(ammo);
                    }
                }
            }

            std::vector<EnhancementWork> enhancementMine;
            if (match.enhancements != nullptr) {
                for (const EnhancementWork& work : *match.enhancements) {
                    if (store.alive(work.owner)
                        && store.motion()[work.owner.index].armyIndex == static_cast<int>(army)) {
                        enhancementMine.push_back(work);
                    }
                }
            }

            tickEconomy(match.economies[army], mine, repairMine, siloMine, true,
                match.resourceFlows ? std::span<UnitResourceFlow>{*match.resourceFlows}
                                    : std::span<UnitResourceFlow>{}, static_cast<int>(army), enhancementMine,
                match.captures != nullptr ? std::span<CaptureWork>{captureMine}
                                          : std::span<CaptureWork>{},
                store.buildPriorities(), match.siloQueue, adjacency);

            // Written back over this army's entries, in order — the two lists were built
            // by the same filter in the same pass, so the nth of `mine` is the nth of
            // this army's work. Nothing completes here any more and nothing is reported:
            // progress, completion and the queue mutation that follows from it all belong to
            // the command-dispatch stage at the head of the tick (`C-112`). What is left is
            // the bill.
            if (match.building != nullptr) {
                std::size_t next = 0;
                for (Construction& work : *match.building) {
                    if (work.armyIndex != static_cast<int>(army) || next >= mine.size()) {
                        continue;
                    }
                    work = mine[next];
                    ++next;
                }
            }
            if (match.enhancements != nullptr) {
                std::size_t next = 0;
                for (EnhancementWork& work : *match.enhancements) {
                    if (store.alive(work.owner)
                        && store.motion()[work.owner.index].armyIndex == static_cast<int>(army)) {
                        work = enhancementMine[next++];
                    }
                }
            }
            std::size_t repairNext = 0;
            for (RepairWork& repair : repairs) {
                if (repair.armyIndex == static_cast<int>(army) && repairNext < repairMine.size()) {
                    repair = repairMine[repairNext];
                    ++repairNext;
                }
            }
            if (match.captures != nullptr) {
                std::size_t next = 0;
                for (CaptureWork& work : *match.captures) {
                    if (work.armyIndex != static_cast<int>(army) || next >= captureMine.size()) {
                        continue;
                    }
                    work = captureMine[next];
                    ++next;
                }
            }
            if (match.siloAmmo != nullptr) {
                std::size_t next = 0;
                for (SiloAmmo& ammo : *match.siloAmmo) {
                    if (store.alive(ammo.owner)
                        && store.motion()[ammo.owner.index].armyIndex == static_cast<int>(army)
                        && next < siloMine.size()) {
                        ammo = siloMine[next++];
                    }
                }
            }
        }

        // AFTER every army has ticked, never during (`C-163`). An army's spare capacity is
        // only known once it has spent, so a share offered mid-loop would be sized against a
        // headroom that the recipient's own tick was about to change.
        shareOverflow(match.economies, match.armies);
    }
    // Funded captures advance after the award above, like repair work: progress, and
    // replacement-entity transfer on completion. The transfer spawns, so this runs
    // after every span-holding pass has finished with the store.
    if (match.captures != nullptr) {
        (void)applyCaptureWork(store, *match.captures, match.events,
                               match.siloAmmo != nullptr ? std::span<SiloAmmo>{*match.siloAmmo}
                                                         : std::span<SiloAmmo>{},
                               match.enhancements != nullptr
                                   ? std::span<EnhancementWork>{*match.enhancements}
                                   : std::span<EnhancementWork>{});
    }
    (void)applyRepairWork(store, catalog, repairs);

    // `CArmyStats` (`C-227`): the engine-owned stats are fed from this tick's economy
    // — the ratios `aibrain.lua`'s hysteresis ladder triggers on, the income and
    // consumption the score screen reads — and the trigger evaluator runs from the
    // per-army beat AFTER TICK 10, retail's own gate. Kills are recorded in
    // `retireDead` above, where the death is known once.
    if (match.armyStats != nullptr) {
        // Per-second, because retail's stats are: the per-tick rates scale back up by
        // the tick count, the same conversion `UnitCatalog` runs at registration.
        const Mag perSecond = Mag::fromInt(static_cast<std::int64_t>(rate.ticksPerSecond()));
        for (std::size_t army = 0; army < match.armyStats->size()
             && army < match.economies.size(); ++army) {
            ArmyStats& stats = (*match.armyStats)[army];
            const Economy& economy = match.economies[army];
            // `Economy_Ratio_*`: stored over capacity — the figure the low/full-store
            // triggers compare against 0.1 / 0.9. An army with no storage reports a
            // full ratio, which is the honest reading of "nothing fits anywhere".
            const auto ratio = [](Mag stored, Mag cap) {
                return cap > Mag{} ? stored.toFx() / cap.toFx() : kFxOne;
            };
            setArmyStat(stats, "Economy_Ratio_Mass",
                        Mag::fromRaw(ratio(economy.stored.mass, economy.storage.mass).raw()));
            setArmyStat(stats, "Economy_Ratio_Energy",
                        Mag::fromRaw(ratio(economy.stored.energy, economy.storage.energy).raw()));
            // `Economy_Income_*` / `Economy_Output_*` are per-SECOND figures in
            // retail's Lua; the per-tick rates scale back up by the tick count.
            setArmyStat(stats, "Economy_Income_Mass", economy.incomePerTick.mass * perSecond.toFx());
            setArmyStat(stats, "Economy_Income_Energy", economy.incomePerTick.energy * perSecond.toFx());
            setArmyStat(stats, "Economy_Output_Mass", economy.usageLastTick.mass * perSecond.toFx());
            setArmyStat(stats, "Economy_Output_Energy", economy.usageLastTick.energy * perSecond.toFx());
            setArmyStat(stats, "Economy_Stored_Mass", economy.stored.mass);
            setArmyStat(stats, "Economy_Stored_Energy", economy.stored.energy);
            setArmyStat(stats, "Economy_TotalProduced_Mass", economy.generatedLifetime.mass);
            setArmyStat(stats, "Economy_TotalProduced_Energy", economy.generatedLifetime.energy);
            addArmyStat(stats, "Economy_TotalConsumed_Mass", economy.usageLastTick.mass);
            addArmyStat(stats, "Economy_TotalConsumed_Energy", economy.usageLastTick.energy);
            // `UnitCap_Current`/`UnitCap_MaxCap` — `aibrain.lua`'s score row and
            // `AIBehaviors.lua`'s experimental gate read both (`CArmyStats`,
            // `C-227`). The cap is the army's configured ceiling; the count is
            // the live `CapCost` sum recomputed at the head of this tick.
            if (army < match.armies.size()) {
                setArmyStat(stats, "UnitCap_Current",
                            Mag::fromRaw(match.armies[army].unitCostTotal.raw()));
                setArmyStat(stats, "UnitCap_MaxCap",
                            Mag::fromRaw(match.armies[army].unitCap.raw()));
            }
        }
        // The evaluator's own gate: it runs from the per-army beat after tick 10
        // (`C-227`), so the first ten beats feed stats without a trigger able to fire.
        if (tickIndex > 10) {
            for (std::size_t army = 0; army < match.armyStats->size(); ++army) {
                for (std::string& name : evaluateArmyStats((*match.armyStats)[army])) {
                    report.armyStatsFired.push_back(ArmyStatFired{
                        .army = static_cast<int>(army), .name = std::move(name)});
                }
            }
        }
    }

    return report;
}

} // namespace rm::sim
