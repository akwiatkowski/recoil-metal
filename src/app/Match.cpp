#include "app/Match.hpp"

#include "core/sim/BuildOrder.hpp"
#include "core/sim/Replay.hpp"
#include "core/sim/SlowUpdate.hpp"
#include "core/sim/StateHash.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace rm::app {

// Defined here, declared `extern` in the header.
bool gPrintEvents = false;

/// Sends one unit to a world position, routed around whatever is in the way.
///
/// Falls back to nothing rather than to a straight line when no route exists:
/// walking into a cliff because the search failed is worse than standing still,
/// and standing still is at least legible as "it cannot get there".
/// Issues a move order AS A COMMAND, records it, and says whether it took.
///
/// THE ONE PATH (P2.5). Every order in this file — a player's right-click, a scripted
/// opponent's attack, a tank rolling off a factory floor — goes through `applyCommand`, so
/// there is no way for the human path and the script path to drift apart. The command is
/// recorded on the way through, which is what makes `scene.commands` the match.
///
/// `player` is who is issuing it, and it is checked rather than assumed: `applyCommand` refuses
/// an order from a player who does not command the unit's army.
/// `queued` is the shift key: the order goes behind whatever the unit is already doing rather
/// than replacing it (§7 P4.1). The scripted opponents never pass it — an opponent that queued
/// its orders would still be walking a route it decided on thirty seconds ago.
[[nodiscard]] bool issueMove(UnitScene& scene, const rm::sim::PassabilityGrid& grid,
                             const rm::HeightField& field, rm::sim::UnitId unit,
                             rm::PlayerIndex player, rm::TickIndex tick, rm::sim::Fx toX,
                             rm::sim::Fx toZ, bool queued) {
    const rm::sim::Command command{
        .tick = tick,
        .player = player,
        .kind = rm::sim::CommandKind::Move,
        .unit = unit,
        .targetX = toX,
        .targetZ = toZ,
        .buildType = 0,
    };

    const bool applied = rm::sim::applyCommand(command, scene.store, scene.catalog,
                                               scene.players, scene.armies,
                                               rm::sim::Terrain{field}, grid, gAppTickRate,
                                               &scene.building, queued);
    if (applied) {
        // Recorded only when it took. A refused order is not part of the match — replaying it
        // would be refused again, so keeping it would only make the log longer.
        scene.commands.record(command);
    }
    return applied;
}

// `issueBuild` used to live here, and it is worth recording why it does not.
//
// THE HOLE: builds do NOT go through `applyCommand`, so a build order skips the authorisation
// check, leaves no entry in the command log, and has its `ConstructionStarted` raised by the
// caller rather than by the sim. `check_one_order_path.sh` does not catch it because that watches
// the movement primitives, and this is a different one. **So P4.2's "every order goes through
// applyCommand" is true of MOVEMENT and not of construction** — a correction to what that
// commit claimed.
//
// **THE BLOCKER IS GONE (`#3090`), and the hole is not closed yet.** Those are two statements
// and both matter.
//
// What blocked it: `resolveBuildable` returned an index into a private `scene.buildable` list,
// typed `rm::UnitTypeIndex`, while `applyCommand` read `catalog.def(command.buildType)`. Two
// numbers, one type name. Routing builds through the sim turned a 36-mass extractor into an
// 18,000-mass experimental and the economy never paid it off. Unifying them was blocked in turn
// by the draw gather, which read `batch = unit.type` — so registering a buildable type before
// anything of it spawned would have shifted every later type past its batch.
//
// Both are fixed. `Scene::batchForType` maps the two spaces, so a type may exist with no batch —
// which is exactly what "buildable but not yet built" is — and `resolveBuildable` registers with
// the catalog like everything else. `Construction::blueprintIndex` is now a real type index, and
// `applyCommand` would read the right definition.
//
// What remains is the routing itself: moving these two call sites onto `applyCommand`, which is
// a behaviour change (authorisation, the command log, and the sim rather than the caller raising
// `ConstructionStarted`) rather than the refactor this was. It is a bounded job now instead of a
// blocked one.
//
// One loose end, deliberate and small: a blueprint registered by `resolveBuildable` and later
// built gets a SECOND type from `spawnUnit`, because that path creates a type and a batch
// together. Both are real type indices resolving through the catalog, so nothing is ambiguous —
// it costs one catalog entry per buildable blueprint. Collapsing them means teaching `spawnUnit`
// to add a batch to an existing type, which belongs with the routing above.

/// The definition a `Construction::blueprintIndex` names.
///
/// ONE INDEX SPACE (`#3090`). That field used to index a private `scene.buildable` list while
/// `sim::applyCommand` read `catalog.def()` — the same type name meaning two different numbers,
/// which is why builds could not go through the one order path. It is a real `UnitTypeIndex`
/// now, so this is a catalog lookup like every other.
///
/// A missing definition is not reachable through the build path — a construction only exists
/// because `resolveBuildable` registered its type — but the catalog returns a pointer, so the
/// empty definition is what a caller gets rather than a dereferenced null.
[[nodiscard]] const rm::unitdef::UnitDef& buildableDef(const UnitScene& scene,
                                                       std::size_t blueprintIndex) {
    static const rm::unitdef::UnitDef kNone{};
    const rm::unitdef::UnitDef* def =
        scene.catalog.def(static_cast<rm::UnitTypeIndex>(blueprintIndex));
    return def != nullptr ? *def : kNone;
}

/// The player driving an army, or none. What `issueMove` needs to attribute an order.
[[nodiscard]] rm::PlayerIndex playerDriving(const UnitScene& scene, int army) {
    for (const rm::sim::Player& player : scene.players) {
        if (rm::sim::commands(player, army)) {
            return player.index;
        }
    }
    return 0;
}

