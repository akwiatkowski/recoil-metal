#include "core/sim/Command.hpp"

#include "core/sim/Combat.hpp"
#include "core/sim/Movement.hpp"
#include "core/sim/Reclaim.hpp"
#include "core/sim/UnitStore.hpp"

#include "core/unit/BuildTree.hpp"
#include "core/unit/Role.hpp"

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <limits>
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

void canonicalizeUnits(std::vector<UnitId>& units) {
    std::ranges::sort(units, [](UnitId a, UnitId b) {
        return a.index < b.index || (a.index == b.index && a.generation < b.generation);
    });
    units.erase(std::unique(units.begin(), units.end()), units.end());
}

} // namespace

std::optional<CommandId> CommandBuffer::submit(CommandIssue issue, UnitStore& store) {
    if (issue.source == kInvalidCommandSource
        || issue.player != static_cast<PlayerIndex>(issue.source) || issue.count == 0) {
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
    const unitdef::Role role = unitdef::roleOf(*assister);
    return role == unitdef::Role::Builder || role == unitdef::Role::Commander;
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
           && a.target == b.target && a.buildType == b.buildType && a.count == b.count;
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

namespace {

[[nodiscard]] const std::shared_ptr<SharedCommand>& ensureSharedCommand(
    const Command& command, CommandSource source, CommandId id, std::uint32_t count,
    UnitStore& store, std::shared_ptr<SharedCommand>& shared) {
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
            .targetX = command.targetX,
            .targetZ = command.targetZ,
            .target = command.target,
            .buildType = command.buildType,
            .creationSerial = store.allocateCommandSerial(),
            .originalCount = count,
            .remainingCount = count,
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
}

[[nodiscard]] bool applyCommandMember(
    const Command& command, CommandSource source, CommandId id, std::uint32_t count,
    std::shared_ptr<SharedCommand>& shared, UnitStore& store,
    const UnitCatalog& catalog, std::span<const Player> players, std::span<const Army> armies,
    const Terrain& terrain, const PassabilityGrid& grid, TickRate rate,
    std::vector<Construction>* building, EventQueue* events, const FeatureStore* features) {
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

    CommandQueue& orders = store.orders()[command.unit.index];

    // THE CAP, retail's, at retail's boundary (`kCommandQueueCap`). It is checked here rather
    // than inside `CommandQueue::give` because retail checks it in `Sim::IssueCommand` — on
    // the way in from a command source, before anything is appended — so it bounds what a
    // PLAYER can pile up and leaves the engine's own inserts (a patrol's synthetic origin)
    // alone. An order that clears the queue is exempt, so a unit at the cap is still
    // commandable.
    if (command.queued && command.kind != CommandKind::Stop && orders.atCapacity()) {
        return false;
    }

    // A STOP IS NOT A QUEUED ORDER HERE, shift or no shift. Recoil allows one — its comment at
    // `CommandAI.cpp:996` says as much, with an exclamation mark — and it needs to, because it
    // has a wait command that a queued stop interacts with. We have none, so a queued stop
    // would be an order to stand still at some future point in a route, which is what deleting
    // the rest of the route already means. It clears and stops.
    if (command.kind == CommandKind::Stop) {
        MoveState& motion = store.motion()[command.unit.index];
        const MoveState previous = motion;
        if (!startCommand(command, store, catalog, terrain, grid, rate, building, events,
                          features)) {
            motion = previous;
            return false;
        }
        MoveState stopped = std::move(motion);
        motion = previous;
        orders.clear();
        cancelActiveConstruction(building, command.unit);
        motion = std::move(stopped);
        (void)ensureSharedCommand(command, source, id, count, store, shared);
        return true;
    }

    if (command.queued) {
        const std::shared_ptr<const SharedCommand> payload =
            ensureSharedCommand(command, source, id, count, store, shared);
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
                        if (startCommand(next->asCommand(), store, catalog, terrain, grid, rate,
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
                if (startCommand(next->asCommand(), store, catalog, terrain, grid, rate, building,
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
    if (!startCommand(command, store, catalog, terrain, grid, rate, building, events,
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
        ensureSharedCommand(command, source, id, count, store, shared);
    QueuedCommand entry{command.unit, payload};
    if (command.kind == CommandKind::Patrol) {
        entry.setPatrolOrigin({store.transforms()[command.unit.index].x,
                               store.transforms()[command.unit.index].z});
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
                                const FeatureStore* features) {
    ApplyCommandResult result;
    if (issue.source == kInvalidCommandSource || issue.id == kInvalidCommandId
        || issue.player != static_cast<PlayerIndex>(issue.source)
        || commandSource(issue.id) != issue.source || issue.count == 0
        || playerFor(issue.player, players) == nullptr || store.commandIdLive(issue.id)) {
        return result;
    }

    std::vector<UnitId> canonical = issue.units;
    canonicalizeUnits(canonical);

    result.accepted.reserve(canonical.size());
    std::shared_ptr<SharedCommand> shared;
    for (const UnitId unit : canonical) {
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
        if (grid == nullptr) {
            continue;
        }
        const Command member{
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
        if (applyCommandMember(member, issue.source, issue.id, issue.count, shared, store,
                               catalog, players, armies,
                               terrain, *grid, rate, building, events, features)) {
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
                  const FeatureStore* features) {
    if (command.player >= static_cast<PlayerIndex>(kInvalidCommandSource)) {
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
        [&grid](UnitId) { return &grid; }, rate, building, events, features);
    return result.acceptedUnit(command.unit);
}

std::size_t advanceOrders(UnitStore& store, const UnitCatalog& catalog, const Terrain& terrain,
                          std::span<const PassabilityGrid* const> gridForType, TickRate rate,
                          std::vector<Construction>* building, EventQueue* events,
                          const FeatureStore* features, std::vector<Construction>* finished) {
    std::size_t started = 0;

    const std::span<CommandQueue> orders = store.orders();
    const std::span<const MoveState> motion = store.motion();

    for (UnitIndex slot = 0; slot < orders.size(); ++slot) {
        if (!store.slotAlive(slot) || orders[slot].empty()) {
            continue;
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
                const PassabilityGrid* pendingGrid = gridFor(*pending);
                if (pendingGrid == nullptr) {
                    return;  // leave it pending until its movement domain exists
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
        // Returns true when a head was retired, so the caller can look at the new one.
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
            (void)orders[slot].finish();
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
            continue;
        }

        // BEFORE the grid lookup, deliberately. A build in progress does not move, so asking
        // which passability grid it would route on is a question with no bearing on whether it
        // rises — and answering it first would stall every construction in a scene that has no
        // grid for the product's motion class. `startPending`, which does need one, looks it up
        // for itself.
        const QueuedCommand* current = orders[slot].active();
        if (current->kind() == CommandKind::Build) {
            serviceBuilds();
            continue;
        }

        const PassabilityGrid* grid = gridFor(*current);
        if (grid == nullptr) {
            continue;  // a missing domain leaves the command queued rather than dropping it
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
        startPending();
    }

    return started;
}

void updateAggressiveOrders(UnitStore& store, const UnitCatalog& catalog,
                            std::span<const Army> armies, const Terrain& terrain,
                            std::span<const PassabilityGrid* const> gridForType, TickRate rate,
                            const Intel* intel) {
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
                    nearestTarget(from, owner, weapon, store, armies, intel, &catalog);
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
        motion.moving = false;
        motion.path.clear();
        motion.pathIndex = 0;
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

        // The cost and the time come from the DEFINITION, and the rate from the clock — the
        // same derivation `UnitCatalog::Rates` does for income, at the one place a construction
        // is created.
        // Construction occupies the builder until completion, so an earlier route must not
        // keep moving the founder while it builds remotely.
        motion.moving = false;
        motion.path.clear();
        motion.pathIndex = 0;

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
    }

    return false;
}

} // namespace

// --- The log --------------------------------------------------------------------------

namespace {

inline constexpr std::string_view kCommandLogMagic = "recoil-metal semantic command log";
inline constexpr std::uint32_t kCommandLogVersion = 1;

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
           " targetIndex targetGeneration unitCount [unitIndex unitGeneration]... buildPath\n";
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
        out << ' ' << std::quoted(blueprint) << '\n';
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
        std::string extra;
        if (!(fields >> std::quoted(blueprint)) || fields >> extra || !log.record(std::move(issue))) {
            return std::nullopt;
        }
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
