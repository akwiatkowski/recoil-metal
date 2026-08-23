#include "core/sim/Command.hpp"

#include "core/sim/Combat.hpp"
#include "core/sim/Movement.hpp"
#include "core/sim/UnitStore.hpp"

#include "core/unit/BuildTree.hpp"

#include <algorithm>
#include <fstream>
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
                                std::vector<Construction>* building, EventQueue* events);

/// Whether an order is finished the moment it is started.
///
/// `Stop` and `Build` are: neither occupies the unit afterwards — a stop is instantaneous by
/// definition, and a construction is paid for by the economy rather than attended by the
/// builder. `Move` and `Attack` are not, and the unit's `moving` flag is what says when they
/// are done.
[[nodiscard]] bool instantaneous(CommandKind kind) noexcept {
    return kind == CommandKind::Stop || kind == CommandKind::Build;
}

[[nodiscard]] const char* kindName(CommandKind kind) noexcept {
    switch (kind) {
    case CommandKind::Move:
        return "move";
    case CommandKind::Stop:
        return "stop";
    case CommandKind::Attack:
        return "attack";
    case CommandKind::Build:
        return "build";
    }
    return "stop";
}

[[nodiscard]] std::optional<CommandKind> kindFromName(std::string_view name) noexcept {
    if (name == "move") {
        return CommandKind::Move;
    }
    if (name == "stop") {
        return CommandKind::Stop;
    }
    if (name == "attack") {
        return CommandKind::Attack;
    }
    if (name == "build") {
        return CommandKind::Build;
    }
    return std::nullopt;
}

} // namespace

bool operator==(const Command& a, const Command& b) noexcept {
    return a.tick == b.tick && a.player == b.player && a.kind == b.kind && a.unit == b.unit
           && a.targetX == b.targetX && a.targetZ == b.targetZ && a.target == b.target
           && a.buildType == b.buildType;
}

