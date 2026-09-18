// THE COMMAND RECORD — semantic issues, in tick/phase order, as text you can diff.
//
// `CommandLog::record` appends the canonical accepted set for an issue; `writeCommandLog` and
// `readCommandLog` are the versioned on-disk form. TEXT, for the same reason the hash log is
// text: a log you can read is a log you can diff, quote in a bug report and hand-edit to
// reproduce something. Fixed-point targets are written as their RAW integers — a decimal
// expansion would be a lossy round trip, which in a determinism artifact is the one
// unacceptable kind of lossy.
//
// The spellings (`commandKindName`, `kindFromName`, `phaseName`) are the log's own
// vocabulary — one direction lives in `Command.cpp` for everyone, the inverse stays here
// because only the reader needs to un-spell a word.
#include "core/sim/Command.hpp"
#include "core/sim/CommandInternal.hpp"

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <optional>
#include <sstream>
#include <stdexcept>

namespace rm::sim {
namespace {

[[nodiscard]] std::optional<CommandKind> kindFromName(std::string_view name) noexcept {
    if (name == "dive") return CommandKind::Dive;
    if (name == "guard") return CommandKind::Guard;
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
    if (name == "reclaim-unit") {
        return CommandKind::ReclaimUnit;
    }
    if (name == "capture") {
        return CommandKind::Capture;
    }
    if (name == "overcharge") {
        return CommandKind::Overcharge;
    }
    if (name == "toggle-factory-repeat") {
        return CommandKind::ToggleFactoryRepeat;
    }
    if (name == "toggle-production") {
        return CommandKind::ToggleProduction;
    }
    if (name == "cycle-build-priority") {
        return CommandKind::CycleBuildPriority;
    }
    if (name == "set-build-priority") {
        return CommandKind::SetBuildPriority;
    }
    if (name == "cycle-retreat-threshold") {
        return CommandKind::CycleRetreatThreshold;
    }
    if (name == "cycle-target-focus") {
        return CommandKind::CycleTargetFocus;
    }
    if (name == "cancel-factory-build") return CommandKind::CancelFactoryBuild;
    if (name == "repair") {
        return CommandKind::Repair;
    }
    if (name == "script") {
        return CommandKind::Script;
    }
    if (name == "missile-launch") {
        return CommandKind::MissileLaunch;
    }
    if (name == "load-transport") return CommandKind::LoadTransport;
    if (name == "unload-transport") return CommandKind::UnloadTransport;
    if (name == "ferry") return CommandKind::Ferry;
    if (name == "silo-build-tactical") return CommandKind::SiloBuildTactical;
    if (name == "silo-build-nuke") return CommandKind::SiloBuildNuke;
    if (name == "toggle-silo-auto") return CommandKind::ToggleSiloAuto;
    if (name == "self-destruct") return CommandKind::SelfDestruct;
    return std::nullopt;
}

// --- The log --------------------------------------------------------------------------


inline constexpr std::string_view kCommandLogMagic = "recoil-metal semantic command log";
/// Version 7 adds `toggle-script-bit` and its trailing `scriptBit` column; v6 added
/// `set-build-priority` and its `priority` column; v5 added immediate `dive`.
/// Versions 2–6 are still read — the columns default there, and a name their
/// writers never produced cannot appear in them anyway.
inline constexpr std::uint32_t kCommandLogVersion = 7;

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
    if (!validCancellation(issue) || issue.source == kInvalidCommandSource || issue.id == kInvalidCommandId
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
           " buildPath scriptTask scriptDataHex cancelCommandId\n";
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
            << std::quoted(scriptData) << ' ' << issue.cancelCommandId << ' '
            << static_cast<unsigned>(issue.priority) << ' '
            << static_cast<unsigned>(issue.scriptBit) << '\n';
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
        if (!(fields >> label >> version) || label != "version"
            || (version != 2 && version != 3 && version != 4 && version != kCommandLogVersion)
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
            || scriptTask.size() > kMaxScriptTaskNameBytes
            || scriptDataHex.size() > kMaxScriptTaskDataBytes * 2
            || scriptDataHex.size() % 2 != 0) {
            return std::nullopt;
        }
        if (version >= 3) {
            std::uint64_t cancelCommandId{};
            if (!(fields >> cancelCommandId) || !fits<CommandId>(cancelCommandId)) return std::nullopt;
            issue.cancelCommandId = static_cast<CommandId>(cancelCommandId);
        }
        if (version >= 6) {
            std::uint64_t priority{};
            if (!(fields >> priority)
                || priority > static_cast<std::uint64_t>(BuildPriority::High)) {
                return std::nullopt;
            }
            issue.priority = static_cast<BuildPriority>(priority);
        }
        if (version >= 7) {
            std::uint64_t scriptBit{};
            if (!(fields >> scriptBit) || scriptBit > 8) {
                return std::nullopt;
            }
            issue.scriptBit = static_cast<std::uint8_t>(scriptBit);
        }
        if (fields >> extra) return std::nullopt;
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
