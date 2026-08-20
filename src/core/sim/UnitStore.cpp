#include "core/sim/UnitStore.hpp"

namespace rm::sim {

UnitId UnitStore::spawn(const Spawn& request) {
    const UnitId id = ids_.acquire();
    const auto slot = static_cast<std::size_t>(id.index);

    if (slot >= transforms_.size()) {
        // A slot the pool has never handed out before. The pool only ever grows by one, so
        // this is an append rather than a resize — asserted by construction rather than
        // checked, since `IdPool::acquire` is the only thing that produces these.
        transforms_.emplace_back();
        motion_.emplace_back();
        health_.emplace_back();
        types_.emplace_back();
        orders_.emplace_back();
        generations_.emplace_back();
    }

    transforms_[slot] = request.transform;
    motion_[slot] = request.motion;
    health_[slot] = request.health;
    types_[slot] = request.type;
    // CLEARED HERE rather than in `kill`, which is the tombstone rule applied to orders: a
    // corpse keeps its arrays so the death blast can read them, and a slot is only wiped when
    // something new moves in. A queue left behind would have the newcomer inherit the dead
    // unit's route — the same class of bug `UnitId`'s generation exists to prevent.
    orders_[slot].clear();
    generations_[slot] = id.generation;
    return id;
}

void UnitStore::kill(UnitId id) {
    if (!ids_.alive(id)) {
        return;
    }
    ids_.release(id);
    // The arrays are deliberately left as they were — a dead unit is a tombstone, not a
    // hole (see the header). What zeroes a corpse's collision radius so it stops shoving
    // the living is `retireDead` in the tick, which is a rule about the match rather than
    // about storage, and it stays there.
    //
    // The generation mirror is NOT updated: the slot now holds a generation the pool has
    // moved past, so `idAt` returns a handle that fails `alive`, which is exactly what a
    // caller asking about an empty slot should get.
}

} // namespace rm::sim