bool applyCommand(const Command& command, UnitStore& store, const UnitCatalog& catalog,
                  std::span<const Player> players, std::span<const Army> armies,
                  const Terrain& terrain, const PassabilityGrid& grid, TickRate rate,
                  std::vector<Construction>* building, bool queued, EventQueue* events) {
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

    CommandQueue& orders = store.orders()[command.unit.index];

    // A STOP IS NOT A QUEUED ORDER HERE, shift or no shift. Recoil allows one — its comment at
    // `CommandAI.cpp:996` says as much, with an exclamation mark — and it needs to, because it
    // has a wait command that a queued stop interacts with. We have none, so a queued stop
    // would be an order to stand still at some future point in a route, which is what deleting
    // the rest of the route already means. It clears and stops.
    if (command.kind == CommandKind::Stop) {
        orders.clear();
        return startCommand(command, store, catalog, terrain, grid, rate, building, events);
    }

    if (queued) {
        switch (orders.give(command, true)) {
        case CommandQueue::Result::CancelledCurrent: {
            // The order the unit was carrying out has been taken away, so it has to be
            // interrupted as well as forgotten. Recoil pushes a stop to the front and lets its
            // own slow update pick it up (`:1044-1049`); stopping here and starting whatever
            // is next reaches the same state one tick sooner.
            MoveState& motion = store.motion()[command.unit.index];
            motion.moving = false;
            motion.path.clear();
            motion.pathIndex = 0;
            if (const Command* next = orders.current()) {
                (void)startCommand(*next, store, catalog, terrain, grid, rate, building, events);
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

    // A PLAIN ORDER IS ROUTED BEFORE IT IS QUEUED, so that a refused one changes nothing at
    // all — not even clearing the queue. That is what keeps "a refused order is not part of the
    // match" true, and it is why this cannot simply be `give` followed by `startCommand`.
    if (!startCommand(command, store, catalog, terrain, grid, rate, building, events)) {
        return false;
    }
    (void)orders.give(command, false);
    // A `Build` is over the moment it is started, so leaving it at the head of the queue would
    // make the builder look busy for a tick. `advanceOrders` would clear it next tick anyway;
    // doing it here keeps "the head of the queue is what the unit is doing" true every tick.
    if (instantaneous(command.kind)) {
        orders.clear();
    }
    return true;
}

std::size_t advanceOrders(UnitStore& store, const UnitCatalog& catalog, const Terrain& terrain,
                          std::span<const PassabilityGrid* const> gridForType, TickRate rate,
                          std::vector<Construction>* building, EventQueue* events) {
    std::size_t started = 0;

    const std::span<CommandQueue> orders = store.orders();
    const std::span<const MoveState> motion = store.motion();

    for (UnitIndex slot = 0; slot < orders.size(); ++slot) {
        if (!store.slotAlive(slot) || orders[slot].empty()) {
            continue;
        }

        // This unit's OWN grid. A missing one leaves the queue where it is rather than routing
        // on a stranger's: a route is only as good as the map it was searched on, and an order
        // silently dropped is worse than one that waits.
        const auto type = static_cast<std::size_t>(store.typeAt(slot));
        const PassabilityGrid* grid = type < gridForType.size() ? gridForType[type] : nullptr;
        if (grid == nullptr) {
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
        // A unit with no firing weapon does not chase: for it an attack is the plain walk
        // it always was, and the finish-by-arrival logic below still owns it.
        if (const Command* head = orders[slot].current();
            head != nullptr && head->kind == CommandKind::Attack
            && store.alive(head->target)) {
            const unitdef::UnitDef* def = catalog.def(store.typeAt(slot));
            Fx reach{};
            if (def != nullptr) {
                for (const unitdef::Weapon& weapon : def->weapons) {
                    if (weapon.fires() && weapon.maxRange > reach) {
                        reach = weapon.maxRange;
                    }
                }
            }
            if (reach > Fx{}) {
                MoveState& chase = store.motion()[slot];
                const Transform& mine = store.transforms()[slot];
                const Transform& theirs = store.transforms()[head->target.index];
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
                        {head->targetX, Fx{}, head->targetZ}, {theirs.x, Fx{}, theirs.z});
                    if (strayed > Fx::fromRaw(reach.raw() / 2) || !chase.moving) {
                        const std::vector<std::array<Fx, 2>> path =
                            findPath(*grid, mine.x, mine.z, theirs.x, theirs.z);
                        if (Command* mutableHead = orders[slot].currentMutable()) {
                            // Recorded whether or not the route was found: a target in an
                            // unreachable spot must not be re-pathed every tick — the next
                            // attempt waits until it strays again.
                            mutableHead->targetX = theirs.x;
                            mutableHead->targetZ = theirs.z;
                        }
                        if (!path.empty()) {
                            orderAlongPath(chase, path);
                        }
                    }
                }
                continue;  // alive target: the order outlives every arrival
            }
        }

        if (slot < motion.size() && motion[slot].moving) {
            continue;  // still carrying out the order at the head
        }

        // The head is done. Drop it and start the next — and keep going while what comes next
        // is either instantaneous or unstartable, so a queue of build orders empties in one
        // tick and a dead waypoint does not stall the route behind it.
        const Command* next = orders[slot].finish();
        while (next != nullptr) {
            const bool wasInstant = instantaneous(next->kind);
            if (startCommand(*next, store, catalog, terrain, *grid, rate, building, events)) {
                ++started;
                if (!wasInstant) {
                    break;
                }
            }
            next = orders[slot].finish();
        }
    }

    return started;
}

namespace {

bool startCommand(const Command& command, UnitStore& store, const UnitCatalog& catalog,
                  const Terrain& terrain, const PassabilityGrid& grid, TickRate rate,
                  std::vector<Construction>* building, EventQueue* events) {
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

    case CommandKind::Move:
    case CommandKind::Attack: {
        // ROUTED, not aimed straight at the destination — which is the difference between a
        // unit walking round a lake and one walking into it. A route that cannot be found is
        // a refused order rather than a straight-line fallback: driving into the water is a
        // worse answer than not moving.
        const Transform& at = store.transforms()[command.unit.index];
        const std::vector<std::array<Fx, 2>> path =
            findPath(grid, at.x, at.z, command.targetX, command.targetZ);
        if (path.empty()) {
            return false;
        }
        orderAlongPath(motion, path);
        (void)terrain;  // the route is already on the map; the tick puts the unit on the ground
        return true;
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
        if (!unitdef::matchesExpression(builder->buildableCategory, *def)) {
            return false;
        }

        // AN UPGRADE, when the target is what the builder's blueprint says it becomes —
        // `General.UpgradesTo`, the tech path. Same command, two differences: the work
        // happens WHERE THE BUILDER STANDS whatever the order said (a factory does not
        // upgrade into a field), and completion replaces the builder instead of standing
        // a second building on top of it (the caller's spawn path reads `upgradeOf`).
        const bool upgrade = !builder->upgradesTo.empty() && builder->upgradesTo == def->name;
        const Transform& builderAt = store.transforms()[command.unit.index];
        const Fx siteX = upgrade ? builderAt.x : command.targetX;
        const Fx siteZ = upgrade ? builderAt.z : command.targetZ;

        // The cost and the time come from the DEFINITION, and the rate from the clock — the
        // same derivation `UnitCatalog::Rates` does for income, at the one place a construction
        // is created.
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
    }

    return false;
}

} // namespace

// --- The log --------------------------------------------------------------------------

void CommandLog::record(const Command& command) {
    // Non-decreasing, checked at record time. A log that went backwards would replay as a
    // different match, and the tick it went backwards on is the only useful thing to know
    // about it — which is knowable here and not at replay time.
    if (!commands_.empty() && command.tick < commands_.back().tick) {
        return;
    }
    commands_.push_back(command);
}

std::span<const Command> CommandLog::at(TickIndex tick) const noexcept {
    const auto begin =
        std::lower_bound(commands_.begin(), commands_.end(), tick,
                         [](const Command& c, TickIndex t) { return c.tick < t; });
    const auto end = std::upper_bound(begin, commands_.end(), tick,
                                      [](TickIndex t, const Command& c) { return t < c.tick; });
    return std::span<const Command>{&*begin, static_cast<std::size_t>(end - begin)};
}

TickIndex CommandLog::lastTick() const noexcept {
    return commands_.empty() ? TickIndex{0} : commands_.back().tick;
}

bool writeCommandLog(const CommandLog& log, const std::string& path,
                     const std::function<std::string(std::uint32_t)>& pathFor) {
    std::ofstream out{path, std::ios::binary | std::ios::trunc};
    if (!out) {
        return false;
    }

    out << "# recoil-metal command log\n";
    out << "# tick player kind unit generation targetX targetZ buildType"
           " targetUnit targetGeneration buildPath\n";
    for (const Command& command : log.all()) {
        out << command.tick << ' ' << command.player << ' ' << kindName(command.kind) << ' '
            << command.unit.index << ' ' << command.unit.generation << ' '
            << command.targetX.raw() << ' ' << command.targetZ.raw() << ' '
            << command.buildType << ' ' << command.target.index << ' '
            << command.target.generation;
        // The content-addressed column: what the type index MEANT in this run. '-' for
        // everything that is not a build, and for a caller with no resolver.
        std::string blueprint;
        if (command.kind == CommandKind::Build && pathFor != nullptr) {
            blueprint = pathFor(command.buildType);
        }
        out << ' ' << (blueprint.empty() ? "-" : blueprint.c_str()) << '\n';
    }
    return out.good();
}

std::optional<CommandLog> readCommandLog(const std::string& path,
                                         std::vector<std::string>* buildPaths) {
    std::ifstream in{path, std::ios::binary};
    if (!in) {
        return std::nullopt;
    }

    CommandLog log;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line.front() == '#') {
            continue;
        }

        std::istringstream fields{line};
        Command command;
        std::string kind;
        std::uint64_t tick = 0;
        unsigned player = 0;
        unsigned long index = 0;
        unsigned long generation = 0;
        long targetX = 0;
        long targetZ = 0;
        unsigned buildType = 0;

        if (!(fields >> tick >> player >> kind >> index >> generation >> targetX >> targetZ
              >> buildType)) {
            return std::nullopt;  // a partial log replays as a different match
        }
        const std::optional<CommandKind> parsed = kindFromName(kind);
        if (!parsed) {
            return std::nullopt;
        }

        // The attack target, appended to the format when chases arrived. OPTIONAL on read:
        // a log written before the columns existed holds only untargeted orders, and those
        // replay exactly as they did — an absent target IS the invalid handle.
        unsigned long targetIndex = 0;
        unsigned long targetGeneration = 0;
        (void)(fields >> targetIndex >> targetGeneration);

        // The blueprint column, optional the same way: '-' and absence both mean "none".
        std::string blueprint;
        (void)(fields >> blueprint);
        if (buildPaths != nullptr) {
            buildPaths->push_back(blueprint == "-" ? std::string{} : blueprint);
        }

        command.tick = tick;
        command.player = static_cast<PlayerIndex>(player);
        command.kind = *parsed;
        command.unit = UnitId{static_cast<UnitIndex>(index), static_cast<Generation>(generation)};
        command.targetX = Fx::fromRaw(static_cast<FxRaw>(targetX));
        command.targetZ = Fx::fromRaw(static_cast<FxRaw>(targetZ));
        command.target = UnitId{static_cast<UnitIndex>(targetIndex),
                                static_cast<Generation>(targetGeneration)};
        command.buildType = static_cast<UnitTypeIndex>(buildType);
        log.record(command);
    }

    return log;
}

} // namespace rm::sim
