#include "core/sim/IdPool.hpp"

namespace rm::sim {

UnitId IdPool::acquire() {
    ++live_;

    if (!free_.empty()) {
        const UnitIndex slot = free_.back();
        free_.pop_back();
        // The generation was already bumped by `release`, so the slot's current value is
        // the new handle's — bumping again here would leave a gap and make the number less
        // readable in a log for no gain.
        return UnitId{slot, generations_[slot]};
    }

    const auto slot = static_cast<UnitIndex>(generations_.size());
    // Generations start at 1 so that a default-constructed UnitId — index 0, generation 0 —
    // is never alive. Costs one number and removes the need for a sentinel index.
    generations_.push_back(1);
    return UnitId{slot, 1};
}

void IdPool::release(UnitId id) {
    if (!alive(id)) {
        // Already stale. Two systems tidying up the same dead unit is ordinary; see the
        // header for why this is not an error.
        return;
    }

    // Bump first, so every copy of the handle held anywhere is stale from here on. Wrapping
    // past the maximum skips zero, because zero is the "never used" marker and a slot that
    // wrapped onto it would start reporting a default-constructed id as alive.
    Generation& generation = generations_[id.index];
    ++generation;
    if (generation == 0) {
        generation = 1;
    }

    free_.push_back(id.index);
    --live_;
}

} // namespace rm::sim
