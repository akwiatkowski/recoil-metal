#pragma once

#include "core/Types.hpp"

#include <cstddef>
#include <vector>

namespace rm::sim {

// A handle to a unit that survives the unit's death.
//
// WHY THIS EXISTS. Unit identity in this engine is currently a DRAW-CALL ADDRESS:
// `UnitRef{batch, instance}` names which instanced draw a unit is in and which slot of that
// draw's instance buffer it occupies (core/sim/Combat.hpp). Everything bad follows from
// that one fact — a unit cannot change model, a unit list cannot be walked without knowing
// the render batching, and a death is a hole in a GPU-facing array. This is the replacement
// (PLAN2.md §6.1).
//
// The generation is what makes it a handle rather than an index. A slot gets reused; the
// generation does not, so an id held across its unit's death names a generation that has
// moved on and fails to resolve. Without it, a weapon that kept aiming at a dead target
// would silently start aiming at whoever inherited the slot — which is not a crash, and is
// therefore the worst kind of bug: a match that plays differently for no visible reason.
//
// HOW THIS DIFFERS FROM RECOIL. `SimObjectIDPool` (Sim/Misc/SimObjectIDPool.h) keeps three
// `unordered_map`s and recycles ids on a delay, and its constructor carries a determinism
// warning: internal table sizes must be constant at runtime or a fresh client and a
// reloaded one desync, because both must execute `Expand` and `Expand` touches the RNG.
//
// A generation counter removes the problem the delay was managing: a stale handle is
// detectably stale forever rather than for a while. And there is no hash container here at
// all, so there is no iteration order to be deterministic ABOUT — which matters more to us
// than to them, because the replay hash is the project's success criterion (PLAN2.md §1.3).
//
// Eight bytes, trivially copyable, no pointer. Pass by value.
struct UnitId {
    UnitIndex index = 0;

    /// Zero is never live: generations start at 1 (see `IdPool::acquire`). So a
    /// default-constructed `UnitId` is invalid without needing a sentinel index, and a
    /// struct that forgot to initialise one does not accidentally name unit zero.
    Generation generation = 0;

    [[nodiscard]] friend bool operator==(const UnitId&, const UnitId&) noexcept = default;
};

/// Hands out stable handles and takes them back.
///
/// Deterministic by construction: the free list is LIFO over a plain vector, so the same
/// sequence of acquires and releases yields the same ids on every run and every platform.
/// That is not a nicety — it is what lets the replay hash mean anything.
class IdPool {
public:
    /// A handle to a slot nothing else holds.
    ///
    /// Reuses the most recently released slot when there is one, so a long match does not
    /// grow the pool for every unit it has ever had. The returned id differs from every id
    /// ever handed out for that slot, because releasing it bumped the generation.
    [[nodiscard]] UnitId acquire();

    /// Gives a slot back. The id passed in, and every copy of it anywhere, is stale from
    /// this moment.
    ///
    /// Releasing an id that is already stale does nothing — deliberately. Two systems each
    /// holding a handle to the same dead unit and each tidying up is the ordinary case, not
    /// a bug, and making the second one an error would push the bookkeeping outward into
    /// every caller.
    void release(UnitId id);

    /// Whether this exact handle still names a live slot.
    [[nodiscard]] bool alive(UnitId id) const noexcept {
        return id.index < generations_.size() && generations_[id.index] == id.generation
               && id.generation != 0;
    }

    /// How many slots are currently held.
    [[nodiscard]] std::size_t liveCount() const noexcept { return live_; }

    /// How many slots exist, live or free. The high-water mark of concurrent units, which
    /// is what a store sized from this pool needs.
    [[nodiscard]] std::size_t capacity() const noexcept { return generations_.size(); }

private:
    /// The current generation of each slot, indexed by slot. Zero means never used.
    std::vector<Generation> generations_;

    /// Slots available for reuse, most recently released last.
    std::vector<UnitIndex> free_;

    std::size_t live_ = 0;
};

} // namespace rm::sim
