#include "core/sim/Reclaim.hpp"

#include "core/sim/Combat.hpp"

#include <algorithm>

namespace rm::sim {

Fx reclaimReach(const UnitCatalog& catalog, UnitTypeIndex type, const MoveState& reclaimer,
                const Feature& wreck) noexcept {
    return catalog.rates(type).buildReachElmos + reclaimer.radiusElmos + wreck.radiusElmos;
}

std::size_t harvestReclaim(UnitStore& store, const UnitCatalog& catalog,
                           FeatureStore& features, std::span<Economy> economies) {
    std::size_t harvesting = 0;

    const std::span<const CommandQueue> orders = store.orders();
    const std::span<const Transform> transforms = store.transforms();
    const std::span<const MoveState> motion = store.motion();

    for (UnitIndex slot = 0; slot < orders.size(); ++slot) {
        if (!store.slotAlive(slot)) {
            continue;
        }
        const Command* head = orders[slot].current();
        if (head == nullptr || head->kind != CommandKind::Reclaim) {
            continue;
        }
        Feature* wreck = features.findMutable(head->target);
        if (wreck == nullptr) {
            continue;  // emptied by someone else; `advanceOrders` retires the order
        }

        const UnitTypeIndex type = store.typeAt(slot);
        const Fx gap = groundDistanceElmos(positionOf(transforms[slot]), wreck->at);
        if (gap > reclaimReach(catalog, type, motion[slot], *wreck)) {
            continue;  // still walking there
        }

        // The drain: BuildRate × reclaimPerBuildRate value per second, already per tick
        // via the catalog's derivation. A non-builder's buildPerTick is zero, so a tank
        // ordered onto a wreck stands there achieving nothing — the same nothing the
        // game gives it.
        const Mag pace = catalog.rates(type).buildPerTick * wreck->reclaimPerBuildRate;
        if (pace <= Mag{}) {
            continue;
        }

        const Mag massGrant = std::min(wreck->massRemaining, pace);
        const Mag energyGrant = std::min(wreck->energyRemaining, pace);
        wreck->massRemaining -= massGrant;
        wreck->energyRemaining -= energyGrant;

        const int owner = motion[slot].armyIndex;
        if (owner >= 0 && static_cast<std::size_t>(owner) < economies.size()) {
            // Straight into the store. `tickEconomy` clamps to storage the same tick, so
            // reclaiming over a full mass bar overflows and is lost — the same rule as
            // every other income, and the game's (the Lua corpus applies no special cap
            // on the reclaim path; see the survey).
            economies[static_cast<std::size_t>(owner)].stored.mass += massGrant;
            economies[static_cast<std::size_t>(owner)].stored.energy += energyGrant;
        }
        ++harvesting;

        if (wreck->massRemaining <= Mag{} && wreck->energyRemaining <= Mag{}) {
            // Emptied: the wreck is gone, scorch record and all — reclaimed ground is
            // clean ground, which is what the game shows too.
            features.remove(head->target);
        }
    }

    return harvesting;
}

} // namespace rm::sim
