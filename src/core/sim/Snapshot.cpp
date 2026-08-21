#include "core/sim/Snapshot.hpp"

#include "core/sim/UnitStore.hpp"

namespace rm::sim {

void snapshotInto(const UnitStore& store, TickIndex tick, Snapshot& out) {
    out.tick = tick;
    out.units.clear();
    out.units.reserve(store.liveCount());

    const std::span<const Transform> transforms = store.transforms();
    const std::span<const MoveState> motion = store.motion();
    const std::span<const Health> health = store.health();

    for (UnitIndex slot = 0; slot < store.slotCount(); ++slot) {
        if (!store.slotAlive(slot)) {
            continue;  // a tombstone: the renderer draws what exists
        }
        out.units.push_back(UnitView{
            .id = store.idAt(slot),
            .type = store.typeAt(slot),
            .armyIndex = slot < motion.size() ? motion[slot].armyIndex : kNoArmy,
            .transform = transforms[slot],
            .distanceTravelledElmos =
                slot < motion.size() ? motion[slot].distanceTravelledElmos : Fx{},
            .speedPerTick = slot < motion.size() ? motion[slot].speedPerTick : Fx{},
            .health = slot < health.size() ? health[slot].current : Mag{},
            .maxHealth = slot < health.size() ? health[slot].maximum : Mag{},
        });
    }
}

Snapshot snapshot(const UnitStore& store, TickIndex tick) {
    Snapshot taken;
    snapshotInto(store, tick, taken);
    return taken;
}

} // namespace rm::sim
