#include "core/sim/Command.hpp"

#include "core/sim/Combat.hpp"
#include "core/sim/Movement.hpp"
#include "core/sim/Reclaim.hpp"
#include "core/sim/ScriptTask.hpp"
#include "core/sim/UnitStore.hpp"

#include "core/unit/BuildTree.hpp"
#include "core/unit/Role.hpp"

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>

namespace rm::sim {
namespace {

/// The player who issued a command, or null.
[[nodiscard]] const Player* playerFor(PlayerIndex index,
                                      std::span<const Player> players) noexcept {
    for (const Player& player : players) {
        if (player.index == index) {
            return &player;
        }
    }
    return nullptr;
}

[[nodiscard]] const Army* armyFor(int index, std::span<const Army> armies) noexcept {
    for (const Army& army : armies) {
        if (army.index == index) return &army;
    }
    return nullptr;
}

/// Whether this player may order this unit.
///
/// TWO conditions, and both matter. The player must command the unit's army — that is the
/// authorisation §7 P2.5 makes possible. And the army must not be defeated: a side that is out
/// of the match does not keep taking orders, which is the same rule `hostile` applies to
/// targeting.
[[nodiscard]] bool authorised(const Player& player, const UnitStore& store, UnitId unit,
                             std::span<const Army> armies) noexcept {
    const int owner = store.motion()[unit.index].armyIndex;
    if (!commands(player, owner)) {
        return false;
    }
    const auto army = static_cast<std::size_t>(owner);
    return army >= armies.size() || !armies[army].defeated;
}

/// Applies one order to the world, without touching the queue. Defined at the foot of the file,
/// declared here because both `applyCommand` and `advanceOrders` need it.
///
/// Split out of `applyCommand` for §7 P4.1: the queue has to start an order it has been
/// holding, and that is the same act as applying a fresh one minus the authorisation and the
/// queueing. Sharing it is what stops a queued route behaving differently from a clicked one.
[[nodiscard]] bool startCommand(const Command& command, UnitStore& store,
                                const UnitCatalog& catalog, const Terrain& terrain,
                                const PassabilityGrid& grid, TickRate rate,
                                std::vector<Construction>* building, EventQueue* events,
                                 const FeatureStore* features);

/// Routes one unit according to its content movement layer. Aircraft fly directly over the
/// map; supported ground classes retain A* and refuse an unreachable destination.
[[nodiscard]] bool routeUnit(UnitIndex slot, Fx toX, Fx toZ, UnitStore& store,
                             const Terrain& terrain, const PassabilityGrid& grid) {
    MoveState& motion = store.motion()[slot];
    if (motion.airborne) {
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

[[nodiscard]] bool hasActiveConstruction(std::span<const Construction> building,
                                         UnitId builder) noexcept {
    return std::ranges::any_of(building, [builder](const Construction& work) {
        return !work.finished() && work.builder == builder;
    });
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

/// Retail's mobile-build range test (`CUnitMobileBuildTask` state 1): compare centre distance
/// after subtracting the builder's smaller footprint side and the product's larger skirt side.
[[nodiscard]] Fx constructionReach(const UnitCatalog& catalog, UnitTypeIndex builder,
                                   UnitTypeIndex product) noexcept {
    const UnitCatalog::Rates& builderRates = catalog.rates(builder);
    return builderRates.buildReachElmos + builderRates.buildFootprintElmos
         + catalog.rates(product).buildSkirtElmos;
}

/// Whether the active queue entry already owns a completed row. Finished constructions remain
/// as match history, so "no unfinished row" alone cannot distinguish completion from approach.
[[nodiscard]] bool finishedConstructionFor(const QueuedCommand& command, UnitIndex slot,
                                           const UnitStore& store, const UnitCatalog& catalog,
                                           std::span<const Construction> building) noexcept {
    const unitdef::UnitDef* builder = catalog.def(store.typeAt(slot));
    const unitdef::UnitDef* product = catalog.def(command.buildType());
    if (builder == nullptr || product == nullptr) {
        return false;
    }
    const bool upgrade = !builder->upgradesTo.empty() && builder->upgradesTo == product->name;
    const bool factoryProduction = builder->hasCategory("FACTORY") && product->isMobile();
    const Transform& at = store.transforms()[slot];
    const Fx siteX = (upgrade || factoryProduction) ? at.x : command.targetX();
    const Fx siteZ = (upgrade || factoryProduction) ? at.z : command.targetZ();
    return std::ranges::any_of(building, [&](const Construction& work) {
        return work.finished() && work.builder == store.idAt(slot)
            && work.blueprintIndex == command.buildType() && work.position[0] == siteX
            && work.position[2] == siteZ;
    });
}

void cancelActiveConstruction(std::vector<Construction>* building, UnitId builder) {
    if (building == nullptr) {
        return;
    }
    std::erase_if(*building, [builder](const Construction& work) {
        return !work.finished() && work.builder == builder;
    });
}

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

void canonicalizeUnits(std::vector<UnitId>& units) {
    std::ranges::sort(units, [](UnitId a, UnitId b) {
        return a.index < b.index || (a.index == b.index && a.generation < b.generation);
    });
    units.erase(std::unique(units.begin(), units.end()), units.end());
}

} // namespace

std::optional<CommandId> CommandBuffer::submit(CommandIssue issue, UnitStore& store) {
    if (issue.source == kInvalidCommandSource
        || issue.player != static_cast<PlayerIndex>(issue.source) || issue.count == 0
        || (issue.kind == CommandKind::Script
            && (issue.scriptTask.empty()
                || issue.scriptTask.size() > kMaxScriptTaskNameBytes
                || issue.scriptData.size() > kMaxScriptTaskDataBytes))
        || (issue.kind != CommandKind::Script
            && (!issue.scriptTask.empty() || !issue.scriptData.empty()))) {
        return std::nullopt;
    }
    const bool explicitId = issue.id != kInvalidCommandId;
    if (!explicitId) {
        const std::optional<CommandId> allocated = store.allocateCommandId(issue.source);
        if (!allocated) {
            return std::nullopt;
        }
        issue.id = *allocated;
    } else if (!store.consumeCommandId(issue.source, issue.id)) {
        return std::nullopt;
    }
    canonicalizeUnits(issue.units);
    // Live input cannot address nobody. Replay may carry an explicit empty accepted set because
    // the original submission still consumed an ID, which is authoritative allocator state.
    if (issue.units.empty() && !explicitId) {
        return std::nullopt;
    }
    const CommandId id = issue.id;
    pending_.push_back(std::move(issue));
    return id;
}

std::vector<CommandIssue> CommandBuffer::take(TickIndex tick, CommandPhase phase) {
    std::vector<CommandIssue> due;
    std::vector<CommandIssue> waiting;
    due.reserve(pending_.size());
    waiting.reserve(pending_.size());
    for (CommandIssue& issue : pending_) {
        if (issue.tick == tick && issue.phase == phase) {
            due.push_back(std::move(issue));
        } else {
            waiting.push_back(std::move(issue));
        }
    }
    pending_ = std::move(waiting);
    std::stable_sort(due.begin(), due.end(), [](const CommandIssue& a, const CommandIssue& b) {
        return a.source < b.source;
    });
    return due;
}

const char* commandKindName(CommandKind kind) noexcept {
    switch (kind) {
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
    case CommandKind::Overcharge:
        return "overcharge";
    case CommandKind::Assist:
        return "assist";
    case CommandKind::ToggleFactoryRepeat:
        return "toggle-factory-repeat";
    case CommandKind::Repair:
        return "repair";
    case CommandKind::Script:
        return "script";
    }
    return "stop";
}

namespace {

[[nodiscard]] std::optional<CommandKind> kindFromName(std::string_view name) noexcept {
    if (name == "move") {
        return CommandKind::Move;
    }
    if (name == "attack-move") {
        return CommandKind::AttackMove;
    }
    if (name == "patrol") {
        return CommandKind::Patrol;
    }
    if (name == "stop") {
        return CommandKind::Stop;
    }
    if (name == "assist") {
        return CommandKind::Assist;
    }
    if (name == "attack") {
        return CommandKind::Attack;
    }
    if (name == "build") {
        return CommandKind::Build;
    }
    if (name == "reclaim") {
        return CommandKind::Reclaim;
    }
    if (name == "overcharge") {
        return CommandKind::Overcharge;
    }
    if (name == "toggle-factory-repeat") {
        return CommandKind::ToggleFactoryRepeat;
    }
    if (name == "repair") {
        return CommandKind::Repair;
    }
    if (name == "script") {
        return CommandKind::Script;
    }
    return std::nullopt;
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

[[nodiscard]] bool validRepair(const Command& command, const UnitStore& store,
                               const UnitCatalog& catalog, std::span<const Army> armies) noexcept {
    if (!store.alive(command.target) || !store.health()[command.target.index].alive()
        || command.target == command.unit) {
        return false;
    }
    const unitdef::UnitDef* builder = catalog.def(store.typeAt(command.unit.index));
    const unitdef::UnitDef* target = catalog.def(store.typeAt(command.target.index));
    if (builder == nullptr || target == nullptr || !builder->isBuilder()
        || store.health()[command.target.index].current >= store.health()[command.target.index].maximum
        || target->buildTime <= Mag{}) {
        return false;
    }
    const int owner = store.motion()[command.unit.index].armyIndex;
    const int targetOwner = store.motion()[command.target.index].armyIndex;
    const auto mine = std::ranges::find_if(armies, [owner](const Army& army) {
        return army.index == owner;
    });
    const auto theirs = std::ranges::find_if(armies, [targetOwner](const Army& army) {
        return army.index == targetOwner;
    });
    if (mine == armies.end() || theirs == armies.end() || !allied(*mine, *theirs)) {
        return false;
    }
    return true;
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

} // namespace

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
           && a.scriptTask == b.scriptTask && a.scriptData == b.scriptData;
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

/// ART-S007 `GrowthFormation` selects these repeating land-block widths by total unit count.
/// The native formation instance owns category matching and rotation; this intake slice is
/// deliberately limited to one homogeneous ground type, where canonical rank fills each slot.
[[nodiscard]] std::size_t growthFormationWidth(std::size_t units) noexcept {
    if (units <= 3) {
        return 3;
    }
    if (units <= 12) {
        return 4;
    }
    if (units <= 20) {
        return 5;
    }
    if (units <= 30) {
        return 6;
    }
    if (units <= 42) {
        return 7;
    }
    return 8;
}

namespace {

[[nodiscard]] const std::shared_ptr<SharedCommand>& ensureSharedCommand(
    const Command& command, CommandSource source, CommandId id, std::uint32_t count,
    Fx formationAnchorX, Fx formationAnchorZ, std::string_view scriptTask,
    std::span<const std::uint8_t> scriptData, UnitStore& store,
    std::shared_ptr<SharedCommand>& shared) {
    if (shared == nullptr) {
        shared = std::make_shared<SharedCommand>(SharedCommand{
            .tick = command.tick,
            .source = source,
            .id = id,
            .player = command.player,
            .kind = command.kind,
            .queued = command.queued,
            // Finalized to the accepted subset after every requested member has been tried.
            .units = {},
            .targetX = formationAnchorX,
            .targetZ = formationAnchorZ,
            .target = command.target,
            .buildType = command.buildType,
            .creationSerial = store.allocateCommandSerial(),
            .originalCount = count,
            .remainingCount = count,
            .scriptTask = std::string{scriptTask},
            .scriptData = {scriptData.begin(), scriptData.end()},
        });
        (void)store.registerCommand(shared);
    }
    return shared;
}

/// Whether retail's outgoing move task preserves motion while this replacement clears it.
/// Of the command kinds implemented here, C-212's keep set contains Stop, Reclaim,
/// BuildMobile, and Upgrade. Factory production is BuildFactory and deliberately not included.
[[nodiscard]] bool replacementKeepsMotion(const Command& command, const UnitStore& store,
                                           const UnitCatalog& catalog) noexcept {
    if (command.kind == CommandKind::Stop || command.kind == CommandKind::Reclaim) {
        return true;
    }
    if (command.kind != CommandKind::Build) {
        return false;
    }
    const unitdef::UnitDef* builder = catalog.def(store.typeAt(command.unit.index));
    const unitdef::UnitDef* product = catalog.def(command.buildType);
    if (builder == nullptr || product == nullptr) {
        return false;
    }
    const bool upgrade = !builder->upgradesTo.empty() && builder->upgradesTo == product->name;
    const bool factoryProduction = builder->hasCategory("FACTORY") && product->isMobile();
    return upgrade || !factoryProduction;
}

void teardownMovement(MoveState& motion) {
    motion.moving = false;
    motion.path.clear();
    motion.pathIndex = 0;
    motion.pathPhaseCellsX = 0;
}

[[nodiscard]] bool applyCommandMember(
    const Command& command, CommandSource source, CommandId id, std::uint32_t count,
    Fx formationAnchorX, Fx formationAnchorZ, std::shared_ptr<SharedCommand>& shared,
    UnitStore& store,
    const UnitCatalog& catalog, std::span<const Player> players, std::span<const Army> armies,
    const Terrain& terrain, const PassabilityGrid* grid, TickRate rate,
    std::vector<Construction>* building, EventQueue* events, const FeatureStore* features,
    PathService* pathService, std::string_view scriptTask,
    std::span<const std::uint8_t> scriptData, ScriptTaskHost* scriptTasks) {
    // A stale handle first, before anything else looks at the slot. A player may click a unit
    // that died on the tick their order was issued, and a replay of an old log may name a unit
    // that no longer exists — in both cases the generation has moved on, so this must not
    // resolve to whoever inherited the slot.
    if (!store.alive(command.unit)) {
        return false;
    }

    const Player* player = playerFor(command.player, players);
    if (player == nullptr || !authorised(*player, store, command.unit, armies)) {
        return false;
    }
    if (command.kind == CommandKind::Assist && !validAssist(command, store, catalog)) {
        return false;
    }
    if (command.kind == CommandKind::Repair && !validRepair(command, store, catalog, armies)) {
        return false;
    }

    CommandQueue& orders = store.orders()[command.unit.index];
    if (scriptTasks != nullptr) {
        orders.bindScriptTaskHost(scriptTasks);
    }

    // Retail enforces the cap before dispatching the command kind, so an opaque script task is
    // bounded exactly like a move or build promise.
    if (command.queued && command.kind != CommandKind::Stop && orders.atCapacity()) {
        return false;
    }

    if (command.kind == CommandKind::Script) {
        if (scriptTasks == nullptr || scriptTask.empty()
            || scriptTask.size() > kMaxScriptTaskNameBytes
            || scriptData.size() > kMaxScriptTaskDataBytes) {
            return false;
        }
        const std::shared_ptr<const SharedCommand> payload = ensureSharedCommand(
            command, source, id, count, formationAnchorX, formationAnchorZ, scriptTask,
            scriptData, store, shared);
        QueuedCommand entry{command.unit, payload};
        if (command.queued) {
            (void)orders.give(std::move(entry), true);
            return true;
        }
        if (pathService != nullptr) {
            pathService->cancel(command.unit);
        }
        cancelActiveConstruction(building, command.unit);
        teardownMovement(store.motion()[command.unit.index]);
        orders.clear();
        orders.append(std::move(entry));
        return true;
    }
    if (grid == nullptr) {
        return false;
    }
    const PassabilityGrid& movementGrid = *grid;

    // THE CAP, retail's, at retail's boundary (`kCommandQueueCap`). It is checked here rather
    // than inside `CommandQueue::give` because retail checks it in `Sim::IssueCommand` — on
    // the way in from a command source, before anything is appended — so it bounds what a
    // PLAYER can pile up and leaves the engine's own inserts (a patrol's synthetic origin)
    // alone. An order that clears the queue is exempt, so a unit at the cap is still
    // commandable.
    // A STOP IS NOT A QUEUED ORDER HERE, shift or no shift. Recoil allows one — its comment at
    // `CommandAI.cpp:996` says as much, with an exclamation mark — and it needs to, because it
    // has a wait command that a queued stop interacts with. We have none, so a queued stop
    // would be an order to stand still at some future point in a route, which is what deleting
    // the rest of the route already means. It clears and stops.
    if (command.kind == CommandKind::Stop) {
        MoveState& motion = store.motion()[command.unit.index];
        const MoveState previous = motion;
        if (!startCommand(command, store, catalog, terrain, movementGrid, rate, building, events,
                          features)) {
            motion = previous;
            return false;
        }
        MoveState stopped = std::move(motion);
        motion = previous;
        if (pathService != nullptr) {
            pathService->cancel(command.unit);
        }
        orders.clear();
        cancelActiveConstruction(building, command.unit);
        motion = std::move(stopped);
        (void)ensureSharedCommand(command, source, id, count, formationAnchorX, formationAnchorZ,
                                  {}, {}, store, shared);
        return true;
    }

    if (command.queued) {
        const std::shared_ptr<const SharedCommand> payload =
            ensureSharedCommand(command, source, id, count, formationAnchorX, formationAnchorZ,
                                {}, {}, store, shared);
        // Factory production is repeatable: Shift-clicking the same tank twice means two tanks,
        // unlike placing the same structure twice, which retains the ordinary cancel gesture.
        if (command.kind == CommandKind::Build) {
            const unitdef::UnitDef* builder = catalog.def(store.typeAt(command.unit.index));
            const unitdef::UnitDef* product = catalog.def(command.buildType);
            if (builder != nullptr && product != nullptr && builder->hasCategory("FACTORY")
                && product->isMobile()) {
                orders.append(QueuedCommand{command.unit, payload});
                return true;
            }
        }
        const bool currentWasPatrol =
            orders.current() != nullptr && orders.current()->kind() == CommandKind::Patrol;
        const bool alreadyPatrolling = std::any_of(
            orders.entries().begin(), orders.entries().end(), [](const QueuedCommand& queued) {
                return queued.kind() == CommandKind::Patrol;
        });
        QueuedCommand entry{command.unit, payload};
        if (command.kind == CommandKind::Move) {
            entry.setTargetPosition(command.targetX, command.targetZ);
        }
        if (command.kind == CommandKind::Patrol && !alreadyPatrolling) {
            entry.setPatrolOrigin({store.transforms()[command.unit.index].x,
                                   store.transforms()[command.unit.index].z});
        }
        const CommandQueue::Result result = orders.give(std::move(entry), true);
        if (command.kind == CommandKind::Patrol
            && (result == CommandQueue::Result::Cancelled
                || result == CommandQueue::Result::CancelledCurrent)) {
            const std::size_t points = static_cast<std::size_t>(std::count_if(
                orders.entries().begin(), orders.entries().end(), [](const QueuedCommand& queued) {
                    return queued.kind() == CommandKind::Patrol;
                }));
            const bool hasHiddenOrigin = std::any_of(
                orders.entries().begin(), orders.entries().end(), [](const QueuedCommand& queued) {
                    return queued.kind() == CommandKind::Patrol && queued.patrolOrigin().has_value();
                });
            if (points + static_cast<std::size_t>(hasHiddenOrigin) < 2) {
                orders.remove(CommandKind::Patrol);
                if (currentWasPatrol) {
                    MoveState& motion = store.motion()[command.unit.index];
                    motion.moving = false;
                    motion.path.clear();
                    motion.pathIndex = 0;
                    if (const QueuedCommand* next = orders.current()) {
                        if (startCommand(next->asCommand(), store, catalog, terrain, movementGrid, rate,
                                         building, events, features)) {
                            orders.markCurrentActive();
                        }
                    }
                    return true;
                }
            }
        }
        switch (result) {
        case CommandQueue::Result::CancelledCurrent: {
            // The order the unit was carrying out has been taken away, so it has to be
            // interrupted as well as forgotten. Recoil pushes a stop to the front and lets its
            // own slow update pick it up (`:1044-1049`); stopping here and starting whatever
            // is next reaches the same state one tick sooner.
            MoveState& motion = store.motion()[command.unit.index];
            motion.moving = false;
            motion.path.clear();
            motion.pathIndex = 0;
            cancelActiveConstruction(building, command.unit);
            if (const QueuedCommand* next = orders.current()) {
                if (startCommand(next->asCommand(), store, catalog, terrain, movementGrid, rate, building,
                                 events, features)) {
                    orders.markCurrentActive();
                }
            }
            return true;
        }
        case CommandQueue::Result::Appended:
        case CommandQueue::Result::Cancelled:
        case CommandQueue::Result::Replaced:
            // Appended or cancelled, and either way the order took. `Replaced` cannot happen
            // on this branch — `give` only returns it for an unqueued order — and is listed so
            // that adding a `Result` is a compile error rather than a silent fall-through.
            return true;
        }
        return true;
    }

    // A plain ground move is accepted as intent, then its owning army's FIFO service performs
    // the existing A* over later beats. Keeping the entry active prevents ordinary dispatch from
    // treating an unpublished path as an arrived move.
    if (command.kind == CommandKind::Move && pathService != nullptr
        && !store.motion()[command.unit.index].airborne) {
        const std::shared_ptr<const SharedCommand> payload =
            ensureSharedCommand(command, source, id, count, formationAnchorX, formationAnchorZ,
                                {}, {}, store, shared);
        MoveState& motion = store.motion()[command.unit.index];
        pathService->cancel(command.unit);
        orders.clear();
        teardownMovement(motion);
        QueuedCommand entry{command.unit, payload};
        entry.setTargetPosition(command.targetX, command.targetZ);
        orders.append(std::move(entry));
        orders.markCurrentActive();
        const Transform& at = store.transforms()[command.unit.index];
        motion.pathPhaseStartX = movementGrid.cellAtWorld(at.x);
        motion.pathPhaseStartZ = movementGrid.cellAtWorld(at.z);
        motion.pathPhaseCellsX = movementGrid.cellsX;
        pathService->enqueue(PathRequest{.unit = command.unit,
                                         .command = id,
                                         .army = motion.armyIndex,
                                         .fromX = at.x,
                                         .fromZ = at.z,
                                         .targetX = command.targetX,
                                         .targetZ = command.targetZ,
                                         .grid = std::make_shared<PassabilityGrid>(movementGrid)});
        return true;
    }

    // Stage the replacement before clearing so a refused order changes nothing. Once accepted,
    // restore the outgoing task for the clear notifications, apply C-212's replacement-kind
    // teardown, insert the replacement, then publish the staged motion from its successful
    // start. Observers therefore see no replacement during `Cleared` and never see the old task
    // destroyed according to the wrong incoming kind.
    MoveState& motion = store.motion()[command.unit.index];
    const MoveState previous = motion;
    const bool keepMotion = replacementKeepsMotion(command, store, catalog);
    if (!keepMotion) {
        teardownMovement(motion);
    }
    if (!startCommand(command, store, catalog, terrain, movementGrid, rate, building, events,
                      features)) {
        motion = previous;
        return false;
    }
    MoveState replacementMotion = std::move(motion);
    motion = previous;
    if (command.kind != CommandKind::Build) {
        cancelActiveConstruction(building, command.unit);
    }
    const std::shared_ptr<const SharedCommand> payload =
        ensureSharedCommand(command, source, id, count, formationAnchorX, formationAnchorZ,
                            {}, {}, store, shared);
    QueuedCommand entry{command.unit, payload};
    if (command.kind == CommandKind::Move) {
        entry.setTargetPosition(command.targetX, command.targetZ);
    }
    if (command.kind == CommandKind::Patrol) {
        entry.setPatrolOrigin({store.transforms()[command.unit.index].x,
                               store.transforms()[command.unit.index].z});
    }
    if (pathService != nullptr) {
        pathService->cancel(command.unit);
    }
    orders.clear();
    if (!keepMotion) {
        teardownMovement(motion);
    }
    orders.append(std::move(entry));
    orders.markCurrentActive();
    motion = std::move(replacementMotion);
    return true;
}

} // namespace

bool ApplyCommandResult::acceptedUnit(UnitId unit) const noexcept {
    return std::ranges::find(accepted, unit) != accepted.end();
}

ApplyCommandResult applyCommand(const CommandIssue& issue, UnitStore& store,
                                const UnitCatalog& catalog,
                                std::span<const Player> players,
                                std::span<const Army> armies, const Terrain& terrain,
                                 const CommandGridForUnit& gridForUnit, TickRate rate,
                                 std::vector<Construction>* building, EventQueue* events,
                                 const FeatureStore* features, PathService* pathService,
                                 ScriptTaskHost* scriptTasks) {
    ApplyCommandResult result;
    if (issue.source == kInvalidCommandSource || issue.id == kInvalidCommandId
        || issue.player != static_cast<PlayerIndex>(issue.source)
        || commandSource(issue.id) != issue.source || issue.count == 0
        || playerFor(issue.player, players) == nullptr || store.commandIdLive(issue.id)
        || (issue.kind == CommandKind::Script
            && (issue.scriptTask.empty()
                || issue.scriptTask.size() > kMaxScriptTaskNameBytes
                || issue.scriptData.size() > kMaxScriptTaskDataBytes))
        || (issue.kind != CommandKind::Script
            && (!issue.scriptTask.empty() || !issue.scriptData.empty()))) {
        return result;
    }

    std::vector<UnitId> canonical = issue.units;
    canonicalizeUnits(canonical);

    // `GrowthFormation` is the travel formation for non-air groups (C-178). Its native slot
    // matcher is not available here, so use its topology only for a homogeneous set of members
    // that would reach the command boundary. Rejected handles must not consume a formation slot.
    const Player* issuer = playerFor(issue.player, players);
    std::vector<UnitId> formationMembers;
    std::optional<UnitTypeIndex> homogeneousType;
    bool useGrowthFormation = issue.kind == CommandKind::Move && canonical.size() > 1;
    for (const UnitId unit : canonical) {
        if (!useGrowthFormation) {
            break;
        }
        if (!store.alive(unit) || issuer == nullptr || !authorised(*issuer, store, unit, armies)
            || store.motion()[unit.index].airborne || gridForUnit == nullptr
            || gridForUnit(unit) == nullptr) {
            continue;
        }
        const UnitTypeIndex type = store.typeAt(unit.index);
        if (homogeneousType.has_value() && *homogeneousType != type) {
            useGrowthFormation = false;
            break;
        }
        homogeneousType = type;
        formationMembers.push_back(unit);
    }
    useGrowthFormation = useGrowthFormation && formationMembers.size() > 1;
    const std::size_t formationWidth = growthFormationWidth(formationMembers.size());

    result.accepted.reserve(canonical.size());
    if (issue.kind == CommandKind::ToggleFactoryRepeat) {
        for (const UnitId unit : canonical) {
            if (!store.alive(unit)) {
                continue;
            }
            const Player* player = playerFor(issue.player, players);
            const unitdef::UnitDef* definition = catalog.def(store.typeAt(unit.index));
            if (player == nullptr || !authorised(*player, store, unit, armies)
                || definition == nullptr || !definition->hasCategory("FACTORY")) {
                continue;
            }
            (void)store.setFactoryRepeat(unit, !store.factoryRepeat(unit));
            result.accepted.push_back(unit);
        }
        return result;
    }
    std::shared_ptr<SharedCommand> shared;
    for (std::size_t rank = 0; rank < canonical.size(); ++rank) {
        const UnitId unit = canonical[rank];
        // Validate the handle and authority before asking a resolver that may index by the
        // handle. `applyCommandMember` repeats these checks at the mutation boundary.
        if (!store.alive(unit)) {
            continue;
        }
        const Player* player = playerFor(issue.player, players);
        if (player == nullptr || !authorised(*player, store, unit, armies)) {
            continue;
        }
        const PassabilityGrid* grid = gridForUnit != nullptr ? gridForUnit(unit) : nullptr;
        if (grid == nullptr && issue.kind != CommandKind::Script) {
            continue;
        }
        Command member{
            .tick = issue.tick,
            .player = issue.player,
            .kind = issue.kind,
            .queued = issue.queued,
            .unit = unit,
            .targetX = issue.targetX,
            .targetZ = issue.targetZ,
            .target = issue.target,
            .buildType = issue.buildType,
        };

        if (useGrowthFormation) {
            // ART-S007's homogeneous GrowthFormation rows enumerate columns centre-outward:
            // odd widths are 0,+1,-1,+2,-2 and even widths are -.5,+.5,-1.5,+1.5. A
            // collision diameter is the existing local-target spacing, so its half supplies the
            // even-row half positions; successive rows are one diameter behind the anchor.
            const std::size_t formationRank = static_cast<std::size_t>(
                std::ranges::find(formationMembers, unit) - formationMembers.begin());
            const std::size_t column = formationRank % formationWidth;
            const std::size_t row = formationRank / formationWidth;
            const Fx radius = store.motion()[unit.index].radiusElmos;
            const Fx diameter = Fx::fromRaw(saturate(FxWide{radius.raw()} * 2));
            if (formationWidth % 2 == 0) {
                const FxWide halfDiameterOffsets = column % 2 == 0
                                                       ? -static_cast<FxWide>(column + 1)
                                                       : static_cast<FxWide>(column);
                member.targetX += Fx::fromRaw(
                    saturate(FxWide{radius.raw()} * halfDiameterOffsets));
            } else {
                const FxWide diameterOffsets = column % 2 == 0
                                                   ? -static_cast<FxWide>(column / 2)
                                                   : static_cast<FxWide>((column + 1) / 2);
                member.targetX += Fx::fromRaw(
                    saturate(FxWide{diameter.raw()} * diameterOffsets));
            }
            member.targetZ -= Fx::fromRaw(saturate(FxWide{diameter.raw()}
                                                    * static_cast<FxWide>(row)));
        } else if (issue.kind == CommandKind::Move && canonical.size() > 1
                   && !store.motion()[unit.index].airborne) {
            // This is deliberately only a generic intake fan-out, not retail's Lua-owned
            // formation geometry. Centre an unrotated line on the clicked anchor; adjacent
            // ranks are one collision diameter apart, so canonical ranks receive distinct local
            // destinations while the immutable shared command still records the click itself.
            const FxWide offsetRanks = static_cast<FxWide>(rank) * 2
                                       - static_cast<FxWide>(canonical.size() - 1);
            const Fx spacing = store.motion()[unit.index].radiusElmos;
            member.targetX += Fx::fromRaw(saturate(FxWide{spacing.raw()} * offsetRanks));
        }

        if (applyCommandMember(member, issue.source, issue.id, issue.count, issue.targetX,
                               issue.targetZ, shared, store, catalog, players, armies, terrain,
                               grid, rate, building, events, features, pathService,
                               issue.scriptTask, issue.scriptData, scriptTasks)) {
            result.accepted.push_back(unit);
        }
    }
    if (shared != nullptr) {
        shared->units = result.accepted;
    }
    return result;
}

bool applyCommand(const Command& command, UnitStore& store, const UnitCatalog& catalog,
                  std::span<const Player> players, std::span<const Army> armies,
                   const Terrain& terrain, const PassabilityGrid& grid, TickRate rate,
                   std::vector<Construction>* building, EventQueue* events,
                   const FeatureStore* features, PathService* pathService,
                   ScriptTaskHost* scriptTasks) {
    if (command.player >= static_cast<PlayerIndex>(kInvalidCommandSource)
        || command.kind == CommandKind::Script) {
        return false;
    }
    const CommandSource source = static_cast<CommandSource>(command.player);
    const std::optional<CommandId> id = store.allocateCommandId(source);
    if (!id) {
        return false;
    }
    const CommandIssue issue{
        .tick = command.tick,
        .source = source,
        .id = *id,
        .player = command.player,
        .kind = command.kind,
        .queued = command.queued,
        .units = {command.unit},
        .targetX = command.targetX,
        .targetZ = command.targetZ,
        .target = command.target,
        .buildType = command.buildType,
    };
    const ApplyCommandResult result = applyCommand(
        issue, store, catalog, players, armies, terrain,
        [&grid](UnitId) { return &grid; }, rate, building, events, features, pathService,
        scriptTasks);
    return result.acceptedUnit(command.unit);
}

bool publishPathResult(const PathResult& result, UnitStore& store) {
    if (result.path.empty() || !store.alive(result.unit)) {
        return false;
    }
    CommandQueue& orders = store.orders()[result.unit.index];
    const QueuedCommand* current = orders.active();
    if (current == nullptr || current->payload().id != result.command) {
        return false;
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
                             ScriptTaskHost* scriptTasks, std::vector<GuardWork>* guardWork) {
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
            }
            continue;
        }
        if (scriptTasks != nullptr) {
            orders[slot].bindScriptTaskHost(scriptTasks);
        }

        // Movement uses this unit's grid; construction uses the PRODUCT's. Commands retain type
        // ids but not derived grids, so the choice must be repeated when a deferred order starts.
        const auto gridFor = [&](const QueuedCommand& command) -> const PassabilityGrid* {
            const auto builderType = static_cast<std::size_t>(store.typeAt(slot));
            const PassabilityGrid* builderGrid =
                builderType < gridForType.size() ? gridForType[builderType] : nullptr;
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
                if (pending->kind() == CommandKind::Repair
                    && !repairStillAllied(slot, pending->target(), store, armies)) {
                    (void)orders[slot].finish();
                    continue;
                }
                if (pending->kind() == CommandKind::Move && pathService != nullptr) {
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
                const bool wasInstant = instantaneous(pending->kind());
                if (startCommand(pending->asCommand(), store, catalog, terrain, *pendingGrid,
                                 rate, building, events, features)) {
                    orders[slot].markCurrentActive();
                    ++started;
                    if (!wasInstant) {
                        return;
                    }
                }
                (void)orders[slot].finish();
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
            if (Construction* work = activeConstruction(*building, store.idAt(slot))) {
                advanceConstruction(*work);
                if (!work->finished()) {
                    return false;  // still rising; the order stays at the head
                }
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
            }
            continue;
        }

        // BEFORE the grid lookup, deliberately. A build in progress does not move, so asking
        // which passability grid it would route on is a question with no bearing on whether it
        // rises — and answering it first would stall every construction in a scene that has no
        // grid for the product's motion class. `startPending`, which does need one, looks it up
        // for itself.
        const QueuedCommand* current = orders[slot].active();
        MoveState& activeMotion = store.motion()[slot];
        const bool wingedAttack = current->kind() == CommandKind::Attack
                               && current->target().generation != 0
                               && store.alive(current->target())
                               && activeMotion.canFly && activeMotion.airWinged;
        if (activeMotion.canFly && !wingedAttack) {
            activeMotion.airCombatState = MoveState::AirCombatState::None;
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
                && activeConstruction(*building, store.idAt(slot)) == nullptr
                && !finishedConstructionFor(*current, slot, store, catalog, *building)) {
                const PassabilityGrid* buildGrid = gridFor(*current);
                if (buildGrid == nullptr) {
                    continue;
                }
                if (!startCommand(current->asCommand(), store, catalog, terrain, *buildGrid,
                                  rate, building, events, features)) {
                    (void)orders[slot].finish();
                    startPending();
                    continue;
                }
                if (activeConstruction(*building, store.idAt(slot)) == nullptr) {
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
        if (current->kind() == CommandKind::Assist && building != nullptr) {
            const unitdef::UnitDef* guardDef = catalog.def(store.typeAt(slot));
            if (guardDef != nullptr && guardDef->hasCategory("FACTORY")
                && !guardDef->isMobile() && guardDef->isBuilder()) {
                if (Construction* work = activeConstruction(*building, store.idAt(slot))) {
                    advanceConstruction(*work);
                    if (!work->finished()) {
                        continue;
                    }
                    // This is the guarding factory's OWN queued build, which block A starts
                    // with its command retained. Complete it through the ordinary factory
                    // count/repeat ladder, but operate behind the active guard order (`C-211`).
                    // Block A starts the first compatible own build. Until this child finishes,
                    // `startCommand` refuses another build for this factory; input can only
                    // append behind Assist, while stop, replacement, or cancellation cancels the
                    // child. Thus the first matching Build here is precisely the one Block A
                    // started, even when later entries build the same product type.
                    const auto own = std::ranges::find_if(
                        orders[slot].entries(), [work](const QueuedCommand& candidate) {
                            return candidate.kind() == CommandKind::Build
                                   && candidate.buildType() == work->blueprintIndex;
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
                        || !unitdef::matchesExpression(guardDef->buildableCategory, *product)) {
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
                    if (startCommand(candidate.asCommand(), store, catalog, terrain, *productGrid,
                                     rate, building, events, features)) {
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
                        || !unitdef::matchesExpression(guardDef->buildableCategory, *product)) {
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
                                       building, events, features);
                    continue;
                }
            }
        }

        // C-183's leash is measured from the guard to the guarded unit's current position
        // (or, in retail's richer task object, its resolved guarded/build position). The
        // guardee's full blueprint width is added to half GuardScanRadius. GuardReturnRadius
        // is not involved: retail never reads it.
        if (current->kind() == CommandKind::Assist && store.alive(current->target())) {
            const unitdef::UnitDef* guardDef = catalog.def(store.typeAt(slot));
            const unitdef::UnitDef* guardedDef = catalog.def(store.typeAt(current->target().index));
            if (guardDef != nullptr && guardedDef != nullptr && guardDef->isMobile()
                && guardDef->guardScanRadiusElmos > Fx{}) {
                std::array<Fx, 3> anchor = positionOf(store.transforms()[current->target().index]);
                if (building != nullptr) {
                    const auto work = std::ranges::find_if(*building, [&](const Construction& item) {
                        return !item.finished() && item.builder == current->target();
                    });
                    if (work != building->end()) {
                        anchor = work->position;
                    }
                }
                const Fx guardedWidth = fxFromFloat(guardedDef->collisionRadiusElmos * 2.0f);
                const Fx leash = guardedWidth
                               + guardDef->guardScanRadiusElmos / Fx::fromInt(2);
                const std::array<Fx, 3> from = positionOf(store.transforms()[slot]);
                const Fx distance = fxHypot(fxHypot(anchor[0] - from[0],
                                                     anchor[2] - from[2]),
                                            anchor[1] - from[1]);
                if (distance > leash) {
                    const PassabilityGrid* returnGrid = gridFor(*current);
                    if (returnGrid != nullptr) {
                        (void)routeUnit(slot, anchor[0], anchor[2], store, terrain, *returnGrid);
                    }
                    continue;
                }
            }
        }

        // C-183's ATTACK branch outranks every assist. A mobile guard whose scan covers a
        // hostile acquires through the ordinary path and pursues it, while Assist stays at
        // the head — no child command, no ids, no log entries. Factory guards never arrive
        // here with live mirror work: that branch continued above, which is the ladder order.
        //
        // The acquisition is a range-overridden copy of each firing weapon, so priorities,
        // restrictions, arcs, incumbency and recon all apply exactly as in combat — the
        // structural equivalent of delegating to `IAiAttacker` with `GuardScanRadius`.
        // Multi-weapon selection among the guard's own guns still resolves to nearest here,
        // and combat-unit Guard remains represented by the existing Assist-capable path.
        if (current->kind() == CommandKind::Assist && !armies.empty()
            && store.alive(current->target())) {
            const unitdef::UnitDef* guardDef = catalog.def(store.typeAt(slot));
            if (guardDef != nullptr && guardDef->isMobile()
                && guardDef->guardScanRadiusElmos > Fx{}) {
                const std::span<const Transform> sight = store.transforms();
                const int armyIndex = store.motion()[slot].armyIndex;
                std::optional<UnitId> prey;
                Fx preyDistance{};
                for (std::size_t w = 0; w < guardDef->weapons.size(); ++w) {
                    const unitdef::Weapon& weapon = guardDef->weapons[w];
                    if (!weapon.fires() || weapon.manuallyFired()
                        || weapon.targetsProjectiles) {
                        continue;
                    }
                    unitdef::Weapon ranged = weapon;
                    ranged.maxRange = guardDef->guardScanRadiusElmos;
                    std::optional<UnitId> incumbent;
                    const auto& cache = store.health()[slot].automaticTargets;
                    if (w < cache.size()) {
                        incumbent = cache[w];
                    }
                    const std::optional<UnitId> found = nearestTarget(
                        positionOf(sight[slot]), armyIndex, ranged, store, armies, intel,
                        &catalog, sight[slot].heading, incumbent, playableRect);
                    if (!found) {
                        continue;
                    }
                    const Fx distance = groundDistanceElmos(
                        positionOf(sight[slot]), positionOf(sight[found->index]));
                    if (!prey || distance < preyDistance
                        || (distance == preyDistance && found->index < prey->index)) {
                        prey = found;
                        preyDistance = distance;
                    }
                }
                if (prey.has_value()) {
                    Fx reach{};
                    const bool targetAirborne = store.motion()[prey->index].airborne;
                    for (const unitdef::Weapon& weapon : guardDef->weapons) {
                        if (!weapon.fires() || weapon.manuallyFired()
                            || weapon.targetsProjectiles
                            || !weapon.canTarget(targetAirborne) || weapon.maxRange <= reach) {
                            continue;
                        }
                        reach = weapon.maxRange;
                    }
                    MoveState& chase = store.motion()[slot];
                    const Fx gap = groundDistanceElmos(positionOf(sight[slot]),
                                                       positionOf(sight[prey->index]));
                    if (reach <= Fx{} || gap <= reach) {
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
        if (current->kind() == CommandKind::Assist && building != nullptr
            && store.alive(current->target())) {
            UnitId founder = current->target();
            std::vector<UnitId> visited;
            while (store.alive(founder) && std::ranges::find(visited, founder) == visited.end()) {
                visited.push_back(founder);
                const QueuedCommand* guarded = orders[founder.index].active();
                if (guarded == nullptr || guarded->kind() != CommandKind::Assist
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
        if (current->kind() == CommandKind::Assist && !guardBuildAssist && features != nullptr
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
        if (current->kind() == CommandKind::Assist && !guardBuildAssist
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
                || head->kind() == CommandKind::Assist)
            && store.alive(head->target())) {
            // An overcharge pursues exactly as an attack does; the reach is the MANUAL
            // weapon's, because that is the gun this order will fire. A fired overcharge
            // forgets its target (`fireOvercharge`), so a spent order falls out of this
            // block and retires below like any arrival. An ASSIST pursues with the BUILD
            // reach — follow the working engineer, hold beside the factory — and, being a
            // standing order, completes only when the target dies, which is exactly this
            // block's rule.
            const bool manual = head->kind() == CommandKind::Overcharge;
            const unitdef::UnitDef* def = catalog.def(store.typeAt(slot));
            Fx reach{};
            if (head->kind() == CommandKind::Assist) {
                reach = catalog.rates(store.typeAt(slot)).buildReachElmos
                      + store.motion()[slot].radiusElmos
                      + store.motion()[head->target().index].radiusElmos;
            } else if (def != nullptr) {
                const bool targetAirborne = store.motion()[head->target().index].airborne;
                for (const unitdef::Weapon& weapon : def->weapons) {
                    if ((manual ? weapon.manuallyFired() : weapon.fires())
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
                    // `C-224` states 1 and 2. A winged entity attack is a fly-through,
                    // never the ground mover's stop-at-weapon-range chase. State 1 attacks
                    // head-on at max speed and at AttackElevation. Once the target is ahead
                    // and both forward vectors agree inside the recovered 30-degree cone,
                    // the attacker is on its six and state 2 owns the chase.
                    if (chase.airCombatState == MoveState::AirCombatState::None) {
                        chase.airCombatState = MoveState::AirCombatState::HeadOn;
                    }
                    const Polar targetDirection = fxPolar(theirs.x - mine.x,
                                                          theirs.z - mine.z);
                    constexpr Fx kThirtyDegreeCos = Fx::fromRatio(866, 1000);
                    const bool targetAhead = targetDirection.length > Fx{}
                        && fxCos(static_cast<Brad>(mine.heading - targetDirection.bearing))
                               > kThirtyDegreeCos;
                    const bool headingsAgree =
                        fxCos(static_cast<Brad>(mine.heading - theirs.heading))
                        > kThirtyDegreeCos;
                    if (chase.airCombatState == MoveState::AirCombatState::HeadOn
                        && targetAhead && headingsAgree) {
                        chase.airCombatState = MoveState::AirCombatState::TailChase;
                    }
                    (void)routeUnit(slot, theirs.x, theirs.z, store, terrain, *grid);
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
        if (const QueuedCommand* head = orders[slot].active();
            head != nullptr && head->kind() == CommandKind::Attack
            && head->target().generation != 0 && !store.alive(head->target())) {
            MoveState& staleRoute = store.motion()[slot];
            staleRoute.moving = false;
            staleRoute.path.clear();
            staleRoute.pathIndex = 0;
            (void)orders[slot].finish();
            startPending();
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

        // An aggressive order with a temporary target belongs to the post-intel pass, even
        // when it is currently holding still. A stale target is cleared there and the original
        // waypoint resumed; treating the hold as arrival here would lose that destination.
        if (const QueuedCommand* head = orders[slot].active();
            head != nullptr
            && (head->kind() == CommandKind::AttackMove || head->kind() == CommandKind::Patrol)
            && head->target().generation != 0) {
            continue;
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
                            const Intel* intel, const PlayableRect* playableRect) {
    const auto armyFor = [armies](int index) -> const Army* {
        for (const Army& army : armies) {
            if (army.index == index) {
                return &army;
            }
        }
        return nullptr;
    };

    for (UnitIndex slot = 0; slot < store.orders().size(); ++slot) {
        if (!store.slotAlive(slot)) {
            continue;
        }
        QueuedCommand* order = store.orders()[slot].activeMutable();
        if (order == nullptr
            || (order->kind() != CommandKind::AttackMove
                && order->kind() != CommandKind::Patrol)) {
            continue;
        }

        const auto type = static_cast<std::size_t>(store.typeAt(slot));
        const PassabilityGrid* grid = type < gridForType.size() ? gridForType[type] : nullptr;
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
                                nullptr, nullptr);
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
                                   store.transforms()[slot].heading, std::nullopt, playableRect);
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

namespace {

bool startCommand(const Command& command, UnitStore& store, const UnitCatalog& catalog,
                  const Terrain& terrain, const PassabilityGrid& grid, TickRate rate,
                  std::vector<Construction>* building, EventQueue* events,
                  const FeatureStore* features) {
    // By SLOT, not by handle: `advanceOrders` starts an order for a slot it has already found
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
    case CommandKind::Assist: {
        // The guard order. A builder helps a LIVING unit of its own army — helping the
        // enemy build is not a thing, and "assist yourself" is a click that means nothing.
        if (!validAssist(command, store, catalog)) {
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
        if (hasActiveConstruction(*building, command.unit)) {
            return false;
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
        if (!unitdef::matchesExpression(builder->buildableCategory, *def)) {
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
            return routedToSite
                || routeUnit(command.unit.index, siteX, siteZ, store, terrain, grid);
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
            .buildPerTick = rate.magPerTick(builder->buildRate),
            .blueprintIndex = command.buildType,
            .upgradeOf = upgrade ? command.unit : UnitId{},
            .builder = command.unit,
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
    case CommandKind::ToggleFactoryRepeat:
        return false;  // applied immediately by semantic issue intake; it never enters a queue
    case CommandKind::Script:
        return false;  // dispatched through ScriptTaskHost, never as a movement/build command
    }

    return false;
}

} // namespace

// --- The log --------------------------------------------------------------------------

namespace {

inline constexpr std::string_view kCommandLogMagic = "recoil-metal semantic command log";
inline constexpr std::uint32_t kCommandLogVersion = 2;

[[nodiscard]] const char* phaseName(CommandPhase phase) noexcept {
    return phase == CommandPhase::PreTick ? "pre-tick" : "post-spawn";
}

[[nodiscard]] std::optional<CommandPhase> phaseFromName(std::string_view name) noexcept {
    if (name == "pre-tick") {
        return CommandPhase::PreTick;
    }
    if (name == "post-spawn") {
        return CommandPhase::PostSpawn;
    }
    return std::nullopt;
}

[[nodiscard]] bool canonicalUnits(std::span<const UnitId> units) noexcept {
    return std::adjacent_find(units.begin(), units.end(), [](UnitId a, UnitId b) {
               return a.index > b.index
                      || (a.index == b.index && a.generation >= b.generation);
           }) == units.end();
}

[[nodiscard]] bool issueBefore(const CommandIssue& a, const CommandIssue& b) noexcept {
    return a.tick < b.tick
           || (a.tick == b.tick
               && static_cast<std::uint8_t>(a.phase) < static_cast<std::uint8_t>(b.phase));
}

template <typename To, typename From>
[[nodiscard]] bool fits(From value) noexcept {
    return value <= static_cast<From>(std::numeric_limits<To>::max());
}

} // namespace

bool CommandLog::record(CommandIssue issue) {
    if (issue.source == kInvalidCommandSource || issue.id == kInvalidCommandId
        || commandSource(issue.id) != issue.source
        || issue.player != static_cast<PlayerIndex>(issue.source) || issue.count == 0
        || !canonicalUnits(issue.units)
        || (issue.kind == CommandKind::Script
            && (issue.scriptTask.empty()
                || issue.scriptTask.size() > kMaxScriptTaskNameBytes
                || issue.scriptData.size() > kMaxScriptTaskDataBytes))
        || (issue.kind != CommandKind::Script
            && (!issue.scriptTask.empty() || !issue.scriptData.empty()))
        || (!issues_.empty() && issueBefore(issue, issues_.back()))) {
        return false;
    }
    issues_.push_back(std::move(issue));
    return true;
}

std::span<const CommandIssue> CommandLog::at(TickIndex tick, CommandPhase phase) const noexcept {
    if (issues_.empty()) {
        return {};
    }
    const CommandIssue key{.tick = tick, .phase = phase};
    const auto begin = std::lower_bound(issues_.begin(), issues_.end(), key, issueBefore);
    const auto end = std::upper_bound(begin, issues_.end(), key, issueBefore);
    return std::span<const CommandIssue>{issues_.data() + (begin - issues_.begin()),
                                         static_cast<std::size_t>(end - begin)};
}

TickIndex CommandLog::lastTick() const noexcept {
    return issues_.empty() ? TickIndex{0} : issues_.back().tick;
}

bool writeCommandLog(const CommandLog& log, const std::string& path,
                     const std::function<std::string(std::uint32_t)>& pathFor) {
    std::ofstream out{path, std::ios::binary | std::ios::trunc};
    if (!out) {
        return false;
    }

    out << kCommandLogMagic << '\n';
    out << "version " << kCommandLogVersion << '\n';
    out << "issue-count " << log.size() << '\n';
    out << "# tick phase source id player kind queued count targetX targetZ buildType"
           " targetIndex targetGeneration unitCount [unitIndex unitGeneration]..."
           " buildPath scriptTask scriptDataHex\n";
    for (const CommandIssue& issue : log.all()) {
        out << issue.tick << ' ' << phaseName(issue.phase) << ' '
            << static_cast<unsigned>(issue.source) << ' ' << issue.id << ' ' << issue.player << ' '
            << commandKindName(issue.kind) << ' ' << (issue.queued ? 1 : 0) << ' '
            << issue.count << ' ' << issue.targetX.raw() << ' ' << issue.targetZ.raw() << ' '
            << issue.buildType << ' ' << issue.target.index << ' ' << issue.target.generation << ' '
            << issue.units.size();
        for (UnitId unit : issue.units) {
            out << ' ' << unit.index << ' ' << unit.generation;
        }
        std::string blueprint;
        if (issue.kind == CommandKind::Build && pathFor != nullptr) {
            blueprint = pathFor(issue.buildType);
        }
        static constexpr char kHex[] = "0123456789abcdef";
        std::string scriptData;
        scriptData.reserve(issue.scriptData.size() * 2);
        for (const std::uint8_t byte : issue.scriptData) {
            scriptData.push_back(kHex[byte >> 4]);
            scriptData.push_back(kHex[byte & 0x0F]);
        }
        out << ' ' << std::quoted(blueprint) << ' ' << std::quoted(issue.scriptTask) << ' '
            << std::quoted(scriptData) << '\n';
    }
    return out.good();
}

std::optional<CommandLog> readCommandLog(const std::string& path,
                                         std::vector<std::string>* buildPaths) {
    std::ifstream in{path, std::ios::binary};
    if (!in) {
        return std::nullopt;
    }

    std::string line;
    if (!std::getline(in, line) || line != kCommandLogMagic) {
        return std::nullopt;
    }
    std::uint64_t version = 0;
    if (!std::getline(in, line)) {
        return std::nullopt;
    }
    {
        std::istringstream fields{line};
        std::string label;
        std::string extra;
        if (!(fields >> label >> version) || label != "version" || version != kCommandLogVersion
            || fields >> extra) {
            return std::nullopt;
        }
    }
    std::uint64_t issueCount = 0;
    if (!std::getline(in, line)) {
        return std::nullopt;
    }
    {
        std::istringstream fields{line};
        std::string label;
        std::string extra;
        if (!(fields >> label >> issueCount) || label != "issue-count" || fields >> extra
            || !fits<std::size_t>(issueCount)) {
            return std::nullopt;
        }
    }

    CommandLog log;
    std::vector<std::string> paths;
    paths.reserve(static_cast<std::size_t>(issueCount));
    std::uint64_t parsedCount = 0;
    while (parsedCount < issueCount && std::getline(in, line)) {
        if (line.empty() || line.front() == '#') {
            continue;
        }

        std::istringstream fields{line};
        std::uint64_t tick = 0;
        std::string phaseText;
        std::uint64_t source = 0;
        std::uint64_t id = 0;
        std::uint64_t player = 0;
        std::string kindText;
        std::uint64_t queued = 0;
        std::uint64_t count = 0;
        std::int64_t targetX = 0;
        std::int64_t targetZ = 0;
        std::uint64_t buildType = 0;
        std::uint64_t targetIndex = 0;
        std::uint64_t targetGeneration = 0;
        std::uint64_t unitCount = 0;
        if (!(fields >> tick >> phaseText >> source >> id >> player >> kindText >> queued >> count
              >> targetX >> targetZ >> buildType >> targetIndex >> targetGeneration
              >> unitCount)) {
            return std::nullopt;
        }
        const std::optional<CommandPhase> phase = phaseFromName(phaseText);
        const std::optional<CommandKind> kind = kindFromName(kindText);
        if (!phase || !kind || source >= kInvalidCommandSource || queued > 1 || count == 0
            || !fits<CommandId>(id) || !fits<PlayerIndex>(player)
            || !fits<std::uint32_t>(count) || targetX < std::numeric_limits<FxRaw>::min()
            || targetX > std::numeric_limits<FxRaw>::max()
            || targetZ < std::numeric_limits<FxRaw>::min()
            || targetZ > std::numeric_limits<FxRaw>::max()
            || !fits<UnitTypeIndex>(buildType) || !fits<UnitIndex>(targetIndex)
            || !fits<Generation>(targetGeneration) || !fits<std::size_t>(unitCount)) {
            return std::nullopt;
        }

        CommandIssue issue{
            .tick = tick,
            .phase = *phase,
            .source = static_cast<CommandSource>(source),
            .id = static_cast<CommandId>(id),
            .player = static_cast<PlayerIndex>(player),
            .kind = *kind,
            .queued = queued == 1,
            .targetX = Fx::fromRaw(static_cast<FxRaw>(targetX)),
            .targetZ = Fx::fromRaw(static_cast<FxRaw>(targetZ)),
            .target = UnitId{static_cast<UnitIndex>(targetIndex),
                             static_cast<Generation>(targetGeneration)},
            .buildType = static_cast<UnitTypeIndex>(buildType),
            .count = static_cast<std::uint32_t>(count),
        };
        issue.units.reserve(static_cast<std::size_t>(unitCount));
        for (std::uint64_t unit = 0; unit < unitCount; ++unit) {
            std::uint64_t index = 0;
            std::uint64_t generation = 0;
            if (!(fields >> index >> generation) || !fits<UnitIndex>(index)
                || !fits<Generation>(generation)) {
                return std::nullopt;
            }
            issue.units.push_back(UnitId{static_cast<UnitIndex>(index),
                                         static_cast<Generation>(generation)});
        }
        std::string blueprint;
        std::string scriptTask;
        std::string scriptDataHex;
        std::string extra;
        if (!(fields >> std::quoted(blueprint) >> std::quoted(scriptTask)
              >> std::quoted(scriptDataHex))
            || fields >> extra || scriptTask.size() > kMaxScriptTaskNameBytes
            || scriptDataHex.size() > kMaxScriptTaskDataBytes * 2
            || scriptDataHex.size() % 2 != 0) {
            return std::nullopt;
        }
        issue.scriptTask = std::move(scriptTask);
        issue.scriptData.reserve(scriptDataHex.size() / 2);
        const auto nibble = [](char digit) -> std::optional<std::uint8_t> {
            if (digit >= '0' && digit <= '9') return static_cast<std::uint8_t>(digit - '0');
            if (digit >= 'a' && digit <= 'f') return static_cast<std::uint8_t>(digit - 'a' + 10);
            return std::nullopt;
        };
        for (std::size_t at = 0; at < scriptDataHex.size(); at += 2) {
            const std::optional<std::uint8_t> high = nibble(scriptDataHex[at]);
            const std::optional<std::uint8_t> low = nibble(scriptDataHex[at + 1]);
            if (!high || !low) return std::nullopt;
            issue.scriptData.push_back(static_cast<std::uint8_t>((*high << 4) | *low));
        }
        if (!log.record(std::move(issue))) return std::nullopt;
        paths.push_back(std::move(blueprint));
        ++parsedCount;
    }
    if (parsedCount != issueCount) {
        return std::nullopt;
    }
    while (std::getline(in, line)) {
        if (!line.empty() && line.front() != '#') {
            return std::nullopt;
        }
    }
    if (!in.eof()) {
        return std::nullopt;
    }
    if (buildPaths != nullptr) {
        *buildPaths = std::move(paths);
    }
    return log;
}

} // namespace rm::sim
