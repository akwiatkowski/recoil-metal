#include "core/sim/Adjacency.hpp"

#include <algorithm>

namespace rm::sim {

bool skirtsShareEdge(Fx ax, Fx az, Fx aHalfX, Fx aHalfZ, Fx bx, Fx bz, Fx bHalfX,
                     Fx bHalfZ) noexcept {
    const Fx dx = ax > bx ? ax - bx : bx - ax;
    const Fx dz = az > bz ? az - bz : bz - az;
    const Fx reachX = aHalfX + bHalfX;
    const Fx reachZ = aHalfZ + bHalfZ;

    // Touching along X (side by side): the x-gap is within tolerance of the two halves
    // meeting, and the z-intervals genuinely overlap — an edge, not a corner. And the
    // mirror case. Overlapping rects count too: free placement allows them, and a
    // structure standing ON the apron is no less adjacent than one beside it.
    const bool sideBySide = dx <= reachX + kAdjacencyGapElmos && dz < reachZ;
    const bool endToEnd = dz <= reachZ + kAdjacencyGapElmos && dx < reachX;
    return sideBySide || endToEnd;
}

void adjacencyEffects(const UnitStore& store, const UnitCatalog& catalog,
                      std::vector<AdjacencyEffects>& out) {
    out.assign(store.slotCount(), AdjacencyEffects{});

    // The participants, gathered once: alive, skirted. A few dozen in a real match.
    struct Participant {
        UnitIndex slot;
        Fx x, z;
        int army;
        const UnitCatalog::AdjacencyInfo* info;
    };
    std::vector<Participant> standing;
    const std::span<const Transform> transforms = store.transforms();
    const std::span<const MoveState> motion = store.motion();
    for (UnitIndex slot = 0; slot < store.slotCount(); ++slot) {
        if (!store.slotAlive(slot)) {
            continue;
        }
        const UnitCatalog::AdjacencyInfo& info = catalog.adjacency(store.typeAt(slot));
        if (!info.participates()) {
            continue;
        }
        standing.push_back(Participant{
            .slot = slot,
            .x = transforms[slot].x + info.skirtCentreOffsetXElmos,
            .z = transforms[slot].z + info.skirtCentreOffsetZElmos,
            .army = motion[slot].armyIndex,
            .info = &info,
        });
    }

    // Every pair, both directions: a gives to b AND b gives to a — a storage boosts the
    // extractor while the extractor (in the full game) discounts the storage's builds.
    // Additive: the adds accumulate on the receiver's own row.
    for (std::size_t i = 0; i < standing.size(); ++i) {
        for (std::size_t j = i + 1; j < standing.size(); ++j) {
            const Participant& a = standing[i];
            const Participant& b = standing[j];
            if (a.army != b.army || a.army < 0) {
                continue;  // no bonus across the front line
            }
            if (!skirtsShareEdge(a.x, a.z, a.info->skirtHalfXElmos, a.info->skirtHalfZElmos,
                                 b.x, b.z, b.info->skirtHalfXElmos,
                                 b.info->skirtHalfZElmos)) {
                continue;
            }
            if (b.info->receives) {
                AdjacencyEffects& onB = out[b.slot];
                onB.massProduction += a.info->givesMassProduction[b.info->sizeIndex];
                onB.energyProduction += a.info->givesEnergyProduction[b.info->sizeIndex];
                onB.energyUpkeep += a.info->givesEnergyUpkeep[b.info->sizeIndex];
            }
            if (a.info->receives) {
                AdjacencyEffects& onA = out[a.slot];
                onA.massProduction += b.info->givesMassProduction[a.info->sizeIndex];
                onA.energyProduction += b.info->givesEnergyProduction[a.info->sizeIndex];
                onA.energyUpkeep += b.info->givesEnergyUpkeep[a.info->sizeIndex];
            }
        }
    }

    // A discount cannot go below free: enough T3 generators around one structure would
    // otherwise push its upkeep negative and PAY the owner to run it.
    for (AdjacencyEffects& effects : out) {
        effects.energyUpkeep = std::max(effects.energyUpkeep, Fx{});
        effects.massProduction = std::max(effects.massProduction, Fx{});
        effects.energyProduction = std::max(effects.energyProduction, Fx{});
    }
}

} // namespace rm::sim
