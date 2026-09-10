#pragma once

// Offline AI-matrix reporting: the pure half of the personality×faction harness.
//
// WHY THIS EXISTS. A matrix run plays a full 1v1 headless and must answer, per faction:
// how long, what state it ended in, what got built (tech desc, price desc), what enemy
// units died and what they were worth, and lifetime mass/energy generated. The sim holds
// none of that in report shape — deaths retire, wrecks get reclaimed, rates are rebuilt
// every tick — so the app accumulates two small ledgers during play (MatchRunner's
// `matrixKills`/`matrixBuilt`) and everything here turns them into the console table and
// the JSON doc at the end. Nothing here touches sim state, the hash, or the save format.
//
// DEPENDENCY RULE. This header resolves nothing itself: the caller maps a type index to
// a `MatrixTypeInfo` from its catalog, so these functions stay unit-testable without
// content and this header stays free of UnitDef. Doubles cross the boundary because JSON
// and printf are decimal media — the fixed-point-to-double conversion is the caller's,
// at the single resolver site.

#include "core/Types.hpp"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <limits>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace rm::app {

/// One unit type's blueprint facts, in report units. `tech` is `unitdef::techOf` (0–4,
/// EXPERIMENTAL counts 4); mass/energy are build costs; buildTime is blueprint
/// `BuildTime` in work units, NOT seconds — seconds need a builder's rate
/// (`buildTime / buildRate`), and no single rate speaks for a dead unit's worth.
struct MatrixTypeInfo {
    std::string blueprint;
    int tech = 0;
    double mass = 0.0;
    double energy = 0.0;
    double buildTime = 0.0;
};

/// One finished construction, recorded where `advanceMatch` spawns it. Upgrades count:
/// a factory becoming its T2 self paid a cost and produced a unit.
struct MatrixBuilt {
    UnitTypeIndex type = 0;
    int army = -1;
};

/// One death, recorded where `advanceMatch` counts it. `killerArmy` is -1 when no unit
/// dealt the fatal blow (decay, self-destruction without an instigator) — a loss for
/// the victim's side, nobody's kill.
struct MatrixKill {
    UnitTypeIndex victimType = 0;
    int victimArmy = -1;
    int killerArmy = -1;
};

/// Type index to report facts. Returning a default `MatrixTypeInfo` for an unknown type
/// keeps one bad index from aborting a run's whole report; the blueprint stays empty so
/// the gap is visible in the JSON rather than silently dropped.
using MatrixTypeResolver = std::function<MatrixTypeInfo(UnitTypeIndex)>;

/// One aggregated row: how many of a type, and what one of them costs.
struct MatrixTypeRow {
    MatrixTypeInfo info;
    std::size_t count = 0;
};

/// Aggregate per-type counts, sorted tech descending, then mass, then energy, then
/// blueprint ascending as the deterministic tiebreak. Shared by the built list and the
/// killed-by-type breakdown so the two orders cannot drift apart.
[[nodiscard]] inline std::vector<MatrixTypeRow>
summarizeMatrixTypes(const std::vector<std::pair<UnitTypeIndex, std::size_t>>& counts,
                     const MatrixTypeResolver& resolve) {
    std::vector<MatrixTypeRow> rows;
    rows.reserve(counts.size());
    for (const auto& [type, count] : counts) {
        if (count == 0) {
            continue;
        }
        rows.push_back(MatrixTypeRow{.info = resolve(type), .count = count});
    }
    // Tech first (a T3 army out-teched a T1 one whatever the counts), then price: mass
    // before energy because FAF's economy binds on mass first, and the blueprint path
    // last so equal-cost types still order the same way every run.
    std::sort(rows.begin(), rows.end(), [](const MatrixTypeRow& a, const MatrixTypeRow& b) {
        if (a.info.tech != b.info.tech) {
            return a.info.tech > b.info.tech;
        }
        if (a.info.mass != b.info.mass) {
            return a.info.mass > b.info.mass;
        }
        if (a.info.energy != b.info.energy) {
            return a.info.energy > b.info.energy;
        }
        return a.info.blueprint < b.info.blueprint;
    });
    return rows;
}

/// What one army built: its ledger entries, counted per type, in report order.
[[nodiscard]] inline std::vector<MatrixTypeRow>
summarizeMatrixBuilt(const std::vector<MatrixBuilt>& built, int army,
                     const MatrixTypeResolver& resolve) {
    std::vector<std::pair<UnitTypeIndex, std::size_t>> counts;
    for (const MatrixBuilt& entry : built) {
        if (entry.army != army) {
            continue;
        }
        bool found = false;
        for (auto& [type, count] : counts) {
            if (type == entry.type) {
                ++count;
                found = true;
                break;
            }
        }
        if (!found) {
            counts.emplace_back(entry.type, 1);
        }
    }
    return summarizeMatrixTypes(counts, resolve);
}

