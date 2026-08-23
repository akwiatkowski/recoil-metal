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

// `AdjacencyBuffs.lua:206-247`, the slice this engine consumes. Rows are receiver sizes
// SIZE4..SIZE20. The numbers are ≈ K/n by design: a full ring of neighbours yields a
// constant total (4 × 0.25 = 8 × 0.125 = 100% for energy storage), so a partial ring
// gives proportionally less by COUNT — there is no edge-length fraction anywhere in the
// original either.
constexpr AdjacencyGrants kNone{};

constexpr AdjacencyGrants kT1PowerGenerator{
    .energyMaintenance = {-0.0625f, -0.03125f, -0.0208f, -0.01563f, -0.0125f},
};
constexpr AdjacencyGrants kT2PowerGenerator{
    .energyMaintenance = {-0.125f, -0.125f, -0.125f, -0.125f, -0.125f},
};
constexpr AdjacencyGrants kT3PowerGenerator{
    .energyMaintenance = {-0.1875f, -0.1875f, -0.1875f, -0.1875f, -0.1875f},
};
constexpr AdjacencyGrants kT1EnergyStorage{
    .energyProduction = {0.25f, 0.125f, 0.083334f, 0.0625f, 0.05f},
};
constexpr AdjacencyGrants kT1MassStorage{
    .massProduction = {0.125f, 0.0625f, 0.03f, 0.03125f, 0.025f},
};

} // namespace

const AdjacencyGrants& adjacencyGrants(AdjacencyClass which) noexcept {
    switch (which) {
    case AdjacencyClass::T1PowerGenerator:
        return kT1PowerGenerator;
    case AdjacencyClass::T2PowerGenerator:
    case AdjacencyClass::Hydrocarbon:
        // `adj.Hydrocarbon = adj.T2PowerGenerator` — the file's own aliasing.
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
