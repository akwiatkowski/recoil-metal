#include "core/sim/Skirmish.hpp"

namespace rm::sim {
namespace {

/// The combat pass's view of the groups.
///
/// Built per tick rather than held, because the spans are the caller's and a batch that
/// grew between ticks would leave a stale one behind — the bug that `grewThisTick`
/// exists to catch on the collision side.
[[nodiscard]] std::vector<CombatGroup> combatView(std::span<const SkirmishGroup> groups) {
    std::vector<CombatGroup> view;
    view.reserve(groups.size());
    for (const SkirmishGroup& group : groups) {
        view.push_back(CombatGroup{
            .instances = group.instances,
            .motion = group.motion,
            .health = group.health,
            .def = group.def,
        });
    }
    return view;
}

/// The collision pass's view of the same groups.
[[nodiscard]] std::vector<CollisionGroup> collisionView(std::span<SkirmishGroup> groups) {
    std::vector<CollisionGroup> view;
    view.reserve(groups.size());
    for (SkirmishGroup& group : groups) {
        view.push_back(CollisionGroup{.instances = group.instances, .motion = group.motion});
    }
    return view;
}

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
void retireDead(std::span<SkirmishGroup> groups, TickReport& report) {
    for (std::size_t batch = 0; batch < groups.size(); ++batch) {
        SkirmishGroup& group = groups[batch];
        for (std::size_t i = 0; i < group.health.size(); ++i) {
            if (group.health[i].alive()) {
                continue;
            }
            if (i >= group.motion.size() || i >= group.instances.size()) {
                continue;
            }
            if (group.motion[i].radiusElmos <= 0.0f) {
                continue;  // already retired on an earlier tick
            }

            report.died.push_back(Death{
                .ref = UnitRef{.batch = batch, .instance = i},
                .at = group.instances[i].position,
                .radiusElmos = group.motion[i].radiusElmos,
            });

            group.instances[i].scale = 0.0f;
            group.motion[i].moving = false;
            group.motion[i].speedElmosPerSecond = 0.0f;
            group.motion[i].radiusElmos = 0.0f;  // and stops shoving the living
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
void recomputeIncome(std::span<const SkirmishGroup> groups, Match& match) {
    for (Economy& economy : match.economies) {
        economy.incomePerSecond = {};
        economy.upkeepPerSecond = {};
        economy.storage = match.baseStorage;
    }

    for (const SkirmishGroup& group : groups) {
        if (group.def == nullptr) {
            continue;  // a decorative batch earns nothing
        }
        for (std::size_t i = 0; i < group.motion.size(); ++i) {
            const int owner = group.motion[i].armyIndex;
            if (owner < 0 || static_cast<std::size_t>(owner) >= match.economies.size()) {
                continue;
            }
            if (i >= group.health.size() || !group.health[i].alive()) {
                continue;
            }

            Economy& economy = match.economies[static_cast<std::size_t>(owner)];
            if (isCommanderId(group.def->name)) {
                // The commander is the trickle and the starting storage, both OURS (see
                // kCommanderTrickle) — not its blueprint's fields, which the spawn does
                // not read either.
                economy.incomePerSecond.mass += kCommanderTrickle.mass;
                economy.incomePerSecond.energy += kCommanderTrickle.energy;
            } else {
                economy.incomePerSecond.mass += group.def->producesMassPerSecond;
                economy.incomePerSecond.energy += group.def->producesEnergyPerSecond;
                economy.upkeepPerSecond.energy += group.def->upkeepEnergyPerSecond;
                economy.storage.mass += group.def->storageMass;
                economy.storage.energy += group.def->storageEnergy;
            }
        }
    }
}

} // namespace

std::vector<int> countCommanders(std::span<const SkirmishGroup> groups,
                                 std::size_t armyCount) {
    std::vector<int> alive(armyCount, 0);
    for (const SkirmishGroup& group : groups) {
        if (group.def == nullptr || !isCommanderId(group.def->name)) {
            continue;
        }
        for (std::size_t i = 0; i < group.motion.size(); ++i) {
            const int army = group.motion[i].armyIndex;
            if (army < 0 || static_cast<std::size_t>(army) >= alive.size()) {
                continue;
            }
            if (i < group.health.size() && group.health[i].alive()) {
                ++alive[static_cast<std::size_t>(army)];
            }
        }
    }
    return alive;
}

TickReport tickSkirmish(std::span<SkirmishGroup> groups, Match& match,
                        const HeightField& field) {
    TickReport report;

    // 1. MOVEMENT, then collisions. Everything downstream reads where a unit has got to
    //    this tick rather than where it started it.
    for (SkirmishGroup& group : groups) {
        tick(group.instances, group.motion, field);
    }
    {
        const std::vector<CollisionGroup> collisions = collisionView(groups);
        resolveCollisions(collisions, field);
    }

    // Everything below is a MATCH, and a scene with no armies is not one — a `--units`
    // crowd scattered for a screenshot has nothing to shoot at and nobody to pay.
    if (match.armies.empty()) {
        return report;
    }

    // 2. AIM, then fire. An unturreted weapon may only shoot along the hull, so a unit
    //    that has stopped facing the wrong way has to be brought round first; otherwise
    //    the facing gate reads as a weapon that does not work.
    {
        const std::vector<CombatGroup> combat = combatView(groups);
        for (SkirmishGroup& group : groups) {
            (void)aimAtTargets(group.instances, group.motion, group.def, combat,
                               match.armies);
        }
    }

    // 3. FIRE, fly, land. Rebuilt after aiming because aiming wrote yaw through the
    //    mutable instance spans and the combat view holds const ones.
    if (match.projectiles != nullptr) {
        std::vector<CombatGroup> combat = combatView(groups);
        report.shotsFired = fireWeapons(combat, match.armies, *match.projectiles);
        advanceProjectiles(*match.projectiles, combat, match.armies, field);
    }

    // 4. The dead, then their explosions, then the defeated. In that order: an army
    //    whose commander died to a shot that landed this tick is defeated this tick, not
    //    next, and an ACU's detonation is enormous enough to decide the tick it goes off.
    retireDead(groups, report);

    // 99 of the 494 shipped weapons are `WeaponCategory = 'Death'` — a blast with no
    // target and no rate of fire. This is where they finally go off.
    //
    // Anything a blast kills is retired on the NEXT tick rather than this one, so a
    // chain of detonations propagates one link per tick instead of recursing here. That
    // is both the cheaper answer and the deterministic one: recursion would make the
    // result depend on the order the batches happen to sit in.
    if (!report.died.empty()) {
        std::vector<CombatGroup> combat = combatView(groups);
        for (const Death& death : report.died) {
            const unitdef::UnitDef* def = groups[death.ref.batch].def;
            if (def == nullptr || deathWeapon(*def) == nullptr) {
                continue;
            }
            report.deathBlastDamage += explodeOnDeath(
                *def, death.at, groups[death.ref.batch].motion[death.ref.instance].armyIndex,
                combat, match.armies);
            ++report.deathBlasts;
        }
    }

    const std::vector<int> alive = countCommanders(groups, match.armies.size());
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
    recomputeIncome(groups, match);

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