/// One army's kill record: enemy units destroyed and what they were worth. Worth is the
/// victim's blueprint cost (mass, energy, build time) — "how much of the enemy's economy
/// died", not damage dealt, so overkill and free reclaim-wrecks cannot inflate it.
/// Self-kills and killer-unknown deaths are nobody's score and stay out of the count.
struct MatrixArmyKills {
    std::size_t kills = 0;
    double massWorth = 0.0;
    double energyWorth = 0.0;
    double buildTimeWorth = 0.0;
    std::vector<MatrixTypeRow> victims;
};

[[nodiscard]] inline MatrixArmyKills tallyMatrixKills(const std::vector<MatrixKill>& kills,
                                                      int army,
                                                      const MatrixTypeResolver& resolve) {
    MatrixArmyKills tally;
    std::vector<std::pair<UnitTypeIndex, std::size_t>> counts;
    for (const MatrixKill& kill : kills) {
        if (kill.killerArmy != army || kill.victimArmy == army) {
            continue;
        }
        ++tally.kills;
        const MatrixTypeInfo info = resolve(kill.victimType);
        tally.massWorth += info.mass;
        tally.energyWorth += info.energy;
        tally.buildTimeWorth += info.buildTime;
        bool found = false;
        for (auto& [type, count] : counts) {
            if (type == kill.victimType) {
                ++count;
                found = true;
                break;
            }
        }
        if (!found) {
            counts.emplace_back(kill.victimType, 1);
        }
    }
    tally.victims = summarizeMatrixTypes(counts, resolve);
    return tally;
}

/// JSON string escaping: quotes, backslashes, and control characters. Blueprint paths
/// are slash-separated and need none of it in practice; escaping anyway because a
/// report writer that assumes its input is clean is a bug waiting for a weird mod name.
inline void writeMatrixJsonString(std::ostream& out, std::string_view value) {
    out.put('"');
    for (const char c : value) {
        switch (c) {
        case '"': out.write("\\\"", 2); break;
        case '\\': out.write("\\\\", 2); break;
        case '\n': out.write("\\n", 2); break;
        case '\r': out.write("\\r", 2); break;
        case '\t': out.write("\\t", 2); break;
        default:
            if (c >= 0x00 && c < 0x20) {
                constexpr char kHex[] = "0123456789abcdef";
                out.write("\\u00", 4);
                out.put(kHex[(c >> 4) & 0xf]);
                out.put(kHex[c & 0xf]);
            } else {
                out.put(c);
            }
            break;
        }
    }
    out.put('"');
}

/// Doubles at full round-trip precision: a JSON doc that cannot be parsed back to the
/// same values is a lossy artifact, and mass totals in long games exceed what six
/// default digits can carry.
inline void writeMatrixJsonNumber(std::ostream& out, double value) {
    std::ostringstream text;
    text.precision(std::numeric_limits<double>::max_digits10);
    text << value;
    out << text.str();
}

inline void writeMatrixTypeRows(std::ostream& out, const std::vector<MatrixTypeRow>& rows,
                                int indent) {
    const std::string pad(static_cast<std::size_t>(indent), ' ');
    const std::string inner(static_cast<std::size_t>(indent + 2), ' ');
    out << "[\n";
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const MatrixTypeRow& row = rows[i];
        out << inner << "{\"blueprint\": ";
        writeMatrixJsonString(out, row.info.blueprint);
        out << ", \"tech\": " << row.info.tech << ", \"count\": " << row.count
            << ", \"mass\": ";
        writeMatrixJsonNumber(out, row.info.mass);
        out << ", \"energy\": ";
        writeMatrixJsonNumber(out, row.info.energy);
        out << ", \"buildTime\": ";
        writeMatrixJsonNumber(out, row.info.buildTime);
        out << "}";
        if (i + 1 < rows.size()) {
            out << ",";
        }
        out << "\n";
    }
    out << pad << "]";
}

/// One army's slice of a progress snapshot: what the 15-second progress line prints.
struct MatrixSnapshotArmy {
    std::size_t alive = 0;
    double incomeMassPerSecond = 0.0;
    double incomeEnergyPerSecond = 0.0;
    std::size_t kills = 0;
};

struct MatrixSnapshot {
    unsigned long long tick = 0;
    double wallSeconds = 0.0;
    std::vector<MatrixSnapshotArmy> armies;
};