// `orderRouted` used to live here, and every order in this file went through it.
//
// It is gone: `issueMove` above does the same job and records the order on the way through, so
// there is one path into the sim rather than two similar ones (P2.5). That is what makes
// "a human log and a script log replay through the same path" a structural fact rather than
// an aspiration — there is no other path to take.


/// Gathers what `army` has standing, by walking the store once.
///
/// A scan, still: `UnitCensus` (P1.3) answers this in O(1) and wiring it in is P3's, where
/// the roles it keys on stop being hardcoded blueprint ids. One pass a second over a few
/// hundred units is not what this costs.
[[nodiscard]] Standing standingFor(UnitScene& scene, int army) {
    Standing standing;
    const std::span<const rm::sim::Transform> transforms = scene.store.transforms();
    const std::span<const rm::sim::MoveState> motion = scene.store.motion();
    const std::span<const rm::sim::Health> health = scene.store.health();

    for (rm::UnitIndex slot = 0; slot < scene.store.slotCount(); ++slot) {
        const rm::unitdef::UnitDef* def = scene.catalog.def(scene.store.typeAt(slot));
        if (def == nullptr) {
            continue;
        }
        if (motion[slot].armyIndex != army || !health[slot].alive()) {
            continue;
        }
        if (rm::sim::isCommanderId(def->name)) {
            standing.commanderAlive = true;
            standing.commanderPosition = rm::sim::positionOf(transforms[slot]);
            standing.commanderBuildRate = def->buildRate;
            standing.commander = scene.store.idAt(slot);
        } else {
            // BY ROLE, not by blueprint id (P3.3). These were four `def->name == "UEB1103"`
            // comparisons, which is why the census only recognised UEF structures: a Cybran
            // mass extractor is a mass extractor and was counted as nothing.
            switch (rm::unitdef::roleOf(*def)) {
            case rm::unitdef::Role::Extractor:
                ++standing.extractors;
                break;
            case rm::unitdef::Role::Energy:
                ++standing.powerGenerators;
                break;
            case rm::unitdef::Role::Factory:
                ++standing.factories;
                standing.factoryPosition = rm::sim::positionOf(transforms[slot]);
                standing.factoryBuildRate = def->buildRate;
                standing.factory = scene.store.idAt(slot);
                break;
            case rm::unitdef::Role::Raider:
            case rm::unitdef::Role::Assault:
                standing.tanks.push_back(scene.store.idAt(slot));
                break;
            default:
                break;
            }
        }
    }
    return standing;
}

/// The nearest living enemy commander to `from`, or nothing when the war is over.
/// Where the attack wave walks: kill it and its army is defeated, which is the
/// whole win condition.
[[nodiscard]] std::optional<std::array<rm::sim::Fx, 3>> nearestEnemyCommander(
    UnitScene& scene, int army, const std::array<rm::sim::Fx, 3>& from) {
    if (army < 0 || static_cast<std::size_t>(army) >= scene.armies.size()) {
        return std::nullopt;
    }
    std::optional<std::array<rm::sim::Fx, 3>> best;
    rm::sim::Fx bestDistance{};
    const std::span<const rm::sim::Transform> transforms = scene.store.transforms();
    const std::span<const rm::sim::MoveState> motion = scene.store.motion();
    const std::span<const rm::sim::Health> health = scene.store.health();

    for (rm::UnitIndex slot = 0; slot < scene.store.slotCount(); ++slot) {
        const rm::unitdef::UnitDef* def = scene.catalog.def(scene.store.typeAt(slot));
        if (def == nullptr || !rm::sim::isCommanderId(def->name)) {
            continue;
        }
        const int theirs = motion[slot].armyIndex;
        if (theirs < 0 || static_cast<std::size_t>(theirs) >= scene.armies.size()
            || !health[slot].alive()
            || !rm::sim::hostile(scene.armies[static_cast<std::size_t>(army)],
                                 scene.armies[static_cast<std::size_t>(theirs)])) {
            continue;
        }
        const rm::sim::Fx distance =
            rm::sim::groundDistanceElmos(from, rm::sim::positionOf(transforms[slot]));
        if (!best || distance < bestDistance) {
            best = rm::sim::positionOf(transforms[slot]);
            bestDistance = distance;
        }
    }
    return best;
}

/// The nearest Mass deposit to `from` that nothing has claimed — no construction
/// (finished ones stay in the list, so a standing extractor counts) within a
/// footprint of it.
[[nodiscard]] const rm::scenario::Marker* nearestFreeDeposit(
    const UnitScene& scene, std::span<const rm::scenario::Marker> markers,
    const std::array<rm::sim::Fx, 3>& from) {
    /// A deposit within this of an existing build site is the SAME deposit —
    /// half an extractor footprint. The old comment said "generous against float drift";
    /// there is no float drift here any more, and the generosity is now purely about the
    /// deposit being a point and the extractor a footprint.
    constexpr rm::sim::Fx kClaimedRadius = rm::sim::Fx::fromInt(8);

    const rm::scenario::Marker* nearest = nullptr;
    rm::sim::Fx nearestDistance{};
    for (const rm::scenario::Marker& marker : markers) {
        if (!marker.isType("Mass")) {
            continue;
        }
        bool claimed = false;
        for (const rm::sim::Construction& work : scene.building) {
            // `work.position` is already fixed point (§7 P10.0); only the MARKER still
            // needs converting, because it comes out of a map file as floats.
            if (rm::sim::groundDistanceElmos(work.position, fxPoint(marker.position))
                < kClaimedRadius) {
                claimed = true;
                break;
            }
        }
        if (claimed) {
            continue;
        }
        const rm::sim::Fx distance =
            rm::sim::groundDistanceElmos(from, fxPoint(marker.position));
        if (nearest == nullptr || distance < nearestDistance) {
            nearest = &marker;
            nearestDistance = distance;
        }
    }
    return nearest;
}

