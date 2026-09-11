#include "core/sim/Snapshot.hpp"

#include "core/sim/UnitStore.hpp"

#include <cstring>

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
        // Value-initialized, not designated: padding bytes are compared by
        // `identical`, so they must be zeroes rather than stack garbage, or
        // two snapshots of one store differ by allocator mood. memset, because
        // value-initialization leaves padding unspecified — only a full-object
        // clear makes the bytes canonical.
        UnitView view{};
        std::memset(&view, 0, sizeof(view));
        view.id = store.idAt(slot);
        view.type = store.typeAt(slot);
        view.armyIndex = slot < motion.size() ? motion[slot].armyIndex : kNoArmy;
        view.transform = transforms[slot];
        view.distanceTravelledElmos =
            slot < motion.size() ? motion[slot].distanceTravelledElmos : Fx{};
        view.speedPerTick = slot < motion.size() ? motion[slot].speedPerTick : Fx{};
        view.health = slot < health.size() ? health[slot].current : Mag{};
        view.maxHealth = slot < health.size() ? health[slot].maximum : Mag{};
        view.shieldActive = slot < health.size() && health[slot].shield.active();
        out.units.push_back(view);
    }
}

Snapshot snapshot(const UnitStore& store, TickIndex tick) {
    Snapshot taken;
    snapshotInto(store, tick, taken);
    return taken;
}

} // namespace rm::sim
