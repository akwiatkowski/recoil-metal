#include "core/sim/Assist.hpp"

#include "core/sim/Combat.hpp"

namespace rm::sim {

std::size_t applyAssistance(const UnitStore& store, const UnitCatalog& catalog,
                            std::vector<Construction>& building) {
    // Cleared first, unconditionally: last tick's help is not this tick's fact.
    for (Construction& work : building) {
        work.assistPerTick = Mag{};
    }
    if (building.empty()) {
        return 0;
    }

    std::size_t helping = 0;
    const std::span<const CommandQueue> orders = store.orders();
    const std::span<const Transform> transforms = store.transforms();
    const std::span<const MoveState> motion = store.motion();

    for (UnitIndex slot = 0; slot < orders.size(); ++slot) {
        if (!store.slotAlive(slot)) {
            continue;
        }
        const Command* head = orders[slot].active();
        if (head == nullptr || head->kind != CommandKind::Assist
            || !store.alive(head->target)) {
            continue;
        }

        const Mag rate = catalog.rates(store.typeAt(slot)).buildPerTick;
        if (rate <= Mag{}) {
            continue;
        }

        // In reach of the TARGET, not of the site: "help that engineer" follows the
        // engineer, and the game's own assist is a guard order on the unit too.
        const Fx reach = catalog.rates(store.typeAt(slot)).buildReachElmos
                       + motion[slot].radiusElmos + motion[head->target.index].radiusElmos;
        if (groundDistanceElmos(positionOf(transforms[slot]),
                                positionOf(transforms[head->target.index]))
            > reach) {
            continue;  // still walking over
        }

        // The OLDEST unfinished construction this target founded — the one it is working
        // on. First match in list order, which is creation order, so every assister of one
        // target picks the same work and the answer is replay-stable.
        for (Construction& work : building) {
            if (work.finished() || !(work.builder == head->target)) {
                continue;
            }
            work.assistPerTick += rate;
            ++helping;
            break;
        }
    }

    return helping;
}

} // namespace rm::sim