/// One decision pass of the scripted opponent, for every army but the player's.
///
/// This is milestone 20's "not an AI", enacted: the pure decisions live in
/// core/sim/BuildOrder.hpp and are tested there; this function only translates them
/// into the scene — a Construction pushed, an attack order routed. Run once a
/// second rather than every tick, because nothing here changes faster than a build
/// finishes and the decisions read the whole scene.
void runOpponents(UnitScene& scene, const rm::vfs::Vfs& content, const rm::HeightField& field,
                  PassabilitySet& passability,
                  std::span<const rm::mapinfo::StartPosition> starts,
                  std::span<const rm::scenario::Marker> markers,
                  std::vector<rm::sim::Opponent>& scripts, float elapsedSeconds,
                  rm::TickIndex tickIndex) {
    // The middle of the map, in fixed point: `structureSite` and `rolloffPoint` place things
    // relative to it, and both are sim geometry now.
    const rm::sim::Fx centreX = rm::sim::Fx::fromInt(field.squaresX * rm::kSquareSize / 2);
    const rm::sim::Fx centreZ = rm::sim::Fx::fromInt(field.squaresZ * rm::kSquareSize / 2);

    // The cadence, and the STAGGER (PLAN2.md §6.7, §7 P3.6). Each opponent thinks once a
    // second, and they take their turns on different ticks of that second rather than all on
    // the same one. Each pass walks the whole scene, so eight armies used to mean eight scene
    // walks landing on one tick in ten and none on the other nine — a stutter, which is what
    // `--bench` is asked to show gone.
    //
    // This replaces `tickIndex % decisionTicks() == 0` at the call site: the same period, now
    // asked per army rather than for all of them at once.
    //
    // ONCE A SECOND, and authored in seconds: nothing an opponent reacts to changes faster
    // than a build finishes. The period kept here because it belongs to the work being paced.
    // It was `inline constexpr int kDecisionTicks = rm::sim::kTicksPerSecond` once, which is a
    // literal wearing a different hat — it names a RATE where a duration belongs, so at 20 Hz
    // the opponents thought twice a second. `check_no_tick_literals.sh` catches that shape now.
    //
    // Staggering does not create an ordering advantage, it spreads one that was already there:
    // the armies were served in array order within a single tick, so army 0 already moved
    // first. A tenth of a second between them changes when, not who.
    const rm::sim::SlowUpdate pacing{gAppTickRate, rm::sim::seconds(1.0f)};

    for (const rm::sim::Army& army : scene.armies) {
        if (army.index == scene.playerArmy || army.defeated
            || static_cast<std::size_t>(army.index) >= scripts.size()) {
            continue;
        }
        if (!pacing.due(static_cast<std::size_t>(army.index), tickIndex)) {
            continue;
        }
        rm::sim::Opponent& script = scripts[static_cast<std::size_t>(army.index)];
        const Standing standing = standingFor(scene, army.index);

        // What is being paid for right now, split by who builds it: the commander
        // owns structures, the factory owns tanks.
        bool structureUnderway = false;
        bool tankUnderway = false;
        for (const rm::sim::Construction& work : scene.building) {
            if (work.armyIndex != army.index || work.finished()) {
                continue;
            }
            (buildableDef(scene, work.blueprintIndex).isMobile() ? tankUnderway
                                                             : structureUnderway) = true;
        }

        const rm::sim::ArmyView view{
            .commanderAlive = standing.commanderAlive,
            .commanderBusy = structureUnderway,
            .factoryBusy = tankUnderway,
            .extractorsStanding = standing.extractors,
            .powerGeneratorsStanding = standing.powerGenerators,
            .factoriesStanding = standing.factories,
            .tanksAlive = standing.tanks.size(),
        };

        // The commander's next structure, placed around ITS OWN start position —
        // the base grows where the map put the army, not where the commander wandered.
        const rm::sim::StructureOrder structure = rm::sim::nextStructure(view);
        if (structure != rm::sim::StructureOrder::None) {
            // A `std::string` and not a `string_view`: `blueprintFor` returns by value now
            // that the path is composed from a roster entry rather than being a `constexpr`
            // literal. A view here bound to the temporary and dangled — the match built its
            // first extractor and then silently stopped, because every later blueprint path
            // was freed memory. Caught by running a match, not by the compiler.
            std::string blueprint;
            std::optional<std::array<rm::sim::Fx, 3>> site;
            const std::size_t slot = standing.powerGenerators + standing.factories;
            const rm::mapinfo::StartPosition& start =
                starts[static_cast<std::size_t>(army.index)];
            const std::array<rm::sim::Fx, 3> home{rm::sim::fxFromFloat(start.x),
                                                  rm::sim::Fx{},
                                                  rm::sim::fxFromFloat(start.z)};
            switch (structure) {
            case rm::sim::StructureOrder::PowerGenerator:
                blueprint = blueprintFor(scene, army, energyStep(scene.opening));
                site = rm::sim::structureSite(home, centreX, centreZ,
                                              static_cast<int>(slot));
                break;
            case rm::sim::StructureOrder::Factory:
                blueprint = blueprintFor(scene, army, factoryStep(scene.opening));
                site = rm::sim::structureSite(home, centreX, centreZ,
                                              static_cast<int>(slot));
                break;
            case rm::sim::StructureOrder::Extractor: {
                blueprint = blueprintFor(scene, army, extractorStep(scene.opening));
                const rm::scenario::Marker* deposit =
                    nearestFreeDeposit(scene, markers, standing.commanderPosition);
                if (deposit != nullptr) {
                    site = fxPoint(deposit->position);
                }
                break;
            }
            case rm::sim::StructureOrder::None:
                break;
            }
            if (site) {
                const std::optional<std::size_t> blueprintIndex =
                    resolveBuildable(scene, content, blueprint);
                if (blueprintIndex) {
                    const rm::unitdef::UnitDef& def = buildableDef(scene, *blueprintIndex);
                    // NOT THROUGH `applyCommand`, and that is a known hole rather than a
                    // preference — see `issueBuild`'s note. `Construction::blueprintIndex`
                    // indexes `scene.buildable`, and `applyCommand` reads `catalog.def()`:
                    // two index spaces wearing one type name.
                    scene.building.push_back(rm::sim::Construction{
                        .armyIndex = army.index,
                        .position = *site,
                        .cost = {.mass = def.buildCostMass, .energy = def.buildCostEnergy},
                        .buildTimeRemaining = def.buildTime,
                        .totalBuildTime = def.buildTime,
                        .buildPerTick = gAppTickRate.magPerTick(standing.commanderBuildRate),
                        .blueprintIndex = *blueprintIndex,
                    });
                    // The event the sim would have raised, raised by the caller instead, so a
                    // consumer sees the same vocabulary whichever path created the work.
                    scene.events.emit(rm::sim::Event{
                        .kind = rm::sim::EventKind::ConstructionStarted,
                        .instigator = standing.commander,
                        .army = army.index,
                        .amount = def.buildCostMass,
                        .at = *site,
                    });
                    std::printf("  [%6.1fs] army %d starts %.*s\n",
                                static_cast<double>(elapsedSeconds), army.index,
                                static_cast<int>(blueprint.size()), blueprint.data());
                }
            }
        }

        // The factory's next tank, built where the factory stands and rolled off it
        // once finished (the spawn handles the rolloff).
        if (rm::sim::wantsTank(view)) {
            const std::optional<std::size_t> blueprintIndex =
                resolveBuildable(scene, content,
                                 blueprintFor(scene, army, scene.opening.waveUnit));
            if (blueprintIndex) {
                const rm::unitdef::UnitDef& def = buildableDef(scene, *blueprintIndex);
                scene.building.push_back(rm::sim::Construction{
                    .armyIndex = army.index,
                    .position = standing.factoryPosition,
                    .cost = {.mass = def.buildCostMass, .energy = def.buildCostEnergy},
                    .buildTimeRemaining = def.buildTime,
                    .totalBuildTime = def.buildTime,
                    .buildPerTick = gAppTickRate.magPerTick(standing.factoryBuildRate),
                    .blueprintIndex = *blueprintIndex,
                });
                scene.events.emit(rm::sim::Event{
                    .kind = rm::sim::EventKind::ConstructionStarted,
                    .instigator = standing.factory,
                    .army = army.index,
                    .amount = def.buildCostMass,
                    .at = standing.factoryPosition,
                });
            }
        }

        // The one attack wave: at strength, every tank walks at the nearest enemy
        // commander. After this, reinforcements are sent as they roll off.
        if (rm::sim::launchesAttack(script, view, scene.opening.waveSize)) {
            const std::optional<std::array<rm::sim::Fx, 3>> target =
                nearestEnemyCommander(scene, army.index, standing.commanderPosition);
            if (target) {
                script.attackLaunched = true;
                std::size_t marching = 0;
                for (const rm::sim::UnitId tank : standing.tanks) {
                    if (!scene.store.alive(tank)) {
                        continue;  // died between the census and the order
                    }
                    const auto type =
                        static_cast<std::size_t>(scene.store.typeAt(tank.index));
                    const rm::sim::PassabilityGrid& grid =
                        passability.gridFor(scene.maxSlopeDegrees[type],
                                            scene.maxWaterDepthElmos[type]);
                    if (issueMove(scene, grid, field, tank,
                                  playerDriving(scene, army.index), tickIndex, (*target)[0],
                                  (*target)[2])) {
                        ++marching;
                    }
                }
                std::printf("  [%6.1fs] army %d ATTACKS with %zu of %zu tanks\n",
                            static_cast<double>(elapsedSeconds), army.index, marching,
                            standing.tanks.size());
            }
        }
    }
}


