// ORDER ADVANCEMENT — what the queue does next, per tick, per unit.
//
// `advanceOrders` owns completion: it decides an order is finished, drops the head that cannot
// be started, and calls `startCommand` on what follows — including construction, which
// materialises its product HERE because that is where retail retires the build task (C-142,
// C-188), inside command dispatch rather than out of an economy write-back.
// `updateAggressiveOrders` runs the combat half after movement and intel: attack-move and
// patrol keep their waypoint while `target` names the hostile that interrupted it.
// `publishPathResult` is the seam where the asynchronous path service answers back through
// command authority.
//
// `startCommand` is defined here — it is the advancement machinery — but declared in
// `CommandInternal.hpp` because `CommandApply.cpp` calls it for orders that start on issue.
#include "core/sim/Command.hpp"
#include "core/sim/CommandInternal.hpp"

#include "core/sim/Combat.hpp"
#include "core/sim/Enhancement.hpp"
#include "core/sim/Capture.hpp"
#include "core/sim/Movement.hpp"
#include "core/sim/Reclaim.hpp"
#include "core/sim/ScriptTask.hpp"
#include "core/sim/Transport.hpp"
#include "core/sim/UnitStore.hpp"

#include "core/unit/BuildTree.hpp"
#include "core/unit/Role.hpp"

#include <algorithm>
#include <memory>
#include <optional>

