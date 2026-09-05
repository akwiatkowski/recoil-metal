#include "core/sim/SaveState.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <limits>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

namespace rm::sim {
namespace {

// A fixed tag lets the decoder reject arbitrary data before interpreting its fields.
constexpr std::array<std::byte, 4> kMagic{
    std::byte{static_cast<unsigned char>('R')}, std::byte{static_cast<unsigned char>('M')},
    std::byte{static_cast<unsigned char>('S')}, std::byte{static_cast<unsigned char>('V')}};
constexpr std::uint32_t kVersion1 = 1;
constexpr std::uint32_t kVersion2 = 2;
constexpr std::uint32_t kVersion3 = 3;
constexpr std::uint32_t kVersion4 = 4;
constexpr std::uint32_t kVersion5 = 5;
constexpr std::uint32_t kVersion6 = 6;
constexpr std::uint32_t kVersion7 = 7;
constexpr std::uint32_t kVersion8 = 8;
constexpr std::uint32_t kVersion9 = 9;
constexpr std::uint32_t kVersion10 = 10;
constexpr std::uint32_t kVersion11 = 11;
constexpr std::uint32_t kVersion12 = 12;
constexpr std::uint32_t kVersion13 = 13;
constexpr std::uint32_t kVersion14 = 14;
constexpr std::uint32_t kVersion15 = 15;
constexpr std::uint32_t kVersion16 = 16;
// MT19937 serializes its 624 state words plus an index, all as unsigned decimal numbers separated
// by one space. This rejects oversized malformed frames before they allocate their payload.
constexpr std::size_t kMaxRandomStatePayload =
    (RandomStream::Snapshot::state_size + 1)
        * (std::numeric_limits<std::uint32_t>::digits10 + 1)
    + RandomStream::Snapshot::state_size;

class PayloadWriter {
public:
    void u8(std::uint8_t value) { bytes_.push_back(std::byte{value}); }
    void u16(std::uint16_t value) {
        u8(static_cast<std::uint8_t>(value));
        u8(static_cast<std::uint8_t>(value >> 8));
    }
    void u32(std::uint32_t value) {
        for (unsigned shift = 0; shift < 32; shift += 8) {
            u8(static_cast<std::uint8_t>(value >> shift));
        }
    }
    void u64(std::uint64_t value) {
        for (unsigned shift = 0; shift < 64; shift += 8) {
            u8(static_cast<std::uint8_t>(value >> shift));
        }
    }
    void i32(std::int32_t value) { u32(std::bit_cast<std::uint32_t>(value)); }
    void i64(std::int64_t value) { u64(std::bit_cast<std::uint64_t>(value)); }
    void count(std::size_t value) {
        if (value > std::numeric_limits<std::uint32_t>::max()) {
            throw std::length_error("save-state collection exceeds v1's 32-bit limit");
        }
        u32(static_cast<std::uint32_t>(value));
    }
    void text(const std::string& value) {
        count(value.size());
        for (const char character : value) {
            u8(static_cast<std::uint8_t>(character));
        }
    }
    void bytes(std::span<const std::uint8_t> value) {
        count(value.size());
        for (const std::uint8_t byte : value) u8(byte);
    }
    [[nodiscard]] std::vector<std::byte> take() { return std::move(bytes_); }

private:
    std::vector<std::byte> bytes_;
};

class PayloadReader {
public:
    explicit PayloadReader(std::span<const std::byte> bytes) : bytes_(bytes) {}
    [[nodiscard]] bool u8(std::uint8_t& value) {
        if (offset_ == bytes_.size()) return false;
        value = std::to_integer<std::uint8_t>(bytes_[offset_++]);
        return true;
    }
    [[nodiscard]] bool u16(std::uint16_t& value) {
        std::uint8_t lo{}, hi{};
        if (!u8(lo) || !u8(hi)) return false;
        value = static_cast<std::uint16_t>(lo | static_cast<std::uint16_t>(hi) << 8);
        return true;
    }
    [[nodiscard]] bool u32(std::uint32_t& value) {
        value = 0;
        for (unsigned shift = 0; shift < 32; shift += 8) { std::uint8_t byte{}; if (!u8(byte)) return false; value |= static_cast<std::uint32_t>(byte) << shift; }
        return true;
    }
    [[nodiscard]] bool u64(std::uint64_t& value) {
        value = 0;
        for (unsigned shift = 0; shift < 64; shift += 8) { std::uint8_t byte{}; if (!u8(byte)) return false; value |= static_cast<std::uint64_t>(byte) << shift; }
        return true;
    }
    [[nodiscard]] bool i32(std::int32_t& value) { std::uint32_t raw{}; return u32(raw) && (value = std::bit_cast<std::int32_t>(raw), true); }
    [[nodiscard]] bool i64(std::int64_t& value) { std::uint64_t raw{}; return u64(raw) && (value = std::bit_cast<std::int64_t>(raw), true); }
    [[nodiscard]] bool count(std::size_t& value, std::size_t minimumBytes = 0) {
        std::uint32_t raw{};
        if (!u32(raw)) return false;
        if constexpr (sizeof(std::size_t) < sizeof(raw)) {
            if (raw > std::numeric_limits<std::size_t>::max()) return false;
        }
        value = raw;
        return minimumBytes == 0 || value <= (bytes_.size() - offset_) / minimumBytes;
    }
    [[nodiscard]] bool text(std::string& value) {
        std::size_t size{};
        if (!count(size) || size > bytes_.size() - offset_) return false;
        value.clear(); value.reserve(size);
        for (std::size_t i = 0; i < size; ++i) value.push_back(static_cast<char>(std::to_integer<unsigned char>(bytes_[offset_++])));
        return true;
    }
    [[nodiscard]] bool bytes(std::vector<std::uint8_t>& value, std::size_t maximum) {
        std::size_t size{};
        if (!count(size) || size > maximum || size > bytes_.size() - offset_) return false;
        value.resize(size);
        for (std::uint8_t& byte : value) if (!u8(byte)) return false;
        return true;
    }
    [[nodiscard]] bool finished() const { return offset_ == bytes_.size(); }
private:
    std::span<const std::byte> bytes_;
    std::size_t offset_ = 0;
};

void writeId(PayloadWriter& writer, UnitId id) { writer.u32(id.index); writer.u32(id.generation); }
[[nodiscard]] bool readId(PayloadReader& reader, UnitId& id) { return reader.u32(id.index) && reader.u32(id.generation); }

void writeResources(PayloadWriter& w, Resources value) {
    w.i64(value.mass.raw()); w.i64(value.energy.raw());
}
bool readResources(PayloadReader& r, Resources& value) {
    std::int64_t mass{}, energy{};
    if (!r.i64(mass) || !r.i64(energy)) return false;
    value = {Mag::fromRaw(mass), Mag::fromRaw(energy)};
    return true;
}
bool readFlag(PayloadReader& r, bool& value) {
    std::uint8_t raw{};
    if (!r.u8(raw) || raw > 1) return false;
    value = raw != 0;
    return true;
}
bool readFx(PayloadReader& r, Fx& value) {
    std::int32_t raw{};
    if (!r.i32(raw)) return false;
    value = Fx::fromRaw(raw);
    return true;
}
bool readMag(PayloadReader& r, Mag& value) {
    std::int64_t raw{};
    if (!r.i64(raw)) return false;
    value = Mag::fromRaw(raw);
    return true;
}

void writeEconomyArmies(PayloadWriter& w, const std::optional<EconomyArmyState>& state) {
    w.u8(state.has_value());
    if (!state) return;
    const auto& s = *state;
    w.count(s.armies.size());
    for (const auto& army : s.armies) {
        w.i32(army.index); w.u8(static_cast<std::uint8_t>(army.faction));
        w.i32(army.alliance); w.u8(army.defeated);
    }
    w.count(s.economies.size());
    for (const auto& economy : s.economies) {
        for (const auto value : {economy.stored, economy.storage, economy.incomePerTick,
                economy.upkeepPerTick, economy.requestedLastTick, economy.usageLastTick,
                economy.sharedIn, economy.upkeepAllocated}) writeResources(w, value);
        w.i32(economy.fundedFraction.raw()); w.i32(economy.multiResourceFunded.raw());
        w.i32(economy.singleResourceFunded.raw());
        w.u8(economy.massIsBinding); w.u8(economy.sharesOverflow);
    }
    w.count(s.building.size());
    for (const auto& work : s.building) {
        if (work.workedThisTick) throw std::invalid_argument("save construction after the economy tick settles");
        w.i32(work.armyIndex);
        for (const auto position : work.position) w.i32(position.raw());
        writeResources(w, work.cost);
        w.i64(work.buildTimeRemaining.raw()); w.i64(work.totalBuildTime.raw());
        w.i64(work.buildPerTick.raw()); w.u64(work.blueprintIndex);
        writeId(w, work.upgradeOf); writeId(w, work.builder); w.u32(work.retainedCommandId);
        w.i64(work.assistPerTick.raw()); writeResources(w, work.allocated);
        w.i32(work.fundedLastTick.raw()); w.u8(work.advancedLastTick);
    }
    w.count(s.commandersEver.size());
    for (const int value : s.commandersEver) w.i32(value);
    w.u8(static_cast<std::uint8_t>(s.victoryMode)); writeResources(w, s.baseStorage);
    w.u8(s.over); w.u8(s.winnerPending); w.u8(s.pendingWinner.has_value());
    if (s.pendingWinner) w.i32(*s.pendingWinner);
    w.u32(s.winnerStableTicks); w.u32(s.defeatPollElapsedTicks);
    w.count(s.defeatCleanupRemainingTicks.size());
    for (const auto ticks : s.defeatCleanupRemainingTicks) w.u32(ticks);
}

bool readEconomyArmies(PayloadReader& r, std::optional<EconomyArmyState>& state) {
    bool present{};
    if (!readFlag(r, present)) return false;
    if (!present) return true;
    auto& s = state.emplace();
    std::size_t count{};
    if (!r.count(count, 10)) return false;
    s.armies.resize(count);
    for (auto& army : s.armies) {
        std::uint8_t faction{};
        if (!r.i32(army.index) || !r.u8(faction) || faction > static_cast<int>(Faction::Seraphim)
            || !r.i32(army.alliance) || !readFlag(r, army.defeated)) return false;
        army.faction = static_cast<Faction>(faction);
        if (army.index != &army - s.armies.data()) return false;
    }
    if (!r.count(count, 142) || count != s.armies.size()) return false;
    s.economies.resize(count);
    for (auto& economy : s.economies) {
        for (auto* value : {&economy.stored, &economy.storage, &economy.incomePerTick,
                &economy.upkeepPerTick, &economy.requestedLastTick, &economy.usageLastTick,
                &economy.sharedIn, &economy.upkeepAllocated}) if (!readResources(r, *value)) return false;
        if (!readFx(r, economy.fundedFraction) || !readFx(r, economy.multiResourceFunded)
            || !readFx(r, economy.singleResourceFunded) || !readFlag(r, economy.massIsBinding)
            || !readFlag(r, economy.sharesOverflow)) return false;
    }
    if (!r.count(count, 113)) return false;
    s.building.resize(count);
    for (auto& work : s.building) {
        std::uint64_t type{};
        if (!r.i32(work.armyIndex)) return false;
        for (auto& position : work.position) if (!readFx(r, position)) return false;
        if (!readResources(r, work.cost) || !readMag(r, work.buildTimeRemaining)
            || !readMag(r, work.totalBuildTime) || !readMag(r, work.buildPerTick) || !r.u64(type)
            || type > std::numeric_limits<std::size_t>::max()
            || !readId(r, work.upgradeOf) || !readId(r, work.builder) || !r.u32(work.retainedCommandId)
            || !readMag(r, work.assistPerTick) || !readResources(r, work.allocated)
            || !readFx(r, work.fundedLastTick) || !readFlag(r, work.advancedLastTick)) return false;
        work.blueprintIndex = static_cast<std::size_t>(type);
        if (work.armyIndex < 0 || static_cast<std::size_t>(work.armyIndex) >= s.armies.size()) return false;
    }
    if (!r.count(count, 4) || count != s.armies.size()) return false;
    s.commandersEver.resize(count);
    for (int& value : s.commandersEver) if (!r.i32(value) || value < 0) return false;
    std::uint8_t mode{};
    bool winner{};
    if (!r.u8(mode) || mode > static_cast<int>(VictoryMode::Supremacy)
        || !readResources(r, s.baseStorage) || !readFlag(r, s.over)
        || !readFlag(r, s.winnerPending) || !readFlag(r, winner)) return false;
    s.victoryMode = static_cast<VictoryMode>(mode);
    if (winner) {
        int alliance{};
        if (!r.i32(alliance)) return false;
        s.pendingWinner = alliance;
    }
    if (!r.u32(s.winnerStableTicks) || !r.u32(s.defeatPollElapsedTicks)
        || !r.count(count, 4) || (count != 0 && count != s.armies.size())) return false;
    s.defeatCleanupRemainingTicks.resize(count);
    for (auto& ticks : s.defeatCleanupRemainingTicks) if (!r.u32(ticks)) return false;
    return true;
}

void writeSharedCommand(PayloadWriter& w, const SharedCommand& command,
                        bool includesScriptTasks) {
    if (includesScriptTasks
        && (command.scriptTask.size() > kMaxScriptTaskNameBytes
            || command.scriptData.size() > kMaxScriptTaskDataBytes)) {
        throw std::length_error("script command exceeds the v12 save-state limit");
    }
    w.u64(command.tick);
    w.u8(command.source);
    w.u32(command.id);
    w.u32(command.player);
    w.u8(static_cast<std::uint8_t>(command.kind));
    w.u8(command.queued);
    w.count(command.units.size());
    for (UnitId unit : command.units) writeId(w, unit);
    w.i32(command.targetX.raw());
    w.i32(command.targetZ.raw());
    writeId(w, command.target);
    w.u16(command.buildType);
    w.u32(command.creationSerial);
    w.u32(command.originalCount);
    w.u32(command.remainingCount);
    if (includesScriptTasks) {
        w.text(command.scriptTask);
        w.bytes(command.scriptData);
    }
}

[[nodiscard]] bool readSharedCommand(PayloadReader& r, SharedCommand& command,
                                     bool includesScriptTasks, bool includesGuard) {
    std::uint8_t source{}, kind{}, queued{};
    std::uint32_t player{};
    std::size_t units{};
    std::int32_t targetX{}, targetZ{};
    if (!r.u64(command.tick) || !r.u8(source) || !r.u32(command.id) || !r.u32(player)
        || !r.u8(kind) || !r.u8(queued) || source > kInvalidCommandSource
        || player > std::numeric_limits<PlayerIndex>::max()
        || kind > static_cast<std::uint8_t>(includesGuard ? CommandKind::Guard
                                          : includesScriptTasks ? CommandKind::Script
                                                                : CommandKind::Repair)
        || queued > 1
        || !r.count(units, 8)) {
        return false;
    }
    command.source = source;
    command.player = static_cast<PlayerIndex>(player);
    command.kind = static_cast<CommandKind>(kind);
    command.queued = queued;
    command.units.resize(units);
    for (UnitId& unit : command.units) if (!readId(r, unit)) return false;
    if (!r.i32(targetX) || !r.i32(targetZ) || !readId(r, command.target)
        || !r.u16(command.buildType) || !r.u32(command.creationSerial)
        || !r.u32(command.originalCount) || !r.u32(command.remainingCount)) {
        return false;
    }
    command.targetX = Fx::fromRaw(targetX);
    command.targetZ = Fx::fromRaw(targetZ);
    if (includesScriptTasks
        && (!r.text(command.scriptTask)
            || command.scriptTask.size() > kMaxScriptTaskNameBytes
            || !r.bytes(command.scriptData, kMaxScriptTaskDataBytes))) {
        return false;
    }
    if (includesScriptTasks
        && ((command.kind == CommandKind::Script && command.scriptTask.empty())
            || (command.kind != CommandKind::Script
                && (!command.scriptTask.empty() || !command.scriptData.empty())))) {
        return false;
    }
    return true;
}

void writeCommandState(PayloadWriter& w, const UnitStore::Snapshot& s,
                       bool includesScriptTasks) {
    w.u32(s.nextCommandSerial);
    for (const std::uint32_t counter : s.nextCommandCounters) w.u32(counter);
    w.count(s.sharedCommands.size());
    for (const SharedCommand& command : s.sharedCommands) {
        writeSharedCommand(w, command, includesScriptTasks);
    }
    w.count(s.orders.size());
    for (const CommandQueue::Snapshot& queue : s.orders) {
        w.u8(queue.activeSerial.has_value());
        if (queue.activeSerial) w.u32(*queue.activeSerial);
        w.count(queue.entries.size());
        for (const CommandQueue::SnapshotEntry& entry : queue.entries) {
            w.count(entry.sharedCommand);
            const QueuedCommand::Snapshot& execution = entry.execution;
            writeId(w, execution.unit);
            w.i32(execution.targetX.raw());
            w.i32(execution.targetZ.raw());
            writeId(w, execution.target);
            w.u8(execution.patrolOrigin.has_value());
            if (execution.patrolOrigin) {
                w.i32((*execution.patrolOrigin)[0].raw());
                w.i32((*execution.patrolOrigin)[1].raw());
            }
            w.u8(execution.returningToPatrolOrigin);
            if (includesScriptTasks) {
                if (execution.scriptState.opaque.size() > kMaxScriptTaskDataBytes) {
                    throw std::length_error("script task state exceeds the v12 save-state limit");
                }
                w.u8(execution.scriptState.created);
                w.u8(execution.scriptState.suspended);
                w.u32(execution.scriptState.sleepBeats);
                w.i32(execution.scriptState.aiResult);
                w.bytes(execution.scriptState.opaque);
            }
        }
    }
}

void writeSiloAmmo(PayloadWriter& w, std::span<const SiloAmmo> ammo) {
    w.count(ammo.size());
    for (const SiloAmmo& value : ammo) {
        writeId(w, value.owner);
        w.count(value.weapon);
        w.u8(value.slot);
        w.i32(value.stored);
        w.i32(value.capacity);
        w.u64(value.totalTicks);
        w.u64(value.elapsedTicks);
        w.i64(value.costPerTick.mass.raw()); w.i64(value.costPerTick.energy.raw());
        w.i64(value.delivered.mass.raw()); w.i64(value.delivered.energy.raw());
    }
}

void writeRedirects(PayloadWriter& w, std::span<const MissileRedirect> redirects) {
    w.count(redirects.size());
    for (const MissileRedirect& value : redirects) {
        writeId(w, value.owner);
        w.i32(value.radiusElmos.raw());
        w.i32(value.cooldownTicks);
        w.i32(value.remaining);
    }
}

// V16 completes the aircraft controller snapshot, including spawn-cached tuning:
// UnitStore::restore has no catalog from which to reconstruct it.
constexpr std::array kAirControllerFx{
    &MoveState::airYawVelocity,
    &MoveState::airTurnSpeed,
    &MoveState::airCombatTurnSpeed,
    &MoveState::airKTurn,
    &MoveState::airKTurnDamping,
    &MoveState::airTightTurnMultiplier,
    &MoveState::airBreakOffTrigger,
    &MoveState::airBreakOffDistance,
    &MoveState::airRandomBreakOffMultiplier,
    &MoveState::airKMove,
    &MoveState::airKMoveDamping,
    &MoveState::airKLift,
    &MoveState::airKLiftDamping,
    &MoveState::airLiftFactor,
    &MoveState::airMinSpeedElmosPerSec,
    &MoveState::airAttackElevation,
    &MoveState::airElevation,
    &MoveState::airElevationAdjustment,
    &MoveState::fuelDrainPerTick
};

void writeAirController(PayloadWriter& w, std::span<const MoveState> motion) {
    w.count(motion.size());
    for (const auto& state : motion) {
        w.u64(state.airCombatDeadline);
        w.u32(state.airSustainedTicks);
        w.u32(state.airSustainedThreshold);
        w.u32(state.airMinChangeTicks);
        w.u32(state.airMaxChangeTicks);
        w.u32(state.idleLandThreshold);
        w.u8(state.airWinged);
        w.u8(state.airTransportation);
        w.u8(state.airBreakOffNearTarget);
        for (auto field : kAirControllerFx) w.i32((state.*field).raw());
    }
}

bool readAirController(PayloadReader& r, std::vector<MoveState>& motion) {
    std::size_t count{};
    constexpr std::size_t kRecordBytes = 8 + 5 * 4 + 3 + kAirControllerFx.size() * 4;
    if (!r.count(count, kRecordBytes) || count != motion.size()) return false;
    for (auto& state : motion) {
        std::uint8_t winged{}, transport{}, near{};
        if (!r.u64(state.airCombatDeadline) || !r.u32(state.airSustainedTicks)
            || !r.u32(state.airSustainedThreshold) || !r.u32(state.airMinChangeTicks)
            || !r.u32(state.airMaxChangeTicks) || !r.u32(state.idleLandThreshold)
            || !r.u8(winged) || winged > 1 || !r.u8(transport) || transport > 1
            || !r.u8(near) || near > 1) return false;
        state.airWinged = winged != 0;
        state.airTransportation = transport != 0;
        state.airBreakOffNearTarget = near != 0;
        for (auto field : kAirControllerFx) {
            std::int32_t value{};
            if (!r.i32(value)) return false;
            state.*field = Fx::fromRaw(value);
        }
    }
    return true;
}

void writeAirMotion(PayloadWriter& w, std::span<const MoveState> motion, bool combat, bool hover) {
    w.count(motion.size());
    for (const MoveState& state : motion) {
        for (const Fx& axis : state.velocity) {
            w.i32(axis.raw());
        }
        w.u8(static_cast<std::uint8_t>(state.airState));
        w.i32(state.altitudeRef.raw());
        w.i32(state.fuelRatio.raw());
        w.u64(state.idleTicks);
        w.u8(state.canFly ? 1 : 0);
        w.i32(state.airMaxSpeedElmosPerSec.raw());
        if (combat) w.u8(static_cast<std::uint8_t>(state.airCombatState));
        if (hover) {
            w.u8(state.hovering);
            w.i32(state.hoverElevation.raw());
        }
    }
}

[[nodiscard]] bool readAirMotion(PayloadReader& r, std::vector<MoveState>& motion, bool combat,
                                  bool hover, bool fullCombat) {
    std::size_t count{};
    if (!r.count(count, (combat ? 35 : 34) + (hover ? 5 : 0)) || count != motion.size()) return false;
    for (MoveState& state : motion) {
        std::int32_t vx{}, vy{}, vz{}, ref{}, fuel{}, cruise{};
        std::uint64_t idle{};
        std::uint8_t air{}, fly{};
        if (!r.i32(vx) || !r.i32(vy) || !r.i32(vz) || !r.u8(air) || air > 3
            || !r.i32(ref) || !r.i32(fuel) || !r.u64(idle)
            || idle > std::numeric_limits<std::uint32_t>::max() || !r.u8(fly) || fly > 1
            || !r.i32(cruise)) {
            return false;
        }
        state.velocity = {Fx::fromRaw(vx), Fx::fromRaw(vy), Fx::fromRaw(vz)};
        state.airState = static_cast<MoveState::AirState>(air);
        state.altitudeRef = Fx::fromRaw(ref);
        state.fuelRatio = Fx::fromRaw(fuel);
        state.idleTicks = static_cast<std::uint32_t>(idle);
        state.canFly = fly != 0;
        state.airMaxSpeedElmosPerSec = Fx::fromRaw(cruise);
        if (combat) {
            std::uint8_t airCombat{};
            if (!r.u8(airCombat) || airCombat > (fullCombat ? 7 : 2)) return false;
            state.airCombatState = static_cast<MoveState::AirCombatState>(airCombat);
        }
        if (hover) {
            std::uint8_t hovering{};
            std::int32_t elevation{};
            if (!r.u8(hovering) || hovering > 1 || !r.i32(elevation)) return false;
            state.hovering = hovering != 0;
            state.hoverElevation = Fx::fromRaw(elevation);
        }
    }
    return true;
}

[[nodiscard]] bool readRedirects(PayloadReader& r, std::vector<MissileRedirect>& redirects) {
    std::size_t count{};
    if (!r.count(count, 20)) return false;
    redirects.resize(count);
    for (MissileRedirect& value : redirects) {
        std::int32_t radius{}, cooldown{}, remaining{};
        if (!readId(r, value.owner) || !r.i32(radius) || !r.i32(cooldown) || !r.i32(remaining)
            || radius < 0 || cooldown < 0 || remaining < 0 || remaining > cooldown) {
            return false;
        }
        value.radiusElmos = Fx::fromRaw(radius);
        value.cooldownTicks = cooldown;
        value.remaining = remaining;
    }
    return true;
}

[[nodiscard]] bool readSiloAmmo(PayloadReader& r, std::vector<SiloAmmo>& ammo) {
    std::size_t count{};
    if (!r.count(count, 56)) return false;
    ammo.resize(count);
    for (SiloAmmo& value : ammo) {
        std::int64_t mass{}, energy{}, deliveredMass{}, deliveredEnergy{};
        std::uint64_t total{}, elapsed{};
        if (!readId(r, value.owner) || !r.count(value.weapon) || !r.u8(value.slot) || value.slot > 1
            || !r.i32(value.stored)
            || !r.i32(value.capacity) || !r.u64(total) || !r.u64(elapsed)
            || !r.i64(mass) || !r.i64(energy) || !r.i64(deliveredMass) || !r.i64(deliveredEnergy)
            || value.stored < 0 || value.capacity < 0 || value.stored > value.capacity
            || total > std::numeric_limits<TickCount>::max()
            || elapsed > std::numeric_limits<TickCount>::max()) return false;
        value.totalTicks = static_cast<TickCount>(total);
        value.elapsedTicks = static_cast<TickCount>(elapsed);
        value.costPerTick = {.mass = Mag::fromRaw(mass), .energy = Mag::fromRaw(energy)};
        value.delivered = {.mass = Mag::fromRaw(deliveredMass), .energy = Mag::fromRaw(deliveredEnergy)};
    }
    return true;
}

[[nodiscard]] bool readCommandState(PayloadReader& r, UnitStore::Snapshot& s,
                                    bool includesScriptTasks, bool includesGuard) {
    if (!r.u32(s.nextCommandSerial)) return false;
    for (std::uint32_t& counter : s.nextCommandCounters) if (!r.u32(counter)) return false;
    std::size_t count{};
    if (!r.count(count, 1)) return false;
    s.sharedCommands.resize(count);
    for (SharedCommand& command : s.sharedCommands) {
        if (!readSharedCommand(r, command, includesScriptTasks, includesGuard)) return false;
    }
    if (!r.count(count, 1)) return false;
    s.orders.resize(count);
    for (CommandQueue::Snapshot& queue : s.orders) {
        std::uint8_t active{};
        if (!r.u8(active) || active > 1) return false;
        if (active) {
            CommandSerial serial{};
            if (!r.u32(serial)) return false;
            queue.activeSerial = serial;
        }
        if (!r.count(count, 1)) return false;
        queue.entries.resize(count);
        for (CommandQueue::SnapshotEntry& entry : queue.entries) {
            std::size_t sharedCommand{};
            std::int32_t targetX{}, targetZ{};
            std::uint8_t hasPatrolOrigin{}, returning{};
            if (!r.count(sharedCommand) || !readId(r, entry.execution.unit)
                || !r.i32(targetX) || !r.i32(targetZ) || !readId(r, entry.execution.target)
                || !r.u8(hasPatrolOrigin) || hasPatrolOrigin > 1) {
                return false;
            }
            entry.sharedCommand = sharedCommand;
            entry.execution.targetX = Fx::fromRaw(targetX);
            entry.execution.targetZ = Fx::fromRaw(targetZ);
            if (hasPatrolOrigin) {
                std::int32_t originX{}, originZ{};
                if (!r.i32(originX) || !r.i32(originZ)) return false;
                entry.execution.patrolOrigin = {Fx::fromRaw(originX), Fx::fromRaw(originZ)};
            }
            if (!r.u8(returning) || returning > 1) return false;
            entry.execution.returningToPatrolOrigin = returning;
            if (includesScriptTasks) {
                std::uint8_t created{}, suspended{};
                if (!r.u8(created) || created > 1 || !r.u8(suspended) || suspended > 1
                    || !r.u32(entry.execution.scriptState.sleepBeats)
                    || !r.i32(entry.execution.scriptState.aiResult)
                    || !r.bytes(entry.execution.scriptState.opaque,
                                kMaxScriptTaskDataBytes)) {
                    return false;
                }
                entry.execution.scriptState.created = created != 0;
                entry.execution.scriptState.suspended = suspended != 0;
            }
        }
    }
    return true;
}

void writeUnits(PayloadWriter& w, const UnitStore::Snapshot& s, bool includesPathPhase,
                   bool includesFactoryRepeat, bool includesAttachmentOffsets, bool includesDoNotTarget,
                   bool includesAutomaticTargets, bool includesAttachmentHeights,
                   bool includesAttachedMotion, bool includesCommands,
                   bool includesScriptTasks) {
    w.count(s.ids.generations.size()); for (Generation v : s.ids.generations) w.u32(v);
    w.count(s.ids.free.size()); for (UnitIndex v : s.ids.free) w.u32(v);
    w.u64(s.ids.live);
    w.count(s.generations.size()); for (Generation v : s.generations) w.u32(v);
    w.count(s.transforms.size()); for (const Transform& v : s.transforms) { w.i32(v.x.raw()); w.i32(v.y.raw()); w.i32(v.z.raw()); w.u16(v.heading); w.u16(v.pitch); w.u16(v.roll); }
    w.count(s.motion.size()); for (const MoveState& v : s.motion) { w.i32(v.armyIndex); w.i32(v.destinationX.raw()); w.i32(v.destinationZ.raw()); w.u8(v.moving); if (includesAttachedMotion) w.u8(v.attached); w.u8(v.airborne); w.u8(v.surfaceWater); w.i32(v.speedPerTick.raw()); w.i32(v.turnPerTick); w.i32(v.radiusElmos.raw()); w.i32(v.distanceTravelledElmos.raw()); w.count(v.path.size()); for (const auto& p : v.path) { w.i32(p[0].raw()); w.i32(p[1].raw()); } w.u64(v.pathIndex); if (includesPathPhase) { w.i32(v.pathPhaseStartX); w.i32(v.pathPhaseStartZ); w.i32(v.pathPhaseCellsX); } }
    w.count(s.health.size()); for (const Health& v : s.health) { w.i64(v.current.raw()); w.i64(v.maximum.raw()); w.i64(v.shield.current.raw()); w.i64(v.shield.maximum.raw()); w.u32(v.shield.regenDelayRemaining); w.u32(v.shield.rechargeRemaining); w.count(v.reloadRemaining.size()); for (int x : v.reloadRemaining) w.i32(x); w.count(v.burstRemaining.size()); for (int x : v.burstRemaining) w.i32(x); if (includesAutomaticTargets) { w.count(v.automaticTargets.size()); for (UnitId target : v.automaticTargets) writeId(w, target); } writeId(w, v.lastHitBy); w.i32(v.veterancy.kills); w.i32(v.veterancy.level); }
    w.count(s.types.size()); for (UnitTypeIndex v : s.types) w.u16(v);
    if (includesFactoryRepeat) { w.count(s.factoryRepeat.size()); for (bool v : s.factoryRepeat) w.u8(v); }
    w.count(s.parents.size()); for (const auto& v : s.parents) { w.u8(v.has_value()); if (v) writeId(w, *v); }
    w.count(s.children.size()); for (const auto& list : s.children) { w.count(list.size()); for (UnitId id : list) writeId(w, id); }
    if (includesAttachmentOffsets) { w.count(s.attachmentOffsets.size()); for (const auto& offset : s.attachmentOffsets) { w.i32(offset[0].raw()); w.i32(offset[1].raw()); } }
    if (includesAttachmentHeights) { w.count(s.attachmentHeights.size()); for (Fx height : s.attachmentHeights) w.i32(height.raw()); }
    if (includesDoNotTarget) { w.count(s.doNotTarget.size()); for (bool v : s.doNotTarget) w.u8(v); }
    if (includesCommands) writeCommandState(w, s, includesScriptTasks);
}

[[nodiscard]] bool readUnits(PayloadReader& r, UnitStore::Snapshot& s, bool includesPathPhase,
                                 bool includesFactoryRepeat, bool includesAttachmentOffsets,
                                 bool includesDoNotTarget, bool includesAutomaticTargets,
                                 bool includesAttachmentHeights, bool includesAttachedMotion,
                                 bool includesCommands, bool includesScriptTasks, bool includesGuard) {
    std::size_t n{};
    if (!r.count(n, 4)) return false; s.ids.generations.resize(n); for (auto& v : s.ids.generations) if (!r.u32(v)) return false;
    if (!r.count(n, 4)) return false; s.ids.free.resize(n); for (auto& v : s.ids.free) if (!r.u32(v)) return false;
    std::uint64_t live{};
    if (!r.u64(live) || live > std::numeric_limits<std::size_t>::max() || !r.count(n, 4)) return false;
    s.ids.live = static_cast<std::size_t>(live);
    s.generations.resize(n); for (auto& v : s.generations) if (!r.u32(v)) return false;
    if (!r.count(n, 18)) return false; s.transforms.resize(n); for (auto& v : s.transforms) { std::int32_t x{},y{},z{}; if (!r.i32(x)||!r.i32(y)||!r.i32(z)||!r.u16(v.heading)||!r.u16(v.pitch)||!r.u16(v.roll)) return false; v.x=Fx::fromRaw(x); v.y=Fx::fromRaw(y); v.z=Fx::fromRaw(z); }
    if (!r.count(n, (includesPathPhase ? 58 : 46) + (includesAttachedMotion ? 1 : 0))) return false; s.motion.resize(n); for (auto& v : s.motion) { std::int32_t x{},z{},speed{},radius{},distance{}; std::uint8_t moving{},attached{},airborne{},water{}; if (!r.i32(v.armyIndex)||!r.i32(x)||!r.i32(z)||!r.u8(moving)||(includesAttachedMotion && !r.u8(attached))||!r.u8(airborne)||!r.u8(water)||moving>1||attached>1||airborne>1||water>1||!r.i32(speed)||!r.i32(v.turnPerTick)||!r.i32(radius)||!r.i32(distance)||!r.count(n,8)) return false; v.destinationX=Fx::fromRaw(x); v.destinationZ=Fx::fromRaw(z); v.moving=moving; v.attached=attached; v.airborne=airborne; v.surfaceWater=water; v.speedPerTick=Fx::fromRaw(speed); v.radiusElmos=Fx::fromRaw(radius); v.distanceTravelledElmos=Fx::fromRaw(distance); v.path.resize(n); for(auto& p:v.path){if(!r.i32(x)||!r.i32(z))return false;p={Fx::fromRaw(x),Fx::fromRaw(z)};} std::uint64_t index{}; if(!r.u64(index)||index>std::numeric_limits<std::size_t>::max())return false; v.pathIndex=static_cast<std::size_t>(index); if (includesPathPhase && (!r.i32(v.pathPhaseStartX) || !r.i32(v.pathPhaseStartZ) || !r.i32(v.pathPhaseCellsX))) return false; }
    if (!r.count(n, 52)) return false; s.health.resize(n); for (auto& v : s.health) { std::int64_t a{},b{},c{},d{}; if(!r.i64(a)||!r.i64(b)||!r.i64(c)||!r.i64(d)||!r.u32(v.shield.regenDelayRemaining)||!r.u32(v.shield.rechargeRemaining)||!r.count(n,4))return false; v.current=Mag::fromRaw(a);v.maximum=Mag::fromRaw(b);v.shield.current=Mag::fromRaw(c);v.shield.maximum=Mag::fromRaw(d);v.reloadRemaining.resize(n);for(auto& x:v.reloadRemaining)if(!r.i32(x))return false;if(!r.count(n,4))return false;v.burstRemaining.resize(n);for(auto& x:v.burstRemaining)if(!r.i32(x))return false;if (includesAutomaticTargets) { if (!r.count(n, 8)) return false; v.automaticTargets.resize(n); for (auto& target : v.automaticTargets) if (!readId(r, target)) return false; } if(!readId(r,v.lastHitBy)||!r.i32(v.veterancy.kills)||!r.i32(v.veterancy.level))return false; }
    if (!r.count(n,2)) return false; s.types.resize(n); for(auto& v:s.types)if(!r.u16(v))return false;
    if (includesFactoryRepeat) { if (!r.count(n, 1)) return false; s.factoryRepeat.resize(n); for (auto&& v : s.factoryRepeat) { std::uint8_t enabled{}; if (!r.u8(enabled) || enabled > 1) return false; v = enabled; } }
    if (!r.count(n,1)) return false; s.parents.resize(n); for(auto& v:s.parents){std::uint8_t has{};if(!r.u8(has)||has>1)return false;if(has){UnitId id;if(!readId(r,id))return false;v=id;}}
    if (!r.count(n,4)) return false; s.children.resize(n); for(auto& list:s.children){if(!r.count(n,8))return false;list.resize(n);for(auto& id:list)if(!readId(r,id))return false;}
    if (includesAttachmentOffsets) { if (!r.count(n, 8)) return false; s.attachmentOffsets.resize(n); for (auto& offset : s.attachmentOffsets) { std::int32_t x{}, z{}; if (!r.i32(x) || !r.i32(z)) return false; offset = {Fx::fromRaw(x), Fx::fromRaw(z)}; } }
    if (includesAttachmentHeights) { if (!r.count(n, 4)) return false; s.attachmentHeights.resize(n); for (Fx& height : s.attachmentHeights) { std::int32_t raw{}; if (!r.i32(raw)) return false; height = Fx::fromRaw(raw); } }
    if (includesDoNotTarget) { if (!r.count(n, 1)) return false; s.doNotTarget.resize(n); for (auto&& v : s.doNotTarget) { std::uint8_t enabled{}; if (!r.u8(enabled) || enabled > 1) return false; v = enabled; } }
    const std::size_t slots=s.transforms.size();
    if (s.ids.generations.size()!=slots || s.generations.size()!=slots || s.motion.size()!=slots
         || s.health.size()!=slots || s.types.size()!=slots
          || (includesFactoryRepeat && s.factoryRepeat.size()!=slots) || s.parents.size()!=slots
           || s.children.size()!=slots || (includesAttachmentOffsets && s.attachmentOffsets.size()!=slots)
           || (includesAttachmentHeights && s.attachmentHeights.size()!=slots)
          || (includesDoNotTarget && s.doNotTarget.size()!=slots)
         || s.ids.live>slots || s.ids.free.size()>slots) return false;

    std::vector<bool> free(slots);
    for (UnitIndex index : s.ids.free) {
        if (index >= slots || free[index]) return false;
        free[index] = true;
    }
    std::size_t liveSlots = 0;
    for (std::size_t slot = 0; slot < slots; ++slot) {
        if (free[slot]) {
            if (s.generations[slot] == s.ids.generations[slot]) return false;
        } else {
            if (s.ids.generations[slot] == 0 || s.generations[slot] != s.ids.generations[slot]) return false;
            ++liveSlots;
        }
    }
    if (liveSlots != s.ids.live) return false;

    const auto isLive = [&](UnitId id) {
        return id.index < slots && !free[id.index] && id.generation != 0
               && id.generation == s.ids.generations[id.index];
    };
    for (std::size_t child = 0; child < slots; ++child) {
        if (s.parents[child] && (!isLive(*s.parents[child]) || free[child])) return false;
    }
    for (std::size_t parent = 0; parent < slots; ++parent) {
        std::vector<bool> seenChild(slots);
        for (UnitId child : s.children[parent]) {
            if (!isLive(UnitId{static_cast<UnitIndex>(parent), s.ids.generations[parent]})
                || !isLive(child) || seenChild[child.index] || !s.parents[child.index]
                || *s.parents[child.index]
                       != UnitId{static_cast<UnitIndex>(parent), s.ids.generations[parent]}) return false;
            seenChild[child.index] = true;
        }
    }
    for (std::size_t child = 0; child < slots; ++child) {
        if (s.parents[child]
            && std::find(s.children[s.parents[child]->index].begin(),
                         s.children[s.parents[child]->index].end(),
                         UnitId{static_cast<UnitIndex>(child), s.ids.generations[child]})
                   == s.children[s.parents[child]->index].end()) return false;
        std::size_t steps = 0;
        for (std::optional<UnitId> parent = s.parents[child]; parent; parent = s.parents[parent->index]) {
            if (++steps > slots) return false;
        }
        if (includesAttachedMotion && s.motion[child].attached != s.parents[child].has_value()) {
            return false;
        }
        if (!includesAttachedMotion) {
            s.motion[child].attached = s.parents[child].has_value();
        }
    }
    if (!includesFactoryRepeat) s.factoryRepeat.resize(slots, false);
    if (!includesDoNotTarget) s.doNotTarget.resize(slots, false);
    if (!includesAttachmentOffsets) {
        s.attachmentOffsets.resize(slots);
        for (std::size_t child = 0; child < slots; ++child) {
            if (s.parents[child]) {
                const UnitIndex parent = s.parents[child]->index;
                s.attachmentOffsets[child] = {s.transforms[child].x - s.transforms[parent].x,
                                              s.transforms[child].z - s.transforms[parent].z};
            }
        }
    }
    if (!includesAttachmentHeights) {
        s.attachmentHeights.resize(slots);
        for (std::size_t child = 0; child < slots; ++child) {
            if (s.parents[child]) {
                s.attachmentHeights[child] =
                    s.transforms[child].y - s.transforms[s.parents[child]->index].y;
            }
        }
    }
    if (includesCommands
        && (!readCommandState(r, s, includesScriptTasks, includesGuard) || s.orders.size() != slots)) {
        return false;
    }
    if (includesCommands) {
        for (std::size_t slot = 0; slot < s.orders.size(); ++slot) {
            const CommandQueue::Snapshot& queue = s.orders[slot];
            const UnitId unit{static_cast<UnitIndex>(slot), s.ids.generations[slot]};
            if (queue.entries.empty()) {
                if (queue.activeSerial) return false;
                continue;
            }
            if (!isLive(unit)) return false;
            if (queue.activeSerial
                && (queue.entries.empty()
                    || queue.entries.front().sharedCommand >= s.sharedCommands.size()
                    || s.sharedCommands[queue.entries.front().sharedCommand].creationSerial
                           != *queue.activeSerial)) {
                return false;
            }
            for (const CommandQueue::SnapshotEntry& entry : queue.entries) {
                if (entry.sharedCommand >= s.sharedCommands.size()) return false;
                const SharedCommand& command = s.sharedCommands[entry.sharedCommand];
                if (entry.execution.unit != unit
                    || std::find(command.units.begin(), command.units.end(), unit)
                           == command.units.end()) {
                    return false;
                }
            }
        }
    }
    return true;
}

void appendU32(std::vector<std::byte>& bytes, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) {
        bytes.push_back(std::byte{static_cast<unsigned char>(value >> shift)});
    }
}

void appendU64(std::vector<std::byte>& bytes, std::uint64_t value) {
    for (unsigned shift = 0; shift < 64; shift += 8) {
        bytes.push_back(std::byte{static_cast<unsigned char>(value >> shift)});
    }
}

[[nodiscard]] bool readU32(std::span<const std::byte> bytes, std::size_t& offset,
                           std::uint32_t& value) {
    if (bytes.size() - offset < sizeof(value)) {
        return false;
    }
    value = 0;
    for (unsigned shift = 0; shift < 32; shift += 8) {
        value |= static_cast<std::uint32_t>(std::to_integer<unsigned char>(bytes[offset++]))
                 << shift;
    }
    return true;
}

[[nodiscard]] bool readU64(std::span<const std::byte> bytes, std::size_t& offset,
                           std::uint64_t& value) {
    if (bytes.size() - offset < sizeof(value)) {
        return false;
    }
    value = 0;
    for (unsigned shift = 0; shift < 64; shift += 8) {
        value |= static_cast<std::uint64_t>(std::to_integer<unsigned char>(bytes[offset++]))
                 << shift;
    }
    return true;
}

} // namespace

