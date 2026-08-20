#pragma once

#include "core/sim/Fx.hpp"

#include <array>
#include <cstddef>
#include <string_view>

namespace rm::sim {

// The scripted opponent — milestone 20's "not an AI", and labelled as such.
//
// A fixed build order plus one attack wave, nothing else: no scouting, no reaction,
// no economy management beyond the sequence below. Supreme Commander's own AI is
// 84,750 lines of Lua and is deliberately not being reimplemented (PLAN.md,
// non-goals); this file is the whole opponent, and every choice in it is OURS —
// the game names no build order — with the reason at the constant.
//
// Two actors, three questions. The commander asks "what structure next?", the
// factory asks "another tank?", and the army asks "is this the moment to attack?".
// They are separate functions rather than one state machine because the commander
// and the factory genuinely act at the same time — folding them together would
// serialize builders the sim runs in parallel.

// FOUR BLUEPRINT PATHS AND A WAVE SIZE USED TO LIVE HERE.
//
//     inline constexpr std::string_view kExtractorBlueprint =
//         "/units/UEB1103/UEB1103_unit.bp";
//     …
//     inline constexpr std::size_t kAttackWaveTanks = 20;
//
// All five are gone (PLAN2.md §7 P3.3). They were game rules in a C++ header — the thing §1.1
// says both reference engines keep none of — and the four paths were all UEF, so the opponent
// could only ever play one faction.
//
// What replaced them: `data/opening.lua` states the plan by ROLE, `core/data/Roster.hpp`
// answers "the T1 extractor for THIS faction" from the blueprint corpus, and the caller passes
// the resulting wave size in. One file now drives all four factions, and changing the opening
// is editing a data file rather than a rebuild.
//
// The arithmetic behind the wave size moved WITH it, into the data file's own comment, rather
// than being deleted along with the constant it justified.

/// One army's situation, as the script needs it. Built by the caller from the
/// scene each decision tick — the script holds no pointers into the sim, which is
/// what keeps it a pure function of what it can see.
struct ArmyView {
    bool commanderAlive = false;
    /// A structure of this army's is under construction (the commander is the only
    /// thing that builds structures here, so "busy" is the army's, not the unit's).
    bool commanderBusy = false;
    /// A tank is under construction at this army's factory.
    bool factoryBusy = false;
    std::size_t extractorsStanding = 0;
    std::size_t powerGeneratorsStanding = 0;
    std::size_t factoriesStanding = 0;
    std::size_t tanksAlive = 0;
};

/// What the script remembers between ticks: nothing but whether the one attack
/// has been called. Everything else is re-read from the scene, so a structure
/// that dies is simply rebuilt — the order below asks what is STANDING.
struct Opponent {
    bool attackLaunched = false;
};

/// What the commander should start next: the fixed order.
enum class StructureOrder : std::uint8_t {
    None,            ///< waiting, building, done, or dead
    PowerGenerator,  ///< first: everything after it is energy-bound
    Extractor,       ///< second extractor: tanks are mass-bound
    Factory,         ///< last: what the rest of the match comes out of
};

/// The commander's next structure, or None while it should wait.
///
/// None before the first extractor stands — that build is ordered at spawn
/// (orderFirstExtractors), and starting the power generator beside a 25%-funded
/// extractor would stall both. None once the factory stands: the commander's part
/// of the order is over.
[[nodiscard]] StructureOrder nextStructure(const ArmyView& view) noexcept;

/// Whether the factory should start another tank. Always yes when it stands idle:
/// the stream never stops, and the stream is what wins matches the wave alone cannot.
[[nodiscard]] bool wantsTank(const ArmyView& view) noexcept;

/// Whether this is the moment to launch the one attack wave. True exactly once: at `waveSize`
/// alive, and never again — after it, the caller sends every new tank straight to the fight
/// instead of reforming waves at home.
///
/// `waveSize` is a PARAMETER now rather than a constant read from this header. It comes from
/// `data/opening.lua`, which is what makes the number a balance decision rather than a rebuild.
[[nodiscard]] bool launchesAttack(const Opponent& script, const ArmyView& view,
                                  std::size_t waveSize) noexcept;

/// Where the script puts structure `slot` (0 = power generator, 1 = factory):
/// fanned around the start position, pushed toward the map centre.
///
/// Toward the centre because that is where a stock map's start plateau extends —
/// starts sit at the map's edge with their flat ground inward — and fanned
/// perpendicular so the structures clear each other. 40 elmos out and ±28 apart
/// clears the factory, the largest footprint the script builds. The height is
/// carried through untouched; the caller grounds the site on the height field.
[[nodiscard]] std::array<Fx, 3> structureSite(const std::array<Fx, 3>& start, Fx centreX,
                                              Fx centreZ,
                                                 int slot) noexcept;

/// Where a factory-built tank first drives: past the factory, toward the centre,
/// far enough that the next tank off the line does not roll into its back.
[[nodiscard]] std::array<Fx, 2> rolloffPoint(const std::array<Fx, 3>& factory, Fx centreX,
                                             Fx centreZ) noexcept;

} // namespace rm::sim
