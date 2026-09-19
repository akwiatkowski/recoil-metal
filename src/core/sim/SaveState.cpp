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
/// Version 17 admits the `ReclaimUnit` command kind. No payload layout changed: the kind byte
/// was already written, only the range a reader accepts widened.
constexpr std::uint32_t kVersion17 = 17;
constexpr std::uint32_t kVersion18 = 18;
constexpr std::uint32_t kVersion19 = 19;
/// Version 20 admits the `Capture` command kind and adds funded unit-capture tasks.
constexpr std::uint32_t kVersion20 = 20;
/// Version 21 adds the bank tuning (`KRoll`, `BankFactor`) to the aircraft controller
/// snapshot; older readers stop before those two words.
constexpr std::uint32_t kVersion21 = 21;
/// Version 22 adds the attachment bone record (parent/self indices plus authored
/// rest offsets) beside the historical offset sections; older readers keep the
/// boneless default.
constexpr std::uint32_t kVersion22 = 22;
/// Version 23 adds the wreck pool (nullable, trailing); older readers keep null features.
constexpr std::uint32_t kVersion23 = 23;
/// Version 24 adds the mounted turret's live pose (yaw, pitch, dual-muzzle phase);
/// older readers restore turrets at rest.
constexpr std::uint32_t kVersion24 = 24;
/// V25 adds the dual manipulator's own angles (`turretYaw2`/`turretPitch2`) —
/// the second arm's independent aim state — as another trailing section.
constexpr std::uint32_t kVersion25 = 25;
/// V26 adds the per-unit production-pause flags beside `factoryRepeat`, and admits
/// the `MissileLaunch` command kind — earlier writers could produce one but no
/// reader accepted it, so nothing decodable is lost by gating it here.
constexpr std::uint32_t kVersion26 = 26;
/// V27 adds the per-unit build-priority tiers beside `productionPaused`. Older saves
/// decode with every unit Normal — the tier the pre-V27 allocator implied anyway.
constexpr std::uint32_t kVersion27 = 27;
/// V28 adds the retreat settings and the live retreat bookkeeping beside the
/// priority tiers: one threshold byte per slot, then a per-slot record of whether
/// the unit is retreated and the two points that state owes. Older saves decode
/// with everything Off and nobody mid-flight — the pre-V28 answer to both.
constexpr std::uint32_t kVersion28 = 28;
/// V29 admits the transport command kinds (LoadTransport, UnloadTransport,
/// Ferry) and adds each queue entry's ferry anchor and transport phase — the
/// execution state a mid-route carrier owes the loop. Older saves decode with
/// no anchor and phase None, which is the state every non-transport entry holds.
constexpr std::uint32_t kVersion29 = 29;
/// V30 adds the per-unit target focus (#15809) beside the retreat settings:
/// one byte per slot. Older saves decode with every unit at Default — the
/// pre-V30 acquisition behaviour.
constexpr std::uint32_t kVersion30 = 30;
// 31: congestion bookkeeping — `blockedTicks` and `yielding` in MoveState.
constexpr std::uint32_t kVersion31 = 31;
// 32: the measured per-tick step (`stepX`/`stepZ`) that `LeadTarget` weapons
// advance their aim by. Older saves decode with both zero — one tick of
// unleaded aim before movement measures again.
constexpr std::uint32_t kVersion32 = 32;
// 33: the silo build queue (`C-241`'s CAiSiloBuildImpl+0x20 list) and each record's
// `autoBuild` flag. Older saves decode with empty queues and auto-mode on — the
// pre-queue refill behaviour by another name.
constexpr std::uint32_t kVersion33 = 33;
// 34: the per-army `CArmyStats` store (`C-227`) — named aggregate and per-blueprint
// statistics plus the pending one-shot triggers — and the self-destruct countdowns
// (`C-345`). Older saves decode with no stat service and nothing counting down.
constexpr std::uint32_t kVersion34 = 34;
// 35: the in-flight projectile pool — every field the state hash walks, so a
// mid-combat save resumes the same shots. `visualId`/`visualOrigin`/
// `visualSerial` stay out: they are presentation identity, never hashed, and a
// restored shot gets a fresh serial from ProjectileTrails like any new one.
// Nullable like `features`: a scene with no projectile list saves the absent
// byte, preserving the match's null-vs-empty distinction.
constexpr std::uint32_t kVersion35 = 35;
// 36: the script-bit disabled masks and maintenance flags (`RULEUTC_*` toggles,
// `C-350`) beside `productionPaused`, plus `ShieldState::rechargeRestoresFull`
// inside each unit's health record. Older saves decode with every feature on
// and maintenance consuming.
constexpr std::uint32_t kVersion36 = 36;
// 37: the path service's queues, counters and in-flight fields plus the intel
// history (retained contacts, seen-ever latches, brownout recovery) as two
// nullable trailing sections. A saved FlowField reduces to its expansion
// count — the deterministic frontier replays on restore — and intel grids
// re-stamp from unit positions on the first update.
constexpr std::uint32_t kVersion37 = 37;
// 38: `C-204`'s water pair on each projectile record — `StayUnderwater` and
// `DestroyOnWater` are launch-time state resolved from the projectile
// blueprint, so a saved torpedo must carry them or it loses its clamp on
// restore. `inWater` stays out: it is recomputed from the restored position
// on the first tick, exactly like retail's `proj+0x334`.
constexpr std::uint32_t kVersion38 = 38;
// 39: `C-171`'s weave state and `C-088`'s friendly-fire flag on each projectile
// record — a saved mid-flight weaving shot must keep its roll schedule and
// offsets or it un-weaves on restore, and a returned missile must keep its
// `CollideFriendly` or it stops being able to hit its own side.
constexpr std::uint32_t kVersion39 = 39;
// 40: `victory.lua`'s `OfferingDraw` flag on each army record — a saved
// mid-negotiation match must keep who has offered or a restored match can
// outlive the draw everyone agreed to. Older saves decode with no offers.
constexpr std::uint32_t kVersion40 = 40;
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

