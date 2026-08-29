#include "core/unit/Adjacency.hpp"

namespace rm::unitdef {

AdjacencyClass adjacencyClassFromName(std::string_view name) noexcept {
    // The root field spells `<class>AdjacencyBuffs`; both the full spelling and the bare
    // class resolve, so a test can say the short thing the table says.
    const auto matches = [name](std::string_view klass) {
        if (name == klass) {
            return true;
        }
        constexpr std::string_view kSuffix = "AdjacencyBuffs";
        return name.size() == klass.size() + kSuffix.size() && name.starts_with(klass)
               && name.ends_with(kSuffix);
    };
    if (matches("T1PowerGenerator")) { return AdjacencyClass::T1PowerGenerator; }
    if (matches("T2PowerGenerator")) { return AdjacencyClass::T2PowerGenerator; }
    if (matches("T3PowerGenerator")) { return AdjacencyClass::T3PowerGenerator; }
    if (matches("Hydrocarbon")) { return AdjacencyClass::Hydrocarbon; }
    if (matches("T1MassExtractor")) { return AdjacencyClass::T1MassExtractor; }
    if (matches("T2MassExtractor")) { return AdjacencyClass::T2MassExtractor; }
    if (matches("T3MassExtractor")) { return AdjacencyClass::T3MassExtractor; }
    if (matches("T1MassFabricator")) { return AdjacencyClass::T1MassFabricator; }
    if (matches("T3MassFabricator")) { return AdjacencyClass::T3MassFabricator; }
    if (matches("T1EnergyStorage")) { return AdjacencyClass::T1EnergyStorage; }
    if (matches("T1MassStorage")) { return AdjacencyClass::T1MassStorage; }
    return AdjacencyClass::None;
}

namespace {

// `AdjacencyBuffs.lua`, the slice this engine consumes. Rows are receiver sizes
// SIZE4..SIZE20. A partial ring gives proportionally less by COUNT — there is no edge-length
// fraction anywhere in the original.
constexpr AdjacencyGrants kNone{};

// THE INVARIANT THAT CHECKS THESE TABLES. For a giver with a 2x2 skirt, `Add x n` is constant
// across the five size rows, where `n` is how many 2x2 structures fit around a receiver of that
// size — 4, 8, 12, 16, 20. So the row values are one number divided by those counts, and any
// transcription slip shows up as a row that breaks the product. Two did (`C-072`).
//
//   T1 power generator   -0.25   fully surrounded
//   T1 energy storage    +0.50
//   T1 mass storage      +0.50
//
// Givers with a 6x6 or 8x8 skirt can only occupy one side, so their `Add` is flat across rows
// and caps at four neighbours: T2 power -0.5, T3 power -0.75.
constexpr AdjacencyGrants kT1PowerGenerator{
    // Written as the file writes them. The last three used to be truncated to `-0.0208`,
    // `-0.01563`, `-0.0125`; those quantise to identical Q18.14 values, so nothing changed
    // numerically, but a literal that does not match the source silently stops matching if the
    // fixed-point format ever gains bits.
    .energyMaintenance = {-0.0625f, -0.03125f, -0.020833f, -0.015625f, -0.0125f},
};
constexpr AdjacencyGrants kT2PowerGenerator{
    .energyMaintenance = {-0.125f, -0.125f, -0.125f, -0.125f, -0.125f},
};
constexpr AdjacencyGrants kT3PowerGenerator{
    .energyMaintenance = {-0.1875f, -0.1875f, -0.1875f, -0.1875f, -0.1875f},
};
// BOTH OF THESE WERE WRONG, and the invariant above is what makes that visible.
//
// The energy row was every value DOUBLED: a full ring gave +100% where retail gives +50%.
// The mass row was right in four places and had `0.03` where the file says `0.041667`, which
// made a full ring of twelve give +36% instead of +50%. Neither is the sort of thing playtesting
// finds — an energy-storage ring that is twice as good reads as a balance opinion.
//
// Retail: `AdjacencyBuffs.lua`, `T1EnergyStorageEnergyProductionBonusSize4..20` and
// `T1MassStorageMassProductionBonusSize4..20`. Claim `C-072`.
constexpr AdjacencyGrants kT1EnergyStorage{
    .energyProduction = {0.125f, 0.0625f, 0.041667f, 0.03125f, 0.025f},
};
constexpr AdjacencyGrants kT1MassStorage{
    .massProduction = {0.125f, 0.0625f, 0.041667f, 0.03125f, 0.025f},
};

} // namespace

const AdjacencyGrants& adjacencyGrants(AdjacencyClass which) noexcept {
    switch (which) {
    case AdjacencyClass::T1PowerGenerator:
        return kT1PowerGenerator;
    case AdjacencyClass::T2PowerGenerator:
    case AdjacencyClass::Hydrocarbon:
        // `HydrocarbonAdjacencyBuffs` is a separate table that lists the T2 power
        // generator's sixteen buff names verbatim — the same grants, not an alias.
        // (The comment here used to claim the file aliased them. It does not.)
        return kT2PowerGenerator;
    case AdjacencyClass::T3PowerGenerator:
        return kT3PowerGenerator;
    case AdjacencyClass::T1EnergyStorage:
        return kT1EnergyStorage;
    case AdjacencyClass::T1MassStorage:
        return kT1MassStorage;
    case AdjacencyClass::T1MassExtractor:
    case AdjacencyClass::T2MassExtractor:
    case AdjacencyClass::T3MassExtractor:
    case AdjacencyClass::T1MassFabricator:
    case AdjacencyClass::T3MassFabricator:
        // These grant only `MassActive` — a discount on a neighbour's BUILD drain, which
        // is deferred by name in the header. Their entry here is deliberate: the class
        // resolves, the grants are zero, and the day build-drain adjacency lands the
        // numbers go here rather than into a new mechanism.
        return kNone;
    case AdjacencyClass::None:
        return kNone;
    }
    return kNone;
}

} // namespace rm::unitdef