[[nodiscard]] MatchRunner makeMatchRunner(UnitScene& scene, const rm::HeightField& field,
                                          PassabilitySet& passability,
                                          const rm::vfs::Vfs& content,
                                          std::span<const rm::mapinfo::StartPosition> starts,
                                          std::span<const rm::scenario::Marker> markers) {
    MatchRunner runner{
        .scene = scene,
        .field = field,
        .passability = passability,
        .content = content,
        .starts = starts,
        .markers = markers,
        .scripts = std::vector<rm::sim::Opponent>(scene.armies.size()),
        .match =
            rm::sim::Match{
                .armies = scene.armies,
                .economies = scene.economies,
                .projectiles = &scene.projectiles,
                .building = &scene.building,
                .events = &scene.events,
                .features = &scene.features,
                .commandersEver = scene.commandersEver,
                .baseStorage = kStartingStorage,
                .intel = &scene.intel,
                // Seeded from the scene rather than defaulted to false, using the same
                // predicate the sim decides on (Skirmish.cpp): a match with one side left
                // is already over. Two cases need it, and both are announcements that
                // would otherwise be wrong:
                //
                //   `--play` pre-runs the match headless and THEN opens the window. A
                //   runner built fresh over those armies would re-detect the end on its
                //   first tick and announce a winner the pre-run already announced.
                //
                //   A `--units` crowd has no armies at all, so `survivorCount` is zero,
                //   which is also `<= 1` — without this the interactive window would
                //   declare a DRAW on tick one of every decorative scene.
                .over = scene.armies.empty()
                        || rm::sim::survivorCount(scene.armies) <= 1,
            },
    };
    scene.grewThisTick = false;
    return runner;
}



