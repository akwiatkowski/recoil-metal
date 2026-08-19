#include "core/sim/Army.hpp"

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

std::size_t applyDefeats(std::vector<Army>& armies, std::span<const int> commandersAlive,
                         std::span<const int> commandersEver) {
    std::size_t newlyDefeated = 0;
    for (Army& army : armies) {
        if (army.defeated) {
            continue;
        }
        const auto i = static_cast<std::size_t>(army.index);
        if (i >= commandersAlive.size() || i >= commandersEver.size()) {
            continue;
        }
        // Only an army that HAD a commander can lose it. Without this a scene with no
        // commanders at all — a `--units` crowd, or a map with no spawns — declares every
        // army defeated on the first tick and announces a draw before anything happens.
        if (commandersEver[i] > 0 && commandersAlive[i] == 0) {
            army.defeated = true;
            ++newlyDefeated;
        }
    }
    return newlyDefeated;
}

std::optional<int> winningTeam(const std::vector<Army>& armies) noexcept {
    std::optional<int> survivor;
    for (const Army& army : armies) {
        if (army.defeated) {
            continue;
        }
        if (survivor && *survivor != army.team) {
            return std::nullopt;  // two teams still standing: the match is on
        }
        survivor = army.team;
    }
    return survivor;  // nullopt when nobody is left, which is a draw
}

bool allied(const Army& a, const Army& b) noexcept { return a.team == b.team; }

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
            .team = index,  // free-for-all: everyone their own team
            .colour = teamColour(i),
            .defeated = false,
        });
    }
    return armies;
}

std::size_t survivorCount(const std::vector<Army>& armies) noexcept {
    return static_cast<std::size_t>(
        std::ranges::count_if(armies, [](const Army& army) { return !army.defeated; }));
}

} // namespace rm::sim
