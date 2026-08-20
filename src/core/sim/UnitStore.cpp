#include "core/sim/UnitStore.hpp"

namespace rm::sim {

UnitId UnitStore::spawn(const Spawn& request) {
    const UnitId id = ids_.acquire();
    const auto slot = static_cast<std::size_t>(id.index);

    if (slot >= instances_.size()) {
        // A slot the pool has never handed out before. The pool only ever grows by one, so
        // this is an append rather than a resize — asserted by construction rather than
        // checked, since `IdPool::acquire` is the only thing that produces these.
        instances_.emplace_back();
        motion_.emplace_back();
        health_.emplace_back();
        types_.emplace_back();
        generations_.emplace_back();
    }

    instances_[slot] = request.instance;
    motion_[slot] = request.motion;
    health_[slot] = request.health;
    types_[slot] = request.type;
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
