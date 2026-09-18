#include "core/sim/Army.hpp"
#include "core/sim/ArmyStats.hpp"

#include <algorithm>
#include <array>

namespace rm::sim {
namespace {

// Faction letter, `L` for land, and the number reserved for a commander. Held as a
// table rather than composed from a letter, so the one place this is a convention is
// the one place it is written down.
struct FactionInfo {
    Faction faction;
    std::string_view name;          ///< as `General.FactionName` spells it
    std::string_view commanderId;
};

constexpr std::array<FactionInfo, 4> kFactions{{
    {Faction::Uef, "UEF", "UEL0001"},
    {Faction::Aeon, "Aeon", "UAL0001"},
    {Faction::Cybran, "Cybran", "URL0001"},
    {Faction::Seraphim, "Seraphim", "XSL0001"},
}};

[[nodiscard]] const FactionInfo& infoFor(Faction faction) noexcept {
    for (const FactionInfo& info : kFactions) {
        if (info.faction == faction) {
            return info;
        }
    }
    return kFactions.front();
}

} // namespace

std::optional<Faction> factionFromName(std::string_view name) noexcept {
    // Case-insensitive, because the corpus spells it "UEF" and "Aeon" and a mod has no
    // reason to agree about either.
    const auto equalsNoCase = [](std::string_view a, std::string_view b) {
        return std::ranges::equal(a, b, [](char x, char y) {
            const auto lower = [](char c) {
                return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c;
            };
            return lower(x) == lower(y);
        });
    };

    for (const FactionInfo& info : kFactions) {
        if (equalsNoCase(info.name, name)) {
            return info.faction;
        }
    }
    return std::nullopt;
}

std::string_view factionName(Faction faction) noexcept { return infoFor(faction).name; }

std::string commanderBlueprintId(Faction faction) noexcept {
    return std::string{infoFor(faction).commanderId};
}

std::string commanderBlueprintPath(Faction faction) {
    const std::string id = commanderBlueprintId(faction);
    return "/units/" + id + "/" + id + "_unit.bp";
}

bool isCommanderId(std::string_view blueprintId) noexcept {
    for (const FactionInfo& info : kFactions) {
        if (info.commanderId == blueprintId) {
            return true;
        }
    }
    return false;
}

std::size_t applyDefeats(std::vector<Army>& armies, std::span<const int> survivalUnits,
                          std::span<const int> commandersEver, bool requireCommanderEver) {
    std::size_t newlyDefeated = 0;
    for (Army& army : armies) {
        if (army.defeated) {
            continue;
        }
        const auto i = static_cast<std::size_t>(army.index);
        if (i >= survivalUnits.size() || (requireCommanderEver && i >= commandersEver.size())) {
            continue;
        }
        if ((!requireCommanderEver || commandersEver[i] > 0) && survivalUnits[i] == 0) {
            army.defeated = true;
            ++newlyDefeated;
        }
    }
    return newlyDefeated;
}

std::optional<int> winningAlliance(const std::vector<Army>& armies) noexcept {
    std::optional<int> survivor;
    for (const Army& army : armies) {
        if (army.defeated) {
            continue;
        }
        if (survivor && *survivor != army.alliance) {
            return std::nullopt;  // two teams still standing: the match is on
        }
        survivor = army.alliance;
    }
    return survivor;  // nullopt when nobody is left, which is a draw
}

bool allied(const Army& a, const Army& b) noexcept { return a.alliance == b.alliance; }

bool hostile(const Army& a, const Army& b) noexcept {
    return !allied(a, b) && !a.defeated && !b.defeated;
}

std::vector<Army> freeForAll(std::size_t armyCount) {
    std::vector<Army> armies;
    armies.reserve(armyCount);
    for (std::size_t i = 0; i < armyCount; ++i) {
        const auto index = static_cast<int>(i);
        armies.push_back(Army{
            .index = index,
            // Round-robin rather than random: the same map must produce the same
            // match, or a screenshot proves nothing twice.
            .faction = kFactions[i % kFactions.size()].faction,
            .alliance = index,  // free-for-all: everyone their own alliance
            .defeated = false,
        });
    }
    return armies;
}

std::size_t survivorCount(const std::vector<Army>& armies) noexcept {
    return static_cast<std::size_t>(
        std::ranges::count_if(armies, [](const Army& army) { return !army.defeated; }));
}

bool commands(const Player& player, int army) noexcept {
    // An unowned army is nobody's to command: `kNoArmy` on either side is not a match, or a
    // player with no army assigned would command every decorative unit on the map.
    return army != kNoArmy && player.army == army;
}

std::vector<PlayerIndex> commandersOf(std::span<const Player> players, int army) {
    std::vector<PlayerIndex> found;
    for (const Player& player : players) {
        if (commands(player, army)) {
            found.push_back(player.index);
        }
    }
    return found;
}

std::vector<Player> onePlayerPerArmy(std::size_t armyCount, int humanArmy) {
    std::vector<Player> players;
    players.reserve(armyCount);
    for (std::size_t i = 0; i < armyCount; ++i) {
        const auto army = static_cast<int>(i);
        players.push_back(Player{
            .index = static_cast<PlayerIndex>(i),
            .army = army,
            .human = army == humanArmy,
            .name = army == humanArmy ? "player" : "script " + std::to_string(army),
        });
    }
    return players;
}

// --- `CArmyStats` (`C-227`) -----------------------------------------------------
//
// The serialized per-army stat store and one-shot threshold-trigger service. See
// `ArmyStats.hpp` for the retail citation; the shapes below are the evaluator's own:
// four comparisons, every condition must pass, qualifying triggers removed before
// delivery.

StatCompare statCompareFromName(std::string_view name) noexcept {
    if (name == "GreaterThan") {
        return StatCompare::GreaterThan;
    }
    if (name == "LessThan") {
        return StatCompare::LessThan;
    }
    if (name == "LessThanOrEqual") {
        return StatCompare::LessThanOrEqual;
    }
    // `GreaterThanOrEqual` is both the spelled name and TriggerManager.lua's default
    // for an unset `CompareType` — unknown spellings land there too rather than
    // inventing a fifth comparison.
    return StatCompare::GreaterThanOrEqual;
}

namespace {

[[nodiscard]] bool statComparePasses(StatCompare op, Mag lhs, Mag rhs) noexcept {
    switch (op) {
    case StatCompare::GreaterThan:
        return lhs > rhs;
    case StatCompare::GreaterThanOrEqual:
        return lhs >= rhs;
    case StatCompare::LessThan:
        return lhs < rhs;
    case StatCompare::LessThanOrEqual:
        return lhs <= rhs;
    }
    return false;
}

[[nodiscard]] bool conditionPasses(const ArmyStats& stats,
                                   const ArmyStatCondition& condition) noexcept {
    const Mag current = condition.category.empty()
        ? armyStat(stats, condition.stat)
        : armyBlueprintStat(stats, condition.stat, condition.category);
    return statComparePasses(condition.op, current, condition.value);
}

} // namespace

Mag armyStat(const ArmyStats& stats, std::string_view name, Mag fallback) noexcept {
    for (const auto& [key, value] : stats.stats) {
        if (key == name) {
            return value;
        }
    }
    return fallback;
}

Mag armyBlueprintStat(const ArmyStats& stats, std::string_view name,
                      std::string_view category, Mag fallback) noexcept {
    for (const auto& [key, value] : stats.blueprintStats) {
        if (key.first == name && key.second == category) {
            return value;
        }
    }
    return fallback;
}

void setArmyStat(ArmyStats& stats, std::string name, Mag value) {
    for (auto& [key, current] : stats.stats) {
        if (key == name) {
            current = value;
            return;
        }
    }
    stats.stats.emplace_back(std::move(name), value);
}

void addArmyStat(ArmyStats& stats, const std::string& name, Mag delta) {
    setArmyStat(stats, name, armyStat(stats, name) + delta);
}

void addArmyBlueprintStat(ArmyStats& stats, const std::string& name,
                          const std::string& category, Mag delta) {
    const std::pair<std::string, std::string> wanted{name, category};
    for (auto& [key, current] : stats.blueprintStats) {
        if (key == wanted) {
            current += delta;
            return;
        }
    }
    stats.blueprintStats.emplace_back(wanted, delta);
}

void setArmyStatsTrigger(ArmyStats& stats, ArmyStatTrigger trigger) {
    for (ArmyStatTrigger& pending : stats.triggers) {
        if (pending.name == trigger.name) {
            pending = std::move(trigger);
            return;
        }
    }
    stats.triggers.push_back(std::move(trigger));
}

std::vector<std::string> evaluateArmyStats(ArmyStats& stats) {
    std::vector<std::string> fired;
    std::erase_if(stats.triggers, [&fired, &stats](const ArmyStatTrigger& trigger) {
        // EVERY condition must pass — and a trigger with none cannot qualify, which
        // is `std::ranges::all_of`'s empty-true deliberately excluded: retail's
        // evaluator tests a list of comparisons, and an empty list has nothing to
        // test.
        const bool qualifies = !trigger.conditions.empty()
            && std::ranges::all_of(trigger.conditions, [&stats](const auto& c) {
                   return conditionPasses(stats, c);
               });
        if (qualifies) {
            fired.push_back(trigger.name);
        }
        // Removal happens HERE, before the names reach the caller — retail removes
        // qualifying triggers before callback delivery, so a callback that re-arms
        // the same name never meets its predecessor still pending.
        return qualifies;
    });
    return fired;
}

} // namespace rm::sim