[[nodiscard]] std::vector<std::byte> encode(const SaveState& state, std::uint32_t version) {
    // The standard stream operators preserve every MT19937 state word and its index.
    std::ostringstream randomState;
    randomState.imbue(std::locale::classic());
    randomState << state.random;
    PayloadWriter payloadWriter;
    payloadWriter.text(randomState.str());
    if (version >= kVersion2) payloadWriter.u64(state.pathServiceBeats);
    writeUnits(payloadWriter, state.units, version >= kVersion2, version >= kVersion3,
                   version >= kVersion4, version >= kVersion5, version >= kVersion6,
                    version >= kVersion7, version >= kVersion7, version >= kVersion8,
                    version >= kVersion12);
    if (version >= kVersion9) writeSiloAmmo(payloadWriter, state.siloAmmo);
    if (version >= kVersion10) writeRedirects(payloadWriter, state.redirects);
    if (version >= kVersion11) {
        writeAirMotion(payloadWriter, state.units.motion, version >= kVersion13,
                       version >= kVersion14);
    }
    if (version >= kVersion15) writeEconomyArmies(payloadWriter, state.economyArmies);
    if (version >= kVersion16) writeAirController(payloadWriter, state.units.motion);
    const std::vector<std::byte> payload = payloadWriter.take();
    if (payload.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::length_error("MT19937 state exceeds the v1 save-state payload limit");
    }

    std::vector<std::byte> bytes;
    bytes.reserve(kMagic.size() + sizeof(version) + sizeof(state.tick) + sizeof(std::uint32_t)
                  + payload.size());
    bytes.insert(bytes.end(), kMagic.begin(), kMagic.end());
    appendU32(bytes, version);
    appendU64(bytes, state.tick);
    appendU32(bytes, static_cast<std::uint32_t>(payload.size()));
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    return bytes;
}

