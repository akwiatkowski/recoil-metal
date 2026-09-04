#include "core/sim/Assist.hpp"

#include "core/sim/Combat.hpp"

#include <algorithm>

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
        const QueuedCommand* head = orders[slot].active();
        if (head == nullptr || head->kind() != CommandKind::Assist
            || !store.alive(head->target())) {
            continue;
        }

        const unitdef::UnitDef* assister = catalog.def(store.typeAt(slot));
        if (assister != nullptr && assister->hasCategory("FACTORY") && !assister->isMobile()) {
            continue;  // factory guard mirrors queued production in command dispatch
        }

        const Mag rate = catalog.rates(store.typeAt(slot)).buildPerTick;
        if (rate <= Mag{}) {
            continue;
        }

        // C-183 follows Unit+0x4e0 (the active guard target) until it reaches the builder
        // actually doing the work. A cycle is malformed but legal to issue one edge at a time;
        // it contributes nothing instead of choosing an arbitrary member as the founder.
        UnitId founder = head->target();
        std::vector<UnitId> visited;
        bool cyclic = false;
        while (store.alive(founder)) {
            if (std::ranges::find(visited, founder) != visited.end()) {
                cyclic = true;
                break;
            }
            visited.push_back(founder);
            const QueuedCommand* guarded = orders[founder.index].active();
            if (guarded == nullptr || guarded->kind() != CommandKind::Assist
                || !store.alive(guarded->target())) {
                break;
            }
            founder = guarded->target();
        }
        if (cyclic || !store.alive(founder)) {
            continue;
        }

        // In reach of the resolved builder, not of the site. Intermediate guards follow the
        // next unit in the chain, so a stretched chain cannot lend build power at a distance.
        const Fx reach = catalog.rates(store.typeAt(slot)).buildReachElmos
                       + motion[slot].radiusElmos + motion[founder.index].radiusElmos;
        if (groundDistanceElmos(positionOf(transforms[slot]),
                                positionOf(transforms[founder.index]))
            > reach) {
            continue;  // still walking over
        }

        // The OLDEST unfinished construction this target founded — the one it is working
        // on. First match in list order, which is creation order, so every assister of one
        // target picks the same work and the answer is replay-stable.
        for (Construction& work : building) {
            if (work.finished() || !(work.builder == founder)) {
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
