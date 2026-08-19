#pragma once

#include "core/scene/TeamColours.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace rm::sim {

// Who owns what, and who is shooting at whom.
//
// Until now nothing in this engine knew: a unit belonged to a batch, team colour was
// indexed by batch (core/scene/TeamColours.hpp), and "select that unit" meant any
// unit. Every milestone after this one needs an owner — a weapon needs to know what
// counts as a target, an economy needs somebody to bank the mass, and a victory
// condition needs somebody to lose.
//
// Deliberately not a player. There is no lobby, no handicap and no diplomacy here;
// an army is the smallest thing the sim can attribute a unit to.

/// The four the game ships. Read from a blueprint's `General.FactionName`, which all
/// 568 state and which takes exactly these four values across the corpus — so this is
/// the file's own answer rather than a list invented here.
enum class Faction : std::uint8_t { Uef, Aeon, Cybran, Seraphim };

[[nodiscard]] std::optional<Faction> factionFromName(std::string_view name) noexcept;

/// The spelling a blueprint uses, so a message can name a faction without a table of
/// its own.
[[nodiscard]] std::string_view factionName(Faction faction) noexcept;

/// The blueprint id of a faction's Armoured Command Unit.
///
/// A CONVENTION rather than a stated fact, and the one place this file guesses: the
/// ids run `UEL0001`, `UAL0001`, `URL0001`, `XSL0001` — faction letter, `L` for land,
/// then the number the game reserves for a commander. Verified against the archive
/// (all four exist, all four are `RULEUMT_Amphibious`, 10000-12000 hp) rather than
/// assumed from the pattern, because a pattern that holds for four cases is not a
/// rule. If a mod renames them this is what breaks, and it breaks loudly — the
/// blueprint simply is not there.
[[nodiscard]] std::string commanderBlueprintId(Faction faction) noexcept;

/// The VFS path of that commander's blueprint.
[[nodiscard]] std::string commanderBlueprintPath(Faction faction);

// One side of a match.
struct Army {
    /// Index into the army list, and what a unit stores to say who owns it.
    int index = 0;

    Faction faction = Faction::Uef;

    /// Allies share a team. Free-for-all means every army has its own, which is what
    /// the default gives: `team` defaulting to `index` would need a constructor, so
    /// the builder below sets it and the default here is only a value.
    int team = 0;

    /// What the player sees. OURS, not the game's — neither game stores a colour with
    /// a unit or a map, both assign at match start (see TeamColours.hpp).
    TeamColour colour{};

    /// Whether this army still has a commander. The victory condition, in one bool,
    /// and the reason it lives on the army rather than being derived from the unit
    /// list: a defeated army's units may still be standing while it plays no further
    /// part.
    bool defeated = false;
};

/// Whether a blueprint id names a commander.
///
/// The victory condition rests on this, so it is a lookup against the four rather than a
/// pattern match on the id: `UEL0001` is a commander and `UEL0101` is a tank, one
/// character apart, and a prefix rule would end a match when a tank died.
[[nodiscard]] bool isCommanderId(std::string_view blueprintId) noexcept;

/// Marks every army with no commander left as defeated, and returns how many newly fell.
///
/// `commandersAlive` is indexed by army: how many living commanders each still has. An
/// army that never had one — nothing spawned for it — is NOT defeated by this, because
/// "never had" and "lost it" are different states and a scene that spawns no commanders
/// at all should not declare everyone dead on the first tick.
[[nodiscard]] std::size_t applyDefeats(std::vector<Army>& armies,
                                       std::span<const int> commandersAlive,
                                       std::span<const int> commandersEver);

/// Who has won, or nothing while the match is still on.
///
/// A TEAM rather than an army, since allies win together. Nothing when two or more teams
/// survive, and nothing when none does — that last is a draw, which is a legitimate
/// outcome (two commanders inside one blast) rather than an error.
[[nodiscard]] std::optional<int> winningTeam(const std::vector<Army>& armies) noexcept;

/// Whether two armies are on the same side. An army is allied with itself, which
/// matters because "do not shoot allies" would otherwise have every unit shoot
/// itself.
[[nodiscard]] bool allied(const Army& a, const Army& b) noexcept;

/// Whether `a` may shoot `b`. Not simply `!allied`: a defeated army is nobody's
/// target, so a corpse army does not keep drawing fire.
[[nodiscard]] bool hostile(const Army& a, const Army& b) noexcept;

/// Builds a free-for-all: one army per start position, each its own team, colours
/// taken in order from the palette.
///
/// Factions are dealt round-robin from the four rather than randomised, so the same
/// map always produces the same match — which is what makes `--march` and
/// `--screenshot` reproducible, and is the same reason the scatter takes a fixed
/// seed.
[[nodiscard]] std::vector<Army> freeForAll(std::size_t armyCount);

/// The armies still in the match. One left means the match is over; zero means every
/// army died in the same tick, which is a draw rather than an error.
[[nodiscard]] std::size_t survivorCount(const std::vector<Army>& armies) noexcept;

} // namespace rm::sim
