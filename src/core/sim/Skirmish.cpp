#include "core/sim/Skirmish.hpp"

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
void retireDead(UnitStore& store, TickReport& report) {
    const std::span<UnitInstance> instances = store.instances();
    const std::span<MoveState> motion = store.motion();
    const std::span<const Health> healths = store.health();

    for (UnitIndex slot = 0; slot < healths.size(); ++slot) {
        if (healths[slot].alive()) {
            continue;
        }
        if (slot >= motion.size() || slot >= instances.size()) {
            continue;
        }
        if (motion[slot].radiusElmos <= 0.0f) {
            continue;  // already retired on an earlier tick
        }

        report.died.push_back(Death{
            .ref = store.idAt(slot),
            .at = instances[slot].position,
            .radiusElmos = motion[slot].radiusElmos,
        });

        instances[slot].scale = 0.0f;
        motion[slot].moving = false;
        motion[slot].speedElmosPerSecond = 0.0f;
        motion[slot].radiusElmos = 0.0f;  // and stops shoving the living
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
void recomputeIncome(const UnitStore& store, const UnitCatalog& catalog, Match& match) {
    for (Economy& economy : match.economies) {
        economy.incomePerSecond = {};
        economy.upkeepPerSecond = {};
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
        if (isCommanderId(def->name)) {
            // The commander is the trickle and the starting storage, both OURS (see
            // kCommanderTrickle) — not its blueprint's fields, which the spawn does
            // not read either.
            economy.incomePerSecond.mass += kCommanderTrickle.mass;
            economy.incomePerSecond.energy += kCommanderTrickle.energy;
        } else {
            economy.incomePerSecond.mass += def->producesMassPerSecond;
            economy.incomePerSecond.energy += def->producesEnergyPerSecond;
            economy.upkeepPerSecond.energy += def->upkeepEnergyPerSecond;
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

TickReport tickSkirmish(UnitStore& store, const UnitCatalog& catalog, Match& match,
                        const HeightField& field, TickRate rate) {
    TickReport report;

    // 1. MOVEMENT, then collisions. Everything downstream reads where a unit has got to
    //    this tick rather than where it started it.
    //
    //    One call each now, over the whole store. It used to be a loop per batch plus a
    //    view built to hand every batch to the collision pass at once — because two units
    //    of different models had to be able to see each other. With one flat array that
    //    problem does not arise.
    tick(store.instances(), store.motion(), field);
    resolveCollisions(store.instances(), store.motion(), field);

    // Everything below is a MATCH, and a scene with no armies is not one — a `--units`
    // crowd scattered for a screenshot has nothing to shoot at and nobody to pay.
    if (match.armies.empty()) {
        return report;
    }

    // 2. AIM, then fire. An unturreted weapon may only shoot along the hull, so a unit
    //    that has stopped facing the wrong way has to be brought round first; otherwise
    //    the facing gate reads as a weapon that does not work.
    (void)aimAtTargets(store, catalog, match.armies);

    // 3. FIRE, fly, land.
    if (match.projectiles != nullptr) {
        report.shotsFired =
            fireWeapons(store, catalog, match.armies, *match.projectiles, rate);
        advanceProjectiles(*match.projectiles, store, match.armies, field);
    }

    // 4. The dead, then their explosions, then the defeated. In that order: an army
    //    whose commander died to a shot that landed this tick is defeated this tick, not
    //    next, and an ACU's detonation is enormous enough to decide the tick it goes off.
    retireDead(store, report);

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
        report.deathBlastDamage += explodeOnDeath(*def, death.at,
                                                 store.motion()[slot].armyIndex, store,
                                                 match.armies);
        ++report.deathBlasts;
    }

    const std::vector<int> alive = countCommanders(store, catalog, match.armies.size());
    report.defeated = applyDefeats(match.armies, alive, match.commandersEver);

    if (!match.over) {
        const std::size_t left = survivorCount(match.armies);
        if (left <= 1) {
            match.over = true;
            report.matchEnded = true;
            report.winner = winningTeam(match.armies);
        }
    }

    // 5. THE ECONOMY, last, so a producer destroyed in step 3 stops paying in the same
    //    tick it died rather than funding one more.
    recomputeIncome(store, catalog, match);

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
                }
            }
        }
    }

    return report;
}

} // namespace rm::sim
