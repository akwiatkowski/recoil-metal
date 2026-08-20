#include "core/sim/Command.hpp"

#include "core/sim/Movement.hpp"
#include "core/sim/UnitStore.hpp"

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
           && a.targetX == b.targetX && a.targetZ == b.targetZ && a.buildType == b.buildType;
}

bool applyCommand(const Command& command, UnitStore& store, const UnitCatalog& catalog,
                  std::span<const Player> players, std::span<const Army> armies,
                  const Terrain& terrain, const PassabilityGrid& grid, TickRate rate,
                  std::vector<Construction>* building) {
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

        // The cost and the time come from the DEFINITION, and the rate from the clock — the
        // same derivation `UnitCatalog::Rates` does for income, at the one place a construction
        // is created.
        building->push_back(Construction{
            .armyIndex = store.motion()[command.unit.index].armyIndex,
            .position = {fxToFloat(command.targetX), 0.0f, fxToFloat(command.targetZ)},
            .cost = {.mass = def->buildCostMass, .energy = def->buildCostEnergy},
            .buildTimeRemaining = def->buildTime,
            .totalBuildTime = def->buildTime,
            .buildPerTick = rate.magPerTick(builder->buildRate),
            .blueprintIndex = command.buildType,
        });
        return true;
    }
    }

    return false;
}

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

bool writeCommandLog(const CommandLog& log, const std::string& path) {
    std::ofstream out{path, std::ios::binary | std::ios::trunc};
    if (!out) {
        return false;
    }

    out << "# recoil-metal command log\n";
    out << "# tick player kind unit generation targetX targetZ buildType\n";
    for (const Command& command : log.all()) {
        out << command.tick << ' ' << command.player << ' ' << kindName(command.kind) << ' '
            << command.unit.index << ' ' << command.unit.generation << ' '
            << command.targetX.raw() << ' ' << command.targetZ.raw() << ' '
            << command.buildType << '\n';
    }
    return out.good();
}

std::optional<CommandLog> readCommandLog(const std::string& path) {
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

        command.tick = tick;
        command.player = static_cast<PlayerIndex>(player);
        command.kind = *parsed;
        command.unit = UnitId{static_cast<UnitIndex>(index), static_cast<Generation>(generation)};
        command.targetX = Fx::fromRaw(static_cast<FxRaw>(targetX));
        command.targetZ = Fx::fromRaw(static_cast<FxRaw>(targetZ));
        command.buildType = static_cast<UnitTypeIndex>(buildType);
        log.record(command);
    }

    return log;
}

} // namespace rm::sim
