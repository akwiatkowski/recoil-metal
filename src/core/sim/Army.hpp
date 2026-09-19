#pragma once

#include "core/Types.hpp"
#include "core/sim/Fx.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace rm::sim {

/// The army index of something nobody owns.
///
/// Not 0, which would be a real army: a scattered decorative unit, a prop that found its way
/// into the list, or a spawn whose owner was never set would all silently belong to the first
/// player. Unowned things are neither selectable nor shootable, so the wrong default here reads
/// as the enemy having units it never built.
///
/// Lived in `Movement.hpp` until P2.4, next to the field that uses it. It belongs here: it is
/// a fact about ownership, and `Army.hpp` is where the other two levels are.
inline constexpr int kNoArmy = -1;

// Who owns what, and who is shooting at whom.
//
// Until now nothing in this engine knew: a unit belonged to a batch, team colour was
// indexed by batch (core/scene/TeamColours.hpp), and "select that unit" meant any
// unit. Every milestone after this one needs an owner — a weapon needs to know what
// counts as a target, an economy needs somebody to bank the mass, and a victory
// condition needs somebody to lose.
//
// THE THREE LEVELS, and the vocabulary trap that comes with them (PLAN2.md §6.3).
//
// Ownership has three levels, and they are not redundant. Several PARTICIPANTS can drive one
// side — a human plus a helper script, or two humans sharing control. A side is what owns units
// and banks resources. And what WINS is neither of those: allies win together, so the thing
// that is eliminated is a group of sides.
//
//   `Player`        — a participant, human or script. Several may command one army.
//   `Army`          — owns units, banks resources, has a faction and a colour.
//   `AllianceIndex` — who wins together, and later who shares vision.
//
// **THE NAMING, WHICH DIVERGES FROM §6.3 DELIBERATELY.** The plan says to call the middle level
// `Team`, following Recoil. We call it `Army`, following Forged Alliance — and the reason is
// that FA is the content we read. `ArmyIndex` runs through all of `lua/sim/`, a `.scmap`'s
// start positions are per army, and `mapinfo.lua` uses `team` for what we call an ALLIANCE.
// Renaming to `Team` would align 477 call sites with an engine we do not load content from,
// and misalign them with the one we do — and would make our `team` mean the opposite of the
// map file's `team`. So:
//
//   | this engine      | Forged Alliance | Recoil     |
//   |------------------|-----------------|------------|
//   | `Player`         | (none)          | `CPlayer`  |
//   | `Army`           | `Army`          | `CTeam`    |
//   | `AllianceIndex`  | `team`          | `AllyTeam` |
//
// That table is the "one place" §6.3 asks for. If a future session prefers Recoil's spelling,
// the cost is in the table, not in the code.
//
// `AllianceIndex` is an index and not a struct, for now, because it would be a struct with one
// member. It becomes one when it holds shared vision, which is the first thing that is
// genuinely per-alliance rather than per-army.

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

    /// Who this army wins with. Allies share one.
    ///
    /// Renamed from `team`, which was the same field doing the same job under a name that
    /// collided with `mapinfo.lua`'s own `team` — see the table above. Free-for-all means
    /// every army has its own: `alliance` defaulting to `index` would need a constructor, so
    /// the builder below sets it and the default here is only a value.
    int alliance = 0;

    // `TeamColour colour` used to be here, and taking it out is §7 P6.3's assertion made true
    // rather than a tidy-up: it was the last thing that made `core/sim` include `core/scene`.
    //
    // A colour is not a fact about an army — it is how one is DRAWN. Two armies with the same
    // colour play an identical match, which is exactly the test for presentation, and the state
    // hash already skipped it for that reason. The palette lives in `core/scene/TeamColours.hpp`
    // and the renderer indexes it by army, which is one call at the one place that needed it.

    /// Whether this army still has a commander. The victory condition, in one bool,
    /// and the reason it lives on the army rather than being derived from the unit
    /// list: a defeated army's units may still be standing while it plays no further
    /// part.
    bool defeated = false;

    /// `victory.lua`'s `OfferingDraw` (`SimUtils.SetOfferDraw`): when every
    /// surviving army offers, the match ends in a draw immediately.
    bool offeringDraw = false;

    /// `victory.lua`'s `RequestingAlliedVictory` (`SimUtils.RequestAlliedVictory`,
    /// `C-346`): a multi-member surviving alliance wins only when every surviving
    /// member asked for the shared win — the tick's terminal check reads it.
    /// `simInit.lua:200` sets it for armies teamed at setup; a sole survivor
    /// needs no one's consent and wins unconditionally.
    bool requestingAlliedVictory = false;

    /// `brain:SetResourceSharing` (`SimUtils.SetResourceSharing`, `C-346`): the
    /// army opted into sharing resources with allies. Presentation state —
    /// retail's flag gates the UI's sharing controls, and `GiveResourcesToPlayer`
    /// does not consult it, so neither does the transfer here.
    bool resourceSharing = false;

    /// `ScenarioInfo.Options.UnitCap`, retail's per-army ceiling on live units
    /// (`CArmyImpl` vfuncs `0xa4`/`0xa8`; the engine writes 500 at session
    /// create when the option is absent — `0x008e8035`). The creation gate at
    /// `0x0074fda0` refuses `costTotal + CapCost > unitCap`, so this is a
    /// fixed-point count: blueprint `CapCost` is fractional (0.1 sonars).
    Fx unitCap = Fx::fromInt(500);

    /// Retail's per-army handicap (`C-060`): `DealDamage` divides the damage a
    /// unit takes by `(handicap + 1)` — the TARGET's army, not the shooter's.
    /// Zero is the unhandicapped default; the skirmish option never sets it in
    /// this engine, so the field exists for the damage rule rather than for
    /// content.
    int handicap = 0;

    /// `aibrain.lua:374`'s AIx flag (C-360): a personality whose name contains
    /// 'cheat' calls `AIUtils.SetupCheat`, which sets `brain.CheatEnabled` and runs
    /// `ApplyCheatBuffs` over the army — `CheatIncome` (MassProduction/
    /// EnergyProduction Mult 2.0) and `CheatBuildRate` (BuildRate Mult 2.0) on every
    /// unit, `IntelCheat` (VisionRadius/OmniRadius +10000) on COMMAND units
    /// (`CheatBuffs.lua`, `aiutilities.lua:1763-1779`). Retail hangs the buffs on the
    /// units; we keep the flag on the army and read it where the buffed values are
    /// consumed — `Unit.lua:209`'s OnCreate re-application then comes free, because a
    /// unit spawned later belongs to the same army.
    bool cheatEnabled = false;

    /// `GetArmyUnitCostTotal` (`CArmyImpl` vfunc `0x4c`): the army's live
    /// `CapCost` sum, recomputed each tick from the store. Derived state —
    /// never saved, never hashed; the units it sums already are.
    Fx unitCostTotal{};
};