void printEvents(const rm::sim::EventQueue& events, float now) {
    for (const rm::sim::Event& event : events.all()) {
        std::printf("  [%6.1fs] %-21s unit %u.%u by %u.%u army %d amount %.0f at %.0f,%.0f\n",
                    static_cast<double>(now),
                    std::string{rm::sim::eventKindName(event.kind)}.c_str(),
                    event.unit.index, event.unit.generation, event.instigator.index,
                    event.instigator.generation, event.army,
                    static_cast<double>(rm::sim::magToFloat(event.amount)),
                    static_cast<double>(rm::sim::fxToFloat(event.at[0])),
                    static_cast<double>(rm::sim::fxToFloat(event.at[2])));
    }
}

rm::sim::TickReport advanceMatch(MatchRunner& runner, int tickIndex, float now) {
    UnitScene& scene = runner.scene;

    // THE TICK'S EVENTS START HERE, not inside `tickSkirmish`. The caller raises some of them
    // itself — the opponents' build orders below, and `UnitCreated` when a finished construction
    // becomes a unit — so the boundary has to be the caller's tick, which is this function.
    //
    // `beginFrame` rather than the `clear()` this used to be: the frame now carries the tick it
    // belongs to, and advancing it is idempotent, so a second caller beginning the frame it is
    // already in destroys nothing. See `Events.hpp` — that property exists because the opposite
    // once cost two event kinds that were emitted and never observable.
    scene.events.beginFrame(static_cast<rm::TickIndex>(tickIndex));

    // The routing table the sim uses to advance queued orders, refreshed for whatever types the
    // catalog now holds. `gridFor` is memoised on the LIMITS, so this is a map lookup per type
    // per tick and builds a grid only the first time a new pair of limits appears.
    runner.gridForType.clear();
    runner.gridForType.reserve(scene.catalog.size());
    for (std::size_t type = 0; type < scene.catalog.size(); ++type) {
        if (type >= scene.maxSlopeDegrees.size() || type >= scene.maxWaterDepthElmos.size()) {
            runner.gridForType.push_back(nullptr);
            continue;
        }
        runner.gridForType.push_back(&runner.passability.gridFor(
            scene.maxSlopeDegrees[type], scene.maxWaterDepthElmos[type]));
    }
    runner.match.passability = runner.gridForType;

    // The opponents decide FIRST, so an order given this tick moves this tick.
    //
    // Called EVERY tick now, and the pacing lives inside — `runOpponents` asks a
    // `sim::SlowUpdate` whether each army's turn is this tick (§7 P3.6). The modulo that used
    // to be on this line was the hand-rolled version of that mechanism, and it could only ever
    // ask the question for all armies at once.
    if (!scene.armies.empty() && !runner.matchOver) {
        runOpponents(scene, runner.content, runner.field, runner.passability, runner.starts,
                     runner.markers, runner.scripts, now,
                     static_cast<rm::TickIndex>(tickIndex));
    }

    // ONE call, and the same one both callers make. What used to be here — the order of
    // movement, collision, aiming, firing, death, defeat and economy — is a fact about
    // core/sim/Skirmish.cpp rather than about whichever loop you are reading.
    const rm::sim::TickReport report =
        rm::sim::tickSkirmish(scene.store, scene.catalog, runner.match,
                              rm::sim::Terrain{runner.field}, gAppTickRate);

    runner.shotsFired += report.shotsFired;
    scene.deathBlasts += report.deathBlasts;
    scene.deathBlastDamage += report.deathBlastDamage;
    if (report.matchEnded) {
        runner.matchOver = true;
    }

    // The scorch each death leaves, sized from what died: a commander marks more ground
    // than a tank. Permanent, because a wreck IS the record of what happened here and a
    // battlefield that tidied itself up would lose it.
    //
    // The caller's work rather than the sim's: a decal buffer is a thing the renderer
    // uploads, and the sim has no business owning one.
    // FROM THE EVENTS, not from the report (§7 P6.3). The tally and the wreck marks are the
    // app's reaction to a death, and reacting to `UnitDestroyed` rather than to
    // `TickReport::died` is what the event queue is for — the caller stops needing a
    // second, differently-shaped channel for the same fact. `report.died` is still what the
    // sim hands back for the callers that need a death's radius, and both agree by
    // construction: `retireDead` fills one from the other.
    for (const rm::sim::Event& event : scene.events.all()) {
        if (event.kind == rm::sim::EventKind::UnitDestroyed) {
            ++runner.unitsDestroyed;
        }
    }
    refreshWreckDecals(scene, runner.field);

    // What finished this tick BECOMES A UNIT: an extractor that is done stands on its
    // deposit, a factory stands by the base, and a tank rolls off the factory floor.
    //
    // The caller's work, and it has to be: a finished construction turns into a model out
    // of the VFS, which is the one thing the sim cannot do for itself.
    for (const rm::sim::Construction& work : report.finished) {
        const auto army = static_cast<std::size_t>(work.armyIndex);
        if (army >= scene.armies.size()) {
            continue;
        }
        ++runner.completedBuilds;
        // OUT of fixed point, here at the edge (§7 P10.0). Putting a unit on the map needs
        // a model and a float transform, so this is the legitimate direction — the sim
        // holds the authority and the renderer gets a copy, never the other way round.
        const std::array<float, 3> site{rm::sim::fxToFloat(work.position[0]),
                                        rm::sim::fxToFloat(work.position[1]),
                                        rm::sim::fxToFloat(work.position[2])};
        // Face the map centre — a base laid out toward the fight reads as one.
        const float yaw = std::atan2(runner.field.widthElmos() * 0.5f - site[0],
                                     runner.field.depthElmos() * 0.5f - site[2]);
        const auto spawned = spawnUnit(scene, runner.content, runner.field,
                                       std::string{scene.pathOf(static_cast<rm::UnitTypeIndex>(work.blueprintIndex))},
                                       site, scene.armies[army], yaw);
        // `UnitFinished` after `UnitCreated`, which `spawnUnit` raised: the pair is Recoil's
        // (`04 §4.2` — `UnitCreated` then `UnitFinished` when the build completes) and the
        // distinction matters to anything that treats a built unit differently from one placed
        // at match start. Both come from the caller, because both need the model to exist.
        if (spawned) {
            scene.events.emit(rm::sim::Event{
                .kind = rm::sim::EventKind::UnitFinished,
                .unit = *spawned,
                .army = work.armyIndex,
                .amount = work.cost.mass,
                .at = work.position,
            });
        }
        if (spawned && buildableDef(scene, work.blueprintIndex).isMobile()) {
            // Off the factory floor: straight to the fight once the wave has gone, to the
            // rally point outside the base while it forms.
            const auto target = runner.scripts[army].attackLaunched
                                    ? nearestEnemyCommander(scene, work.armyIndex,
                                                            work.position)
                                    : std::nullopt;
            const std::array<rm::sim::Fx, 2> to =
                target ? std::array<rm::sim::Fx, 2>{(*target)[0], (*target)[2]}
                       : rm::sim::rolloffPoint(
                             work.position,
                             rm::sim::Fx::fromInt(runner.field.squaresX * rm::kSquareSize / 2),
                             rm::sim::Fx::fromInt(runner.field.squaresZ * rm::kSquareSize
                                                  / 2));
            const auto type = static_cast<std::size_t>(scene.store.typeAt(spawned->index));
            const rm::sim::PassabilityGrid& grid =
                runner.passability.gridFor(scene.maxSlopeDegrees[type],
                                           scene.maxWaterDepthElmos[type]);
            (void)issueMove(scene, grid, runner.field, *spawned,
                            playerDriving(scene, work.armyIndex),
                            static_cast<rm::TickIndex>(tickIndex), to[0], to[1]);
        }
    }

    // Nothing to rebuild any more. This used to re-point a vector of per-batch spans,
    // because a spawn into an existing batch reallocated the vector it landed in and left
    // every span into it dangling — a segfault for the first reader that looked straight
    // after the call. The store hands out spans on demand instead, so a grown array is
    // simply a longer span next time somebody asks.
    scene.grewThisTick = false;

    // THE SNAPSHOT (§7 P7.1), last thing in the tick — after the spawns, so a unit that
    // appeared this tick is in it. `publish` rotates: what was current becomes previous, and
    // the frame loop draws between the two.
    scene.publish(static_cast<rm::TickIndex>(tickIndex) + 1);

    // LAST, so the dump is the whole tick: the sim's own events and then the caller's
    // `UnitCreated`/`UnitFinished`, in the order they happened. Printed before the caller-side
    // work at first, which meant the two kinds only the caller can raise were emitted after the
    // print and cleared by the next tick — declared, emitted, and never once observable.
    if (gPrintEvents) {
        printEvents(scene.events, now);
    }

    return report;
}

