#include "core/sim/Skirmish.hpp"

#include "core/sim/Adjacency.hpp"
#include "core/sim/Assist.hpp"
#include "core/sim/Reclaim.hpp"
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
                EventQueue* events, FeatureStore* features) {
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
                     TickRate rate) {
    // The commander's trickle, per tick. Computed once for the whole pass rather than per
    // commander: it is the same number for all of them.
    const Resources trickle{
        .mass = rate.magPerTick(kCommanderTrickleMassPerSecond),
        .energy = rate.magPerTick(kCommanderTrickleEnergyPerSecond),
    };

    // Who stands beside whom, this tick (`core/sim/Adjacency.hpp`): a storage feeding the
    // extractor it touches, a generator discounting its neighbours' upkeep. Derived state,
    // recomputed like the income itself — a structure that died in step 3 takes its
    // bonuses with it in the same tick its production stops.
    std::vector<AdjacencyEffects> adjacency;
    adjacencyEffects(store, catalog, adjacency);

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
            // the unbuffed, which is everything that stands alone.
            const AdjacencyEffects& beside = adjacency[slot];
            economy.incomePerTick.mass += rates.massPerTick * beside.massProduction;
            economy.incomePerTick.energy += rates.energyPerTick * beside.energyProduction;
            economy.upkeepPerTick.energy += rates.upkeepEnergyPerTick * beside.energyUpkeep;
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

    // 0a. WHO IS HELPING, read from where everyone stood at the end of the last tick. It
    //     belongs with the dispatch stage below and immediately before it, because that is
    //     when retail's assisting builders do their work: each keeps its own build task in the
    //     same command-dispatch stage, judging its own reach from the previous beat's motion
    //     output (`C-187`, `C-142`). Recomputed every tick from orders and positions, so a
    //     helper that walked away, died or was re-tasked stops contributing at once
    //     (`core/sim/Assist.hpp`).
    if (match.building != nullptr) {
        (void)applyAssistance(store, catalog, *match.building);
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
        for (const PathResult& result : match.pathService->service()) {
            (void)publishPathResult(result, store);
        }
    }
    std::vector<GuardWork> guardWork;
    report.ordersStarted = advanceOrders(store, catalog, terrain, match.passability, rate,
                                          match.building, match.events, match.features,
                                          &report.finished, match.pathService, match.armies,
                                          match.intel,
                                          match.playableRect ? &*match.playableRect
                                                             : nullptr,
                                          match.scriptTasks, &guardWork);

    // 1. MOVEMENT, then collisions. Everything downstream reads where a unit has got to
    //    this tick rather than where it started it.
    //
    //    One call each now, over the whole store. It used to be a loop per batch plus a
    //    view built to hand every batch to the collision pass at once — because two units
    //    of different models had to be able to see each other. With one flat array that
    //    problem does not arise.
    tick(store.transforms(), store.motion(), terrain);
    store.propagateAttachments();

    //    THE SPATIAL INDEX IS REBUILT TWICE, and both points are load-bearing (§7 P5.2).
    //    Here, because collisions ask which units are near each other and `tick` has just
    //    moved all of them; and again below, because collisions move them too and combat must
    //    not aim at where a unit was before it was shoved.
    //
    //    Each rebuild is one pass over the slots and a sort — cheap against what it replaces,
    //    which was a scan over every unit for every shooter, every projectile and every blast.
    store.reindex(spatialCellSize(store));
    resolveCollisions(store, terrain, match.passability);
    // Collision resolution can move either member independently. Reapply attachment-local
    // transforms before publishing positions to combat, so children never lag a parent by a tick.
    store.propagateAttachments();
    store.reindex(spatialCellSize(store));

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
        match.intel->update(store, catalog, match.armies, &terrain);
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
                            match.intel, playableRect);

    // 2. AIM, then fire. An unturreted weapon may only shoot along the hull, so a unit
    //    that has stopped facing the wrong way has to be brought round first; otherwise
    //    the facing gate reads as a weapon that does not work.
    (void)aimAtTargets(store, catalog, match.armies, match.intel, match.projectiles,
                          playableRect, tickIndex, rate);

    // 3. FIRE, fly, land.
    if (match.projectiles != nullptr) {
        report.shotsFired =
            fireWeapons(store, catalog, match.armies, *match.projectiles, rate, match.events,
                             match.intel, playableRect, tickIndex,
                             match.siloAmmo != nullptr ? std::span<SiloAmmo>{*match.siloAmmo}
                                                       : std::span<SiloAmmo>{});
        // The held overcharges, after the guns and before the flight: a shot authorised
        // this tick flies this tick, and the energy it burned is gone before the economy
        // pass reads the store.
        report.shotsFired += fireOvercharge(store, catalog, match.armies, *match.projectiles,
                                            match.economies, rate, match.events);
        advanceProjectiles(*match.projectiles, store, match.armies, terrain, rate,
                           match.events, &catalog,
                           match.redirects != nullptr ? std::span<MissileRedirect>{*match.redirects}
                                                      : std::span<MissileRedirect>{});
    }

    // 4. The dead, then their explosions, then C-210's defeat poll. A commander that died to a
    //    shot this tick is visible to the next three-second poll, and an ACU's detonation is
    //    still resolved before that poll samples the surviving commanders.
    retireDead(store, catalog, report, match.events, match.features);

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
                                                 &catalog);
        ++report.deathBlasts;
    }

    advanceDefeatCleanup(store, catalog, match);

    // C-210: the retail commander check is a three-second poll. Its phase is match state, not
    // a caller-local counter, so a match remains deterministic when its tick rate changes.
    const TickCount defeatPollTicks = rate.ticks(seconds(3.0f));
    ++match.defeatPollElapsedTicks;
    if (match.defeatPollElapsedTicks >= defeatPollTicks) {
        match.defeatPollElapsedTicks = 0;
        const std::vector<int> alive = match.victoryMode == VictoryMode::Supremacy
                                           ? countSupremacyUnits(store, catalog, match.armies.size())
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
                                       match.victoryMode != VictoryMode::Supremacy);

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

    if (!match.over) {
        const std::optional<int> winner = winningAlliance(match.armies);
        // A team wins when it is the only ALLIANCE left, even if several allied armies
        // survived. `survivorCount <= 1` left a successful 2v2 running forever. No
        // survivors is the other terminal state and remains an ordinary draw.
        const bool terminal = winner || survivorCount(match.armies) == 0;
        if (!terminal) {
            match.winnerPending = false;
            match.pendingWinner.reset();
            match.winnerStableTicks = 0;
        } else {
            if (!match.winnerPending || match.pendingWinner != winner) {
                match.winnerPending = true;
                match.pendingWinner = winner;
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

    // 5. THE ECONOMY, last, so a producer destroyed in step 3 stops paying in the same
    //    tick it died rather than funding one more.
    //
    //    Recompute capacity first, then harvest INSIDE the economy step: reclaim credits the
    //    store directly,
    //    and running before `tickEconomy` means this tick's haul meets this tick's storage
    //    cap — reclaiming over a full mass bar overflows and is lost, the same rule as
    //    every other income (`core/sim/Reclaim.hpp`).
    recomputeIncome(store, catalog, match, rate);
    if (match.features != nullptr) {
        (void)harvestReclaim(store, catalog, *match.features, match.economies);
        (void)applyGuardReclaim(store, catalog, *match.features, match.economies, guardWork);
    }
    // Manual reclaim has priority over autonomous patrol service when both reach the same
    // final scrap. Patrol helpers also use the freshly recomputed storage cap to avoid waste.
    (void)servicePatrolBuilders(store, catalog, match.armies, match.features, match.economies);

    // Explicit repair is an economy consumer, not a pre-allocation debit. Its requests enter
    // the same pass as upkeep and construction; their awarded ratios are applied below.
    std::vector<RepairWork> repairs;
    collectRepairWork(store, catalog, match.armies, repairs, guardWork);

    // Components are keyed by a generational UnitId. Remove before partitioning so a dead silo
    // cannot pay, and a subsequently recycled slot cannot inherit its ammunition (`C-081`).
    if (match.siloAmmo != nullptr) {
        std::erase_if(*match.siloAmmo, [&store](const SiloAmmo& ammo) {
            return !store.alive(ammo.owner);
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

    if (match.building != nullptr || match.siloAmmo != nullptr || !repairs.empty()) {
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
            std::vector<SiloAmmo> siloMine;
            if (match.siloAmmo != nullptr) {
                for (const SiloAmmo& ammo : *match.siloAmmo) {
                    if (store.alive(ammo.owner)
                        && store.motion()[ammo.owner.index].armyIndex == static_cast<int>(army)) {
                        siloMine.push_back(ammo);
                    }
                }
            }

            tickEconomy(match.economies[army], mine, repairMine, siloMine);

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
            std::size_t repairNext = 0;
            for (RepairWork& repair : repairs) {
                if (repair.armyIndex == static_cast<int>(army) && repairNext < repairMine.size()) {
                    repair = repairMine[repairNext];
                    ++repairNext;
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
    (void)applyRepairWork(store, catalog, repairs);

    return report;
}

} // namespace rm::sim