/// Whether a blueprint id names a commander.
///
/// The victory condition rests on this, so it is a lookup against the four rather than a
/// pattern match on the id: `UEL0001` is a commander and `UEL0101` is a tank, one
/// character apart, and a prefix rule would end a match when a tank died.
[[nodiscard]] bool isCommanderId(std::string_view blueprintId) noexcept;

/// Marks every army with no qualifying units left as defeated, and returns how many newly fell.
///
/// `survivalUnits` is indexed by army. When `requireCommanderEver` is true, an army that never
/// had a commander is not defeated: Assassination distinguishes "never had" from "lost it".
/// Category predicates pass false because retail tests their current unit count directly.
[[nodiscard]] std::size_t applyDefeats(std::vector<Army>& armies,
                                        std::span<const int> survivalUnits,
                                        std::span<const int> commandersEver,
                                        bool requireCommanderEver = true);

/// Who has won, or nothing while the match is still on.
///
/// AN ALLIANCE rather than an army, since allies win together — which is the property §7 P2.4
/// asks to be tested: a 2v2 in which one army of a pair dies is not over, and the same match
/// with the pair split into four is. Nothing when two or more alliances survive, and nothing
/// when none does — that last is a draw, a legitimate outcome (two commanders inside one
/// blast) rather than an error.
[[nodiscard]] std::optional<int> winningAlliance(const std::vector<Army>& armies) noexcept;

/// Whether two armies are on the same side. An army is allied with itself, which
/// matters because "do not shoot allies" would otherwise have every unit shoot
/// itself.
[[nodiscard]] bool allied(const Army& a, const Army& b) noexcept;

/// Whether `a` may shoot `b`. Not simply `!allied`: a defeated army is nobody's
/// target, so a corpse army does not keep drawing fire.
[[nodiscard]] bool hostile(const Army& a, const Army& b) noexcept;

/// A participant: who is giving the orders.
///
/// THE LEVEL THAT WAS MISSING. This header used to say "deliberately not a player", which
/// §6.3 reads as the design admitting a gap — and it was right. Without it there is nowhere to
/// record that a human and a helper script are driving the same army, or which army the
/// player at the keyboard is, and both of those are things the app currently keeps in an
/// `int` beside the scene.
///
/// Several players may name the same army. That is the point of having the level at all, and
/// the thing `commandersOf` below answers.
struct Player {
    PlayerIndex index = 0;

    /// The army whose units this player commands. NOT unique across players.
    int army = kNoArmy;

    /// Whether a human is driving. A script is not a lesser player — it issues the same
    /// orders through the same path (P2.5) — this only says which source they come from.
    bool human = false;

    /// For the HUD and the log. Not identity: two players may share a name and still be two.
    std::string name;
};

/// Whether `player` commands `army`.
[[nodiscard]] bool commands(const Player& player, int army) noexcept;

/// Every player commanding an army, in index order.
///
/// Order matters and is the players' own: an order applied by two players in a different
/// sequence would be a different match.
[[nodiscard]] std::vector<PlayerIndex> commandersOf(std::span<const Player> players,
                                                    int army);

/// One human at the keyboard against `armyCount - 1` scripts, which is what every entry point
/// in this engine currently builds by hand.
[[nodiscard]] std::vector<Player> onePlayerPerArmy(std::size_t armyCount,
                                                  int humanArmy = kNoArmy);

/// Builds a free-for-all: one army per start position, each its own alliance, colours
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
