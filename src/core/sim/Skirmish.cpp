#include "core/sim/Skirmish.hpp"

#include "core/sim/Adjacency.hpp"
#include "core/sim/Reclaim.hpp"

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

        // THE WRECK, as an object (§7 P6.2). Made here rather than by the caller, and read from
        // the slot BEFORE retirement zeroes the radius — which is the same reason `Death`
        // carries its position and size rather than a handle to look them up through.
        if (features != nullptr) {
            // What the wreck is WORTH comes from the definition — `BuildCost × MassMult`,
            // computed at parse time (`UnitDef::wreckMass`). A type with no definition, or
            // one whose blueprint states no Wreckage table (the ACUs, the walls), leaves a
            // scorch record with nothing in it, which is exactly `Unit.lua:1762-1765`.
            const unitdef::UnitDef* def = catalog.def(store.typeAt(slot));
            (void)features->add(Feature{
                .at = positionOf(transforms[slot]),
                .radiusElmos = motion[slot].radiusElmos,
                .fromType = store.typeAt(slot),
                .armyIndex = motion[slot].armyIndex,
                .massRemaining = def != nullptr ? def->wreckMass : Mag{},
                .energyRemaining = def != nullptr ? def->wreckEnergy : Mag{},
                .reclaimPerBuildRate = def != nullptr ? def->reclaimPerBuildRate : Fx{},
            });
        }

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
            economy.storage.mass += def->storageMass;
            economy.storage.energy += def->storageEnergy;
        }
    }
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
                        const Terrain& terrain, TickRate rate) {
    TickReport report;

    // 0. THE ORDER QUEUES, before anything moves (§7 P4.1). A unit that finished its order last
    //    tick starts the next one now, so a shift-queued route runs waypoint to waypoint
    //    without a gap the player can see. First in the tick for the same reason the scripted
    //    opponents decide first: an order started this tick should move this tick.
    report.ordersStarted = advanceOrders(store, catalog, terrain, match.passability, rate,
                                         match.building, match.events, match.features);

    // 1. MOVEMENT, then collisions. Everything downstream reads where a unit has got to
    //    this tick rather than where it started it.
    //
    //    One call each now, over the whole store. It used to be a loop per batch plus a
    //    view built to hand every batch to the collision pass at once — because two units
    //    of different models had to be able to see each other. With one flat array that
    //    problem does not arise.
    tick(store.transforms(), store.motion(), terrain);

    //    THE SPATIAL INDEX IS REBUILT TWICE, and both points are load-bearing (§7 P5.2).
    //    Here, because collisions ask which units are near each other and `tick` has just
    //    moved all of them; and again below, because collisions move them too and combat must
    //    not aim at where a unit was before it was shoved.
    //
    //    Each rebuild is one pass over the slots and a sort — cheap against what it replaces,
    //    which was a scan over every unit for every shooter, every projectile and every blast.
    store.reindex(spatialCellSize(store));
    resolveCollisions(store, terrain);
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

    // Attack-move and patrol acquire only from the post-movement, post-intel world. Their
    // temporary target then feeds the ordinary aiming and firing passes below.
    updateAggressiveOrders(store, catalog, match.armies, terrain, match.passability, rate,
                           match.intel);

    // 2. AIM, then fire. An unturreted weapon may only shoot along the hull, so a unit
    //    that has stopped facing the wrong way has to be brought round first; otherwise
    //    the facing gate reads as a weapon that does not work.
    (void)aimAtTargets(store, catalog, match.armies, match.intel);

    // 3. FIRE, fly, land.
    if (match.projectiles != nullptr) {
        report.shotsFired =
            fireWeapons(store, catalog, match.armies, *match.projectiles, rate, match.events,
                        match.intel);
        // The held overcharges, after the guns and before the flight: a shot authorised
        // this tick flies this tick, and the energy it burned is gone before the economy
        // pass reads the store.
        report.shotsFired += fireOvercharge(store, catalog, match.armies, *match.projectiles,
                                            match.economies, rate, match.events);
        advanceProjectiles(*match.projectiles, store, match.armies, terrain, rate,
                           match.events, &catalog);
    }

    // 4. The dead, then their explosions, then the defeated. In that order: an army
    //    whose commander died to a shot that landed this tick is defeated this tick, not
    //    next, and an ACU's detonation is enormous enough to decide the tick it goes off.
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

    const std::vector<int> alive = countCommanders(store, catalog, match.armies.size());
    const std::vector<bool> defeatedBefore = [&match] {
        std::vector<bool> before;
        before.reserve(match.armies.size());
        for (const Army& army : match.armies) {
            before.push_back(army.defeated);
        }
        return before;
    }();
    report.defeated = applyDefeats(match.armies, alive, match.commandersEver);

    // WHICH armies fell, not just how many. `applyDefeats` returns a count, which is all the
    // report ever needed; an event has to name the army, so the flags are compared either side
    // of the call rather than by changing a function four tests assert the return value of.
    for (std::size_t i = 0; i < match.armies.size() && i < defeatedBefore.size(); ++i) {
        if (match.armies[i].defeated && !defeatedBefore[i]) {
            emit(match.events, Event{.kind = EventKind::TeamDefeated,
                                     .army = match.armies[i].index});
        }
    }

    if (!match.over) {
        const std::size_t left = survivorCount(match.armies);
        if (left <= 1) {
            match.over = true;
            report.matchEnded = true;
            report.winner = winningAlliance(match.armies);
            // A draw is an ordinary outcome — every commander dying at once — so the event
            // carries `kNoArmy` rather than being suppressed.
            emit(match.events, Event{.kind = EventKind::GameOver,
                                     .army = report.winner.value_or(kNoArmy)});
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
    }
    // Manual reclaim has priority over autonomous patrol service when both reach the same
    // final scrap. Patrol helpers also use the freshly recomputed storage cap to avoid waste.
    (void)servicePatrolBuilders(store, catalog, match.armies, match.features, match.economies);

    // An upgrade whose unit died is CANCELLED, not completed: the work was that unit
    // becoming something, and there is no longer anything to become it. Before the economy
    // pass, so a cancelled upgrade stops drawing resources the same tick its factory fell.
    if (match.building != nullptr) {
        std::erase_if(*match.building, [&store](const Construction& work) {
            return work.isUpgrade() && !work.finished() && !store.alive(work.upgradeOf);
        });
    }

    if (match.building != nullptr) {
        for (std::size_t army = 0; army < match.economies.size(); ++army) {
            // Partitioned per army because `tickEconomy` is documented to be given one
            // army's work, and charging the wrong one is a caller's mistake to avoid.
            //
            // The empty case still goes through: a base with nothing under construction
            // still runs its structures, and skipping the call would make upkeep free
            // whenever the build queue happened to be empty.
            std::vector<Construction> mine;
            for (const Construction& work : *match.building) {
                if (work.armyIndex == static_cast<int>(army)) {
                    mine.push_back(work);
                }
            }

            tickEconomy(match.economies[army], mine);

            // Written back over this army's entries, in order — the two lists were built
            // by the same filter in the same pass, so the nth of `mine` is the nth of
            // this army's work. What NEWLY finished is reported; nothing is removed, for
            // the reason on TickReport::finished.
            std::size_t next = 0;
            for (Construction& work : *match.building) {
                if (work.armyIndex != static_cast<int>(army) || next >= mine.size()) {
                    continue;
                }
                const bool wasFinished = work.finished();
                work = mine[next];
                ++next;
                if (!wasFinished && work.finished()) {
                    report.finished.push_back(work);
                    emit(match.events,
                         Event{.kind = EventKind::ConstructionFinished,
                               .army = work.armyIndex,
                               .amount = work.cost.mass,
                               .at = work.position});
                }
            }
        }
    }

    return report;
}

} // namespace rm::sim
