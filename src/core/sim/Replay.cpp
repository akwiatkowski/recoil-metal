#include "core/sim/Replay.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace rm::sim {
namespace {

// The first line of every log. Present so that pointing the checker at the wrong file says
// "not a replay" instead of reporting a divergence on tick zero.
constexpr const char* kMagic = "recoil-metal hash log";

} // namespace

std::uint64_t widthsFingerprint() noexcept {
    // Packed rather than hashed: eight small numbers fit, and a fingerprint you can read
    // off the file tells you WHICH width moved. A hash would only tell you that one did.
    std::uint64_t f = 0;
    auto pack = [&f](std::size_t bytes, int slot) {
        f |= static_cast<std::uint64_t>(bytes & 0xFULL) << (slot * 4);
    };
    pack(sizeof(UnitIndex), 0);
    pack(sizeof(ObjectIndex), 1);
    pack(sizeof(Generation), 2);
    pack(sizeof(TeamIndex), 3);
    pack(sizeof(PlayerIndex), 4);
    pack(sizeof(AllianceIndex), 5);
    pack(sizeof(UnitTypeIndex), 6);
    pack(sizeof(TickIndex), 7);
    pack(sizeof(TickCount), 8);
    pack(sizeof(StateHash), 9);
    return f;
}

Divergence compareHeaders(const ReplayHeader& recorded, const ReplayHeader& replayed) {
    Divergence d;
    auto refuse = [&d](std::string why) {
        d.incomparable = true;
        d.why = std::move(why);
        return d;
    };

    if (recorded.formatVersion != replayed.formatVersion) {
        return refuse("recorded with hash-log format v"
                      + std::to_string(recorded.formatVersion) + ", this build writes v"
                      + std::to_string(replayed.formatVersion)
                      + " — the hashed field set changed, so the two are not comparable");
    }
    if (recorded.ticksPerSecond != replayed.ticksPerSecond) {
        // Not rescaled, refused. Every authored duration lands on a different tick at a
        // different rate, so the two runs did not play the same match — comparing them
        // would report a divergence that is really a configuration difference.
        return refuse("recorded at " + std::to_string(recorded.ticksPerSecond)
                      + " Hz, replayed at " + std::to_string(replayed.ticksPerSecond)
                      + " Hz — a log is only comparable against its own tick rate");
    }
    if (recorded.widthsFingerprint != replayed.widthsFingerprint) {
        return refuse("recorded with different integer widths (core/Types.hpp changed) — "
                      "old logs cannot be compared against new hashes");
    }
    return d;
}

Divergence compareHashes(std::span<const StateHash> recorded,
                         std::span<const StateHash> replayed) {
    Divergence d;
    const std::size_t common = std::min(recorded.size(), replayed.size());

    for (std::size_t i = 0; i < common; ++i) {
        if (recorded[i] != replayed[i]) {
            d.diverged = true;
            d.tick = static_cast<TickIndex>(i);
            d.recorded = recorded[i];
            d.replayed = replayed[i];
            return d;
        }
    }

    if (recorded.size() != replayed.size()) {
        // One run stopped before the other. Reported at the tick it ran out, because that
        // is where the two histories part company.
        d.diverged = true;
        d.tick = static_cast<TickIndex>(common);
        d.recorded = recorded.size() > common ? recorded[common] : 0;
        d.replayed = replayed.size() > common ? replayed[common] : 0;
        d.why = "one run has " + std::to_string(recorded.size()) + " ticks, the other "
                + std::to_string(replayed.size());
    }
    return d;
}

std::expected<void, ReplayError> writeHashLog(const std::string& path,
                                              const ReplayHeader& header,
                                              std::span<const StateHash> hashes) {
    std::ofstream out{path, std::ios::trunc};
    if (!out) {
        return std::unexpected(ReplayError{ReplayError::Code::CannotOpen,
                                          "cannot open " + path + " for writing"});
    }

    out << kMagic << "\n";
    out << "version " << header.formatVersion << "\n";
    out << "ticks-per-second " << header.ticksPerSecond << "\n";
    out << "widths " << std::hex << header.widthsFingerprint << std::dec << "\n";
    out << "tick-count " << hashes.size() << "\n";
    for (const StateHash h : hashes) {
        out << std::hex << h << std::dec << "\n";
    }
    if (!out) {
        return std::unexpected(
            ReplayError{ReplayError::Code::CannotOpen, "write to " + path + " failed"});
    }
    return {};
}

std::expected<ReplayLog, ReplayError> readHashLog(const std::string& path) {
    std::ifstream in{path};
    if (!in) {
        return std::unexpected(ReplayError{ReplayError::Code::CannotOpen,
                                          "cannot open " + path + " for reading"});
    }

    std::string line;
    if (!std::getline(in, line) || line != kMagic) {
        return std::unexpected(ReplayError{ReplayError::Code::NotAReplay,
                                          path + " is not a recoil-metal hash log"});
    }

    ReplayLog log;
    auto field = [&in](const char* name, auto& value, bool hex) {
        std::string text;
        if (!std::getline(in, text)) {
            return false;
        }
        std::istringstream parts{text};
        std::string key;
        parts >> key;
        if (key != name) {
            return false;
        }
        if (hex) {
            parts >> std::hex;
        }
        parts >> value;
        return static_cast<bool>(parts);
    };

    std::uint64_t declaredTicks = 0;
    if (!field("version", log.header.formatVersion, false)) {
        return std::unexpected(
            ReplayError{ReplayError::Code::NotAReplay, path + ": missing version"});
    }
    if (log.header.formatVersion != ReplayHeader::kFormatVersion) {
        return std::unexpected(
            ReplayError{ReplayError::Code::BadVersion,
                        path + ": hash-log format v" + std::to_string(log.header.formatVersion)
                            + ", this build reads v"
                            + std::to_string(ReplayHeader::kFormatVersion)});
    }
    if (!field("ticks-per-second", log.header.ticksPerSecond, false)
        || !field("widths", log.header.widthsFingerprint, true)
        || !field("tick-count", declaredTicks, false)) {
        return std::unexpected(
            ReplayError{ReplayError::Code::NotAReplay, path + ": malformed header"});
    }
    log.header.tickCount = declaredTicks;

    StateHash h = 0;
    while (in >> std::hex >> h) {
        log.hashes.push_back(h);
    }

    if (log.hashes.size() != declaredTicks) {
        // A truncated log that compared equal as far as it went would read as agreement.
        return std::unexpected(
            ReplayError{ReplayError::Code::Truncated,
                        path + ": header promises " + std::to_string(declaredTicks)
                            + " ticks, file holds " + std::to_string(log.hashes.size())});
    }
    return log;
}

} // namespace rm::sim