[[nodiscard]] std::optional<SaveState> decode(std::span<const std::byte> bytes,
                                              std::optional<std::uint32_t> requiredVersion) {
    std::size_t offset = 0;
    if (bytes.size() < kMagic.size()
        || !std::equal(kMagic.begin(), kMagic.end(), bytes.begin())) {
        return std::nullopt;
    }
    offset += kMagic.size();

    std::uint32_t version{};
    std::uint64_t tick{};
    std::uint32_t payloadSize{};
    if (!readU32(bytes, offset, version)
        || (version != kVersion1 && version != kVersion2 && version != kVersion3
              && version != kVersion4 && version != kVersion5 && version != kVersion6
               && version != kVersion7 && version != kVersion8 && version != kVersion9
               && version != kVersion10 && version != kVersion11 && version != kVersion12
               && version != kVersion13 && version != kVersion14 && version != kVersion15
               && version != kVersion16)
        || (requiredVersion && version != *requiredVersion) || !readU64(bytes, offset, tick)
        || !readU32(bytes, offset, payloadSize) || bytes.size() - offset != payloadSize) {
        return std::nullopt;
    }

    PayloadReader reader{bytes.subspan(offset)};
    std::string payload;
    if (!reader.text(payload) || payload.size() > kMaxRandomStatePayload) return std::nullopt;

    std::istringstream randomState{payload};
    randomState.imbue(std::locale::classic());
    RandomStream::Snapshot random;
    if (!(randomState >> random)) {
        return std::nullopt;
    }
    // The frame has one canonical representation, so equivalent but altered payload text is not
    // silently accepted as a valid save.
    std::ostringstream canonicalState;
    canonicalState.imbue(std::locale::classic());
    canonicalState << random;
    if (canonicalState.str() != payload) {
        return std::nullopt;
    }
    std::uint64_t pathServiceBeats{};
    if (version >= kVersion2 && !reader.u64(pathServiceBeats)) return std::nullopt;
    UnitStore::Snapshot units;
    if (!readUnits(reader, units, version >= kVersion2, version >= kVersion3, version >= kVersion4,
                       version >= kVersion5, version >= kVersion6, version >= kVersion7,
                       version >= kVersion7, version >= kVersion8,
                       version >= kVersion12, version >= kVersion14)) return std::nullopt;
    std::vector<SiloAmmo> siloAmmo;
    if (version >= kVersion9 && !readSiloAmmo(reader, siloAmmo)) return std::nullopt;
    std::vector<MissileRedirect> redirects;
    if (version >= kVersion10 && !readRedirects(reader, redirects)) return std::nullopt;
    if (version >= kVersion11
        && !readAirMotion(reader, units.motion, version >= kVersion13,
                          version >= kVersion14, version >= kVersion16)) return std::nullopt;
    std::optional<EconomyArmyState> economyArmies;
    if (version >= kVersion15 && !readEconomyArmies(reader, economyArmies)) return std::nullopt;
    if (version >= kVersion16 && !readAirController(reader, units.motion)) return std::nullopt;
    if (!reader.finished()) return std::nullopt;
    SaveState decoded{.tick = tick,
                      .random = std::move(random),
                       .pathServiceBeats = pathServiceBeats,
                       .units = std::move(units), .siloAmmo = std::move(siloAmmo),
                       .redirects = std::move(redirects), .economyArmies = std::move(economyArmies)};
    // One binary representation per state rejects alternate encodings and trailing data.
    const std::vector<std::byte> canonical = encode(decoded, version);
    if (canonical.size() != bytes.size()
        || !std::equal(canonical.begin(), canonical.end(), bytes.begin())) return std::nullopt;
    return decoded;
}