void march(UnitScene& scene, const rm::HeightField& field, PassabilitySet& passability,
           const MarchOptions& options, std::span<const rm::AmbientEmitter> ambient,
           std::vector<rm::Particle>& dust, const rm::vfs::Vfs& content,
           std::span<const rm::mapinfo::StartPosition> starts,
           std::span<const rm::scenario::Marker> markers) {
    std::size_t routed = 0;
    std::size_t total = 0;
    std::vector<bool> announced(scene.armies.size(), false);
    // `--march` sends everything at one point; `--play` lets the match decide.
    if (options.orderAll) {
        for (rm::UnitIndex slot = 0; slot < scene.store.slotCount(); ++slot) {
            {
                ++total;
                const auto type = static_cast<std::size_t>(scene.store.typeAt(slot));
                const rm::sim::PassabilityGrid& grid = passability.gridFor(
                    scene.maxSlopeDegrees[type], scene.maxWaterDepthElmos[type]);
                if (issueMove(scene, grid, field, scene.store.idAt(slot),
                              playerDriving(scene, scene.store.motion()[slot].armyIndex), 0,
                              rm::sim::fxFromFloat(options.x),
                              rm::sim::fxFromFloat(options.z))) {
                    ++routed;
                }
            }
        }
    }

    // Whole ticks from a duration, rather than feeding a wall clock: this has
    // to land on exactly the same state every run.
    // Through the RUN'S rate, not the default one. This read `kTicksPerSecond` and so ran a
    // 520-second `--play` for 5200 ticks whatever `--tick-rate` said — at 20 Hz that is 260
    // seconds of match, which is why nothing was built and nobody fired. Exactly the bug
    // §5.1 exists to prevent, found by running the four rates rather than by reading.
    const auto ticks =
        static_cast<int>(gAppTickRate.ticks(rm::sim::seconds(options.seconds)));

    // The caller-side tick, shared with the windowed frame loop. See MatchRunner.
    MatchRunner runner =
        makeMatchRunner(scene, field, passability, content, starts, markers);

    // Per-tick state hashes, kept when this run has been asked to record or check them.
    // Reserved up front so the recording cannot itself perturb what it measures by
    // reallocating mid-match.
    const bool hashing = !options.hashLogPath.empty() || !options.checkHashLogPath.empty();
    std::vector<rm::StateHash> hashes;
    if (hashing) {
        hashes.reserve(static_cast<std::size_t>(ticks));
    }

    const float kTickSeconds = gAppTickRate.secondsPerTick();
    float dustDebt = 0.0f;
    float ambientDebt = 0.0f;
    std::uint32_t dustSeed = 0x51ED27u;
    std::vector<rm::DustEmitter> emitters;

    for (int i = 0; i < ticks; ++i) {
        const float now = static_cast<float>(i) * kTickSeconds;

        // ONE call, and the same one the windowed frame loop makes: the opponents'
        // decisions, the sim's tick, the wrecks, and the finished builds that become
        // units. See MatchRunner for what is deliberately left to each caller.
        const rm::sim::TickReport report = advanceMatch(runner, i, now);

        // Fingerprinted after the WHOLE caller-side tick, which deliberately includes the
        // units a finished construction spawned: what got built is part of what the match
        // did, and a hash that stopped at the sim's own passes would agree about two runs
        // that built different things. The renderer's side stays out on its own — the decal
        // buffer is not reachable from a group or a match, so `hashMatch` cannot see it.
        if (hashing) {
            hashes.push_back(rm::sim::hashMatch(scene.store, runner.match));
        }

        if (!scene.armies.empty()) {

            // What the tick decided, announced. The decisions themselves are the sim's;
            // saying them out loud is this run's, which is why only the printing is left.
            for (const rm::sim::Army& army : scene.armies) {
                if (army.defeated && !announced[static_cast<std::size_t>(army.index)]) {
                    announced[static_cast<std::size_t>(army.index)] = true;
                    std::printf("  [%6.1fs] army %d (%s) has lost its commander\n",
                                static_cast<double>(now), army.index,
                                std::string{rm::sim::factionName(army.faction)}.c_str());
                }
            }
            if (report.matchEnded) {
                if (report.winner) {
                    std::printf("  [%6.1fs] team %d WINS\n", static_cast<double>(now),
                                *report.winner);
                } else {
                    std::printf("  [%6.1fs] a DRAW: every army lost its commander\n",
                                static_cast<double>(now));
                }
            }
        }

        // What finished this tick, narrated. The spawning itself is `advanceMatch`'s, so
        // that the windowed loop gets the buildings too; only saying so is this run's.
        for (const rm::sim::Construction& work : report.finished) {
            const auto army = static_cast<std::size_t>(work.armyIndex);
            if (army >= scene.armies.size()) {
                continue;
            }
            std::printf("  [%6.1fs] army %zu completes %s\n", static_cast<double>(now), army,
                        buildableDef(scene, work.blueprintIndex).name.c_str());
        }

        // Dust as the walk happens, aged as the walk continues, so what a capture
        // shows is a trail rather than a puff at everyone's feet.
        emitters.clear();
        for (rm::UnitIndex slot = 0; slot < scene.store.slotCount(); ++slot) {
            const rm::sim::MoveState& motion = scene.store.motion()[slot];
            emitters.push_back(rm::DustEmitter{
                .position = {rm::sim::fxToFloat(scene.store.transforms()[slot].x),
                             rm::sim::fxToFloat(scene.store.transforms()[slot].y),
                             rm::sim::fxToFloat(scene.store.transforms()[slot].z)},
                .moving = motion.moving,
                .topSpeedElmosPerSecond = rm::sim::fxToFloat(motion.speedPerTick)
                                          * static_cast<float>(
                                              gAppTickRate.ticksPerSecond()),
                .radiusElmos = rm::sim::fxToFloat(motion.radiusElmos),
            });
        }
        rm::advanceParticles(dust, kTickSeconds);
        rm::emitDust(dust, emitters, field, kTickSeconds, dustDebt, dustSeed);
        rm::emitAmbient(dust, ambient, kTickSeconds, ambientDebt, dustSeed);
    }

    // A marched scene has been walked, so its walk cycles are paced by the
    // ground covered — the same rule the windowed path follows. This is also
    // what makes such a screenshot independent of `--time`: the sim decided
    // where the legs are, not the clock.
    // The walk cycles, paced from the snapshot, then gathered for drawing. Per TYPE because the
    // animation belongs to the model: every unit of one type shares its clock length, and the
    // pass writes each unit's own phase from the ground it has covered.
    //
    // Alpha 1 explicitly: a marched scene is a capture of one tick, and a blend of two would
    // make the screenshot depend on where the pre-run happened to stop.
    scene.publish(0);
    scene.gatherForDrawing(1.0f);

    // --- Determinism ---------------------------------------------------------
    //
    // Written before the summaries below, so that a run whose whole purpose was the
    // artifact says whether it got one before saying anything else.
    if (!options.commandLogPath.empty()) {
        if (rm::sim::writeCommandLog(scene.commands, options.commandLogPath)) {
            std::printf("commands: %zu order(s) over %llu tick(s) written to %s\n",
                        scene.commands.size(),
                        static_cast<unsigned long long>(scene.commands.lastTick()),
                        options.commandLogPath.c_str());
        } else {
            std::printf("commands: cannot write %s\n", options.commandLogPath.c_str());
        }
    }

    if (hashing) {
        rm::sim::ReplayHeader header;
        header.ticksPerSecond = static_cast<int>(gAppTickRate.ticksPerSecond());
        header.widthsFingerprint = rm::sim::widthsFingerprint();
        header.tickCount = hashes.size();

        if (!options.checkHashLogPath.empty()) {
            const auto recorded = rm::sim::readHashLog(options.checkHashLogPath);
            if (!recorded) {
                std::printf("determinism: cannot read %s — %s\n",
                            options.checkHashLogPath.c_str(),
                            recorded.error().message.c_str());
            } else {
                const rm::sim::Divergence heads =
                    rm::sim::compareHeaders(recorded->header, header);
                if (heads.incomparable) {
                    std::printf("determinism: NOT COMPARABLE — %s\n", heads.why.c_str());
                } else {
                    const rm::sim::Divergence d =
                        rm::sim::compareHashes(recorded->hashes, hashes);
                    if (!d.diverged) {
                        std::printf("determinism: MATCH — %zu ticks identical to %s\n",
                                    hashes.size(), options.checkHashLogPath.c_str());
                    } else {
                        // The first divergent tick, which is the only one with diagnostic
                        // value: every later tick is downstream of it.
                        std::printf("determinism: DIVERGED at tick %llu —"
                                    " recorded %016llx, this run %016llx\n",
                                    static_cast<unsigned long long>(d.tick),
                                    static_cast<unsigned long long>(d.recorded),
                                    static_cast<unsigned long long>(d.replayed));
                        if (!d.why.empty()) {
                            std::printf("            %s\n", d.why.c_str());
                        }
                        std::printf("            re-run with --hash-log to capture this"
                                    " run, then diff the two files at that line\n");
                    }
                }
            }
        }

        if (!options.hashLogPath.empty()) {
            const auto written =
                rm::sim::writeHashLog(options.hashLogPath, header, hashes);
            if (written) {
                std::printf("determinism: %zu tick hashes written to %s\n", hashes.size(),
                            options.hashLogPath.c_str());
            } else {
                std::printf("determinism: cannot write %s — %s\n",
                            options.hashLogPath.c_str(), written.error().message.c_str());
            }
        }
    }

    // Saying how many found a route matters on a map like aw04, where most
    // units are on islands: a unit that cannot walk there stays put, and
    // without this line that reads as the sim being broken.
    if (options.orderAll) {
        std::printf("march: %zu of %zu units routed to (%.0f, %.0f), %d ticks simulated,"
                    " %zu dust particles still in the air\n",
                    routed, total, static_cast<double>(options.x),
                    static_cast<double>(options.z), ticks, dust.size());
    } else {
        std::printf("play: %d ticks simulated, %zu dust particles still in the air\n", ticks,
                    dust.size());
    }

    if (!scene.armies.empty()) {
        // What the fight came to. Reported rather than inferred from a screenshot,
        // because a unit that died is a unit that is no longer in the frame and its
        // absence looks the same as its never having been there.
        rm::sim::Mag remaining{};
        rm::sim::Mag maximum{};
        for (const rm::sim::Health& one : scene.store.health()) {
            remaining += one.current;
            maximum += one.maximum;
        }

        std::printf("combat: %zu shots fired, %zu in flight, %zu unit(s) destroyed,"
                    " %.0f of %.0f hp left\n",
                    runner.shotsFired, scene.projectiles.size(), runner.unitsDestroyed,
                    static_cast<double>(rm::sim::magToFloat(remaining)),
                    static_cast<double>(rm::sim::magToFloat(maximum)));

        // Each commander's state, because the match hangs on exactly these numbers
        // and "the fight is still on" and "the fight never reached anyone" read the
        // same from the aggregate.
        for (rm::UnitIndex slot = 0; slot < scene.store.slotCount(); ++slot) {
            const rm::unitdef::UnitDef* def = scene.catalog.def(scene.store.typeAt(slot));
            if (def == nullptr || !rm::sim::isCommanderId(def->name)) {
                continue;
            }
            std::printf("  army %d commander: %.0f of %.0f hp\n",
                        scene.store.motion()[slot].armyIndex,
                        static_cast<double>(
                            rm::sim::magToFloat(scene.store.health()[slot].current)),
                        static_cast<double>(
                            rm::sim::magToFloat(scene.store.health()[slot].maximum)));
        }
        if (scene.deathBlasts > 0 || scene.features.size() > 0) {
            // Counted from the FEATURES now, not by dividing a vertex buffer by the vertices
            // per mark (§7 P6.2). The old form was the tell that the decal buffer was the only
            // record: to say how many things had died you had to do arithmetic on triangles.
            std::printf("wreckage: %zu wreck(s), %zu death explosion(s) dealing %.0f damage\n",
                        scene.features.size(), scene.deathBlasts,
                        static_cast<double>(rm::sim::magToFloat(scene.deathBlastDamage)));
        }
    }

    if (!scene.economies.empty()) {
        const rm::sim::Economy& first = scene.economies.front();
        std::printf("economy: %zu of %zu build(s) complete; army 0 holds %.0f mass /"
                    " %.0f energy, earning %.1f / %.1f a second against %.1f energy of"
                    " upkeep, %.0f%% funded\n",
                    runner.completedBuilds, scene.building.size(),
                    static_cast<double>(rm::sim::magToFloat(first.stored.mass)),
                    static_cast<double>(rm::sim::magToFloat(first.stored.energy)),
                    // Reported PER SECOND, which is what a reader wants, converted back from
                    // the per-tick figure the sim keeps.
                    static_cast<double>(rm::sim::magToFloat(first.incomePerTick.mass))
                        * gAppTickRate.ticksPerSecond(),
                    static_cast<double>(rm::sim::magToFloat(first.incomePerTick.energy))
                        * gAppTickRate.ticksPerSecond(),
                    static_cast<double>(rm::sim::magToFloat(first.upkeepPerTick.energy))
                        * gAppTickRate.ticksPerSecond(),
                    static_cast<double>(rm::sim::fxToFloat(first.fundedFraction)) * 100.0);
    }
}

} // namespace rm::app