void writeEconomyArmies(PayloadWriter& w, const std::optional<EconomyArmyState>& state,
                        bool includesOfferingDraw) {
    w.u8(state.has_value());
    if (!state) return;
    const auto& s = *state;
    w.count(s.armies.size());
    for (const auto& army : s.armies) {
        w.i32(army.index); w.u8(static_cast<std::uint8_t>(army.faction));
        w.i32(army.alliance); w.u8(army.defeated);
        if (includesOfferingDraw) w.u8(army.offeringDraw);
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
    if (includesOfferingDraw) w.u64(s.pendingSurvivorMask);
    w.count(s.defeatCleanupRemainingTicks.size());
    for (const auto ticks : s.defeatCleanupRemainingTicks) w.u32(ticks);
}
bool readEconomyArmies(PayloadReader& r, std::optional<EconomyArmyState>& state,
                       bool includesOfferingDraw) {
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
            || !r.i32(army.alliance) || !readFlag(r, army.defeated)
            || (includesOfferingDraw && !readFlag(r, army.offeringDraw))) return false;
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
    // No layout change for the new modes (v17's ReclaimUnit precedent): old writers
    // never produced values above Supremacy, so widening the accepted range cannot
    // misread an old file.
    if (!r.u8(mode) || mode > static_cast<int>(VictoryMode::Sandbox)
        || !readResources(r, s.baseStorage) || !readFlag(r, s.over)
        || !readFlag(r, s.winnerPending) || !readFlag(r, winner)) return false;
    s.victoryMode = static_cast<VictoryMode>(mode);
    if (winner) {
        int alliance{};
        if (!r.i32(alliance)) return false;
        s.pendingWinner = alliance;
    }
    if (!r.u32(s.winnerStableTicks) || !r.u32(s.defeatPollElapsedTicks)
        || (includesOfferingDraw && !r.u64(s.pendingSurvivorMask))
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
                                     bool includesScriptTasks, bool includesGuard,
                                     bool includesReclaimUnit, bool includesCapture,
                                     bool includesMissileLaunch, bool includesTransport) {
    std::uint8_t source{}, kind{}, queued{};
    std::uint32_t player{};
    std::size_t units{};
    std::int32_t targetX{}, targetZ{};
    // CancelFactoryBuild and Dive never enter a queue, so the highest SAVED kind of
    // the guard era is Guard; version 17 admits ReclaimUnit above it, version 20
    // admits Capture above that, and version 26 admits MissileLaunch — its writers
    // could produce the kind before any reader would. Version 29 admits the three
    // transport kinds, Ferry highest.
    const CommandKind highestKind = includesTransport   ? CommandKind::Ferry
                                    : includesMissileLaunch ? CommandKind::MissileLaunch
                                    : includesCapture    ? CommandKind::Capture
                                    : includesReclaimUnit ? CommandKind::ReclaimUnit
                                    : includesGuard       ? CommandKind::Guard
                                    : includesScriptTasks ? CommandKind::Script
                                                          : CommandKind::Repair;
    if (!r.u64(command.tick) || !r.u8(source) || !r.u32(command.id) || !r.u32(player)
        || !r.u8(kind) || !r.u8(queued) || source > kInvalidCommandSource
        || player > std::numeric_limits<PlayerIndex>::max()
        || kind > static_cast<std::uint8_t>(highestKind)
        || kind == static_cast<std::uint8_t>(CommandKind::CancelFactoryBuild)
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
                       bool includesScriptTasks, bool includesTransport) {
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
            if (includesTransport) {
                w.u8(execution.transportAnchor.has_value());
                if (execution.transportAnchor) {
                    w.i32((*execution.transportAnchor)[0].raw());
                    w.i32((*execution.transportAnchor)[1].raw());
                }
                w.u8(static_cast<std::uint8_t>(execution.transportPhase));
            }
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

void writeSiloAmmo(PayloadWriter& w, std::span<const SiloAmmo> ammo, bool includesSiloQueue) {
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
        if (includesSiloQueue) w.u8(value.autoBuild);
    }
}

void writeSiloQueue(PayloadWriter& w, std::span<const SiloBuild> queue) {
    w.count(queue.size());
    for (const SiloBuild& entry : queue) {
        writeId(w, entry.owner);
        w.u8(entry.slot);
    }
}

void writeInstalledEnhancements(PayloadWriter& w, const UnitStore::Snapshot& units) {
    w.count(static_cast<std::size_t>(std::count_if(units.enhancements.begin(), units.enhancements.end(),
        [](const auto& slots) { return !slots.empty(); })));
    for (std::size_t unit = 0; unit < units.enhancements.size(); ++unit) {
        const auto& slots = units.enhancements[unit];
        if (slots.empty()) continue;
        w.u32(static_cast<std::uint32_t>(unit));
        w.count(slots.size());
        for (const auto& [slot, name] : slots) { w.text(slot); w.text(name); }
    }
}
bool readInstalledEnhancements(PayloadReader& r, UnitStore::Snapshot& units) {
    std::size_t count{};
    if (!r.count(count, 8) || count > units.transforms.size()) return false;
    if (count != 0) units.enhancements.resize(units.transforms.size());
    for (std::size_t entry = 0; entry < count; ++entry) {
        std::uint32_t unit{};
        std::size_t slots{};
        if (!r.u32(unit) || unit >= units.enhancements.size() || !r.count(slots, 8)
            || slots == 0 || !units.enhancements[unit].empty()) return false;
        for (std::size_t item = 0; item < slots; ++item) {
            std::string slot, name;
            if (!r.text(slot) || !r.text(name) || slot.empty() || name.empty()
                || !units.enhancements[unit].emplace(std::move(slot), std::move(name)).second) return false;
        }
    }
    return true;
}

void writeEnhancements(PayloadWriter& w, std::span<const EnhancementWork> work) {
    w.count(work.size());
    for (const auto& value : work) {
        writeId(w, value.owner);
        w.text(value.name);
        writeResources(w, value.cost);
        w.i64(value.totalBuildTime.raw());
        w.i64(value.buildTimeRemaining.raw());
        w.i64(value.buildPerTick.raw());
        writeResources(w, value.allocated);
        w.i32(value.fundedLastTick.raw());
        w.u8(value.paused ? 1 : 0);
    }
}

bool readEnhancements(PayloadReader& r, std::vector<EnhancementWork>& work) {
    std::size_t count{};
    if (!r.count(count, 1)) return false;
    work.resize(count);
    for (auto& value : work) {
        std::int64_t total{}, remaining{}, rate{};
        std::int32_t funded{};
        std::uint8_t paused{};
        if (!readId(r, value.owner) || !r.text(value.name)
            || !readResources(r, value.cost) || !r.i64(total) || !r.i64(remaining)
            || !r.i64(rate) || !readResources(r, value.allocated) || !r.i32(funded)
            || !r.u8(paused) || paused > 1 || total <= 0 || remaining < 0 || remaining > total
            || rate <= 0 || funded < 0 || funded > kFxOne.raw()
            || value.cost.mass < Mag{} || value.cost.energy < Mag{}
            || value.allocated.mass < Mag{} || value.allocated.energy < Mag{}) return false;
        value.totalBuildTime = Mag::fromRaw(total);
        value.buildTimeRemaining = Mag::fromRaw(remaining);
        value.buildPerTick = Mag::fromRaw(rate);
        value.fundedLastTick = Fx::fromRaw(funded);
        value.paused = paused != 0;
    }
    return true;
}

void writeCaptures(PayloadWriter& w, std::span<const CaptureWork> work) {
    w.count(work.size());
    for (const auto& value : work) {
        w.i32(value.armyIndex);
        w.u32(value.captor);
        writeId(w, value.target);
        w.i32(value.workTicks);
        w.i32(value.progress);
        writeResources(w, value.demand);
        w.i32(value.funded.raw());
        w.u8(value.inReach ? 1 : 0);
    }
}

// V22 trails the payload, like every earlier addition: the bone record sits beside
// the historical offset sections in the snapshot but serializes after them, so older
// readers stop before it and older shapes need no interior edits.
void writeBoneAttachments(PayloadWriter& w, const UnitStore::Snapshot& s) {
    w.count(s.attachmentParentBones.size());
    for (std::size_t i = 0; i < s.attachmentParentBones.size(); ++i) {
        w.i32(s.attachmentParentBones[i]);
        w.i32(s.attachmentSelfBones[i]);
        w.i32(s.attachmentParentRest[i][0].raw());
        w.i32(s.attachmentParentRest[i][1].raw());
        w.i32(s.attachmentParentRestHeights[i].raw());
        w.i32(s.attachmentSelfRest[i][0].raw());
        w.i32(s.attachmentSelfRest[i][1].raw());
        w.i32(s.attachmentSelfRestHeights[i].raw());
    }
}

// V23 trails the payload after the bone record: the wreck pool, allocator included,
// so a restored scene reclaims the same mass from the same slots. Null scenes write
// only the absent byte, preserving the match's null-vs-empty distinction.
void writeFeatures(PayloadWriter& w, const std::optional<FeatureStore::Snapshot>& s) {
    w.u8(s.has_value());
    if (!s) {
        return;
    }
    w.count(s->ids.generations.size());
    for (Generation v : s->ids.generations) w.u32(v);
    w.count(s->ids.free.size());
    for (UnitIndex v : s->ids.free) w.u32(v);
    w.u64(s->ids.live);
    w.count(s->generations.size());
    for (Generation v : s->generations) w.u32(v);
    w.count(s->features.size());
    for (const Feature& v : s->features) {
        w.i32(v.at[0].raw());
        w.i32(v.at[1].raw());
        w.i32(v.at[2].raw());
        w.i32(v.radiusElmos.raw());
        w.u16(v.fromType);
        w.i32(v.armyIndex);
        w.i64(v.health.raw());
        w.i64(v.maximumHealth.raw());
        w.i64(v.maximumMassReclaim.raw());
        w.i64(v.maximumEnergyReclaim.raw());
        w.i64(v.massRemaining.raw());
        w.i64(v.energyRemaining.raw());
        w.i64(v.reclaimWorkRemaining.raw());
        w.i64(v.reclaimWorkTotal.raw());
        w.i32(v.reclaimFraction.raw());
        w.i32(v.damageRatio.raw());
        w.i32(v.maximumReclaimPerBuildRate.raw());
        w.i32(v.reclaimPerBuildRate.raw());
    }
    w.u64(s->revision);
}

bool readFeatures(PayloadReader& r, std::optional<FeatureStore::Snapshot>& snapshot) {
    bool present{};
    if (!readFlag(r, present)) return false;
    if (!present) {
        snapshot.reset();
        return true;
    }
    snapshot.emplace();
    FeatureStore::Snapshot& s = *snapshot;
    std::size_t count{};
    if (!r.count(count, 4)) return false;
    s.ids.generations.resize(count);
    for (auto& v : s.ids.generations)
        if (!r.u32(v)) return false;
    if (!r.count(count, 4)) return false;
    s.ids.free.resize(count);
    for (auto& v : s.ids.free)
        if (!r.u32(v)) return false;
    std::uint64_t live{};
    if (!r.u64(live) || live > std::numeric_limits<std::size_t>::max()) return false;
    s.ids.live = static_cast<std::size_t>(live);
    if (!r.count(count, 4)) return false;
    s.generations.resize(count);
    for (auto& v : s.generations)
        if (!r.u32(v)) return false;
    if (!r.count(count, 102)) return false;
    s.features.resize(count);
    for (auto& v : s.features) {
        std::int32_t x{}, y{}, z{}, radius{}, army{}, fraction{}, damage{}, maxRate{}, rate{};
        std::int64_t health{}, maxHealth{}, maxMass{}, maxEnergy{}, mass{}, energy{}, work{},
            workTotal{};
        if (!r.i32(x) || !r.i32(y) || !r.i32(z) || !r.i32(radius) || !r.u16(v.fromType)
            || !r.i32(army) || !r.i64(health) || !r.i64(maxHealth) || !r.i64(maxMass)
            || !r.i64(maxEnergy) || !r.i64(mass) || !r.i64(energy) || !r.i64(work)
            || !r.i64(workTotal) || !r.i32(fraction) || !r.i32(damage) || !r.i32(maxRate)
            || !r.i32(rate))
            return false;
        v.at = {Fx::fromRaw(x), Fx::fromRaw(y), Fx::fromRaw(z)};
        v.radiusElmos = Fx::fromRaw(radius);
        v.armyIndex = army;
        v.health = Mag::fromRaw(health);
        v.maximumHealth = Mag::fromRaw(maxHealth);
        v.maximumMassReclaim = Mag::fromRaw(maxMass);
        v.maximumEnergyReclaim = Mag::fromRaw(maxEnergy);
        v.massRemaining = Mag::fromRaw(mass);
        v.energyRemaining = Mag::fromRaw(energy);
        v.reclaimWorkRemaining = Mag::fromRaw(work);
        v.reclaimWorkTotal = Mag::fromRaw(workTotal);
        v.reclaimFraction = Fx::fromRaw(fraction);
        v.damageRatio = Fx::fromRaw(damage);
        v.maximumReclaimPerBuildRate = Fx::fromRaw(maxRate);
        v.reclaimPerBuildRate = Fx::fromRaw(rate);
    }
    if (!r.u64(s.revision)) return false;
    const std::size_t slots = s.features.size();
    if (s.generations.size() != slots || s.ids.live > slots || s.ids.free.size() > slots)
        return false;
    return true;
}

bool readBoneAttachments(PayloadReader& r, UnitStore::Snapshot& s) {
    std::size_t count{};
    if (!r.count(count, 32)) return false;
    if (count != s.transforms.size()) return false;
    s.attachmentParentBones.resize(count);
    s.attachmentSelfBones.resize(count);
    s.attachmentParentRest.resize(count);
    s.attachmentParentRestHeights.resize(count);
    s.attachmentSelfRest.resize(count);
    s.attachmentSelfRestHeights.resize(count);
    for (std::size_t i = 0; i < count; ++i) {
        std::int32_t parentBone{}, selfBone{}, px{}, pz{}, py{}, sx{}, sz{}, sy{};
        if (!r.i32(parentBone) || !r.i32(selfBone) || !r.i32(px) || !r.i32(pz)
            || !r.i32(py) || !r.i32(sx) || !r.i32(sz) || !r.i32(sy))
            return false;
        s.attachmentParentBones[i] = parentBone;
        s.attachmentSelfBones[i] = selfBone;
        s.attachmentParentRest[i] = {Fx::fromRaw(px), Fx::fromRaw(pz)};
        s.attachmentParentRestHeights[i] = Fx::fromRaw(py);
        s.attachmentSelfRest[i] = {Fx::fromRaw(sx), Fx::fromRaw(sz)};
        s.attachmentSelfRestHeights[i] = Fx::fromRaw(sy);
    }
    return true;
}

bool readCaptures(PayloadReader& r, std::vector<CaptureWork>& work) {
    std::size_t count{};
    if (!r.count(count, 1)) return false;
    work.resize(count);
    for (auto& value : work) {
        std::int32_t budget{}, progress{}, funded{};
        std::uint32_t captor{};
        std::uint8_t inReach{};
        if (!r.i32(value.armyIndex) || !r.u32(captor) || !readId(r, value.target)
            || !r.i32(budget) || !r.i32(progress) || !readResources(r, value.demand)
            || !r.i32(funded) || !r.u8(inReach) || inReach > 1 || budget <= 0
            || progress < 0 || progress > budget || funded < 0 || funded > kFxOne.raw()
            || value.demand.mass < Mag{} || value.demand.energy < Mag{}) return false;
        value.captor = captor;
        value.workTicks = budget;
        value.progress = progress;
        value.funded = Fx::fromRaw(funded);
        value.inReach = inReach != 0;
    }
    return true;
}

// V34's `CArmyStats` (`C-227`): the serialized per-army stat store — named aggregate
// and per-blueprint statistics plus the pending one-shot triggers. Null vs empty is
// preserved like `features`: a scene with no stat service writes only the absent byte.
void writeArmyStats(PayloadWriter& w,
                    const std::optional<std::vector<ArmyStats>>& state) {
    w.u8(state.has_value());
    if (!state) {
        return;
    }
    w.count(state->size());
    for (const ArmyStats& stats : *state) {
        w.count(stats.stats.size());
        for (const auto& [name, value] : stats.stats) {
            w.text(name);
            w.i64(value.raw());
        }
        w.count(stats.blueprintStats.size());
        for (const auto& [key, value] : stats.blueprintStats) {
            w.text(key.first);
            w.text(key.second);
            w.i64(value.raw());
        }
        w.count(stats.triggers.size());
        for (const ArmyStatTrigger& trigger : stats.triggers) {
            w.text(trigger.name);
            w.count(trigger.conditions.size());
            for (const ArmyStatCondition& condition : trigger.conditions) {
                w.text(condition.stat);
                w.text(condition.category);
                w.u8(static_cast<std::uint8_t>(condition.op));
                w.i64(condition.value.raw());
            }
        }
    }
}

bool readArmyStats(PayloadReader& r,
                   std::optional<std::vector<ArmyStats>>& state) {
    bool present{};
    if (!readFlag(r, present)) return false;
    if (!present) {
        state.reset();
        return true;
    }
    state.emplace();
    std::size_t count{};
    // 4 bytes of count per army at minimum; each stat row costs at least a length
    // word plus its value, each blueprint row two lengths plus a value, each trigger
    // a name and a condition count.
    if (!r.count(count, 4)) return false;
    state->resize(count);
    for (ArmyStats& stats : *state) {
        if (!r.count(count, 12)) return false;
        stats.stats.resize(count);
        for (auto& [name, value] : stats.stats) {
            if (!r.text(name) || !readMag(r, value)) return false;
        }
        if (!r.count(count, 16)) return false;
        stats.blueprintStats.resize(count);
        for (auto& [key, value] : stats.blueprintStats) {
            if (!r.text(key.first) || !r.text(key.second) || !readMag(r, value))
                return false;
        }
        if (!r.count(count, 8)) return false;
        stats.triggers.resize(count);
        for (ArmyStatTrigger& trigger : stats.triggers) {
            if (!r.text(trigger.name) || !r.count(count, 17)) return false;
            trigger.conditions.resize(count);
            for (ArmyStatCondition& condition : trigger.conditions) {
                std::uint8_t op{};
                if (!r.text(condition.stat) || !r.text(condition.category)
                    || !r.u8(op) || op > static_cast<std::uint8_t>(StatCompare::LessThanOrEqual)
                    || !readMag(r, condition.value)) return false;
                condition.op = static_cast<StatCompare>(op);
            }
        }
    }
    return true;
}

// V34's self-destruct countdowns (`C-345`): the unit handle and the ticks left.
void writeSelfDestructs(PayloadWriter& w, std::span<const SelfDestructWork> work) {
    w.count(work.size());
    for (const SelfDestructWork& value : work) {
        writeId(w, value.unit);
        w.u32(value.remainingTicks);
    }
}

bool readSelfDestructs(PayloadReader& r, std::vector<SelfDestructWork>& work) {
    std::size_t count{};
    if (!r.count(count, 12)) return false;
    work.resize(count);
    for (SelfDestructWork& value : work) {
        if (!readId(r, value.unit) || !r.u32(value.remainingTicks)) return false;
    }
    return true;
}

// V35's projectile pool: every field the state hash walks, in the same order
// that was saved. `visualId`/`visualOrigin`/`visualSerial` are deliberately
// absent — presentation identity, never hashed.
void writeProjectiles(PayloadWriter& w,
                      const std::optional<std::vector<Projectile>>& state,
                      bool waterFlags, bool weaveFlags) {
    w.u8(state.has_value());
    if (!state) {
        return;
    }
    w.count(state->size());
    for (const Projectile& shot : *state) {
        for (const Fx v : shot.position) w.i32(v.raw());
        for (const Fx v : shot.velocity) w.i32(v.raw());
        writeId(w, shot.guidanceTarget);
        w.i32(shot.turnPerTick);
        w.i32(shot.accelerationPerTickSquared.raw());
        w.i32(shot.maxSpeedPerTick.raw());
        w.i64(shot.damage.base.raw());
        w.u32(std::bit_cast<std::uint32_t>(shot.damage.paralyze.value));
        w.u8(shot.damage.overrideCount);
        for (std::uint8_t i = 0; i < shot.damage.overrideCount; ++i) {
            w.u8(shot.damage.overrideArmor[i]);
            w.i64(shot.damage.overrideDamage[i].raw());
        }
        w.i32(shot.damageRadiusElmos.raw());
        w.u8(static_cast<std::uint8_t>(shot.targetLayers));
        w.i32(shot.firedByArmy);
        w.u8(shot.interceptor ? 1 : 0);
        w.i64(shot.maxHealth.raw());
        w.i64(shot.health.raw());
        w.count(shot.categories.size());
        for (const std::string& tag : shot.categories) w.text(tag);
        writeId(w, shot.firedBy);
        w.u8(static_cast<std::uint8_t>(shot.arc));
        w.i32(shot.ticksRemaining);
        w.u8(static_cast<std::uint8_t>(shot.pendingImpact));
        writeId(w, shot.impactTarget);
        // V38 trails the record: `C-204`'s launch-time water pair.
        if (waterFlags) {
            w.u8(shot.stayUnderwater ? 1 : 0);
            w.u8(shot.destroyOnWater ? 1 : 0);
        }
        // V39 trails the record: `C-171`'s weave schedule and `C-088`'s
        // friendly-fire flag.
        if (weaveFlags) {
            w.i32(shot.zigZagAmplitudeElmos.raw());
            w.i32(shot.zigZagPeriodTicks);
            w.i32(shot.zigZagNextRoll);
            w.i32(shot.zigZagOffsetX.raw());
            w.i32(shot.zigZagOffsetY.raw());
            w.i32(shot.zigZagOffsetZ.raw());
            for (const Fx v : shot.zigZagApplied) w.i32(v.raw());
            for (const Fx v : shot.aimPoint) w.i32(v.raw());
            w.u8(shot.friendlyFire ? 1 : 0);
        }
    }
}

bool readProjectiles(PayloadReader& r,
                     std::optional<std::vector<Projectile>>& state,
                     bool waterFlags, bool weaveFlags) {
    bool present{};
    if (!readFlag(r, present)) return false;
    if (!present) {
        state.reset();
        return true;
    }
    state.emplace();
    std::size_t count{};
    // 4 bytes of count per shot at minimum; each shot costs at least its
    // fixed fields before any category text.
    if (!r.count(count, 60)) return false;
    state->resize(count);
    for (Projectile& shot : *state) {
        std::int32_t px{}, py{}, pz{}, vx{}, vy{}, vz{}, turn{}, accel{}, maxSpeed{},
            radius{}, army{}, ticks{};
        std::int64_t base{}, maxHealth{}, health{};
        std::uint32_t paralyzeBits{};
        std::uint8_t overrides{}, layers{}, interceptor{}, arc{}, impact{};
        for (std::int32_t* v : {&px, &py, &pz, &vx, &vy, &vz}) {
            if (!r.i32(*v)) return false;
        }
        if (!readId(r, shot.guidanceTarget) || !r.i32(turn) || !r.i32(accel)
            || !r.i32(maxSpeed) || !r.i64(base) || !r.u32(paralyzeBits)
            || !r.u8(overrides) || overrides > unitdef::DamageProfile::kMaxOverrides)
            return false;
        shot.position = {Fx::fromRaw(px), Fx::fromRaw(py), Fx::fromRaw(pz)};
        shot.velocity = {Fx::fromRaw(vx), Fx::fromRaw(vy), Fx::fromRaw(vz)};
        shot.turnPerTick = turn;
        shot.accelerationPerTickSquared = Fx::fromRaw(accel);
        shot.maxSpeedPerTick = Fx::fromRaw(maxSpeed);
        shot.damage.base = Mag::fromRaw(base);
        shot.damage.paralyze =
            Seconds{std::bit_cast<decltype(Seconds{}.value)>(paralyzeBits)};
        shot.damage.overrideCount = overrides;
        for (std::uint8_t i = 0; i < overrides; ++i) {
            std::uint8_t armor{};
            std::int64_t damage{};
            if (!r.u8(armor) || !r.i64(damage)) return false;
            shot.damage.overrideArmor[i] = armor;
            shot.damage.overrideDamage[i] = Mag::fromRaw(damage);
        }
        if (!r.i32(radius) || !r.u8(layers)
            || layers > static_cast<std::uint8_t>(unitdef::TargetLayerMask::Both)
            || !r.i32(army) || !r.u8(interceptor) || interceptor > 1
            || !r.i64(maxHealth) || !r.i64(health)) return false;
        shot.damageRadiusElmos = Fx::fromRaw(radius);
        shot.targetLayers = static_cast<unitdef::TargetLayerMask>(layers);
        shot.firedByArmy = army;
        shot.interceptor = interceptor != 0;
        shot.maxHealth = Mag::fromRaw(maxHealth);
        shot.health = Mag::fromRaw(health);
        if (!r.count(count, 1)) return false;
        shot.categories.resize(count);
        for (std::string& tag : shot.categories) {
            if (!r.text(tag)) return false;
        }
        std::sort(shot.categories.begin(), shot.categories.end());
        if (!readId(r, shot.firedBy) || !r.u8(arc)
            || arc > static_cast<std::uint8_t>(unitdef::BallisticArc::High)
            || !r.i32(ticks) || !r.u8(impact)
            || impact > static_cast<std::uint8_t>(ImpactType::UnitUnderwater)
            || !readId(r, shot.impactTarget)) return false;
        shot.arc = static_cast<unitdef::BallisticArc>(arc);
        shot.ticksRemaining = ticks;
        shot.pendingImpact = static_cast<ImpactType>(impact);
        // V38's trailing water pair; older saves leave both flags false,
        // which is what a pre-C-204 shot was anyway.
        if (waterFlags) {
            std::uint8_t stay{}, destroy{};
            if (!r.u8(stay) || stay > 1 || !r.u8(destroy) || destroy > 1) {
                return false;
            }
            shot.stayUnderwater = stay != 0;
            shot.destroyOnWater = destroy != 0;
        }
        // V39's trailing weave schedule and friendly-fire flag; older saves
        // leave the shot un-weaving and hostile-only, which is what a
        // pre-C-171/pre-C-088 shot was anyway.
        if (weaveFlags) {
            std::int32_t amplitude{}, period{}, nextRoll{}, ox{}, oy{}, oz{};
            std::uint8_t friendly{};
            if (!r.i32(amplitude) || !r.i32(period) || !r.i32(nextRoll)
                || !r.i32(ox) || !r.i32(oy) || !r.i32(oz)) {
                return false;
            }
            shot.zigZagAmplitudeElmos = Fx::fromRaw(amplitude);
            shot.zigZagPeriodTicks = period;
            shot.zigZagNextRoll = nextRoll;
            shot.zigZagOffsetX = Fx::fromRaw(ox);
            shot.zigZagOffsetY = Fx::fromRaw(oy);
            shot.zigZagOffsetZ = Fx::fromRaw(oz);
            for (Fx& v : shot.zigZagApplied) {
                std::int32_t raw{};
                if (!r.i32(raw)) return false;
                v = Fx::fromRaw(raw);
            }
            for (Fx& v : shot.aimPoint) {
                std::int32_t raw{};
                if (!r.i32(raw)) return false;
                v = Fx::fromRaw(raw);
            }
            if (!r.u8(friendly) || friendly > 1) return false;
            shot.friendlyFire = friendly != 0;
        }
    }
    return true;
}

// --- V37: path service and intel history --------------------------------------
//
// A saved FlowField is (grid fingerprint, goal, overlay, expansion count,
// stale flag): the deterministic Dijkstra replays the count on restore and
// reproduces the exact frontier — costs, next pointers, closed/touched maps
// and the open-heap order the state hash feeds. Requests drop their grid
// pointer; the caller rebinds it by unit on restore.

void writeSavedRequest(PayloadWriter& w, const PathService::Snapshot::SavedRequest& r) {
    writeId(w, r.unit);
    w.u64(r.command);
    w.i32(r.army);
    w.i32(r.fromX.raw());
    w.i32(r.fromZ.raw());
    w.i32(r.targetX.raw());
    w.i32(r.targetZ.raw());
}

[[nodiscard]] bool readSavedRequest(PayloadReader& r,
                                    PathService::Snapshot::SavedRequest& out) {
    std::uint64_t command{};
    std::int32_t army{}, fx{}, fz{}, tx{}, tz{};
    if (!readId(r, out.unit) || !r.u64(command) || !r.i32(army) || !r.i32(fx)
        || !r.i32(fz) || !r.i32(tx) || !r.i32(tz)) {
        return false;
    }
    out.command = static_cast<CommandId>(command);
    out.army = army;
    out.fromX = Fx::fromRaw(fx);
    out.fromZ = Fx::fromRaw(fz);
    out.targetX = Fx::fromRaw(tx);
    out.targetZ = Fx::fromRaw(tz);
    return true;
}

void writeRequestQueue(PayloadWriter& w,
                       const std::deque<PathService::Snapshot::SavedRequest>& queue) {
    w.count(queue.size());
    for (const auto& request : queue) {
        writeSavedRequest(w, request);
    }
}

void writePathService(PayloadWriter& w, const std::optional<PathService::Snapshot>& state) {
    w.u8(state.has_value());
    if (!state) {
        return;
    }
    w.count(state->admissions.size());
    for (const auto& queue : state->admissions) writeRequestQueue(w, queue);
    w.count(state->pending.size());
    for (const auto& queue : state->pending) writeRequestQueue(w, queue);
    w.count(state->activeRequests.size());
    for (const auto& request : state->activeRequests) {
        w.u8(request.has_value());
        if (request) writeSavedRequest(w, *request);
    }
    w.count(state->activeFields.size());
    for (const auto& field : state->activeFields) {
        w.i32(field.army);
        w.u64(field.gridFingerprint);
        w.i32(field.goalX);
        w.i32(field.goalZ);
        w.i32(field.startCell);
    }
    w.count(state->retryWaits.size());
    for (std::size_t wait : state->retryWaits) w.u64(wait);
    w.count(state->failureCounts.size());
    for (std::size_t count : state->failureCounts) w.u64(count);
    w.u64(state->serviceBeats);
    w.count(state->fields.size());
    for (const auto& field : state->fields) {
        w.u64(field.grid);
        w.i32(field.goalX);
        w.i32(field.goalZ);
        w.u64(field.used);
        w.u64(field.closed);
        w.u8(field.stale ? 1 : 0);
        w.count(field.blocked.size());
        for (std::uint8_t cell : field.blocked) w.u8(cell);
    }
    w.count(state->blocked.size());
    for (const auto& [fingerprint, layer] : state->blocked) {
        w.u64(fingerprint);
        w.count(layer.size());
        for (std::uint8_t cell : layer) w.u8(cell);
    }
    w.u64(state->fieldClock);
}

[[nodiscard]] bool readRequestQueue(
    PayloadReader& r, std::deque<PathService::Snapshot::SavedRequest>& queue) {
    std::size_t count{};
    if (!r.count(count, 28)) return false;
    for (std::size_t i = 0; i < count; ++i) {
        PathService::Snapshot::SavedRequest request;
        if (!readSavedRequest(r, request)) return false;
        queue.push_back(request);
    }
    return true;
}

[[nodiscard]] bool readPathService(PayloadReader& r,
                                   std::optional<PathService::Snapshot>& state) {
    bool present{};
    if (!readFlag(r, present)) return false;
    if (!present) {
        state.reset();
        return true;
    }
    state.emplace();
    std::size_t armies{};
    if (!r.count(armies, 4)) return false;
    state->admissions.resize(armies);
    for (auto& queue : state->admissions) {
        if (!readRequestQueue(r, queue)) return false;
    }
    if (!r.count(armies, 4)) return false;
    state->pending.resize(armies);
    for (auto& queue : state->pending) {
        if (!readRequestQueue(r, queue)) return false;
    }
    if (!r.count(armies, 4)) return false;
    state->activeRequests.resize(armies);
    for (auto& request : state->activeRequests) {
        bool has{};
        if (!readFlag(r, has)) return false;
        if (has) {
            PathService::Snapshot::SavedRequest saved;
            if (!readSavedRequest(r, saved)) return false;
            request = saved;
        }
    }
    std::size_t fields{};
    if (!r.count(fields, 24)) return false;
    state->activeFields.resize(fields);
    for (auto& field : state->activeFields) {
        if (!r.i32(field.army) || !r.u64(field.gridFingerprint) || !r.i32(field.goalX)
            || !r.i32(field.goalZ) || !r.i32(field.startCell)) {
            return false;
        }
    }
    std::size_t counters{};
    if (!r.count(counters, 8)) return false;
    state->retryWaits.resize(counters);
    for (std::size_t& wait : state->retryWaits) {
        std::uint64_t value{};
        if (!r.u64(value)) return false;
        wait = static_cast<std::size_t>(value);
    }
    if (!r.count(counters, 8)) return false;
    state->failureCounts.resize(counters);
    for (std::size_t& count : state->failureCounts) {
        std::uint64_t value{};
        if (!r.u64(value)) return false;
        count = static_cast<std::size_t>(value);
    }
    if (!r.u64(state->serviceBeats)) return false;
    if (!r.count(fields, 32)) return false;
    state->fields.resize(fields);
    for (auto& field : state->fields) {
        std::uint64_t closed{};
        std::uint8_t stale{};
        if (!r.u64(field.grid) || !r.i32(field.goalX) || !r.i32(field.goalZ)
            || !r.u64(field.used) || !r.u64(closed) || !r.u8(stale) || stale > 1) {
            return false;
        }
        field.closed = static_cast<std::size_t>(closed);
        field.stale = stale != 0;
        std::size_t cells{};
        if (!r.count(cells, 1)) return false;
        field.blocked.resize(cells);
        for (std::uint8_t& cell : field.blocked) {
            if (!r.u8(cell)) return false;
        }
    }
    std::size_t layers{};
    if (!r.count(layers, 12)) return false;
    state->blocked.resize(layers);
    for (auto& [fingerprint, layer] : state->blocked) {
        if (!r.u64(fingerprint)) return false;
        std::size_t cells{};
        if (!r.count(cells, 1)) return false;
        layer.resize(cells);
        for (std::uint8_t& cell : layer) {
            if (!r.u8(cell)) return false;
        }
    }
    return r.u64(state->fieldClock);
}

void writeIntel(PayloadWriter& w, const std::optional<Intel::Snapshot>& state) {
    w.u8(state.has_value());
    if (!state) {
        return;
    }
    w.count(state->retained.size());
    for (const auto& contacts : state->retained) {
        w.count(contacts.size());
        for (const RetainedRadarContact& contact : contacts) {
            writeId(w, contact.unit);
            w.i32(contact.x.raw());
            w.i32(contact.z.raw());
            w.u8(contact.maybeDead ? 1 : 0);
            w.u32(contact.deadTicks);
        }
    }
    w.count(state->seenEver.size());
    for (const auto& seen : state->seenEver) {
        w.count(seen.size());
        for (UnitId unit : seen) writeId(w, unit);
    }
    w.count(state->intelRecovery.size());
    for (TickCount count : state->intelRecovery) w.u32(count);
    w.count(state->intelRecoveryUnit.size());
    for (UnitId unit : state->intelRecoveryUnit) writeId(w, unit);
}

[[nodiscard]] bool readIntel(PayloadReader& r, std::optional<Intel::Snapshot>& state) {
    bool present{};
    if (!readFlag(r, present)) return false;
    if (!present) {
        state.reset();
        return true;
    }
    state.emplace();
    std::size_t alliances{};
    if (!r.count(alliances, 4)) return false;
    state->retained.resize(alliances);
    for (auto& contacts : state->retained) {
        std::size_t count{};
        if (!r.count(count, 17)) return false;
        contacts.resize(count);
        for (RetainedRadarContact& contact : contacts) {
            std::int32_t x{}, z{};
            std::uint8_t dead{};
            if (!readId(r, contact.unit) || !r.i32(x) || !r.i32(z) || !r.u8(dead)
                || dead > 1 || !r.u32(contact.deadTicks)) {
                return false;
            }
            contact.x = Fx::fromRaw(x);
            contact.z = Fx::fromRaw(z);
            contact.maybeDead = dead != 0;
        }
    }
    if (!r.count(alliances, 4)) return false;
    state->seenEver.resize(alliances);
    for (auto& seen : state->seenEver) {
        std::size_t count{};
        if (!r.count(count, 8)) return false;
        seen.resize(count);
        for (UnitId& unit : seen) {
            if (!readId(r, unit)) return false;
        }
    }
    std::size_t slots{};
    if (!r.count(slots, 4)) return false;
    state->intelRecovery.resize(slots);
    for (TickCount& count : state->intelRecovery) {
        if (!r.u32(count)) return false;
    }
    if (!r.count(slots, 8)) return false;
    state->intelRecoveryUnit.resize(slots);
    for (UnitId& unit : state->intelRecoveryUnit) {
        if (!readId(r, unit)) return false;
    }
    return true;
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

void writeSubMotion(PayloadWriter& w, std::span<const MoveState> motion) {
    w.count(motion.size());
    for (const auto& state : motion) {
        w.u8(state.submersible);
        w.u8(state.submerged);
        w.u8(state.diveTargetSubmerged);
        w.i32(state.submarineOffset.raw());
        w.i32(state.submarineElevation.raw());
        w.i32(state.divePerTick.raw());
    }
}

void writeTurretPose(PayloadWriter& w, std::span<const MoveState> motion) {
    w.count(motion.size());
    for (const auto& state : motion) {
        // Signed, matching the reader's range check: a `Brad` past half a turn
        // is the negative of the same angle, and the cast below wraps it back.
        w.i32(static_cast<std::int16_t>(state.turretYaw));
        w.i32(static_cast<std::int16_t>(state.turretPitch));
        w.u8(state.turretMuzzlePhase);
    }
}

bool readTurretPose(PayloadReader& r, std::vector<MoveState>& motion) {
    std::size_t count{};
    if (!r.count(count, 9) || count != motion.size()) return false;
    for (auto& state : motion) {
        std::int32_t yaw{}, pitch{};
        std::uint8_t phase{};
        if (!r.i32(yaw) || !r.i32(pitch) || !r.u8(phase) || phase > 1
            || yaw < -32768 || yaw > 32767 || pitch < -32768 || pitch > 32767) return false;
        state.turretYaw = static_cast<Brad>(yaw);
        state.turretPitch = static_cast<Brad>(pitch);
        state.turretMuzzlePhase = phase;
    }
    return true;
}

void writeTurretPoseDual(PayloadWriter& w, std::span<const MoveState> motion) {
    w.count(motion.size());
    for (const auto& state : motion) {
        w.i32(static_cast<std::int16_t>(state.turretYaw2));
        w.i32(static_cast<std::int16_t>(state.turretPitch2));
    }
}

bool readTurretPoseDual(PayloadReader& r, std::vector<MoveState>& motion) {
    std::size_t count{};
    if (!r.count(count, 8) || count != motion.size()) return false;
    for (auto& state : motion) {
        std::int32_t yaw{}, pitch{};
        if (!r.i32(yaw) || !r.i32(pitch)
            || yaw < -32768 || yaw > 32767 || pitch < -32768 || pitch > 32767) return false;
        state.turretYaw2 = static_cast<Brad>(yaw);
        state.turretPitch2 = static_cast<Brad>(pitch);
    }
    return true;
}

void writeCongestion(PayloadWriter& w, std::span<const MoveState> motion) {
    w.count(motion.size());
    for (const auto& state : motion) {
        w.u16(state.blockedTicks);
        w.u8(state.yielding);
        w.i32(state.lastGoalDistance.raw());
    }
}

bool readCongestion(PayloadReader& r, std::vector<MoveState>& motion) {
    std::size_t count{};
    if (!r.count(count, 7) || count != motion.size()) return false;
    for (auto& state : motion) {
        std::uint16_t blocked{};
        std::uint8_t yielding{};
        std::int32_t anchor{};
        if (!r.u16(blocked) || !r.u8(yielding) || yielding > 1 || !r.i32(anchor)) {
            return false;
        }
        state.blockedTicks = blocked;
        state.yielding = yielding != 0;
        state.lastGoalDistance = Fx::fromRaw(anchor);
    }
    return true;
}

void writeLeadStep(PayloadWriter& w, std::span<const MoveState> motion) {
    w.count(motion.size());
    for (const auto& state : motion) {
        w.i32(state.stepX.raw());
        w.i32(state.stepZ.raw());
    }
}

bool readLeadStep(PayloadReader& r, std::vector<MoveState>& motion) {
    std::size_t count{};
    if (!r.count(count, 8) || count != motion.size()) return false;
    for (auto& state : motion) {
        std::int32_t x{}, z{};
        if (!r.i32(x) || !r.i32(z)) return false;
        state.stepX = Fx::fromRaw(x);
        state.stepZ = Fx::fromRaw(z);
    }
    return true;
}

bool readSubMotion(PayloadReader& r, std::vector<MoveState>& motion) {
    std::size_t count{};
    if (!r.count(count, 15) || count != motion.size()) return false;
    for (auto& state : motion) {
        std::uint8_t enabled{}, submerged{}, target{};
        std::int32_t offset{}, elevation{}, speed{};
        if (!r.u8(enabled) || enabled > 1 || !r.u8(submerged) || submerged > 1
            || !r.u8(target) || target > 1 || !r.i32(offset) || !r.i32(elevation)
            || !r.i32(speed) || offset > 0 || elevation > 0 || speed < 0
            || (enabled && !state.surfaceWater)
            || (!enabled && (submerged || target || offset || elevation || speed))) return false;
        state.submersible = enabled;
        state.submerged = submerged;
        state.diveTargetSubmerged = target;
        state.submarineOffset = Fx::fromRaw(offset);
        state.submarineElevation = Fx::fromRaw(elevation);
        state.divePerTick = Fx::fromRaw(speed);
    }
    return true;
}

void writeAirController(PayloadWriter& w, std::span<const MoveState> motion, bool bank) {
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
        if (bank) {
            w.i32(state.airKRoll.raw());
            w.i32(state.airBankFactor.raw());
        }
    }
}

bool readAirController(PayloadReader& r, std::vector<MoveState>& motion, bool bank) {
    std::size_t count{};
    const std::size_t kRecordBytes =
        8 + 5 * 4 + 3 + kAirControllerFx.size() * 4 + (bank ? 2 * 4 : 0);
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
        if (bank) {
            std::int32_t roll{}, factor{};
            if (!r.i32(roll) || !r.i32(factor)) return false;
            state.airKRoll = Fx::fromRaw(roll);
            state.airBankFactor = Fx::fromRaw(factor);
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

[[nodiscard]] bool readSiloAmmo(PayloadReader& r, std::vector<SiloAmmo>& ammo,
                                bool includesSiloQueue) {
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
        std::uint8_t autoBuild = 1;
        if (includesSiloQueue && !r.u8(autoBuild)) return false;
        if (autoBuild > 1) return false;
        value.autoBuild = autoBuild != 0;
    }
    return true;
}

[[nodiscard]] bool readSiloQueue(PayloadReader& r, std::vector<SiloBuild>& queue) {
    std::size_t count{};
    if (!r.count(count, 8)) return false;
    queue.resize(count);
    for (SiloBuild& entry : queue) {
        if (!readId(r, entry.owner) || !r.u8(entry.slot) || entry.slot > 1) return false;
    }
    return true;
}

[[nodiscard]] bool readCommandState(PayloadReader& r, UnitStore::Snapshot& s,
                                    bool includesScriptTasks, bool includesGuard,
                                    bool includesReclaimUnit, bool includesCapture,
                                    bool includesMissileLaunch, bool includesTransport) {
    if (!r.u32(s.nextCommandSerial)) return false;
    for (std::uint32_t& counter : s.nextCommandCounters) if (!r.u32(counter)) return false;
    std::size_t count{};
    if (!r.count(count, 1)) return false;
    s.sharedCommands.resize(count);
    for (SharedCommand& command : s.sharedCommands) {
        if (!readSharedCommand(r, command, includesScriptTasks, includesGuard,
                               includesReclaimUnit, includesCapture, includesMissileLaunch,
                               includesTransport)) {
            return false;
        }
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
            if (includesTransport) {
                std::uint8_t hasAnchor{}, phase{};
                if (!r.u8(hasAnchor) || hasAnchor > 1) return false;
                if (hasAnchor) {
                    std::int32_t anchorX{}, anchorZ{};
                    if (!r.i32(anchorX) || !r.i32(anchorZ)) return false;
                    entry.execution.transportAnchor =
                        {Fx::fromRaw(anchorX), Fx::fromRaw(anchorZ)};
                }
                if (!r.u8(phase)
                    || phase > static_cast<std::uint8_t>(TransportPhase::ToDrop)) {
                    return false;
                }
                entry.execution.transportPhase = static_cast<TransportPhase>(phase);
            }
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
                   bool includesScriptTasks, bool includesProductionPaused,
                   bool includesBuildPriority, bool includesRetreat,
                   bool includesTransport, bool includesFocus, bool includesScriptBits) {
    w.count(s.ids.generations.size()); for (Generation v : s.ids.generations) w.u32(v);
    w.count(s.ids.free.size()); for (UnitIndex v : s.ids.free) w.u32(v);
    w.u64(s.ids.live);
    w.count(s.generations.size()); for (Generation v : s.generations) w.u32(v);
    w.count(s.transforms.size()); for (const Transform& v : s.transforms) { w.i32(v.x.raw()); w.i32(v.y.raw()); w.i32(v.z.raw()); w.u16(v.heading); w.u16(v.pitch); w.u16(v.roll); }
    w.count(s.motion.size()); for (const MoveState& v : s.motion) { w.i32(v.armyIndex); w.i32(v.destinationX.raw()); w.i32(v.destinationZ.raw()); w.u8(v.moving); if (includesAttachedMotion) w.u8(v.attached); w.u8(v.airborne); w.u8(v.surfaceWater); w.i32(v.speedPerTick.raw()); w.i32(v.turnPerTick); w.i32(v.radiusElmos.raw()); w.i32(v.distanceTravelledElmos.raw()); w.count(v.path.size()); for (const auto& p : v.path) { w.i32(p[0].raw()); w.i32(p[1].raw()); } w.u64(v.pathIndex); if (includesPathPhase) { w.i32(v.pathPhaseStartX); w.i32(v.pathPhaseStartZ); w.i32(v.pathPhaseCellsX); } }
    w.count(s.health.size()); for (const Health& v : s.health) { w.i64(v.current.raw()); w.i64(v.maximum.raw()); w.i64(v.shield.current.raw()); w.i64(v.shield.maximum.raw()); w.u32(v.shield.regenDelayRemaining); w.u32(v.shield.rechargeRemaining); if (includesScriptBits) w.u8(v.shield.rechargeRestoresFull); w.count(v.reloadRemaining.size()); for (int x : v.reloadRemaining) w.i32(x); w.count(v.burstRemaining.size()); for (int x : v.burstRemaining) w.i32(x); if (includesAutomaticTargets) { w.count(v.automaticTargets.size()); for (UnitId target : v.automaticTargets) writeId(w, target); } writeId(w, v.lastHitBy); w.i32(v.veterancy.kills); w.i32(v.veterancy.level); }
    if (includesScriptBits) { w.count(s.scriptBitsDisabled.size()); for (std::uint16_t v : s.scriptBitsDisabled) w.u16(v); w.count(s.maintenanceActive.size()); for (bool v : s.maintenanceActive) w.u8(v); }
    w.count(s.types.size()); for (UnitTypeIndex v : s.types) w.u16(v);
    if (includesFactoryRepeat) { w.count(s.factoryRepeat.size()); for (bool v : s.factoryRepeat) w.u8(v); }
    w.count(s.parents.size()); for (const auto& v : s.parents) { w.u8(v.has_value()); if (v) writeId(w, *v); }
    w.count(s.children.size()); for (const auto& list : s.children) { w.count(list.size()); for (UnitId id : list) writeId(w, id); }
    if (includesAttachmentOffsets) { w.count(s.attachmentOffsets.size()); for (const auto& offset : s.attachmentOffsets) { w.i32(offset[0].raw()); w.i32(offset[1].raw()); } }
    if (includesAttachmentHeights) { w.count(s.attachmentHeights.size()); for (Fx height : s.attachmentHeights) w.i32(height.raw()); }
    if (includesDoNotTarget) { w.count(s.doNotTarget.size()); for (bool v : s.doNotTarget) w.u8(v); }
    if (includesProductionPaused) { w.count(s.productionPaused.size()); for (bool v : s.productionPaused) w.u8(v); }
    if (includesBuildPriority) { w.count(s.buildPriority.size()); for (BuildPriority v : s.buildPriority) w.u8(static_cast<std::uint8_t>(v)); }
    if (includesRetreat) { w.count(s.retreatThreshold.size()); for (RetreatThreshold v : s.retreatThreshold) w.u8(static_cast<std::uint8_t>(v)); w.count(s.retreats.size()); for (const RetreatState& v : s.retreats) { w.u8(v.active); w.i32(v.returnX.raw()); w.i32(v.returnZ.raw()); w.i32(v.toX.raw()); w.i32(v.toZ.raw()); } }
    if (includesFocus) { w.count(s.targetFocus.size()); for (TargetFocus v : s.targetFocus) w.u8(static_cast<std::uint8_t>(v)); }
    if (includesCommands) writeCommandState(w, s, includesScriptTasks, includesTransport);
}

/// Reads the v36 `rechargeRestoresFull` byte into `v.shield`. A helper because
/// the health loop is one expression; a byte over 1 is malformed.
[[nodiscard]] bool readShieldRestoreFlag(PayloadReader& r, Health& v) {
    std::uint8_t flag{};
    if (!r.u8(flag) || flag > 1) return false;
    v.shield.rechargeRestoresFull = flag != 0;
    return true;
}

[[nodiscard]] bool readUnits(PayloadReader& r, UnitStore::Snapshot& s, bool includesPathPhase,
                                 bool includesFactoryRepeat, bool includesAttachmentOffsets,
                                 bool includesDoNotTarget, bool includesAutomaticTargets,
                                 bool includesAttachmentHeights, bool includesAttachedMotion,
                                 bool includesCommands, bool includesScriptTasks, bool includesGuard,
                                 bool includesReclaimUnit, bool includesCapture,
                                 bool includesProductionPaused, bool includesMissileLaunch,
                                 bool includesBuildPriority, bool includesRetreat,
                                 bool includesTransport, bool includesFocus, bool includesScriptBits) {
    std::size_t n{};
    if (!r.count(n, 4)) return false; s.ids.generations.resize(n); for (auto& v : s.ids.generations) if (!r.u32(v)) return false;
    if (!r.count(n, 4)) return false; s.ids.free.resize(n); for (auto& v : s.ids.free) if (!r.u32(v)) return false;
    std::uint64_t live{};
    if (!r.u64(live) || live > std::numeric_limits<std::size_t>::max() || !r.count(n, 4)) return false;
    s.ids.live = static_cast<std::size_t>(live);
    s.generations.resize(n); for (auto& v : s.generations) if (!r.u32(v)) return false;
    if (!r.count(n, 18)) return false; s.transforms.resize(n); for (auto& v : s.transforms) { std::int32_t x{},y{},z{}; if (!r.i32(x)||!r.i32(y)||!r.i32(z)||!r.u16(v.heading)||!r.u16(v.pitch)||!r.u16(v.roll)) return false; v.x=Fx::fromRaw(x); v.y=Fx::fromRaw(y); v.z=Fx::fromRaw(z); }
    if (!r.count(n, (includesPathPhase ? 58 : 46) + (includesAttachedMotion ? 1 : 0))) return false; s.motion.resize(n); for (auto& v : s.motion) { std::int32_t x{},z{},speed{},radius{},distance{}; std::uint8_t moving{},attached{},airborne{},water{}; if (!r.i32(v.armyIndex)||!r.i32(x)||!r.i32(z)||!r.u8(moving)||(includesAttachedMotion && !r.u8(attached))||!r.u8(airborne)||!r.u8(water)||moving>1||attached>1||airborne>1||water>1||!r.i32(speed)||!r.i32(v.turnPerTick)||!r.i32(radius)||!r.i32(distance)||!r.count(n,8)) return false; v.destinationX=Fx::fromRaw(x); v.destinationZ=Fx::fromRaw(z); v.moving=moving; v.attached=attached; v.airborne=airborne; v.surfaceWater=water; v.speedPerTick=Fx::fromRaw(speed); v.radiusElmos=Fx::fromRaw(radius); v.distanceTravelledElmos=Fx::fromRaw(distance); v.path.resize(n); for(auto& p:v.path){if(!r.i32(x)||!r.i32(z))return false;p={Fx::fromRaw(x),Fx::fromRaw(z)};} std::uint64_t index{}; if(!r.u64(index)||index>std::numeric_limits<std::size_t>::max())return false; v.pathIndex=static_cast<std::size_t>(index); if (includesPathPhase && (!r.i32(v.pathPhaseStartX) || !r.i32(v.pathPhaseStartZ) || !r.i32(v.pathPhaseCellsX))) return false; }
    if (!r.count(n, 52)) return false; s.health.resize(n); for (auto& v : s.health) { std::int64_t a{},b{},c{},d{}; if(!r.i64(a)||!r.i64(b)||!r.i64(c)||!r.i64(d)||!r.u32(v.shield.regenDelayRemaining)||!r.u32(v.shield.rechargeRemaining)||(includesScriptBits && !readShieldRestoreFlag(r, v))||!r.count(n,4))return false; v.current=Mag::fromRaw(a);v.maximum=Mag::fromRaw(b);v.shield.current=Mag::fromRaw(c);v.shield.maximum=Mag::fromRaw(d);v.reloadRemaining.resize(n);for(auto& x:v.reloadRemaining)if(!r.i32(x))return false;if(!r.count(n,4))return false;v.burstRemaining.resize(n);for(auto& x:v.burstRemaining)if(!r.i32(x))return false;if (includesAutomaticTargets) { if (!r.count(n, 8)) return false; v.automaticTargets.resize(n); for (auto& target : v.automaticTargets) if (!readId(r, target)) return false; } if(!readId(r,v.lastHitBy)||!r.i32(v.veterancy.kills)||!r.i32(v.veterancy.level))return false; }
    if (includesScriptBits) { if (!r.count(n, 2)) return false; s.scriptBitsDisabled.resize(n); for (auto& v : s.scriptBitsDisabled) if (!r.u16(v)) return false; if (!r.count(n, 1)) return false; s.maintenanceActive.resize(n); for (auto&& v : s.maintenanceActive) { std::uint8_t enabled{}; if (!r.u8(enabled) || enabled > 1) return false; v = enabled; } }
    if (!r.count(n,2)) return false; s.types.resize(n); for(auto& v:s.types)if(!r.u16(v))return false;
    if (includesFactoryRepeat) { if (!r.count(n, 1)) return false; s.factoryRepeat.resize(n); for (auto&& v : s.factoryRepeat) { std::uint8_t enabled{}; if (!r.u8(enabled) || enabled > 1) return false; v = enabled; } }
    if (!r.count(n,1)) return false; s.parents.resize(n); for(auto& v:s.parents){std::uint8_t has{};if(!r.u8(has)||has>1)return false;if(has){UnitId id;if(!readId(r,id))return false;v=id;}}
    if (!r.count(n,4)) return false; s.children.resize(n); for(auto& list:s.children){if(!r.count(n,8))return false;list.resize(n);for(auto& id:list)if(!readId(r,id))return false;}
    if (includesAttachmentOffsets) { if (!r.count(n, 8)) return false; s.attachmentOffsets.resize(n); for (auto& offset : s.attachmentOffsets) { std::int32_t x{}, z{}; if (!r.i32(x) || !r.i32(z)) return false; offset = {Fx::fromRaw(x), Fx::fromRaw(z)}; } }
    if (includesAttachmentHeights) { if (!r.count(n, 4)) return false; s.attachmentHeights.resize(n); for (Fx& height : s.attachmentHeights) { std::int32_t raw{}; if (!r.i32(raw)) return false; height = Fx::fromRaw(raw); } }
    if (includesDoNotTarget) { if (!r.count(n, 1)) return false; s.doNotTarget.resize(n); for (auto&& v : s.doNotTarget) { std::uint8_t enabled{}; if (!r.u8(enabled) || enabled > 1) return false; v = enabled; } }
    if (includesProductionPaused) { if (!r.count(n, 1)) return false; s.productionPaused.resize(n); for (auto&& v : s.productionPaused) { std::uint8_t enabled{}; if (!r.u8(enabled) || enabled > 1) return false; v = enabled; } }
    if (includesBuildPriority) { if (!r.count(n, 1)) return false; s.buildPriority.resize(n); for (auto& v : s.buildPriority) { std::uint8_t tier{}; if (!r.u8(tier) || tier > static_cast<std::uint8_t>(BuildPriority::High)) return false; v = static_cast<BuildPriority>(tier); } }
    if (includesRetreat) { if (!r.count(n, 1)) return false; s.retreatThreshold.resize(n); for (auto& v : s.retreatThreshold) { std::uint8_t tier{}; if (!r.u8(tier) || tier > static_cast<std::uint8_t>(RetreatThreshold::High)) return false; v = static_cast<RetreatThreshold>(tier); } if (!r.count(n, 4)) return false; s.retreats.resize(n); for (auto& v : s.retreats) { std::uint8_t active{}; std::int32_t x{}, z{}, tx{}, tz{}; if (!r.u8(active) || active > 1 || !r.i32(x) || !r.i32(z) || !r.i32(tx) || !r.i32(tz)) return false; v = RetreatState{.active = active != 0, .returnX = Fx::fromRaw(x), .returnZ = Fx::fromRaw(z), .toX = Fx::fromRaw(tx), .toZ = Fx::fromRaw(tz)}; } }
    if (includesFocus) { if (!r.count(n, 1)) return false; s.targetFocus.resize(n); for (auto& v : s.targetFocus) { std::uint8_t focus{}; if (!r.u8(focus) || focus > static_cast<std::uint8_t>(TargetFocus::EconomyOnly)) return false; v = static_cast<TargetFocus>(focus); } }
    const std::size_t slots=s.transforms.size();
    if (s.ids.generations.size()!=slots || s.generations.size()!=slots || s.motion.size()!=slots
         || s.health.size()!=slots || s.types.size()!=slots
          || (includesFactoryRepeat && s.factoryRepeat.size()!=slots) || s.parents.size()!=slots
           || s.children.size()!=slots || (includesAttachmentOffsets && s.attachmentOffsets.size()!=slots)
           || (includesAttachmentHeights && s.attachmentHeights.size()!=slots)
          || (includesDoNotTarget && s.doNotTarget.size()!=slots)
           || (includesProductionPaused && s.productionPaused.size()!=slots)
           || (includesBuildPriority && s.buildPriority.size()!=slots)
           || (includesScriptBits && (s.scriptBitsDisabled.size()!=slots || s.maintenanceActive.size()!=slots))
           || (includesRetreat && (s.retreatThreshold.size()!=slots || s.retreats.size()!=slots))
           || (includesFocus && s.targetFocus.size()!=slots)
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
    if (!includesProductionPaused) s.productionPaused.resize(slots, false);
    if (!includesBuildPriority) s.buildPriority.resize(slots, BuildPriority::Normal);
    if (!includesScriptBits) { s.scriptBitsDisabled.resize(slots, 0); s.maintenanceActive.resize(slots, true); }
    if (!includesRetreat) { s.retreatThreshold.resize(slots, RetreatThreshold::Off); s.retreats.resize(slots); }
    if (!includesFocus) { s.targetFocus.resize(slots, TargetFocus::Default); }
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
        && (!readCommandState(r, s, includesScriptTasks, includesGuard, includesReclaimUnit,
                              includesCapture, includesMissileLaunch, includesTransport)
            || s.orders.size() != slots)) {
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
                    version >= kVersion12, version >= kVersion26, version >= kVersion27,
                    version >= kVersion28, version >= kVersion29, version >= kVersion30,
                    version >= kVersion36);
    if (version >= kVersion9)
        writeSiloAmmo(payloadWriter, state.siloAmmo, version >= kVersion33);
    if (version >= kVersion10) writeRedirects(payloadWriter, state.redirects);
    if (version >= kVersion11) {
        writeAirMotion(payloadWriter, state.units.motion, version >= kVersion13,
                       version >= kVersion14);
    }
    if (version >= kVersion15) writeEconomyArmies(payloadWriter, state.economyArmies,
                                                  version >= kVersion40);
    if (version >= kVersion16)
        writeAirController(payloadWriter, state.units.motion, version >= kVersion21);
    if (version >= kVersion18) writeSubMotion(payloadWriter, state.units.motion);
    if (version >= kVersion19) {
        writeEnhancements(payloadWriter, state.enhancements);
        writeInstalledEnhancements(payloadWriter, state.units);
    }
    if (version >= kVersion20) {
        writeCaptures(payloadWriter, state.captures);
    }
    if (version >= kVersion22) {
        writeBoneAttachments(payloadWriter, state.units);
    }
    if (version >= kVersion23) {
        writeFeatures(payloadWriter, state.features);
    }
    if (version >= kVersion24) writeTurretPose(payloadWriter, state.units.motion);
    if (version >= kVersion25) writeTurretPoseDual(payloadWriter, state.units.motion);
    if (version >= kVersion31) writeCongestion(payloadWriter, state.units.motion);
    if (version >= kVersion32) writeLeadStep(payloadWriter, state.units.motion);
    if (version >= kVersion33) writeSiloQueue(payloadWriter, state.siloQueue);
    if (version >= kVersion34) writeArmyStats(payloadWriter, state.armyStats);
    if (version >= kVersion34) writeSelfDestructs(payloadWriter, state.selfDestructs);
    if (version >= kVersion35) writeProjectiles(payloadWriter, state.projectiles,
                                               version >= kVersion38, version >= kVersion39);
    if (version >= kVersion37) writePathService(payloadWriter, state.pathService);
    if (version >= kVersion37) writeIntel(payloadWriter, state.intel);
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
               && version != kVersion16 && version != kVersion17 && version != kVersion18
               && version != kVersion19 && version != kVersion20 && version != kVersion21
               && version != kVersion22 && version != kVersion23 && version != kVersion24
               && version != kVersion25 && version != kVersion26
               && version != kVersion27 && version != kVersion28
               && version != kVersion29 && version != kVersion30
               && version != kVersion31 && version != kVersion32
               && version != kVersion33 && version != kVersion34 && version != kVersion35
               && version != kVersion36 && version != kVersion37 && version != kVersion38
               && version != kVersion39 && version != kVersion40)
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
                       version >= kVersion12, version >= kVersion14,
                       version >= kVersion17, version >= kVersion20,
                       version >= kVersion26, version >= kVersion26,
                       version >= kVersion27, version >= kVersion28,
                       version >= kVersion29, version >= kVersion30,
                       version >= kVersion36)) return std::nullopt;
    std::vector<SiloAmmo> siloAmmo;
    if (version >= kVersion9
        && !readSiloAmmo(reader, siloAmmo, version >= kVersion33)) return std::nullopt;
    std::vector<MissileRedirect> redirects;
    if (version >= kVersion10 && !readRedirects(reader, redirects)) return std::nullopt;
    if (version >= kVersion11
        && !readAirMotion(reader, units.motion, version >= kVersion13,
                          version >= kVersion14, version >= kVersion16)) return std::nullopt;
    std::optional<EconomyArmyState> economyArmies;
    if (version >= kVersion15 && !readEconomyArmies(reader, economyArmies,
                                                    version >= kVersion40)) return std::nullopt;
    if (version >= kVersion16
        && !readAirController(reader, units.motion, version >= kVersion21))
        return std::nullopt;
    if (version >= kVersion18 && !readSubMotion(reader, units.motion)) return std::nullopt;
    std::vector<EnhancementWork> enhancements;
    if (version >= kVersion19 && (!readEnhancements(reader, enhancements)
        || !readInstalledEnhancements(reader, units))) return std::nullopt;
    std::vector<CaptureWork> captures;
    if (version >= kVersion20 && !readCaptures(reader, captures)) return std::nullopt;
    if (version >= kVersion22 && !readBoneAttachments(reader, units)) return std::nullopt;
    std::optional<FeatureStore::Snapshot> features;
    if (version >= kVersion23 && !readFeatures(reader, features)) return std::nullopt;
    if (version >= kVersion24 && !readTurretPose(reader, units.motion)) return std::nullopt;
    if (version >= kVersion25 && !readTurretPoseDual(reader, units.motion)) return std::nullopt;
    if (version >= kVersion31 && !readCongestion(reader, units.motion)) return std::nullopt;
    if (version >= kVersion32 && !readLeadStep(reader, units.motion)) return std::nullopt;
    std::vector<SiloBuild> siloQueue;
    if (version >= kVersion33 && !readSiloQueue(reader, siloQueue)) return std::nullopt;
    std::optional<std::vector<ArmyStats>> armyStats;
    if (version >= kVersion34 && !readArmyStats(reader, armyStats)) return std::nullopt;
    std::vector<SelfDestructWork> selfDestructs;
    if (version >= kVersion34 && !readSelfDestructs(reader, selfDestructs)) return std::nullopt;
    std::optional<std::vector<Projectile>> projectiles;
    if (version >= kVersion35 && !readProjectiles(reader, projectiles,
                                                 version >= kVersion38,
                                                 version >= kVersion39)) return std::nullopt;
    std::optional<PathService::Snapshot> pathService;
    if (version >= kVersion37 && !readPathService(reader, pathService)) return std::nullopt;
    std::optional<Intel::Snapshot> intel;
    if (version >= kVersion37 && !readIntel(reader, intel)) return std::nullopt;
    SaveState decoded{.tick = tick,
                      .random = std::move(random),
                       .pathServiceBeats = pathServiceBeats,
                       .units = std::move(units), .siloAmmo = std::move(siloAmmo),
                       .siloQueue = std::move(siloQueue),
                       .redirects = std::move(redirects), .economyArmies = std::move(economyArmies),
                       .enhancements = std::move(enhancements),
                       .captures = std::move(captures), .armyStats = std::move(armyStats),
                       .selfDestructs = std::move(selfDestructs),
                       .features = std::move(features),
                       .projectiles = std::move(projectiles),
                       .pathService = std::move(pathService),
                       .intel = std::move(intel)};
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
    return rm::sim::encode(state, kVersion40);
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
        .pendingSurvivorMask = match.pendingSurvivorMask,
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
    match.pendingSurvivorMask = pendingSurvivorMask;
    match.defeatPollElapsedTicks = defeatPollElapsedTicks;
    match.defeatCleanupRemainingTicks = defeatCleanupRemainingTicks;
}

} // namespace rm::sim