std::vector<std::byte> SaveState::encodeV1(const SaveState& state) {
    return rm::sim::encode(state, kVersion1);
}

std::optional<SaveState> SaveState::decodeV1(std::span<const std::byte> bytes) {
    return rm::sim::decode(bytes, kVersion1);
}

std::vector<std::byte> SaveState::encodeV2(const SaveState& state) {
    return rm::sim::encode(state, kVersion2);
}

std::optional<SaveState> SaveState::decodeV2(std::span<const std::byte> bytes) {
    return rm::sim::decode(bytes, kVersion2);
}

std::vector<std::byte> SaveState::encode(const SaveState& state) {
    return rm::sim::encode(state, kVersion16);
}

std::optional<SaveState> SaveState::decode(std::span<const std::byte> bytes) {
    return rm::sim::decode(bytes, std::nullopt);
}

EconomyArmyState EconomyArmyState::capture(const Match& match) {
    return {.armies = match.armies,
        .economies = {match.economies.begin(), match.economies.end()},
        .building = match.building != nullptr ? *match.building : std::vector<Construction>{},
        .commandersEver = {match.commandersEver.begin(), match.commandersEver.end()},
        .victoryMode = match.victoryMode, .baseStorage = match.baseStorage,
        .over = match.over, .winnerPending = match.winnerPending,
        .pendingWinner = match.pendingWinner, .winnerStableTicks = match.winnerStableTicks,
        .defeatPollElapsedTicks = match.defeatPollElapsedTicks,
        .defeatCleanupRemainingTicks = match.defeatCleanupRemainingTicks};
}

void EconomyArmyState::restore(Match& match, std::vector<Economy>& economyStorage,
                               std::vector<int>& commanderStorage) const {
    if (match.building == nullptr && !building.empty()) {
        throw std::invalid_argument("restoring construction requires match building storage");
    }
    match.armies = armies;
    economyStorage = economies;
    commanderStorage = commandersEver;
    match.economies = economyStorage;
    match.commandersEver = commanderStorage;
    if (match.building != nullptr) *match.building = building;
    match.victoryMode = victoryMode;
    match.baseStorage = baseStorage;
    match.over = over;
    match.winnerPending = winnerPending;
    match.pendingWinner = pendingWinner;
    match.winnerStableTicks = winnerStableTicks;
    match.defeatPollElapsedTicks = defeatPollElapsedTicks;
    match.defeatCleanupRemainingTicks = defeatCleanupRemainingTicks;
}

} // namespace rm::sim
