#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace rm::unitdef {

// The adjacency buff tables, from the game's own `lua/sim/AdjacencyBuffs.lua:206-247`.
//
// HOW THE ORIGINAL WORKS, in one paragraph so the numbers below have a shape to hang on.
// A structure's root `Adjacency` field names the table it GRANTS to whatever stands
// skirt-to-skirt with it — `T1MassStorage`, `T1PowerGenerator` — and each grant is an
// ADDITIVE percentage (`Affects = {[t]={Add=add}}`, `Stacks = 'ALWAYS'`) keyed by the
// RECEIVER's size category (`EntityCategory = 'STRUCTURE SIZE<n>'`): a big receiver has
// more edge to share, so each single neighbour moves it less. Production bonuses are
// positive (storage feeding a producer), consumption and maintenance bonuses negative
// (a generator discounting its neighbours' running costs).
//
// WHAT THIS ENGINE TAKES, and what it defers by name: `MassProduction` and
// `EnergyProduction` (storage beside a producer), and `EnergyMaintenance` (a generator
// beside anything with upkeep). Deferred: `EnergyActive`/`MassActive` (a discount on the
// consumption of a structure that is actively BUILDING — this engine's `Construction`
// does not know which structure is doing the work), and `EnergyWeapon`/`RateOfFire`
// (weapons cost no energy here yet). Named so their absence is a decision on record
// rather than a surprise.

/// Which buff table a structure grants — the root `Adjacency` field, as a closed set.
/// `Hydrocarbon` shares `T2PowerGenerator`'s numbers by the file's own
/// `adj.Hydrocarbon = adj.T2PowerGenerator`.
enum class AdjacencyClass : std::uint8_t {
    None,
    T1PowerGenerator,
    T2PowerGenerator,
    T3PowerGenerator,
    Hydrocarbon,
    T1MassExtractor,
    T2MassExtractor,
    T3MassExtractor,
    T1MassFabricator,
    T3MassFabricator,
    T1EnergyStorage,
    T1MassStorage,
};

/// The `Adjacency = '<name>AdjacencyBuffs'` string, resolved. Unknown names — a mod's own
/// table — read as `None` rather than as somebody else's numbers.
[[nodiscard]] AdjacencyClass adjacencyClassFromName(std::string_view name) noexcept;

/// The receiver-size axis: `STRUCTURE SIZE4/8/12/16/20`, from the receiver's own
/// `Categories`. Index 0..4.
///
/// A structure stating no SIZE category currently has one DERIVED from its skirt
/// (`sim::UnitCatalog`), which is an invention: retail gates every adjacency buff on
/// `EntityCategory = 'STRUCTURE SIZEn'`, so such a structure is permanently inert there. 174
/// of the 568 shipped units are in that position. It matters little today — only one of them
/// has any modelled production or upkeep — but it will the moment build-cost adjacency lands.
/// See claim `C-073`.
inline constexpr std::size_t kAdjacencySizeSteps = 5;

/// One giver's additive grants, per receiver size. `constexpr float` because these are
/// authored constants (the sim receives them as fixed point through the catalog, which
/// converts once at registration — the same boundary every blueprint rate crosses).
struct AdjacencyGrants {
    /// `MassProduction` / `EnergyProduction`: added to the receiving PRODUCER's output.
    std::array<float, kAdjacencySizeSteps> massProduction{};
    std::array<float, kAdjacencySizeSteps> energyProduction{};
    /// `EnergyMaintenance`: added to the receiving structure's upkeep — negative, a
    /// discount.
    std::array<float, kAdjacencySizeSteps> energyMaintenance{};
};

/// The table for one class — `AdjacencyBuffs.lua:206-247`, transcribed. Zeroes for `None`.
[[nodiscard]] const AdjacencyGrants& adjacencyGrants(AdjacencyClass which) noexcept;

} // namespace rm::unitdef
