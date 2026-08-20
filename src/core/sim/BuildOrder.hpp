#pragma once

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

/// The blueprints the script builds, one per role. UEF for every faction — the same
/// simplification the first extractor already documents (main.mm,
/// orderFirstExtractors): this milestone is about the match, not about
/// faction-specific structures, and four blueprints per role would demonstrate
/// nothing the one does not.
inline constexpr std::string_view kExtractorBlueprint = "/units/UEB1103/UEB1103_unit.bp";
inline constexpr std::string_view kPowerGeneratorBlueprint =
    "/units/UEB1101/UEB1101_unit.bp";
inline constexpr std::string_view kFactoryBlueprint = "/units/UEB0101/UEB0101_unit.bp";
inline constexpr std::string_view kTankBlueprint = "/units/UEL0201/UEL0201_unit.bp";

/// The same four as bare ids — what a loaded definition's `name` carries, and what
/// the caller counts standing units by.
inline constexpr std::string_view kExtractorId = "UEB1103";
inline constexpr std::string_view kPowerGeneratorId = "UEB1101";
inline constexpr std::string_view kFactoryId = "UEB0101";
inline constexpr std::string_view kTankId = "UEL0201";

/// The attack wave, in tanks. Arithmetic, not taste — from the blueprints:
/// a UEL0001 commander (12000 hp, and 100 dps: its zephyr states Damage = 100,
/// RateOfFire = 1) kills one 300 hp UEL0201 every 3 seconds, so a wave of N tanks
/// at 24 dps each (Damage = 24, RateOfFire = 1) lands roughly
/// 24 * 3 * N(N+1)/2 damage before it is gone. N = 20 gives 15,120 against the
/// commander's 12,000; N = 19 gives 13,680 — the margin over the model's
/// optimism (travel time, walls of dead tanks blocking the living) is deliberate,
/// and the stream of reinforcements behind the wave is what actually closes a
/// match the model gets wrong.
inline constexpr std::size_t kAttackWaveTanks = 20;

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
/// the stream never stops, and the stream is what wins matches the wave alone
/// cannot (see kAttackWaveTanks).
[[nodiscard]] bool wantsTank(const ArmyView& view) noexcept;

/// Whether this is the moment to launch the one attack wave. True exactly once:
/// at kAttackWaveTanks alive, and never again — after it, the caller sends every
/// new tank straight to the fight instead of reforming waves at home.
[[nodiscard]] bool launchesAttack(const Opponent& script, const ArmyView& view) noexcept;

/// Where the script puts structure `slot` (0 = power generator, 1 = factory):
/// fanned around the start position, pushed toward the map centre.
///
/// Toward the centre because that is where a stock map's start plateau extends —
/// starts sit at the map's edge with their flat ground inward — and fanned
/// perpendicular so the structures clear each other. 40 elmos out and ±28 apart
/// clears the factory, the largest footprint the script builds. The height is
/// carried through untouched; the caller grounds the site on the height field.
[[nodiscard]] std::array<float, 3> structureSite(const std::array<float, 3>& start,
                                                 float centreX, float centreZ,
                                                 int slot) noexcept;

/// Where a factory-built tank first drives: past the factory, toward the centre,
/// far enough that the next tank off the line does not roll into its back.
[[nodiscard]] std::array<float, 2> rolloffPoint(const std::array<float, 3>& factory,
                                                float centreX, float centreZ) noexcept;

} // namespace rm::sim