/// One army's final outcome, assembled caller-side from the sim (generated totals,
/// alive count), the ledgers, and the run config.
struct MatrixArmyOutcome {
    int army = 0;
    std::string personality;
    std::string faction;
    double generatedMass = 0.0;
    double generatedEnergy = 0.0;
    std::size_t alive = 0;
    std::vector<MatrixTypeRow> built;
    MatrixArmyKills kills;
};

/// The whole run as one JSON doc: config (so a result from March is comparable to one
/// from September only by commit hash, explicitly), snapshots, and the outcome.
struct MatrixDoc {
    std::string commit;
    std::string map;
    int simSeed = 1;
    unsigned long long tickCap = 0;
    unsigned long long ticksPlayed = 0;
    double wallSeconds = 0.0;
    // `null` when the cap stopped the run; otherwise the winning team, if any (a draw
    // has no winner and is still a finished game).
    bool finished = false;
    bool hasWinner = false;
    int winner = 0;
    std::vector<MatrixArmyOutcome> armies;
    std::vector<MatrixSnapshot> snapshots;
};

inline void writeMatrixJson(std::ostream& out, const MatrixDoc& doc) {
    out << "{\n";
    out << "  \"version\": 1,\n";
    out << "  \"commit\": ";
    writeMatrixJsonString(out, doc.commit);
    out << ",\n  \"map\": ";
    writeMatrixJsonString(out, doc.map);
    out << ",\n  \"simSeed\": " << doc.simSeed
        << ",\n  \"tickCap\": " << doc.tickCap << ",\n  \"ticksPlayed\": " << doc.ticksPlayed
        << ",\n  \"wallSeconds\": ";
    writeMatrixJsonNumber(out, doc.wallSeconds);
    out << ",\n  \"finished\": " << (doc.finished ? "true" : "false")
        << ",\n  \"winner\": ";
    if (doc.hasWinner) {
        out << doc.winner;
    } else {
        out << "null";
    }
    out << ",\n  \"armies\": [\n";
    for (std::size_t i = 0; i < doc.armies.size(); ++i) {
        const MatrixArmyOutcome& army = doc.armies[i];
        out << "    {\"army\": " << army.army << ", \"personality\": ";
        writeMatrixJsonString(out, army.personality);
        out << ", \"faction\": ";
        writeMatrixJsonString(out, army.faction);
        out << ", \"generatedMass\": ";
        writeMatrixJsonNumber(out, army.generatedMass);
        out << ", \"generatedEnergy\": ";
        writeMatrixJsonNumber(out, army.generatedEnergy);
        out << ", \"alive\": " << army.alive << ",\n      \"built\": ";
        writeMatrixTypeRows(out, army.built, 6);
        out << ",\n      \"kills\": " << army.kills.kills << ", \"killMassWorth\": ";
        writeMatrixJsonNumber(out, army.kills.massWorth);
        out << ", \"killEnergyWorth\": ";
        writeMatrixJsonNumber(out, army.kills.energyWorth);
        out << ", \"killBuildTimeWorth\": ";
        writeMatrixJsonNumber(out, army.kills.buildTimeWorth);
        out << ",\n      \"killedByType\": ";
        writeMatrixTypeRows(out, army.kills.victims, 6);
        out << "}";
        if (i + 1 < doc.armies.size()) {
            out << ",";
        }
        out << "\n";
    }
    out << "  ],\n  \"snapshots\": [\n";
    for (std::size_t i = 0; i < doc.snapshots.size(); ++i) {
        const MatrixSnapshot& snapshot = doc.snapshots[i];
        out << "    {\"tick\": " << snapshot.tick << ", \"wallSeconds\": ";
        writeMatrixJsonNumber(out, snapshot.wallSeconds);
        out << ", \"armies\": [";
        for (std::size_t a = 0; a < snapshot.armies.size(); ++a) {
            const MatrixSnapshotArmy& army = snapshot.armies[a];
            if (a > 0) {
                out << ", ";
            }
            out << "{\"alive\": " << army.alive << ", \"incomeMass\": ";
            writeMatrixJsonNumber(out, army.incomeMassPerSecond);
            out << ", \"incomeEnergy\": ";
            writeMatrixJsonNumber(out, army.incomeEnergyPerSecond);
            out << ", \"kills\": " << army.kills << "}";
        }
        out << "]}";
        if (i + 1 < doc.snapshots.size()) {
            out << ",";
        }
        out << "\n";
    }
    out << "  ]\n}\n";
}

} // namespace rm::app
