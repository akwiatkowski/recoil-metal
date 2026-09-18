#include "core/sim/Adjacency.hpp"

namespace rm::sim {

bool skirtsShareEdge(Fx ax, Fx az, Fx aHalfX, Fx aHalfZ, Fx bx, Fx bz, Fx bHalfX,
                     Fx bHalfZ, Fx tolerance) noexcept {
    const Fx dx = ax > bx ? ax - bx : bx - ax;
    const Fx dz = az > bz ? az - bz : bz - az;
    const Fx reachX = aHalfX + bHalfX;
    const Fx reachZ = aHalfZ + bHalfZ;

    // Touching along X (side by side): the x-gap is within tolerance of the two halves
    // meeting, and the z-intervals genuinely overlap — an edge, not a corner. And the
    // mirror case. Overlapping rects count too: free placement allows them, and a
    // structure standing ON the apron is no less adjacent than one beside it. Grid
    // placement passes a ZERO tolerance: there the edges meet exactly or not at all, which
    // is retail's rule (C-074) and the reason the slack existed only for free placement.
    const bool sideBySide = dx <= reachX + tolerance && dz < reachZ;
    const bool endToEnd = dz <= reachZ + tolerance && dx < reachX;
    return sideBySide || endToEnd;
}

void adjacencyEffects(const UnitStore& store, const UnitCatalog& catalog,
                      std::vector<AdjacencyEffects>& out, Fx tolerance) {
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
                                 b.info->skirtHalfZElmos, tolerance)) {
                continue;
            }
            if (b.info->receives) {
                AdjacencyEffects& onB = out[b.slot];
                onB.massProduction += a.info->givesMassProduction[b.info->sizeIndex];
                onB.energyProduction += a.info->givesEnergyProduction[b.info->sizeIndex];
                onB.energyUpkeep += a.info->givesEnergyUpkeep[b.info->sizeIndex];
                onB.massBuild += a.info->givesMassBuild[b.info->sizeIndex];
                onB.energyBuild += a.info->givesEnergyBuild[b.info->sizeIndex];
                // The RateOfFire grant reaches only retail's receiver — SIZE4
                // ARTILLERY with a weapon (`C-051`(b)/(c)).
                if (b.info->receivesRateOfFire) {
                    onB.rateOfFire += a.info->givesRateOfFire[b.info->sizeIndex];
                }
            }
            if (a.info->receives) {
                AdjacencyEffects& onA = out[a.slot];
                onA.massProduction += b.info->givesMassProduction[a.info->sizeIndex];
                onA.energyProduction += b.info->givesEnergyProduction[a.info->sizeIndex];
                onA.energyUpkeep += b.info->givesEnergyUpkeep[a.info->sizeIndex];
                onA.massBuild += b.info->givesMassBuild[a.info->sizeIndex];
                onA.energyBuild += b.info->givesEnergyBuild[a.info->sizeIndex];
                if (a.info->receivesRateOfFire) {
                    onA.rateOfFire += b.info->givesRateOfFire[a.info->sizeIndex];
                }
            }
        }
    }
}

AdjacencyPreview adjacencyPreview(const UnitStore& store, const UnitCatalog& catalog,
                                  int army, const UnitCatalog::AdjacencyInfo& ghost, Fx x,
                                  Fx z, Fx tolerance) {
    AdjacencyPreview preview;
    if (!ghost.participates() || army < 0) {
        return preview;
    }
    const Fx gx = x + ghost.skirtCentreOffsetXElmos;
    const Fx gz = z + ghost.skirtCentreOffsetZElmos;
    const std::span<const Transform> transforms = store.transforms();
    const std::span<const MoveState> motion = store.motion();
    for (UnitIndex slot = 0; slot < store.slotCount(); ++slot) {
        if (!store.slotAlive(slot) || motion[slot].armyIndex != army) {
            continue;
        }
        const UnitCatalog::AdjacencyInfo& theirs = catalog.adjacency(store.typeAt(slot));
        if (!theirs.participates()) {
            continue;
        }
        if (!skirtsShareEdge(gx, gz, ghost.skirtHalfXElmos, ghost.skirtHalfZElmos,
                             transforms[slot].x + theirs.skirtCentreOffsetXElmos,
                             transforms[slot].z + theirs.skirtCentreOffsetZElmos,
                             theirs.skirtHalfXElmos, theirs.skirtHalfZElmos, tolerance)) {
            continue;
        }
        AdjacencyLink link{.slot = slot};
        if (ghost.receives) {
            link.toGhost.massProduction = theirs.givesMassProduction[ghost.sizeIndex];
            link.toGhost.energyProduction = theirs.givesEnergyProduction[ghost.sizeIndex];
            link.toGhost.energyUpkeep = theirs.givesEnergyUpkeep[ghost.sizeIndex];
            link.toGhost.massBuild = theirs.givesMassBuild[ghost.sizeIndex];
            link.toGhost.energyBuild = theirs.givesEnergyBuild[ghost.sizeIndex];
            if (ghost.receivesRateOfFire) {
                link.toGhost.rateOfFire = theirs.givesRateOfFire[ghost.sizeIndex];
            }
            preview.received.massProduction += link.toGhost.massProduction;
            preview.received.energyProduction += link.toGhost.energyProduction;
            preview.received.energyUpkeep += link.toGhost.energyUpkeep;
            preview.received.massBuild += link.toGhost.massBuild;
            preview.received.energyBuild += link.toGhost.energyBuild;
            preview.received.rateOfFire += link.toGhost.rateOfFire;
        }
        if (theirs.receives) {
            link.fromGhost.massProduction = ghost.givesMassProduction[theirs.sizeIndex];
            link.fromGhost.energyProduction = ghost.givesEnergyProduction[theirs.sizeIndex];
            link.fromGhost.energyUpkeep = ghost.givesEnergyUpkeep[theirs.sizeIndex];
            link.fromGhost.massBuild = ghost.givesMassBuild[theirs.sizeIndex];
            link.fromGhost.energyBuild = ghost.givesEnergyBuild[theirs.sizeIndex];
            if (theirs.receivesRateOfFire) {
                link.fromGhost.rateOfFire = ghost.givesRateOfFire[theirs.sizeIndex];
            }
        }
        // A touching pair with no grant in either direction is geometry, not adjacency —
        // no link, so no line and no row.
        if (link.toGhost.any() || link.fromGhost.any()) {
            preview.links.push_back(link);
        }
    }
    return preview;
}

} // namespace rm::sim
