#pragma once

#include "core/lua/LuaValue.hpp"
#include "core/unit/Role.hpp"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace rm::data {

// The scripted opponent's plan, AS DATA.
//
// WHY (PLAN2.md §7 P3.3). What the opponent opens with was five constants in a C++ header: four
// blueprint paths and `kAttackWaveTanks = 20`. Changing the opening meant editing and
// recompiling, and the paths were all UEF, so the opponent could only play one faction.
//
// This is the same plan expressed as a role sequence and a number, read from `data/opening.lua`.
// The roles resolve through a `Roster` at load, so the SAME opening drives all four factions —
// which is §7 P3.3's stated test.
//
// A LUA FILE, because this engine already reads Lua for maps and blueprints and adding a second
// config format would be a dependency with no argument for it. It is our own file in our own
// shape, not something the retail archive states — §1.1's "own format plus importers".
//
// WHAT IS STILL IN C++, and it is worth being exact: the ORDER of the opening is data, and the
// RULE for when to advance is not. `nextStructure` still says "wait for the first extractor,
// then power, then a second extractor, then the factory" as control flow. Turning that into data
// too means a condition language, which is P6's command queue rather than this. What P3.3
// removes is the identity of the things built, which is the part that made the opponent
// faction-locked.

/// One thing to build, by role rather than by id.
struct OpeningStep {
    unitdef::Role role = unitdef::Role::Unknown;

    /// Which tech tier, or 0 for "the cheapest this faction fields". Zero is the useful
    /// default: an opening wants the entry-level version and should not have to know which
    /// tiers exist.
    int tech = 0;

    /// Extra categories the pick must also carry.
    ///
    /// A ROLE IS NOT ALWAYS ENOUGH, and finding that out cost a match. `role = 'factory'` is
    /// ambiguous in a game with three domains: the cheapest T1 factory is the AIR factory, so
    /// the opening built one and the "tank wave" came out as air scouts. `requires = { 'LAND' }`
    /// is what says which.
    ///
    /// It also has a LIMIT worth knowing: narrowing to `{'LAND', 'TANK'}` to get the tank the
    /// old constant named resolves for three factions and fails for Cybran, which fields no T1
    /// tank at all. `requires` can only ask for what a faction has — which is what `fallback`
    /// below is for.
    ///
    /// Deliberately raw category tags rather than a `Domain` enum. The tags are the corpus's own
    /// vocabulary and it is open — `LAND`, `AIR`, `NAVAL` today, and whatever a mod adds — so an
    /// enum here would be a list to keep in step with content for no gain. The same reason
    /// `CategoryTerm` holds strings.
    std::vector<std::string> requires_;

    /// A looser set to try if `requires_` matches nothing.
    ///
    /// WHY THIS EXISTS, and it is not a generalisation for its own sake — it is the only way to
    /// write the plan this engine actually wants. The wave should be a TANK, which three factions
    /// field and Cybran does not: its T1 army is bots and the tank line starts at T2. Asking for
    /// a tank fails for Cybran; asking for any T1 land raider gets everyone a bot, and 40 bots
    /// do not close a match that 20 tanks do — measured, because bots die four times faster than
    /// the wave model assumes.
    ///
    /// So the plan says both: "a tank if you have one, otherwise whatever T1 land unit you
    /// have". Empty means no fallback, and a step that resolves to nothing is simply not built.
    std::vector<std::string> fallback;
};

/// The whole plan.
struct Opening {
    /// What the commander builds, in order. The first entry is ordered at spawn.
    std::vector<OpeningStep> structures;

    /// What the factory produces, and what the wave is made of.
    OpeningStep waveUnit{unitdef::Role::Raider, 1, {"LAND", "TANK"}, {"LAND"}};

    /// How many of them before the one attack.
    ///
    /// Was `kAttackWaveTanks = 20`, and the arithmetic behind that number moved into the data
    /// file's own comment rather than being lost — and had to be RE-DERIVED there, because the
    /// wave unit changed from a tank to a bot when the plan stopped naming UEF ids. The model is
    /// the same one and reproduces the old 20 for the old unit; for the bot it gives 40.
    std::size_t waveSize = 40;

    /// Whether this describes anything. An opening with no structures is a parse that found
    /// nothing, and a caller should refuse it rather than open with nothing.
    [[nodiscard]] bool valid() const noexcept { return !structures.empty() && waveSize > 0; }
};

/// Parses one from a Lua table.
///
/// Nothing when a role name is not one this engine knows — which is what lets a newer data file
/// fail loudly on an older binary rather than silently opening with three quarters of a plan.
[[nodiscard]] std::optional<Opening> parseOpening(const lua::Value& table);

/// Reads one from a file.
[[nodiscard]] std::optional<Opening> loadOpening(const std::filesystem::path& path);

/// The plan the engine ships, for callers with no file to read — and the fallback that keeps a
/// missing data file from being a crash.
///
/// IDENTICAL to what the deleted constants described, so the match this produces is the one the
/// golden log records. The file in `data/opening.lua` says the same thing; this is the same
/// values in C++ so that a build with no data directory still plays.
[[nodiscard]] Opening defaultOpening();

} // namespace rm::data