namespace rm::sim {
namespace {

[[nodiscard]] const Army* armyFor(int index, std::span<const Army> armies) noexcept {
    for (const Army& army : armies) {
        if (army.index == index) return &army;
    }
    return nullptr;
}
/// Routes one unit according to its content movement layer. Aircraft fly directly over the
/// map; supported ground classes retain A* and refuse an unreachable destination.
[[nodiscard]] bool routeUnit(UnitIndex slot, Fx toX, Fx toZ, UnitStore& store,
                             const Terrain& terrain, const PassabilityGrid& grid) {
    MoveState& motion = store.motion()[slot];
    // A flyer's route is the air whether it is flying or merely flyable:
    // orderTo is also the takeoff commit, and a landed aircraft sent through
    // ground A* searches its (empty) movement domain and goes nowhere.
    if (motion.airborne || motion.canFly) {
        orderTo(motion, terrain, toX, toZ);
        return true;
    }
    const Transform& at = store.transforms()[slot];
    const std::vector<std::array<Fx, 2>> path = findPath(grid, at.x, at.z, toX, toZ);
    if (path.empty()) {
        return false;
    }
    orderAlongPath(motion, path);
    return true;
}
/// Whether the active queue entry already owns a completed row. Finished constructions remain
/// as match history, so "no unfinished row" alone cannot distinguish completion from approach.
[[nodiscard]] bool finishedConstructionFor(const QueuedCommand& command, UnitIndex slot,
                                           const UnitStore& store, const UnitCatalog& catalog,
                                           std::span<const Construction> building) noexcept {
    const auto site = buildSiteFor(command.buildType(), command.targetX(), command.targetZ(),
                                   slot, store, catalog);
    if (!site) {
        return false;
    }
    return std::ranges::any_of(building, [&](const Construction& work) {
        return work.finished() && work.builder == store.idAt(slot)
            && work.blueprintIndex == command.buildType() && work.position[0] == site->first
            && work.position[2] == site->second;
    });
}
/// The unfinished row THIS Build order owns: the builder's, on the site the order resolves to
/// (`buildSiteFor` — the pad for an upgrade or a factory product, the target for a placed
/// structure). Scoping by site is what an abandoned scaffold needs: the founder that walked
/// away still has its row, and only the order pointing back at that row may reattach to it.
[[nodiscard]] Construction* activeConstructionFor(const QueuedCommand& command, UnitIndex slot,
                                                  const UnitStore& store,
                                                  const UnitCatalog& catalog,
                                                  std::vector<Construction>& building) noexcept {
    const auto site = buildSiteFor(command.buildType(), command.targetX(), command.targetZ(),
                                   slot, store, catalog);
    if (!site) {
        return nullptr;
    }
    const auto found = std::ranges::find_if(building, [&](const Construction& work) {
        return !work.finished() && work.builder == store.idAt(slot)
            && work.blueprintIndex == command.buildType()
            && work.position[0] == site->first && work.position[2] == site->second;
    });
    return found != building.end() ? &*found : nullptr;
}
/// Whether `builder`'s army is on `slot`'s side. No alliance state (a bare test store) counts
/// as allied, the way the repair path already reads it.
[[nodiscard]] bool alliedBuilder(UnitId builder, UnitIndex slot, const UnitStore& store,
                                 std::span<const Army> armies) noexcept {
    if (!store.alive(builder)) {
        return false;
    }
    if (armies.empty()) {
        return true;
    }
    const int mine = store.motion()[slot].armyIndex;
    const int theirs = store.motion()[builder.index].armyIndex;
    const auto a = std::ranges::find_if(armies, [mine](const Army& army) { return army.index == mine; });
    const auto b = std::ranges::find_if(armies, [theirs](const Army& army) { return army.index == theirs; });
    return a != armies.end() && b != armies.end() && allied(*a, *b);
}
/// The `CapCost` an army's unfinished constructions already reserve. Retail's
/// rising unit is a live entity from the moment the scaffold exists, so it
/// counts toward the cap the whole time it is being built — matching the gate
/// at `0x0074fda0`, which adds only the NEW unit's `CapCost` to a total that
/// already includes it.
[[nodiscard]] Fx reservedCapCost(std::span<const Construction> building,
                                 const UnitCatalog& catalog, int armyIndex) noexcept {
    Fx reserved{};
    for (const Construction& work : building) {
        if (work.armyIndex != armyIndex || work.finished()) {
            continue;
        }
        if (const unitdef::UnitDef* def =
                catalog.def(static_cast<UnitTypeIndex>(work.blueprintIndex))) {
            reserved = reserved + def->capCost;
        }
    }
    return reserved;
}

/// Retail's creation gate (`0x0074fda0`): `costTotal + reserved + new CapCost`
/// over `unitCap` refuses the unit. `armies` empty means a bare test store with
/// no army records — nothing is capped.
[[nodiscard]] bool unitCapBlocks(std::span<const Army> armies, int armyIndex,
                                 const UnitCatalog& catalog,
                                 std::span<const Construction> building,
                                 const unitdef::UnitDef& product) noexcept {
    if (armies.empty() || armyIndex < 0
        || static_cast<std::size_t>(armyIndex) >= armies.size()) {
        return false;
    }
    const Army& army = armies[static_cast<std::size_t>(armyIndex)];
    return army.unitCostTotal + reservedCapCost(building, catalog, armyIndex)
               + product.capCost
           > army.unitCap;
}

} // namespace

bool factoryProductionCapped(const Command& command, const UnitStore& store,
                             const UnitCatalog& catalog, std::span<const Army> armies,
                             std::span<const Construction> building) noexcept {
    if (command.kind != CommandKind::Build || !store.slotAlive(command.unit.index)) {
        return false;
    }
    const unitdef::UnitDef* builder = catalog.def(store.typeAt(command.unit.index));
    const unitdef::UnitDef* product = catalog.def(command.buildType);
    if (builder == nullptr || product == nullptr || !builder->hasCategory("FACTORY")
        || !product->isMobile()) {
        return false;  // only factory production holds; anything else refuses
    }
    return unitCapBlocks(armies, store.motion()[command.unit.index].armyIndex, catalog,
                         building, *product);
}

namespace {

/// Whether an order is finished the moment it is started.
///
/// A stop is instantaneous by definition. A build occupies its founder until completion;
/// movement kinds use the unit's `moving` flag.
[[nodiscard]] bool instantaneous(CommandKind kind) noexcept {
    return kind == CommandKind::Stop;
}
enum class ScriptDispatch : std::uint8_t {
    Waiting,
    Finished,
    Aborted,
    Delay,
};

struct ScriptDispatchResult {
    ScriptDispatch state = ScriptDispatch::Waiting;
    bool created = false;
};

/// Runs one native scheduler turn for the active script command.
///
/// Status zero deliberately loops without a safety cap: retail's task scheduler does the same,
/// and an adapter that returns Repeat forever has authored an infinite simulation task. Hiding
/// that bug behind an arbitrary cap would make the resulting match depend on our chosen number.
[[nodiscard]] ScriptDispatchResult dispatchScriptTask(CommandQueue& orders,
                                                       ScriptTaskHost& host,
                                                       bool endOfBeat) {
    QueuedCommand* command = orders.currentMutable();
    if (command == nullptr || command->kind() != CommandKind::Script) {
        return {};
    }

    ScriptTaskState& state = command->scriptState();
    bool created = false;
    if (!state.created) {
        state.created = true;
        host.onCreate(command->unit(), command->payload().scriptTask,
                      command->payload().scriptData, state);
        orders.markCurrentActive();
        created = true;
    }
    if (state.suspended) {
        return {.created = created};
    }
    if (state.sleepBeats > 0) {
        --state.sleepBeats;
        return {.created = created};
    }

    while (true) {
        const std::int32_t result = host.taskTick(
            command->unit(), command->payload().scriptTask, command->payload().scriptData, state);
        if (result == static_cast<std::int32_t>(ScriptTaskStatus::Repeat)) {
            continue;
        }
        if (result == static_cast<std::int32_t>(ScriptTaskStatus::Done)) {
            (void)orders.finish();
            return {.state = ScriptDispatch::Finished, .created = created};
        }
        if (result == static_cast<std::int32_t>(ScriptTaskStatus::Abort) || result < -4) {
            (void)orders.abort();
            return {.state = ScriptDispatch::Aborted, .created = created};
        }
        if (result == static_cast<std::int32_t>(ScriptTaskStatus::Suspend)) {
            state.suspended = true;
            return {.created = created};
        }
        if (result == static_cast<std::int32_t>(ScriptTaskStatus::Delay)) {
            return {.state = endOfBeat ? ScriptDispatch::Waiting : ScriptDispatch::Delay,
                    .created = created};
        }
        if (result > static_cast<std::int32_t>(ScriptTaskStatus::NextBeat)) {
            state.sleepBeats = static_cast<std::uint32_t>(result - 1);
        }
        return {.created = created};
    }
}
[[nodiscard]] bool guardCanWork(UnitIndex slot, const UnitStore& store,
    const UnitCatalog& catalog, std::string_view cap) {
    const auto* def = catalog.def(store.typeAt(slot));
    return def != nullptr && effectiveBuildPerTick(store, catalog, slot) > Mag{}
        && (!def->commandCapsDeclared || def->hasCommandCap(cap));
}
} // namespace

bool publishPathResult(const PathResult& result, UnitStore& store,
                       const UnitCatalog& catalog) {
    if (!store.alive(result.unit)) {
        return false;
    }
    CommandQueue& orders = store.orders()[result.unit.index];
    const QueuedCommand* current = orders.active();
    if (current == nullptr || current->payload().id != result.command) {
        return false;
    }
    if (result.path.empty()) {
        // C-176's retries are spent: the walk was refused, but a lift may not
        // be — the rewrite turns the dead move into a boarding run (#15800).
        return current->kind() == CommandKind::Move
               && offerAutoEmbark(store, catalog, result.unit.index,
                                  current->asCommand());
    }
    orderAlongPath(store.motion()[result.unit.index], result.path);
    return true;
}
std::size_t advanceOrders(UnitStore& store, const UnitCatalog& catalog, const Terrain& terrain,
                           std::span<const PassabilityGrid* const> gridForType, TickRate rate,
                           std::vector<Construction>* building, EventQueue* events,
                           const FeatureStore* features, std::vector<Construction>* finished,
                             PathService* pathService, std::span<const Army> armies,
                             const Intel* intel, const PlayableRect* playableRect,
                             ScriptTaskHost* scriptTasks, std::vector<GuardWork>* guardWork,
                             RandomStream* random, TickIndex tick,
                             std::span<const PassabilityGrid* const> gridForTypeSubmerged) {
    std::size_t started = 0;
    std::vector<UnitIndex> delayedScripts;
    if (guardWork != nullptr) {
        guardWork->clear();
    }

    const std::span<CommandQueue> orders = store.orders();
    const std::span<const MoveState> motion = store.motion();

    for (UnitIndex slot = 0; slot < orders.size(); ++slot) {
        if (!store.slotAlive(slot)) {
            continue;
        }
        if (orders[slot].empty()) {
            if (store.motion()[slot].canFly) {
                store.motion()[slot].airCombatState = MoveState::AirCombatState::None;
                store.motion()[slot].airCombatDeadline = 0;
                store.motion()[slot].airSustainedTicks = 0;
                store.motion()[slot].airYawVelocity = {};
            }
            continue;
        }
        if (scriptTasks != nullptr) {
            orders[slot].bindScriptTaskHost(scriptTasks);
        }

        const auto builderType = static_cast<std::size_t>(store.typeAt(slot));
        // The LAYER the unit occupies now, not just its type: a submerged
        // submarine routes on its own (usually shallower) water grid, and
        // C-219's hold-the-old-layer rule is what keying on `submerged` —
        // which flips only at the transition's end — reproduces.
        const PassabilityGrid* approachGrid =
            layerGridFor(gridForType, gridForTypeSubmerged,
                         motion[slot].submersible && motion[slot].submerged, builderType);
        // Movement uses this unit's grid; construction uses the PRODUCT's. Commands retain type
        // ids but not derived grids, so the choice must be repeated when a deferred order starts.
        const auto gridFor = [&](const QueuedCommand& command) -> const PassabilityGrid* {
            const PassabilityGrid* builderGrid = approachGrid;
            if (command.kind() != CommandKind::Build) {
                return builderGrid;
            }

            const unitdef::UnitDef* product = catalog.def(command.buildType());
            const bool navalFactory = product != nullptr && product->hasCategory("NAVAL")
                                     && product->hasCategory("FACTORY");
            if (product != nullptr && !product->isMobile() && !navalFactory) {
                return builderGrid;  // the same ordinary-structure fallback as gridForBuild
            }
            const auto productType = static_cast<std::size_t>(command.buildType());
            return productType < gridForType.size() ? gridForType[productType] : nullptr;
        };

        // Run the dispatcher microsteps for commands that have not started yet. Retail can
        // discard several ordinary no-task/inline-done heads in one beat; a real running task
        // is the barrier. Cyclic completion takes a different path below and deliberately does
        // not call this helper until the next beat.
        const auto startPending = [&] {
            while (const QueuedCommand* pending = orders[slot].current()) {
                if (pending->kind() == CommandKind::Script) {
                    if (scriptTasks == nullptr) {
                        return;
                    }
                    const ScriptDispatchResult result =
                        dispatchScriptTask(orders[slot], *scriptTasks, false);
                    started += static_cast<std::size_t>(result.created);
                    if (result.state == ScriptDispatch::Finished) {
                        continue;
                    }
                    if (result.state == ScriptDispatch::Delay) {
                        delayedScripts.push_back(slot);
                    }
                    return;
                }
                const PassabilityGrid* pendingGrid = gridFor(*pending);
                if (pendingGrid == nullptr) {
                    return;  // leave it pending until its movement domain exists
                }
                if ((pending->kind() == CommandKind::Repair || pending->kind() == CommandKind::Guard)
                    && !repairStillAllied(slot, pending->target(), store, armies)) {
                    (void)orders[slot].finish();
                    continue;
                }
                if (pending->kind() == CommandKind::Move && pathService != nullptr
                    && !store.motion()[slot].airborne
                    && !store.motion()[slot].canFly) {
                    MoveState& pendingMotion = store.motion()[slot];
                    const Transform& at = store.transforms()[slot];
                    pendingMotion.pathPhaseStartX = pendingGrid->cellAtWorld(at.x);
                    pendingMotion.pathPhaseStartZ = pendingGrid->cellAtWorld(at.z);
                    pendingMotion.pathPhaseCellsX = pendingGrid->cellsX;
                    pathService->enqueue(PathRequest{.unit = store.idAt(slot),
                                                     .command = pending->payload().id,
                                                     .army = pendingMotion.armyIndex,
                                                     .fromX = at.x,
                                                     .fromZ = at.z,
                                                     .targetX = pending->asCommand().targetX,
                                                     .targetZ = pending->asCommand().targetZ,
                                                     .grid = std::make_shared<PassabilityGrid>(*pendingGrid)});
                    orders[slot].markCurrentActive();
                    ++started;
                    return;
                }
                if (building != nullptr
                    && factoryProductionCapped(pending->asCommand(), store, catalog, armies,
                                               *building)) {
                    return;  // at the cap: production waits, the order keeps its slot
                }
                const bool wasInstant = instantaneous(pending->kind());
                if (startCommand(pending->asCommand(), store, catalog, terrain, *pendingGrid,
                                 rate, building, events, features, armies, approachGrid)) {
                    orders[slot].markCurrentActive();
                    ++started;
                    if (!wasInstant) {
                        return;
                    }
                } else if (pending->kind() == CommandKind::Build && building != nullptr
                           && joinableConstructionAt(*building, pending->asCommand(), store,
                                                     catalog, armies)) {
                    // The site is held by an allied row — worked or abandoned. Either way
                    // the order means "build there": keep it active at the head and let
                    // the dispatch beat turn the refusal into a lend or a takeover.
                    orders[slot].markCurrentActive();
                    ++started;
                    return;
                } else if (pending->kind() == CommandKind::Build && building != nullptr
                           && finished != nullptr) {
                    // PRODUCTION QUEUED ON A RISING FACTORY. The founder's construction
                    // completed THIS BEAT (`finished` holds the row) and the cascade would
                    // otherwise drop the parked mobile-product order before the caller's
                    // completion hand-over can move it to the factory unit that is about to
                    // stand. Leave it at the head for that hand-over; a factory construction
                    // that was cancelled never reaches `finished`, so the refusal below stays
                    // the answer for every other illegal build.
                    const unitdef::UnitDef* founder = catalog.def(store.typeAt(slot));
                    const unitdef::UnitDef* product = catalog.def(pending->buildType());
                    if (founder != nullptr && product != nullptr && product->isMobile()
                        && !founder->hasCategory("FACTORY")) {
                        const bool risingNow = std::ranges::any_of(
                            *finished, [&](const Construction& work) {
                                if (work.builder != store.idAt(slot) || work.isUpgrade()) {
                                    return false;
                                }
                                const unitdef::UnitDef* def = catalog.def(
                                    static_cast<UnitTypeIndex>(work.blueprintIndex));
                                return def != nullptr && def->hasCategory("FACTORY")
                                    && !def->isMobile();
                            });
                        if (risingNow) {
                            return;
                        }
                    }
                    (void)orders[slot].finish();
                } else if (pending->kind() == CommandKind::Move
                           && offerAutoEmbark(store, catalog, slot, pending->asCommand())) {
                    // The walk was refused but a lift was found (#15800): the
                    // rewritten queue starts with the boarding order, so
                    // dispatch loops once more to start it this beat.
                    continue;
                } else {
                    (void)orders[slot].finish();
                }
            }
        };

        // THE BUILD, in the stage retail puts it in (`C-112`, closing its last open residue).
        //
        // Retail's build task materialises its target and then retires its own order from
        // inside the COMMAND-DISPATCH stage, the first stage of a beat (`C-142`, `C-188`) —
        // not out of an end-of-beat economy write-back, which is where this used to happen and
        // which made construction a SECOND queue-head mutation site. `C-211`'s surviving
        // restatement is that exactly one site advances a unit's queue as a consequence of
        // that unit's own sub-task finishing, and this is now it.
        //
        // IT CASCADES, and that is read from the binary rather than assumed:
        // `CUnitMobileBuildTask::TaskTick` (`0x005fdf10`) is a five-state machine whose every
        // state transition is followed by `xor eax,eax` — task status `0`, which `C-188` gives
        // as "re-run the same task immediately, same beat". So a build ordered on a beat also
        // materialises on that beat, and a build that completes retires and lets the next
        // order start and materialise behind it without waiting for the next one.
        //
        // Returns true when a completed build left work for dispatch, so the caller can service
        // the resulting head (or the next repetition of the same one).
        const auto materialiseHead = [&]() -> bool {
            const QueuedCommand* head = orders[slot].active();
            if (head == nullptr || head->kind() != CommandKind::Build || building == nullptr) {
                return false;
            }
            bool completedUpgrade = false;
            if (Construction* work =
                    activeConstructionFor(*head, slot, store, catalog, *building)) {
                // The row may predate the order — a scaffold this builder abandoned and was
                // sent back to. The reach gate stands either way: out of reach keeps routing
                // toward the site, in reach stands the builder still and works.
                const Fx reach =
                    constructionReach(catalog, store.typeAt(slot), head->buildType());
                const Fx gap = groundDistanceElmos(positionOf(store.transforms()[slot]),
                                                   work->position);
                if (gap > reach) {
                    MoveState& mine = store.motion()[slot];
                    const bool routedToSite =
                        mine.airborne
                            ? mine.moving && mine.destinationX == work->position[0]
                                  && mine.destinationZ == work->position[2]
                            : mine.moving && !mine.path.empty()
                                  && mine.path.back()[0] == work->position[0]
                                  && mine.path.back()[1] == work->position[2];
                    if (!routedToSite && approachGrid != nullptr) {
                        (void)routeUnit(slot, work->position[0], work->position[2], store,
                                        terrain, *approachGrid);
                    }
                    return false;
                }
                teardownMovement(store.motion()[slot]);
                // The store flag is authoritative; the record mirrors it so callers that
                // never run `tickSkirmish` — tests driving the queue directly — see the
                // pause in the same tick it was ordered.
                work->paused = store.productionPaused(work->builder);
                advanceConstruction(*work);
                if (!work->finished()) {
                    return false;  // still rising; the order stays at the head
                }
                completedUpgrade = work->isUpgrade();
                if (finished != nullptr) {
                    finished->push_back(*work);
                }
                emit(events, Event{.kind = EventKind::ConstructionFinished,
                                   .army = work->armyIndex,
                                   .amount = work->cost.mass,
                                   .at = work->position});
            }
            // Finished, or cancelled out from under the order — either way this builder's own
            // sub-task is over, so the dispatch stage retires the head and starts what follows.
            const unitdef::UnitDef* builder = catalog.def(store.typeAt(slot));
            const unitdef::UnitDef* product = catalog.def(head->buildType());
            const bool repeats = store.factoryRepeat(store.idAt(slot)) && builder != nullptr
                                  && product != nullptr && builder->hasCategory("FACTORY")
                                  && product->isMobile();
            const std::shared_ptr<SharedCommand> payload =
                store.liveCommand(head->payload().id);
            if (repeats) {
                if (payload != nullptr && payload->remainingCount > 1) {
                    // A repeated shared factory command still consumes each ordinary member
                    // completion. Only its final member restores the batch before cycling.
                    if (!store.decreaseCommandCount(payload->id)) {
                        return false;
                    }
                    orders[slot].deactivateCurrent();
                } else if (payload != nullptr) {
                    payload->remainingCount = payload->originalCount;
                    (void)orders[slot].cycle();
                } else {
                    (void)orders[slot].cycle();
                }
            } else {
                if (payload != nullptr && payload->remainingCount > 1) {
                    if (!store.decreaseCommandCount(payload->id)) {
                        return false;
                    }
                    orders[slot].deactivateCurrent();
                } else {
                    // Ordinary final-count completion retires only this unit's queue entry.
                    // Retail's global zero-count removal is a distinct out-of-band operation
                    // (`C-211`), not the dispatcher's final-count path.
                    (void)orders[slot].finish();
                }
            }
            // The app replaces an upgraded unit after this pass. Leave its successor's
            // orders pending; validating them against the old blueprint would discard them.
            if (completedUpgrade) return false;
            startPending();
            return true;
        };
        const auto serviceBuilds = [&] {
            while (materialiseHead()) {
            }
        };

        if (orders[slot].active() == nullptr) {
            startPending();
            serviceBuilds();
            if (orders[slot].active() == nullptr && store.motion()[slot].canFly) {
                store.motion()[slot].airCombatState = MoveState::AirCombatState::None;
                store.motion()[slot].airCombatDeadline = 0;
                store.motion()[slot].airSustainedTicks = 0;
                store.motion()[slot].airYawVelocity = {};
            }
            continue;
        }

        // BEFORE the grid lookup, deliberately. A build in progress does not move, so asking
        // which passability grid it would route on is a question with no bearing on whether it
        // rises — and answering it first would stall every construction in a scene that has no
        // grid for the product's motion class. `startPending`, which does need one, looks it up
        // for itself.
        const QueuedCommand* current = orders[slot].active();
        if (current->kind() == CommandKind::Guard
            && (!validGuard(current->asCommand(), store, catalog)
                || !repairStillAllied(slot, current->target(), store, armies))) {
            teardownMovement(store.motion()[slot]);
            (void)orders[slot].finish();
            startPending();
            continue;
        }
        MoveState& activeMotion = store.motion()[slot];
        const bool wingedAttack = current->kind() == CommandKind::Attack
                               && current->target().generation != 0
                               && store.alive(current->target())
                               && activeMotion.canFly && activeMotion.airWinged;
        if (activeMotion.canFly && !wingedAttack) {
            activeMotion.airCombatState = MoveState::AirCombatState::None;
            activeMotion.airCombatDeadline = 0;
            activeMotion.airSustainedTicks = 0;
            activeMotion.airYawVelocity = {};
        }
        if (current->kind() == CommandKind::Script) {
            if (scriptTasks == nullptr) {
                continue;
            }
            const ScriptDispatchResult result =
                dispatchScriptTask(orders[slot], *scriptTasks, false);
            if (result.state == ScriptDispatch::Finished) {
                startPending();
                serviceBuilds();
            } else if (result.state == ScriptDispatch::Delay) {
                delayedScripts.push_back(slot);
            }
            continue;
        }
        if (current->kind() == CommandKind::Build) {
            // A mobile build task is active while its engineer approaches. It owns no
            // Construction row until the exact range gate passes, so revisit `startCommand`
            // each beat to turn an arrived approach into materialised work.
            if (building != nullptr
                && activeConstructionFor(*current, slot, store, catalog, *building) == nullptr
                && !finishedConstructionFor(*current, slot, store, catalog, *building)) {
                const PassabilityGrid* buildGrid = gridFor(*current);
                if (buildGrid == nullptr) {
                    continue;
                }
                if (factoryProductionCapped(current->asCommand(), store, catalog, armies,
                                            *building)) {
                    continue;  // at the cap: the factory waits, its order stays
                }
                if (!startCommand(current->asCommand(), store, catalog, terrain, *buildGrid,
                                  rate, building, events, features, armies, approachGrid)) {
                    // Refused on the way in: the site changed under the order. Three cases.
                    //
                    // A COLLEAGUE got there first — an allied builder's construction of the
                    // same blueprint stands on the site and is being worked. Retail treats a
                    // build order onto an existing construction as assisting it, so hold in
                    // reach, lend this beat's rate, and let the order complete with the
                    // colleague's work. A finished one means the job is done.
                    //
                    // An ABANDONED scaffold — the founder was re-tasked or died — is the same
                    // intent with nobody on the tools: this order takes the work over. The
                    // row changes hands, the builder's own head becomes its owner, and from
                    // this beat `materialiseHead` walks it the last mile and advances it.
                    //
                    // ANYTHING ELSE — an enemy on the spot, a different structure — refuses
                    // the order, and the refusal must stop the engineer: without the teardown
                    // it kept coasting along its stale route and parked on the deposit with an
                    // empty queue, which read as "walked there and did nothing".
                    MoveState& mine = store.motion()[slot];
                    Construction* colleague = constructionAtSite(
                        *building, current->buildType(), current->targetX(), current->targetZ());
                    if (colleague != nullptr && !(colleague->builder == store.idAt(slot))
                        && constructionArmyAllied(*colleague, slot, store, armies)) {
                        if (colleague->finished()) {
                            teardownMovement(mine);
                            (void)orders[slot].finish();
                            startPending();
                            continue;
                        }
                        if (constructionWorkedOn(*colleague, store, catalog)) {
                            const Fx reach = constructionReach(catalog, store.typeAt(slot),
                                                               current->buildType());
                            const Fx gap = groundDistanceElmos(
                                positionOf(store.transforms()[slot]), colleague->position);
                            if (gap <= reach) {
                                teardownMovement(mine);
                                colleague->assistPerTick +=
                                    effectiveBuildPerTick(store, catalog, slot);
                            } else if (!mine.moving) {
                                if (approachGrid != nullptr)
                                    (void)routeUnit(slot, colleague->position[0],
                                                    colleague->position[2], store, terrain,
                                                    *approachGrid);
                            }
                            continue;
                        }
                        // The site is nobody's task: hand it to this builder and let the
                        // ordinary build loop take it from here.
                        colleague->builder = store.idAt(slot);
                        colleague->paused = store.productionPaused(colleague->builder);
                        serviceBuilds();
                        continue;
                    }
                    teardownMovement(mine);
                    (void)orders[slot].finish();
                    startPending();
                    continue;
                }
                if (activeConstructionFor(*current, slot, store, catalog, *building) == nullptr) {
                    continue;  // still walking into build range
                }
            }
            serviceBuilds();
            continue;
        }

        // An immobile factory guarding another builder mirrors compatible factory production.
        // Retail reserves the guarded command up front, creates a child factory-build task with
        // no command pointer, and leaves the Guard task itself active (`C-183`, `C-211`). The
        // existing Construction record is that child task: it therefore completes without a
        // second command-count mutation.
        if (isGuardCommand(current->kind()) && building != nullptr) {
            const unitdef::UnitDef* guardDef = catalog.def(store.typeAt(slot));
            if (guardDef != nullptr && guardDef->hasCategory("FACTORY")
                && !guardDef->isMobile() && guardDef->isBuilder()) {
                if (Construction* work = activeConstruction(*building, store.idAt(slot))) {
                    work->paused = store.productionPaused(work->builder);
                    advanceConstruction(*work);
                    if (!work->finished()) {
                        continue;
                    }
                    // This is the guarding factory's OWN queued build, which block A starts
                    // with its command retained. Complete it through the ordinary factory
                    // count/repeat ladder, but operate behind the active guard order (`C-211`).
                    const auto own = std::ranges::find_if(
                        orders[slot].entries(), [work](const QueuedCommand& candidate) {
                            return candidate.kind() == CommandKind::Build
                                   && candidate.payload().id == work->retainedCommandId;
                        });
                    if (own != orders[slot].entries().end()) {
                        const std::shared_ptr<SharedCommand> payload =
                            store.liveCommand(own->payload().id);
                        if (payload != nullptr && payload->remainingCount > 1) {
                            (void)store.decreaseCommandCount(payload->id);
                        } else if (payload != nullptr && store.factoryRepeat(store.idAt(slot))) {
                            payload->remainingCount = payload->originalCount;
                            (void)orders[slot].cycleExact(payload.get());
                        } else if (payload != nullptr) {
                            (void)orders[slot].removeExact(payload.get());
                        }
                    }
                    if (finished != nullptr) {
                        finished->push_back(*work);
                    }
                    emit(events, Event{.kind = EventKind::ConstructionFinished,
                                       .army = work->armyIndex,
                                       .amount = work->cost.mass,
                                       .at = work->position});
                }

                if (!store.alive(current->target())) {
                    // An already-created child is independent of the guarded unit. The branch
                    // above returned while it was unfinished; once none remains, retire Assist.
                    (void)orders[slot].finish();
                    continue;
                }

                // Retail's factory-assist helper scans the guarding factory's retained queue
                // before it considers the guardee's queue. The selected entry remains behind
                // the active Assist and is retired only when its factory-build task completes
                // (`C-211` block A at 0x00619337–0x00619408).
                const std::deque<QueuedCommand>& ownCandidates = orders[slot].entries();
                for (const QueuedCommand& candidate : ownCandidates) {
                    if (candidate.kind() != CommandKind::Build) {
                        continue;
                    }
                    const unitdef::UnitDef* product = catalog.def(candidate.buildType());
                    if (product == nullptr || !product->isMobile()
                        || !canBuild(store, catalog, slot, *product)) {
                        continue;
                    }
                    const auto productType = static_cast<std::size_t>(candidate.buildType());
                    const PassabilityGrid* productGrid =
                        productType < gridForType.size() ? gridForType[productType] : nullptr;
                    if (productGrid == nullptr
                        || (product->motion != unitdef::MotionType::Air
                            && !sitePlaceable(*productGrid, store.transforms()[slot].x,
                                              store.transforms()[slot].z,
                                              fxFromFloat(product->collisionRadiusElmos)))) {
                        continue;
                    }
                    if (factoryProductionCapped(candidate.asCommand(), store, catalog, armies,
                                                *building)) {
                        continue;  // this product waits on the cap; try the next
                    }
                    if (startCommand(candidate.asCommand(), store, catalog, terrain, *productGrid,
                                     rate, building, events, features, armies)) {
                        if (auto* work = activeConstruction(*building, store.idAt(slot))) {
                            work->retainedCommandId = candidate.payload().id;
                        }
                        break;
                    }
                }
                if (activeConstruction(*building, store.idAt(slot)) != nullptr) {
                    continue;
                }

                CommandQueue& guarded = orders[current->target().index];
                std::shared_ptr<SharedCommand> selected;
                Command mirrored;
                const PassabilityGrid* productGrid = nullptr;
                const std::deque<QueuedCommand>& candidates = guarded.entries();
                for (std::size_t at = 0; at < candidates.size(); ++at) {
                    const QueuedCommand& candidate = candidates[at];
                    if (candidate.kind() != CommandKind::Build) {
                        continue;
                    }
                    const unitdef::UnitDef* product = catalog.def(candidate.buildType());
                    if (product == nullptr || !product->isMobile()
                        || !canBuild(store, catalog, slot, *product)) {
                        continue;
                    }
                    const std::shared_ptr<SharedCommand> payload =
                        store.liveCommand(candidate.payload().id);
                    if (payload == nullptr) {
                        continue;
                    }
                    const bool repeating = store.factoryRepeat(current->target());
                    if (at == 0 && payload->remainingCount <= 1
                        && (candidates.size() > 1 || !repeating)) {
                        continue;
                    }
                    const auto productType = static_cast<std::size_t>(candidate.buildType());
                    productGrid = productType < gridForType.size() ? gridForType[productType]
                                                                   : nullptr;
                    if (productGrid == nullptr
                        || (product->motion != unitdef::MotionType::Air
                            && !sitePlaceable(*productGrid, store.transforms()[slot].x,
                                              store.transforms()[slot].z,
                                              fxFromFloat(product->collisionRadiusElmos)))) {
                        continue;
                    }
                    if (factoryProductionCapped(candidate.asCommand(), store, catalog, armies,
                                                *building)) {
                        continue;  // capped product waits; the queue is left alone
                    }
                    selected = payload;
                    mirrored = candidate.asCommand();
                    mirrored.unit = store.idAt(slot);
                    break;
                }

                if (selected != nullptr) {
                    if (selected->remainingCount > 1) {
                        (void)store.decreaseCommandCount(selected->id);
                    } else if (store.factoryRepeat(current->target())) {
                        selected->remainingCount = selected->originalCount;
                        (void)guarded.cycleExact(selected.get());
                    } else {
                        (void)guarded.removeExact(selected.get());
                    }
                    (void)startCommand(mirrored, store, catalog, terrain, *productGrid, rate,
                                       building, events, features, armies);
                    continue;
                }
            }
        }

        // BINGO FUEL and the pad — `C-183`'s refuel/staging rung, first on the
        // ladder. A docked aircraft refuels at the pad's `AI.RefuelingMultiplier`
        // times its own drain rate (`C-223`: the ratio climbs by
        // `multiplier / FuelUseTime × 0.1` a beat) and relaunches at a full tank.
        // A bingo one finds the nearest allied `AIRSTAGINGPLATFORM` whose own
        // `AI.StagingPlatformScanRadius` reaches it — the radius lives on the
        // PAD — with a `DockingSlots` berth free, and flies there; docking is the
        // same attach transport cargo gets (`C-225`). With no pad in reach it
        // holds station as before: idleness runs the auto-land timer, the ground
        // refuels it (`C-222`, `C-223`), and the still-headed order takes off
        // again when prey or the leash calls. The quarter is an adapter
        // convention — no retail bingo fraction was recovered — chosen so a
        // scout's typical 400-second tank keeps a hundred-second reserve.
        constexpr Fx kBingoFuelRatio = Fx::fromRatio(1, 4);
        const MoveState& guardMotion = store.motion()[slot];
        if (isGuardCommand(current->kind()) && guardMotion.canFly
            && (guardMotion.fuelRatio < kBingoFuelRatio || guardMotion.attached)) {
            const UnitId self = store.idAt(slot);
            if (!guardMotion.attached) {
                const Transform& at = store.transforms()[slot];
                UnitId pad{};
                const unitdef::UnitDef* padDef = nullptr;
                Fx nearest{};
                for (UnitIndex other = 0; other < store.slotCount(); ++other) {
                    if (other == slot || !store.slotAlive(other)
                        || !alliedBuilder(store.idAt(other), slot, store, armies)) {
                        continue;
                    }
                    const unitdef::UnitDef* def = catalog.def(store.typeAt(other));
                    if (def == nullptr || !def->isAirStagingPad()
                        || static_cast<std::size_t>(def->transport.dockingSlots)
                               <= store.childrenOf(store.idAt(other)).size()) {
                        continue;
                    }
                    // A length, not a square: Fx distance² saturates past
                    // ~360 elmos and would admit every pad on the map.
                    const Fx dist = fxPolar(store.transforms()[other].x - at.x,
                                            store.transforms()[other].z - at.z)
                                        .length;
                    if (dist > def->stagingScanRadiusElmos
                        || (padDef != nullptr && dist >= nearest)) {
                        continue;
                    }
                    pad = store.idAt(other);
                    padDef = def;
                    nearest = dist;
                }
                if (padDef == nullptr) {
                    teardownMovement(store.motion()[slot]);
                    continue;
                }
                const Transform& padAt = store.transforms()[pad.index];
                // Both radii plus a pad — the same reach a transport load uses.
                const Fx reach = store.motion()[pad.index].radiusElmos
                                 + guardMotion.radiusElmos + Fx::fromInt(2);
                if (fxPolar(padAt.x - at.x, padAt.z - at.z).length > reach) {
                    orderTo(store.motion()[slot], terrain, padAt.x, padAt.z);
                    continue;
                }
                // Dock on a deterministic berth — a two-column grid at half the
                // pad's radius — so the attach can capture the offset.
                const std::size_t berth = store.childrenOf(pad).size();
                const Fx half = store.motion()[pad.index].radiusElmos * Fx::fromRatio(1, 2);
                Transform& place = store.transforms()[slot];
                place.x = padAt.x + Fx::fromInt(static_cast<int>(berth % 2) * 2 - 1) * half;
                place.z = padAt.z + Fx::fromInt(static_cast<int>((berth / 2) % 2) * 2 - 1) * half;
                place.y = padAt.y + fxFromFloat(padDef->sizeYElmos);
                teardownMovement(store.motion()[slot]);
                (void)store.attach(pad, self);
            }
            // Docked on a pad: refuel, and relaunch when the tank reads full.
            const std::optional<UnitId> parent = store.parentOf(self);
            const unitdef::UnitDef* padDef =
                parent && store.alive(*parent) ? catalog.def(store.typeAt(parent->index))
                                               : nullptr;
            if (padDef != nullptr && padDef->isAirStagingPad()) {
                MoveState& docked = store.motion()[slot];
                docked.fuelRatio =
                    std::min(Fx::fromInt(1),
                             docked.fuelRatio
                                 + docked.fuelDrainPerTick
                                       * fxFromFloat(padDef->refuelingMultiplier));
                if (docked.fuelRatio >= Fx::fromInt(1)) {
                    // It lifts off the deck it parked on: Bottom plus the guard
                    // order still at the head, and the ordinary rungs fly it.
                    docked.airState = MoveState::AirState::Bottom;
                    (void)store.detach(self);
                }
            }
            continue;
        }

        // C-183's FERRY rung outranks the leash and every assist: a transport
        // guarding a FERRYBEACON flies the beacon's route. The drive itself
        // lives in the transport pass (`advanceGuardFerry`) — this ladder just
        // yields the tick so no rung routes the carrier back.
        if (isGuardCommand(current->kind()) && store.alive(current->target())) {
            const unitdef::UnitDef* guardDef = catalog.def(store.typeAt(slot));
            const unitdef::UnitDef* beaconDef =
                catalog.def(store.typeAt(current->target().index));
            if (guardDef != nullptr && guardDef->isTransport()
                && beaconDef != nullptr && beaconDef->isFerryBeacon()) {
                continue;
            }
        }

        // C-183's leash is measured from the guard to the guarded unit's current position
        // (or, in retail's richer task object, its resolved guarded/build position). The
        // guardee's full blueprint width is added to half GuardScanRadius. GuardReturnRadius
        // is not involved: retail never reads it.
        if (isGuardCommand(current->kind())) {
            const auto work = building != nullptr ? std::span<const Construction>{*building}
                                                  : std::span<const Construction>{};
            if (const auto anchor = guardReturnPosition(slot, current->target(), store, catalog, work)) {
                if (const auto* returnGrid = gridFor(*current)) {
                    (void)routeUnit(slot, (*anchor)[0], (*anchor)[2], store, terrain, *returnGrid);
                }
                continue;
            }
        }

        // C-183's ATTACK branch outranks every assist. A mobile guard whose scan covers a
        // hostile acquires through the ordinary path and pursues it, while Guard/Assist stays at
        // the head — no child command, no ids, no log entries. Factory guards never arrive
        // here with live mirror work: that branch continued above, which is the ladder order.
        //
        // The acquisition is a range-overridden copy of each firing weapon, so priorities,
        // restrictions, arcs, incumbency and recon all apply exactly as in combat — the
        // structural equivalent of delegating to `IAiAttacker` with `GuardScanRadius`.
        // Multi-weapon arbitration follows `CAiAttackerImpl` (`C-183`, `0x61a440`):
        // the longest-range weapon whose envelope — minRange to maxRange — covers
        // the gap owns the hold; a target inside every envelope's dead zone or
        // outside every maxRange still holds or chases on the widest envelope.
        // Combat guards share this ladder; engineering rungs separately require build power.
        if (isGuardCommand(current->kind()) && !armies.empty()
            && store.alive(current->target())) {
            const unitdef::UnitDef* guardDef = catalog.def(store.typeAt(slot));
            if (guardDef != nullptr && guardDef->isMobile()
                && guardDef->guardScanRadiusElmos > Fx{}) {
                const std::span<const Transform> sight = store.transforms();
                const auto prey = guardAttackTarget(slot, store, catalog, armies, intel, playableRect, tick, rate);
                if (prey.has_value()) {
                    const bool targetAirborne = store.motion()[prey->index].airborne;
                    MoveState& chase = store.motion()[slot];
                    const Fx gap = groundDistanceElmos(positionOf(sight[slot]),
                                                       positionOf(sight[prey->index]));
                    // `CAiAttackerImpl` arbitration: the longest-range weapon
                    // that can hit — its [minRange, maxRange] envelope covers
                    // the gap — owns the hold. `reach` is that weapon's
                    // maxRange; `widest` is the longest envelope at all, the
                    // fallback when the target sits inside every dead zone or
                    // outside every maxRange.
                    Fx reach{};
                    Fx widest{};
                    for (const unitdef::Weapon& weapon : guardDef->weapons) {
                        if (!weapon.fires() || weapon.manuallyFired()
                            || weapon.targetsProjectiles
                            || !weapon.canTarget(targetAirborne)) {
                            continue;
                        }
                        widest = std::max(widest, weapon.maxRange);
                        if (gap >= weapon.minRange && gap <= weapon.maxRange
                            && weapon.maxRange > reach) {
                            reach = weapon.maxRange;
                        }
                    }
                    if (reach > Fx{} || widest <= Fx{} || gap <= widest) {
                        chase.moving = false;
                        chase.path.clear();
                        chase.pathIndex = 0;
                    } else {
                        const PassabilityGrid* chaseGrid = gridFor(*current);
                        if (chaseGrid == nullptr) {
                            continue;
                        }
                        (void)routeUnit(slot, sight[prey->index].x, sight[prey->index].z,
                                        store, terrain, *chaseGrid);
                    }
                    continue;
                }
            }
        }

        // BUILD ASSIST outranks reclaim and repair. Resolve the same transitive guard chain as
        // applyAssistance; if its founder is actively building, the ordinary Assist chase and
        // work pass below own this beat.
        bool guardBuildAssist = false;
        if (isGuardCommand(current->kind()) && building != nullptr
            && store.alive(current->target())) {
            UnitId founder = current->target();
            std::vector<UnitId> visited;
            while (store.alive(founder) && std::ranges::find(visited, founder) == visited.end()) {
                visited.push_back(founder);
                const QueuedCommand* guarded = orders[founder.index].active();
                if (guarded == nullptr || !isGuardCommand(guarded->kind())
                    || !store.alive(guarded->target())) {
                    break;
                }
                founder = guarded->target();
            }
            guardBuildAssist = std::ranges::any_of(*building, [&](const Construction& item) {
                return !item.finished() && item.builder == founder;
            });
        }

        // RECLAIM copies the guardee's live feature target. It remains an Assist command: this
        // derived work record only bridges command dispatch to the end-of-beat economy pass.
        if (isGuardCommand(current->kind()) && guardCanWork(slot, store, catalog, "RULEUCC_Reclaim")
            && !guardBuildAssist && features != nullptr
            && store.alive(current->target())) {
            const QueuedCommand* guarded = orders[current->target().index].active();
            if (guarded != nullptr && guarded->kind() == CommandKind::Reclaim
                && features->find(guarded->target()) != nullptr) {
                const Feature& wreck = *features->find(guarded->target());
                if (guardWork != nullptr) {
                    guardWork->push_back(GuardWork{.builder = slot,
                                                   .kind = GuardWorkKind::Reclaim,
                                                   .target = guarded->target()});
                }
                MoveState& move = store.motion()[slot];
                const Fx reach = reclaimReach(catalog, store.typeAt(slot), move, wreck);
                if (groundDistanceElmos(positionOf(store.transforms()[slot]), wreck.at) <= reach) {
                    move.moving = false;
                    move.path.clear();
                    move.pathIndex = 0;
                } else if (const PassabilityGrid* reclaimGrid = gridFor(*current)) {
                    (void)routeUnit(slot, wreck.at[0], wreck.at[2], store, terrain, *reclaimGrid);
                }
                continue;
            }
        }

        // REPAIR is the final useful rung: scan around the guardee, but rank by distance to
        // the guard. Strict improvement preserves slot/grid order on an exact tie.
        if (isGuardCommand(current->kind()) && guardCanWork(slot, store, catalog, "RULEUCC_Repair")
            && !guardBuildAssist
            && store.alive(current->target())) {
            const unitdef::UnitDef* guardDef = catalog.def(store.typeAt(slot));
            const Army* owner = armyFor(store.motion()[slot].armyIndex, armies);
            std::optional<UnitIndex> repair;
            Fx repairDistance{};
            if (guardDef != nullptr && owner != nullptr && guardDef->guardScanRadiusElmos > Fx{}) {
                const std::array<Fx, 3> centre = positionOf(
                    store.transforms()[current->target().index]);
                const std::array<Fx, 3> from = positionOf(store.transforms()[slot]);
                for (UnitIndex target = 0; target < store.slotCount(); ++target) {
                    if (!store.slotAlive(target) || target == slot) continue;
                    const Health& health = store.health()[target];
                    const Army* candidateArmy = armyFor(store.motion()[target].armyIndex, armies);
                    const unitdef::UnitDef* targetDef = catalog.def(store.typeAt(target));
                    if (!health.alive() || health.current >= health.maximum
                        || candidateArmy == nullptr || !allied(*owner, *candidateArmy)
                        || targetDef == nullptr || targetDef->buildTime <= Mag{}
                        || groundDistanceElmos(centre, positionOf(store.transforms()[target]))
                               > guardDef->guardScanRadiusElmos) {
                        continue;
                    }
                    const Fx distance = groundDistanceElmos(
                        from, positionOf(store.transforms()[target]));
                    if (!repair || distance < repairDistance) {
                        repair = target;
                        repairDistance = distance;
                    }
                }
            }
            if (repair) {
                if (guardWork != nullptr) {
                    guardWork->push_back(GuardWork{.builder = slot,
                                                   .kind = GuardWorkKind::Repair,
                                                   .target = store.idAt(*repair)});
                }
                MoveState& move = store.motion()[slot];
                const Fx reach = repairReach(catalog, store.typeAt(slot), move,
                                             store.motion()[*repair]);
                const Transform& targetAt = store.transforms()[*repair];
                if (repairDistance <= reach) {
                    move.moving = false;
                    move.path.clear();
                    move.pathIndex = 0;
                } else if (const PassabilityGrid* repairGrid = gridFor(*current)) {
                    (void)routeUnit(slot, targetAt.x, targetAt.z, store, terrain, *repairGrid);
                }
                continue;
            }
        }

        // A service-owned plain move has an active command entry but no published route yet.
        // It must remain the head until its army's resumable A* completes or refuses it.
        if (current->kind() == CommandKind::Move && pathService != nullptr
            && pathService->contains(store.idAt(slot), current->payload().id)) {
            continue;
        }

        const PassabilityGrid* grid = gridFor(*current);
        if (grid == nullptr) {
            continue;  // a missing domain leaves the command queued rather than dropping it
        }

        // C-177 validates an already-published land route only on its accepted start cell's
        // staggered pass. The path service has just completed this tick's pass, so its retained
        // beat is the shared match clock rather than a caller-local counter.
        MoveState& currentMotion = store.motion()[slot];
        if (current->kind() == CommandKind::Move && pathService != nullptr
             && !currentMotion.path.empty() && currentMotion.pathPhaseCellsX > 0
             && pathPhaseDue(currentMotion.pathPhaseStartX, currentMotion.pathPhaseStartZ,
                             currentMotion.pathPhaseCellsX,
                             pathService->lastServiceBeat())) {
            const std::array<Fx, 2>& final = currentMotion.path.back();
            if (!grid->passableAt(grid->cellAtWorld(final[0]), grid->cellAtWorld(final[1]))) {
                // Withdraw only the derived route. The active command remains at the queue head
                // and the admitted request keeps it command-owned until C-176's retry policy
                // either finds a route or retires the third failed search.
                currentMotion.moving = false;
                currentMotion.path.clear();
                currentMotion.pathIndex = 0;
                const Transform& at = store.transforms()[slot];
                pathService->enqueue(PathRequest{.unit = store.idAt(slot),
                                                 .command = current->payload().id,
                                                 .army = currentMotion.armyIndex,
                                                 .fromX = at.x,
                                                 .fromZ = at.z,
                                                 .targetX = final[0],
                                                 .targetZ = final[1],
                                                 .grid = std::make_shared<PassabilityGrid>(*grid)});
                continue;
            }
        }

        // A FIRED missile order names its own launcher — `fireMissiles` marks the spent
        // order by aiming it at the silo itself, a target no click can produce. It retires
        // like any arrival, and it must retire HERE: below, the chase would read the
        // self-target as one more living thing to follow and hold the order forever.
        if (const QueuedCommand* head = orders[slot].active();
            head != nullptr && head->kind() == CommandKind::MissileLaunch
            && head->target() == store.idAt(slot)) {
            (void)orders[slot].finish();
            startPending();
            continue;
        }

        // THE CHASE. An attack naming a LIVING target never completes by arrival — it
        // completes when the target dies — so it is handled here, before the finish logic,
        // and the slot moves on. Three sub-cases, in priority order:
        //
        //   in range      hold: stop moving and let the automatic targeting fire. The order
        //                 stays at the head, so a target that breaks away re-arms the chase.
        //   target moved  re-route to where the target IS, and record that goal in the
        //                 order's own targetX/Z — the chase's memory, which is also what
        //                 keeps a stationary fight from pathfinding every tick.
        //   else          keep walking the route already ordered.
        //
        // A position-target attack never enters this block: it is routed movement, and the
        // finish-or-cycle logic below owns it. An entity-target attack was already validated to
        // have a suitable weapon when it started.
        if (const QueuedCommand* head = orders[slot].active();
            head != nullptr
            && (head->kind() == CommandKind::Attack || head->kind() == CommandKind::Overcharge
                || head->kind() == CommandKind::MissileLaunch
                || isGuardCommand(head->kind()))
            && store.alive(head->target())) {
            // An overcharge pursues exactly as an attack does; the reach is the MANUAL
            // weapon's, because that is the gun this order will fire. A fired overcharge
            // forgets its target (`fireOvercharge`), so a spent order falls out of this
            // block and retires below like any arrival. A MISSILE launch pursues with the
            // silo weapon's reach; its spent marker is the self-target above, which never
            // reaches this block. An ASSIST pursues with the BUILD reach — follow the
            // working engineer, hold beside the factory — and, being a standing order,
            // completes only when the target dies, which is exactly this block's rule.
            const bool missile = head->kind() == CommandKind::MissileLaunch;
            const bool manual = missile || head->kind() == CommandKind::Overcharge;
            // `AutoSurfaceMode` (`C-203`/`C-323`): the attack task's one
            // consumer of the Dive toggle's second state — retail's
            // `CUnitAttackTargetTask` calls `SetNewTargetLayer(LAYER_Water)`
            // when the mode is on and stays submerged when it is off
            // (`0x005fa17c`), unconditionally, not only when a weapon already
            // reaches the target. Our layer target is `diveTargetSubmerged`;
            // the dive stepper walks the hull up and the layer commits at the
            // endpoint, exactly like the manual Dive.
            if (head->kind() == CommandKind::Attack) {
                MoveState& surfacing = store.motion()[slot];
                if (surfacing.submersible && surfacing.autoSurface) {
                    surfacing.diveTargetSubmerged = false;
                }
            }
            const unitdef::UnitDef* def = catalog.def(store.typeAt(slot));
            Fx reach{};
            if (isGuardCommand(head->kind())) {
                reach = catalog.rates(store.typeAt(slot)).buildReachElmos
                      + store.motion()[slot].radiusElmos
                      + store.motion()[head->target().index].radiusElmos;
            } else if (def != nullptr) {
                const bool targetAirborne = store.motion()[head->target().index].airborne;
                for (const unitdef::Weapon& weapon : def->weapons) {
                    if ((missile ? weapon.siloLaunched()
                                 : manual ? weapon.manuallyFired() : weapon.fires())
                        && weapon.canTarget(targetAirborne)
                        && weapon.maxRange > reach) {
                        reach = weapon.maxRange;
                    }
                }
            }
            if (reach > Fx{}) {
                MoveState& chase = store.motion()[slot];
                const Transform& mine = store.transforms()[slot];
                const Transform& theirs = store.transforms()[head->target().index];
                if (head->kind() == CommandKind::Attack && chase.canFly && chase.airWinged) {
                    if (random == nullptr) {
                        throw std::logic_error("winged attacks require the match random stream");
                    }
                    // `C-224`: a bomber leads its ground target by
                    // `PredictAheadForBombDrop` seconds — the release point the
                    // weapon's `BombDropThreshold` gate measures against. Air
                    // targets and non-bomb aircraft chase the target itself.
                    const MoveState& targetMotion = store.motion()[head->target().index];
                    TickCount bombLeadTicks = 0;
                    if (!targetMotion.airborne && def != nullptr
                        && def->airPredictAheadForBombDropSec > 0.0f
                        && std::ranges::any_of(def->weapons, [](const unitdef::Weapon& w) {
                               return w.needToComputeBombDrop;
                           })) {
                        bombLeadTicks = rate.ticks(seconds(def->airPredictAheadForBombDropSec));
                    }
                    updateWingedAttack(chase, mine, theirs,
                        targetMotion.airborne,
                        Fx::fromInt(terrain.field().squaresX * kSquareSize),
                        Fx::fromInt(terrain.field().squaresZ * kSquareSize), tick, *random,
                        targetMotion.stepX, targetMotion.stepZ, bombLeadTicks);
                    if (QueuedCommand* mutableHead = orders[slot].activeMutable()) {
                        mutableHead->setTargetPosition(theirs.x, theirs.z);
                    }
                    continue;
                }
                const Fx gap = groundDistanceElmos({mine.x, mine.y, mine.z},
                                                   {theirs.x, theirs.y, theirs.z});
                if (gap <= reach) {
                    chase.moving = false;
                    chase.path.clear();
                    chase.pathIndex = 0;
                } else {
                    // Re-route when the target has strayed from the last routed goal by
                    // more than half the weapon's reach — far enough that the old route
                    // ends outside the fight, close enough that a crawling target is
                    // still caught. Or when the unit stands idle out of range, which is
                    // how a fresh chase starts and how a failed route retries.
                    const Fx strayed = groundDistanceElmos(
                        {head->targetX(), Fx{}, head->targetZ()}, {theirs.x, Fx{}, theirs.z});
                    if (strayed > Fx::fromRaw(reach.raw() / 2) || !chase.moving) {
                        const bool routed =
                            routeUnit(slot, theirs.x, theirs.z, store, terrain, *grid);
                        if (QueuedCommand* mutableHead = orders[slot].activeMutable()) {
                            // Recorded whether or not the route was found: a target in an
                            // unreachable spot must not be re-pathed every tick — the next
                            // attempt waits until it strays again.
                            mutableHead->setTargetPosition(theirs.x, theirs.z);
                        }
                        (void)routed;
                    }
                }
                continue;  // alive target: the order outlives every arrival
            }
        }

        // An entity attack ends on target death, not when its now-stale route happens to arrive.
        // It is an ordinary completion, so dispatch may start the follower in this same beat.
        // A missile launch's target gets the same rule: the warhead is not fired at a grave.
        if (const QueuedCommand* head = orders[slot].active();
            head != nullptr
            && (head->kind() == CommandKind::Attack
                || head->kind() == CommandKind::MissileLaunch)
            && head->target() != store.idAt(slot)
            && head->target().generation != 0 && !store.alive(head->target())) {
            MoveState& staleRoute = store.motion()[slot];
            staleRoute.moving = false;
            staleRoute.path.clear();
            staleRoute.pathIndex = 0;
            (void)orders[slot].finish();
            startPending();
            continue;
        }

        // THE MISSILE HOLD, the position half. A ground zero cannot walk into range, so the
        // order never completes by arrival: in the envelope it stands for `fireMissiles`,
        // short of it a mobile launcher walks the rest of the way, and a silo out of reach
        // simply waits — the shot it cannot take yet is the order, not a reason to drop it.
        // Retirement is the fire pass's spent marker or a dead unit target, both handled
        // above; nothing below must read the hold as an arrival.
        if (const QueuedCommand* head = orders[slot].active();
            head != nullptr && head->kind() == CommandKind::MissileLaunch
            && head->target().generation == 0) {
            const unitdef::UnitDef* def = catalog.def(store.typeAt(slot));
            Fx reach{};
            if (def != nullptr) {
                for (const unitdef::Weapon& weapon : def->weapons) {
                    if (weapon.siloLaunched() && weapon.maxRange > reach) {
                        reach = weapon.maxRange;
                    }
                }
            }
            MoveState& hold = store.motion()[slot];
            const Transform& at = store.transforms()[slot];
            const Fx gap = groundDistanceElmos({at.x, Fx{}, at.z},
                                               {head->targetX(), Fx{}, head->targetZ()});
            if (gap <= reach) {
                hold.moving = false;
                hold.path.clear();
                hold.pathIndex = 0;
            } else if (!hold.moving && def != nullptr && def->isMobile()) {
                (void)routeUnit(slot, head->targetX(), head->targetZ(), store, terrain, *grid);
            }
            continue;
        }

        // Repair starts only inside MaxBuildDistance, but an active beam is retained out to
        // twice that distance before its finite order ends (`C-182`).
        if (const QueuedCommand* head = orders[slot].active();
            head != nullptr && head->kind() == CommandKind::Repair) {
            if (!store.alive(head->target()) || !store.health()[head->target().index].alive()
                || store.health()[head->target().index].current
                       >= store.health()[head->target().index].maximum
                || !repairStillAllied(slot, head->target(), store, armies)) {
                (void)orders[slot].finish();
                startPending();
                continue;
            }
            const Fx reach = repairReach(catalog, store.typeAt(slot), store.motion()[slot],
                                         store.motion()[head->target().index]);
            const Transform& builderAt = store.transforms()[slot];
            const Fx gap = groundDistanceElmos(positionOf(builderAt),
                                                positionOf(store.transforms()[head->target().index]));
            if (gap <= reach) {
                // Repair has now genuinely reached its initial range. `Repair` has no
                // positional intent, so its per-entry target coordinates can remember this
                // established state without extending persisted command state.
                if (QueuedCommand* mutableHead = orders[slot].activeMutable()) {
                    mutableHead->setTargetPosition(builderAt.x, builderAt.z);
                }
                teardownMovement(store.motion()[slot]);
                continue;
            }
            if (store.motion()[slot].moving) {
                continue;  // still approaching the initial build-distance reach
            }
            const bool established = head->targetX() == builderAt.x
                                     && head->targetZ() == builderAt.z;
            // Only an established beam retains the target through retail's two-range
            // hysteresis. An approach that ended short retries its route.
            if (established && gap <= reach * 2) {
                teardownMovement(store.motion()[slot]);
                continue;
            }
            if (established) {
                // The beam's two-range allowance has been exceeded. It is a terminal loss of
                // the established repair contact, not a new approach to the target.
                (void)orders[slot].finish();
                startPending();
                continue;
            }
            if (routeUnit(slot, store.transforms()[head->target().index].x,
                          store.transforms()[head->target().index].z, store, terrain, *grid)) {
                continue;
            }
            (void)orders[slot].finish();
            startPending();
            continue;
        }
        // A capture ends when its target is gone — transferred by this captor or
        // destroyed by anything else. The transfer leaves a stale handle behind by
        // design, and this is what retires it; the hold block above already refused
        // to pursue the inadmissible.
        if (const QueuedCommand* head = orders[slot].active();
            head != nullptr && head->kind() == CommandKind::Capture) {
            if (!store.alive(head->target()) || !store.health()[head->target().index].alive()) {
                (void)orders[slot].finish();
                startPending();
                continue;
            }
        }

        // An aggressive order with a temporary target belongs to the post-intel pass, even
        // when it is currently holding still. A stale target is cleared there and the original
        // waypoint resumed; treating the hold as arrival here would lose that destination.
        if (const QueuedCommand* head = orders[slot].active();
            head != nullptr
            && (head->kind() == CommandKind::AttackMove || head->kind() == CommandKind::Patrol)
            && head->target().generation != 0) {
            continue;
        }

        // THE UNIT-RECLAIM HOLD: the target is a unit, so it can walk away. In reach, hold
        // still while `reclaimUnits` un-builds it; short of reach, follow — a fresh route
        // every tick it is out of reach, the way an assist follows its target. The order
        // completes when the target is gone, however it went.
        if (const QueuedCommand* head = orders[slot].active();
            head != nullptr && head->kind() == CommandKind::ReclaimUnit) {
            if (store.alive(head->target()) && store.health()[head->target().index].alive()) {
                MoveState& mine = store.motion()[slot];
                const Transform& there = store.transforms()[head->target().index];
                const Fx gap = groundDistanceElmos(positionOf(store.transforms()[slot]),
                                                   positionOf(there));
                if (gap <= repairReach(catalog, store.typeAt(slot), mine,
                                       store.motion()[head->target().index])) {
                    mine.moving = false;
                    mine.path.clear();
                    mine.pathIndex = 0;
                    continue;
                }
                if (mine.moving) {
                    continue;
                }
                if (routeUnit(slot, there.x, there.z, store, terrain, *grid)) {
                    continue;
                }
            }
            // Gone, or unreachable: the order is done. Fall through.
        }

        // THE CAPTURE HOLD, the reclaim hold's twin: the target is a unit, so it can
        // walk away. In reach, hold still while the funded capture task converts it;
        // short of reach, follow with a fresh route every tick. The order completes
        // when the target is gone — transferred by this unit or destroyed by
        // anything else. A target that stops being capturable (loaded, allied)
        // retires the order rather than holding a progress bar that can never fill.
        if (const QueuedCommand* head = orders[slot].active();
            head != nullptr && head->kind() == CommandKind::Capture) {
            if (store.alive(head->target()) && store.health()[head->target().index].alive()
                && capturableTarget(slot, head->target(), store, catalog, armies)) {
                MoveState& mine = store.motion()[slot];
                const Transform& there = store.transforms()[head->target().index];
                const Fx gap = groundDistanceElmos(positionOf(store.transforms()[slot]),
                                                   positionOf(there));
                // `C-250`: the approach ends at a 5-ogrid footprint-edge gap —
                // the captor holds here while the funded task works, which
                // admits out to 10 ogrids.
                if (captureEdgeDistance(catalog, store.typeAt(slot),
                                        store.typeAt(head->target().index), gap)
                    <= kCaptureApproachEdgeElmos) {
                    mine.moving = false;
                    mine.path.clear();
                    mine.pathIndex = 0;
                    continue;
                }
                if (mine.moving) {
                    continue;
                }
                if (routeUnit(slot, there.x, there.z, store, terrain, *grid)) {
                    continue;
                }
            }
            // Gone, inadmissible, or unreachable: the order is done. Fall through.
        }

        // THE HARVEST HOLD, the reclaim twin of the chase above: a reclaim naming a wreck
        // that still holds value never completes by arrival — it completes when the wreck
        // is GONE, drained by this unit or any other. In reach it holds still and lets
        // `harvestReclaim` do the work; short of reach and idle, it walks the rest of the
        // way. A wreck it cannot route to is dropped by falling through to the finish
        // logic, which is what an unreachable order deserves.
        if (const QueuedCommand* head = orders[slot].active();
            head != nullptr && head->kind() == CommandKind::Reclaim && features != nullptr) {
            if (const Feature* wreck = features->find(head->target())) {
                MoveState& mine = store.motion()[slot];
                const Fx gap = groundDistanceElmos(positionOf(store.transforms()[slot]),
                                                   wreck->at);
                if (gap <= reclaimReach(catalog, store.typeAt(slot), mine, *wreck)) {
                    mine.moving = false;
                    mine.path.clear();
                    mine.pathIndex = 0;
                    continue;  // the wreck outlives every arrival; the harvest empties it
                }
                if (mine.moving) {
                    continue;  // still walking there
                }
                // Arrived short — the route ended outside reach. One more attempt from
                // here; a second failure falls through and retires the order rather than
                // pathfinding every tick at a wreck across a wall.
                if (routeUnit(slot, wreck->at[0], wreck->at[2], store, terrain, *grid)) {
                    continue;
                }
            }
            // The wreck is gone (or unreachable): the order is done. Fall through.
        }

        // THE TRANSPORT HOLD: boarding, unloading and the ferry loop never
        // complete by arrival — a carrier that has stopped is usually just
        // waiting to descend, and a waiting cargo is parked, not done. Their
        // lifecycle belongs to `updateTransports`, which alone can see the
        // attach and the touchdown that actually end them.
        if (const QueuedCommand* head = orders[slot].active();
            head != nullptr && (head->kind() == CommandKind::LoadTransport
                                || head->kind() == CommandKind::UnloadTransport
                                || head->kind() == CommandKind::Ferry)) {
            continue;
        }

        // THE FERRY-WAIT HOLD, `C-199`'s `CUnitWaitForFerryTask` by another
        // name. A move whose destination sits inside a live same-army ferry's
        // pickup ring does not complete on arrival — the unit is WAITING FOR
        // THE FERRY, and the held-open order is the assignment the carrier's
        // Loading phase reads (`waitingAtBeacon` in Transport.cpp). Without
        // the hold the move would retire on arrival and a unit sent to the
        // beacon would become indistinguishable from one merely parked there.
        // The ring lives on the ferry order's `transportAnchor`, so a dead or
        // cancelled ferry releases its waiters to ordinary completion.
        if (const QueuedCommand* head = orders[slot].active();
            head != nullptr && head->kind() == CommandKind::Move) {
            const Transform& at = store.transforms()[slot];
            bool waiting = false;
            for (UnitIndex ferry = 0; ferry < orders.size() && !waiting; ++ferry) {
                if (ferry == slot || !store.slotAlive(ferry)
                    || store.motion()[ferry].armyIndex != motion[slot].armyIndex) {
                    continue;
                }
                const QueuedCommand* route = orders[ferry].active();
                if (route == nullptr || !route->transportAnchor()) {
                    continue;  // no beacon: not a live ferry route
                }
                const std::array<Fx, 2>& beacon = *route->transportAnchor();
                const Fx dx = head->targetX() - beacon[0];
                const Fx dz = head->targetZ() - beacon[1];
                if (dx * dx + dz * dz
                    > rm::sim::kFerryPickupRadius * rm::sim::kFerryPickupRadius) {
                    continue;  // the destination is not this ferry's pickup
                }
                const Fx px = at.x - beacon[0];
                const Fx pz = at.z - beacon[1];
                waiting = px * px + pz * pz
                          <= rm::sim::kFerryPickupRadius * rm::sim::kFerryPickupRadius;
            }
            if (waiting) {
                continue;
            }
        }

        if (slot < motion.size() && motion[slot].moving) {
            continue;  // still carrying out the order at the head
        }

        // The first patrol's starting point is execution state, not a fabricated second command.
        // Its destination leg keeps the same entry at the head and exposes the hidden origin;
        // the return leg restores immutable intent and then rotates to the next real waypoint.
        if (const QueuedCommand* head = orders[slot].active();
            head != nullptr && head->kind() == CommandKind::Patrol) {
            QueuedCommand* entry = orders[slot].currentEntryMutable();
            if (entry != nullptr && entry->patrolOrigin()) {
                if (entry->returningToPatrolOrigin()) {
                    entry->restorePatrolDestination();
                    if (orders[slot].size() > 1) {
                        (void)orders[slot].cycle();
                    } else {
                        orders[slot].deactivateCurrent();
                    }
                } else {
                    entry->routeToPatrolOrigin();
                    orders[slot].deactivateCurrent();
                }
                continue;
            }
            if (orders[slot].size() > 1) {
                (void)orders[slot].cycle();
                continue;
            }
        }

        // Retail keys Attack cycling on target KIND. A position attack is a repeatable waypoint
        // while another command follows; entity attacks and lone waypoints retire.
        if (const QueuedCommand* head = orders[slot].active();
            head != nullptr && orders[slot].size() > 1
            && head->kind() == CommandKind::Attack && head->target().generation == 0) {
            (void)orders[slot].cycle();
            continue;
        }

        // An ordinary completion immediately re-enters command dispatch in this beat.
        (void)orders[slot].finish();
        currentMotion.pathPhaseCellsX = 0;
        startPending();
    }

    // Status -4 resumes after every unit has had its ordinary command-dispatch turn. A second
    // -4 waits for the next beat rather than spinning forever at the end-of-beat boundary.
    if (scriptTasks != nullptr) {
        for (const UnitIndex slot : delayedScripts) {
            if (!store.slotAlive(slot)) {
                continue;
            }
            QueuedCommand* command = orders[slot].activeMutable();
            if (command == nullptr || command->kind() != CommandKind::Script
                || command->scriptState().suspended) {
                continue;
            }
            (void)dispatchScriptTask(orders[slot], *scriptTasks, true);
        }
    }
    return started;
}
void updateAggressiveOrders(UnitStore& store, const UnitCatalog& catalog,
                            std::span<const Army> armies, const Terrain& terrain,
                            std::span<const PassabilityGrid* const> gridForType, TickRate rate,
                            const Intel* intel, const PlayableRect* playableRect,
                            TickIndex tick,
                            std::span<const PassabilityGrid* const> gridForTypeSubmerged) {
    const auto armyFor = [armies](int index) -> const Army* {
        for (const Army& army : armies) {
            if (army.index == index) {
                return &army;
            }
        }
        return nullptr;
    };

    for (UnitIndex slot = 0; slot < store.orders().size(); ++slot) {
        if (!store.slotAlive(slot) || store.motion()[slot].attached) {
            continue;  // cargo rides; it does not pick fights off the rack
        }
        QueuedCommand* order = store.orders()[slot].activeMutable();
        if (order == nullptr
            || (order->kind() != CommandKind::AttackMove
                && order->kind() != CommandKind::Patrol)) {
            continue;
        }

        const auto type = static_cast<std::size_t>(store.typeAt(slot));
        const MoveState& layer = store.motion()[slot];
        const PassabilityGrid* grid =
            layerGridFor(gridForType, gridForTypeSubmerged,
                         layer.submersible && layer.submerged, type);
        const unitdef::UnitDef* def = catalog.def(store.typeAt(slot));
        if (grid == nullptr || def == nullptr) {
            continue;
        }

        bool armed = false;
        for (const unitdef::Weapon& weapon : def->weapons) {
            if (weapon.fires()) {
                armed = true;
            }
        }
        if (!armed) {
            continue;  // an unarmed patrol is still a patrol; it simply never interrupts
        }

        MoveState& motion = store.motion()[slot];
        const auto resumeWaypoint = [&] {
            motion.moving = false;
            motion.path.clear();
            motion.pathIndex = 0;
            return startCommand(order->asCommand(), store, catalog, terrain, *grid, rate, nullptr,
                                nullptr, nullptr, armies);
        };
        const int owner = store.motion()[slot].armyIndex;
        const Army* mine = armyFor(owner);
        const auto targetVisibleAndHostile = [&](UnitId target) {
            if (!store.alive(target) || !store.health()[target.index].alive() || mine == nullptr) {
                return false;
            }
            if (playableRect != nullptr
                && !playableRect->contains(positionOf(store.transforms()[target.index]))) {
                return false;
            }
            const Army* theirs = armyFor(store.motion()[target.index].armyIndex);
            return theirs != nullptr && hostile(*mine, *theirs)
                   && (intel == nullptr
                        || contactKindForUnit(mine->alliance, target.index, store, catalog,
                                              armies, *intel)
                               == ContactKind::Seen);
        };

        if (order->target().generation != 0 && !targetVisibleAndHostile(order->target())) {
            order->setTarget(UnitId{});
            (void)resumeWaypoint();
        }

        const std::array<Fx, 3> from = positionOf(store.transforms()[slot]);
        if (order->target().generation == 0) {
            std::optional<UnitId> nearest;
            Fx nearestDistance{};
            for (const unitdef::Weapon& weapon : def->weapons) {
                if (!weapon.fires()) {
                    continue;
                }
                const std::optional<UnitId> candidate =
                    nearestTarget(from, owner, weapon, store, armies, intel, &catalog,
                                   store.transforms()[slot].heading, std::nullopt, playableRect,
                                   {}, std::nullopt, tick, rate, store.targetFocuses()[slot]);
                if (!candidate) {
                    continue;
                }
                const Fx distance =
                    groundDistanceElmos(from, positionOf(store.transforms()[candidate->index]));
                if (!nearest || distance < nearestDistance
                    || (distance == nearestDistance && candidate->index < nearest->index)) {
                    nearest = candidate;
                    nearestDistance = distance;
                }
            }
            if (nearest) {
                order->setTarget(*nearest);
            }
        }

        if (order->target().generation == 0) {
            continue;
        }

        const Transform& mineAt = store.transforms()[slot];
        const Transform& targetAt = store.transforms()[order->target().index];
        const bool targetAirborne = store.motion()[order->target().index].airborne;
        const Fx gap = groundDistanceElmos(positionOf(mineAt), positionOf(targetAt));
        const bool canEngage = std::any_of(
            def->weapons.begin(), def->weapons.end(),
            [gap, targetAirborne](const unitdef::Weapon& weapon) {
                return weapon.fires() && weapon.canTarget(targetAirborne)
                    && gap >= weapon.minRange && gap <= weapon.maxRange;
            });
        if (canEngage) {
            motion.moving = false;
            motion.path.clear();
            motion.pathIndex = 0;
            continue;
        }

        const unitdef::Weapon* widest = nullptr;
        for (const unitdef::Weapon& weapon : def->weapons) {
            if (weapon.fires() && weapon.canTarget(targetAirborne)
                && (widest == nullptr || weapon.maxRange > widest->maxRange)) {
                widest = &weapon;
            }
        }
        if (widest == nullptr || gap < widest->minRange) {
            order->setTarget(UnitId{});
            (void)resumeWaypoint();
            continue;
        }

        if (!motion.moving) {
            if (!routeUnit(slot, targetAt.x, targetAt.z, store, terrain, *grid)) {
                order->setTarget(UnitId{});
                (void)resumeWaypoint();
            }
        }
    }
}

bool startCommand(const Command& command, UnitStore& store, const UnitCatalog& catalog,
                  const Terrain& terrain, const PassabilityGrid& grid, TickRate,
                  std::vector<Construction>* building, EventQueue* events,
                  const FeatureStore* features, std::span<const Army> armies,
                  const PassabilityGrid* approachGrid) {
    // to be live, and a `Build` started for a unit that died this tick would charge a dead
    // army. The handle check belongs to `applyCommand`, where a stale handle is the ordinary
    // case; here it would be a second answer to a question already asked.
    if (!store.slotAlive(command.unit.index)) {
        return false;
    }
    MoveState& motion = store.motion()[command.unit.index];

    switch (command.kind) {
    case CommandKind::Stop:
        // In place, and the route cleared: without that the unit resumes its old orders the
        // moment something else sets `moving`.
        teardownMovement(motion);
        return true;

    case CommandKind::Overcharge: {
        // The pursuit below is the attack's; what is checked here is what makes this order
        // MEAN anything — a living target and a manual weapon to fire at it. The energy is
        // deliberately NOT checked: the store may fill while the unit walks over, so a
        // short bar holds the shot rather than refusing the click (`fireOvercharge` gates).
        if (!store.alive(command.target)) {
            return false;
        }
        const unitdef::UnitDef* def = catalog.def(store.typeAt(command.unit.index));
        Fx reach{};
        if (def != nullptr) {
            const bool targetAirborne = store.motion()[command.target.index].airborne;
            for (const unitdef::Weapon& weapon : def->weapons) {
                if (weapon.manuallyFired() && weapon.canTarget(targetAirborne)
                    && weapon.maxRange > reach) {
                    reach = weapon.maxRange;
                }
            }
        }
        if (reach <= Fx{}) {
            return false;  // no manual weapon, no overcharge — a tank cannot be asked to
        }
        // Already in reach: hold here and let `fireOvercharge` do the rest. Checked
        // before routing because `findPath` answers EMPTY inside one coarse cell, and an
        // in-range shot refused for want of a route it does not need would read as a
        // weapon that does not work — the same trap the reclaim start steps around.
        const Transform& at = store.transforms()[command.unit.index];
        const Transform& theirs = store.transforms()[command.target.index];
        if (groundDistanceElmos(positionOf(at), positionOf(theirs)) <= reach) {
            motion.moving = false;
            motion.path.clear();
            motion.pathIndex = 0;
            return true;
        }
        // Out of reach: pursue the target directly. This used to fall through to `Attack`, but
        // adding another targeted command between the cases silently redirected overcharge into
        // that command's validation; spelling the shared routing out keeps the kinds independent.
        return routeUnit(command.unit.index, theirs.x, theirs.z, store, terrain, grid);
    }
    case CommandKind::MissileLaunch: {
        // The silo's order: a unit target pursues like an attack, a position aims where it
        // was clicked. What makes the order MEAN anything — a counted manual weapon — was
        // checked at the door (`validMissileLaunch`); the reach is that weapon's envelope.
        const unitdef::UnitDef* def = catalog.def(store.typeAt(command.unit.index));
        Fx reach{};
        if (def != nullptr) {
            for (const unitdef::Weapon& weapon : def->weapons) {
                if (weapon.siloLaunched() && weapon.maxRange > reach) {
                    reach = weapon.maxRange;
                }
            }
        }
        if (reach <= Fx{}) {
            return false;  // queued orders skip the door; a promise without a tube is refused
        }
        Fx aimX = command.targetX;
        Fx aimZ = command.targetZ;
        if (command.target.generation != 0) {
            if (!store.alive(command.target)) {
                return false;
            }
            const Transform& theirs = store.transforms()[command.target.index];
            aimX = theirs.x;
            aimZ = theirs.z;
        }
        const Transform& at = store.transforms()[command.unit.index];
        if (groundDistanceElmos(positionOf(at), {aimX, Fx{}, aimZ}) <= reach) {
            // In the envelope already: hold here and let `fireMissiles` do the rest — the
            // same coarse-cell trap as the overcharge start.
            motion.moving = false;
            motion.path.clear();
            motion.pathIndex = 0;
            return true;
        }
        // Out of reach: a mobile launcher walks to it; a silo HOLDS — a unit target can
        // still walk into the envelope, and a position simply waits to be cancelled.
        if (def != nullptr && def->isMobile()) {
            return routeUnit(command.unit.index, aimX, aimZ, store, terrain, grid);
        }
        motion.moving = false;
        motion.path.clear();
        motion.pathIndex = 0;
        return true;
    }
    case CommandKind::Guard:
    case CommandKind::Assist: {
        // Legacy Assist requires a builder. Explicit Guard also permits combat units;
        // dispatch and each subsequent beat validate the allied, living, non-self target.
        if (!(command.kind == CommandKind::Guard ? validGuard(command, store, catalog)
                                                : validAssist(command, store, catalog))) {
            return false;
        }
        // In build reach already: stand and help. The same coarse-cell trap as the
        // overcharge start — an in-reach order must not be refused for want of a route.
        const Fx reach = catalog.rates(store.typeAt(command.unit.index)).buildReachElmos
                       + store.motion()[command.unit.index].radiusElmos
                       + store.motion()[command.target.index].radiusElmos;
        const Transform& at = store.transforms()[command.unit.index];
        const Transform& theirs = store.transforms()[command.target.index];
        if (groundDistanceElmos(positionOf(at), positionOf(theirs)) <= reach) {
            motion.moving = false;
            motion.path.clear();
            motion.pathIndex = 0;
            return true;
        }
        return routeUnit(command.unit.index, theirs.x, theirs.z, store, terrain, grid);
    }
    case CommandKind::Move:
    case CommandKind::AttackMove:
    case CommandKind::Patrol:
        return routeUnit(command.unit.index, command.targetX, command.targetZ, store, terrain,
                         grid);

    case CommandKind::Attack: {
        if (command.target.generation == 0) {
            return routeUnit(command.unit.index, command.targetX, command.targetZ, store, terrain,
                             grid);
        }
        if (!store.alive(command.target)) {
            return false;
        }
        const unitdef::UnitDef* def = catalog.def(store.typeAt(command.unit.index));
        const bool targetAirborne = store.motion()[command.target.index].airborne;
        if (def == nullptr
            || std::none_of(def->weapons.begin(), def->weapons.end(),
                            [targetAirborne](const unitdef::Weapon& weapon) {
                                return weapon.fires() && weapon.canTarget(targetAirborne);
                            })) {
            return false;
        }
        if (motion.canFly && motion.airWinged) {
            // A newly accepted entity attack starts a fresh tactical run. Retaining
            // the prior run's counter can immediately break away from the new target.
            motion.airCombatState = MoveState::AirCombatState::None;
            motion.airCombatDeadline = 0;
            motion.airSustainedTicks = 0;
        }
        // ROUTED, not aimed straight at the destination — which is the difference between a
        // unit walking round a lake and one walking into it. A route that cannot be found is
        // a refused order rather than a straight-line fallback: driving into the water is a
        // worse answer than not moving.
        return routeUnit(command.unit.index, command.targetX, command.targetZ, store, terrain,
                         grid);
    }

    case CommandKind::Build: {
        if (building == nullptr) {
            return false;  // a scene with no construction list cannot build
        }
        const unitdef::UnitDef* def = catalog.def(command.buildType);
        if (def == nullptr) {
            return false;  // a type the catalog does not know
        }

        // A builder builds. Anything else issuing a build order is a caller bug, and refusing
        // it deterministically is better than letting a tank found a factory.
        const unitdef::UnitDef* builder = catalog.def(store.typeAt(command.unit.index));
        if (builder == nullptr || !builder->isBuilder()) {
            return false;
        }

        // THE BUILD TREE, enforced where the order lands: the builder's own
        // `BuildableCategory` must name the definition, or a T1 factory turns out T2 tanks
        // the moment anything asks. Nothing legal ever hit this — the UI only offers what
        // `buildableBy` lists and the scripted opponent builds from a vetted opening — but
        // the FAF opponent asks for whatever its data names, and the rule belongs to the
        // sim, not to every caller's manners.
        if (!canBuild(store, catalog, command.unit.index, *def)) {
            return false;
        }

        // AN UPGRADE, when the target is what the builder's blueprint says it becomes —
        // `General.UpgradesTo`, the tech path. Upgrades and factory products both happen on
        // the builder's pad, whatever construction-plan location the command carried.
        const bool upgrade = !builder->upgradesTo.empty() && builder->upgradesTo == def->name;
        const bool factoryProduction = builder->hasCategory("FACTORY") && def->isMobile();
        const Transform& builderAt = store.transforms()[command.unit.index];
        const Fx siteX = (upgrade || factoryProduction) ? builderAt.x : command.targetX;
        const Fx siteZ = (upgrade || factoryProduction) ? builderAt.z : command.targetZ;

        if (upgrade || factoryProduction) {
            // One pad, one job — an unfinished row on this pad still belongs to whatever
            // order owns it, and a second product or upgrade cannot materialise beside it.
            const bool padBusy = std::ranges::any_of(*building, [&](const Construction& work) {
                return !work.finished() && work.builder == command.unit
                    && work.position[0] == siteX && work.position[2] == siteZ;
            });
            if (padBusy) {
                return false;
            }
        } else {
            // An unfinished row of the same blueprint ON the exact site means occupied —
            // whoever founded it — even when the product's skirt is too small for the
            // generic placeable check to notice. The builder's own row is a RESUME, not a
            // second job: an interrupt left the scaffold standing and this order reattaches
            // to it, through the same approach gate a fresh build uses. Anybody else's is
            // refused outright, and the caller's join path decides lend vs takeover.
            const Construction* held =
                constructionAtSite(*building, command.buildType, siteX, siteZ);
            if (held != nullptr && !held->finished()) {
                if (held->builder != command.unit) {
                    return false;
                }
                if (groundDistanceElmos(positionOf(builderAt), {siteX, Fx{}, siteZ})
                    > constructionReach(catalog, store.typeAt(command.unit.index),
                                        command.buildType)) {
                    const bool routedToSite =
                        motion.airborne
                            ? motion.moving && motion.destinationX == siteX
                                  && motion.destinationZ == siteZ
                            : motion.moving && !motion.path.empty()
                                  && motion.path.back()[0] == siteX
                                  && motion.path.back()[1] == siteZ;
                    // The same rule as the fresh-build approach: the builder's grid, never
                    // the product's placement grid.
                    return routedToSite
                        || routeUnit(command.unit.index, siteX, siteZ, store, terrain,
                                     approachGrid != nullptr ? *approachGrid : grid);
                }
                return true;  // in reach — dispatch takes it from here
            }
        }

        if (!upgrade && !terrain.resourceSitePlaceable(def->buildRestriction, siteX, siteZ)) {
            return false;
        }

        // Validate the TARGET's terrain domain before creating work. Callers pass the grid
        // selected for the product being built; aircraft need no ground footprint and upgrades
        // keep the factory's existing foundation.
        if (!upgrade && def->motion != unitdef::MotionType::Air) {
            const Fx radius = fxFromFloat(def->collisionRadiusElmos);
            if (!sitePlaceable(grid, siteX, siteZ, radius)) {
                return false;
            }
            // Mobile products are assembled on a factory pad rather than placed on the build
            // map. Structures must not overlap a living footprint or earlier construction.
            if (!def->isMobile()
                && !buildSitePlaceable(grid, siteX, siteZ, radius, store, catalog, *building)) {
                return false;
            }
        }

        // A mobile builder does not create work remotely. Retail subtracts the engineer's
        // smaller footprint and the product's larger skirt from centre distance, then compares
        // MaxBuildDistance. Factories and upgrades already use their own pad and bypass this
        // mobile-task approach state.
        if (!upgrade && !factoryProduction
            && groundDistanceElmos(positionOf(builderAt), {siteX, Fx{}, siteZ})
                   > constructionReach(catalog, store.typeAt(command.unit.index),
                                       command.buildType)) {
            const bool routedToSite = motion.airborne
                ? motion.moving && motion.destinationX == siteX && motion.destinationZ == siteZ
                : motion.moving && !motion.path.empty() && motion.path.back()[0] == siteX
                      && motion.path.back()[1] == siteZ;
            // A shipyard needs water under its footprint, but its engineer can approach
            // across land. Never route the builder on the product's placement grid.
            return routedToSite
                || routeUnit(command.unit.index, siteX, siteZ, store, terrain,
                             approachGrid != nullptr ? *approachGrid : grid);
        }

        // THE UNIT CAP, retail's creation gate (`0x0074fda0`): the entity that
        // is about to exist would take `costTotal + reserved + CapCost` over
        // `UnitCap`, so the create is refused. A factory's product never gets
        // here — `factoryProductionCapped` holds its order upstream — leaving
        // this to refuse mobile scaffolds and upgrades outright, which is what
        // retail's gate does to a creation with no retrying task behind it.
        if (unitCapBlocks(armies, store.motion()[command.unit.index].armyIndex, catalog,
                          *building, *def)) {
            return false;
        }


        // The cost and the time come from the DEFINITION, and the rate from the clock — the
        // same derivation `UnitCatalog::Rates` does for income, at the one place a construction
        // is created.
        // Construction occupies the builder until completion, so an earlier route must not
        // keep moving the founder while it builds remotely.
        teardownMovement(motion);

        building->push_back(Construction{
            .armyIndex = store.motion()[command.unit.index].armyIndex,
            // Straight through. This used to be `{fxToFloat(targetX), 0.0f,
            // fxToFloat(targetZ)}` — an `Fx` the caller already had, rounded into a float,
            // inside the sim (§7 P10.0). The `y` is zero because a build order names a place
            // on the map and the ground decides the height.
            .position = {siteX, Fx{}, siteZ},
            .cost = {.mass = def->buildCostMass, .energy = def->buildCostEnergy},
            .buildTimeRemaining = def->buildTime,
            .totalBuildTime = def->buildTime,
            .buildPerTick = effectiveBuildPerTick(store, catalog, command.unit.index),
            .blueprintIndex = command.buildType,
            .upgradeOf = upgrade ? command.unit : UnitId{},
            .builder = command.unit,
            // Born paused when its founder is: the store flag is the authority, and a work
            // started under a hold must not bill its first beat as if unpaused.
            .paused = store.productionPaused(command.unit),
        });
        emit(events, Event{
                         .kind = EventKind::ConstructionStarted,
                         .instigator = command.unit,
                         .army = store.motion()[command.unit.index].armyIndex,
                         .amount = def->buildCostMass,
                         .at = {siteX, Fx{}, siteZ},
                     });
        return true;
    }

    case CommandKind::Reclaim: {
        // A scene with nothing on the ground refuses the kind, the same shape as `Build`
        // with no construction list.
        if (features == nullptr) {
            return false;
        }
        const Feature* wreck = features->find(command.target);
        if (wreck == nullptr) {
            return false;  // already emptied, or a stale handle from a replay
        }
        if (wreck->massRemaining <= Mag{} && wreck->energyRemaining <= Mag{}) {
            return false;  // a bare scorch record — an ACU's, a wall's — holds nothing
        }

        // ONLY A BUILDER RECLAIMS. The harvest multiplies by `buildPerTick`, so a tank's
        // zero would make this an order that never completes — refusing it here is the
        // same rule as a tank refusing to found a factory, for the same reason.
        const unitdef::UnitDef* reclaimer = catalog.def(store.typeAt(command.unit.index));
        if (reclaimer == nullptr || !reclaimer->isBuilder()) {
            return false;
        }

        // In reach already: stand and harvest, no route needed. Otherwise walk there —
        // and an unroutable wreck refuses the order, exactly as an unroutable move does.
        const Transform& at = store.transforms()[command.unit.index];
        MoveState& mine = store.motion()[command.unit.index];
        const Fx gap = groundDistanceElmos(positionOf(at), wreck->at);
        if (gap <= reclaimReach(catalog, store.typeAt(command.unit.index), mine, *wreck)) {
            mine.moving = false;
            mine.path.clear();
            mine.pathIndex = 0;
            return true;
        }
        return routeUnit(command.unit.index, wreck->at[0], wreck->at[2], store, terrain, grid);
    }
    case CommandKind::ReclaimUnit: {
        // The unit twin of Reclaim: the target is a living unit of another side, the
        // reclaimer a builder, and the reach the build reach. Own or allied units are not
        // reclaimable here: retail permits it, but its rules for that case are unread, and an
        // order that quietly ate one's own tanks would be worse than a refused one.
        if (!store.alive(command.target) || command.target.index == command.unit.index
            || !store.health()[command.target.index].alive()) {
            return false;
        }
        const unitdef::UnitDef* reclaimer = catalog.def(store.typeAt(command.unit.index));
        const unitdef::UnitDef* target = catalog.def(store.typeAt(command.target.index));
        if (reclaimer == nullptr || target == nullptr || !reclaimer->isBuilder()
            || std::max(target->buildCostMass, target->buildCostEnergy) <= Mag{}) {
            return false;
        }
        const Transform& at = store.transforms()[command.unit.index];
        const Transform& there = store.transforms()[command.target.index];
        const Fx reach = repairReach(catalog, store.typeAt(command.unit.index), motion,
                                     store.motion()[command.target.index]);
        if (groundDistanceElmos(positionOf(at), positionOf(there)) <= reach) {
            teardownMovement(motion);
            return true;
        }
        return routeUnit(command.unit.index, there.x, there.z, store, terrain, grid);
    }
    case CommandKind::Capture: {
        // The capture twin of ReclaimUnit: the captor walks into build reach and the
        // funded task does the rest. Alliance was established at issue time, like
        // reclaim; what dispatch re-checks is the world that changed since — the
        // target's life, the captor's capability, and the capture domain.
        if (!store.alive(command.target) || command.target.index == command.unit.index
            || !store.health()[command.target.index].alive()) {
            return false;
        }
        const unitdef::UnitDef* captor = catalog.def(store.typeAt(command.unit.index));
        const unitdef::UnitDef* victim = catalog.def(store.typeAt(command.target.index));
        const MoveState& targetMotion = store.motion()[command.target.index];
        if (captor == nullptr || victim == nullptr || !captor->isBuilder()
            || !captor->hasCategory("CAPTURE") || victim->hasCategory("COMMAND")
            || targetMotion.airborne
            || (targetMotion.submersible && targetMotion.submerged)
            || targetMotion.attached || !store.childrenOf(command.target).empty()) {
            return false;
        }
        const Transform& at = store.transforms()[command.unit.index];
        const Transform& there = store.transforms()[command.target.index];
        const Fx gap = groundDistanceElmos(positionOf(at), positionOf(there));
        // `C-250`: the approach ends at a 5-ogrid footprint-edge gap; the
        // funded task works out to 10.
        if (captureEdgeDistance(catalog, store.typeAt(command.unit.index),
                                store.typeAt(command.target.index), gap)
            <= kCaptureApproachEdgeElmos) {
            teardownMovement(motion);
            return true;
        }
        return routeUnit(command.unit.index, there.x, there.z, store, terrain, grid);
    }
    case CommandKind::Repair: {
        // Validation at issue time establishes the alliance and damaged target. A queued repair
        // is still refused when it reaches the head after the target has already been restored.
        if (!store.alive(command.target) || !store.health()[command.target.index].alive()
            || store.health()[command.target.index].current
                   >= store.health()[command.target.index].maximum) {
            return false;
        }
        const unitdef::UnitDef* builder = catalog.def(store.typeAt(command.unit.index));
        const unitdef::UnitDef* target = catalog.def(store.typeAt(command.target.index));
        if (builder == nullptr || target == nullptr || !builder->isBuilder()
            || target->buildTime <= Mag{}) {
            return false;
        }
        const Transform& at = store.transforms()[command.unit.index];
        const Transform& theirs = store.transforms()[command.target.index];
        const Fx reach = repairReach(catalog, store.typeAt(command.unit.index), motion,
                                     store.motion()[command.target.index]);
        if (groundDistanceElmos(positionOf(at), positionOf(theirs)) <= reach) {
            teardownMovement(motion);
            return true;
        }
        return routeUnit(command.unit.index, theirs.x, theirs.z, store, terrain, grid);
    }
    case CommandKind::LoadTransport:
    case CommandKind::UnloadTransport:
    case CommandKind::Ferry:
        // Owned by `updateTransports`: the chase, the descent and the attach
        // all live there, because each retires on attachment or touchdown
        // rather than on arrival. Starting only marks the order active.
        return true;

    case CommandKind::Dive:
    case CommandKind::ToggleFactoryRepeat:
    case CommandKind::ToggleProduction:
    case CommandKind::CycleBuildPriority:
    case CommandKind::SetBuildPriority:
    case CommandKind::CycleRetreatThreshold:
    case CommandKind::CycleTargetFocus:
    case CommandKind::CancelFactoryBuild:
    case CommandKind::SiloBuildTactical:
    case CommandKind::SiloBuildNuke:
    case CommandKind::ToggleSiloAuto:
    case CommandKind::SelfDestruct:
    case CommandKind::ToggleScriptBit:
    case CommandKind::OfferDraw:
        return false;  // applied immediately by semantic issue intake; it never enters a queue
    case CommandKind::Script:
        return false;  // dispatched through ScriptTaskHost, never as a movement/build command
    }

    return false;
}

} // namespace rm::sim
