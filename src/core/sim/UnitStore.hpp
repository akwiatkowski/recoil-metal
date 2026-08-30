#pragma once

#include "core/Types.hpp"
#include "core/sim/CommandQueue.hpp"
#include "core/sim/Health.hpp"
#include "core/sim/IdPool.hpp"
#include "core/sim/Movement.hpp"
#include "core/sim/SpatialGrid.hpp"
#include "core/sim/Transform.hpp"

#include <cstddef>
#include <span>
#include <vector>

namespace rm::sim {

// Every unit in a match, in one place.
//
// WHY THIS EXISTS. Unit state is currently three parallel deques of per-batch vectors in an
// anonymous namespace in `main.mm` — `instances`, `motion`, `health` — that must stay
// index-locked, addressed as `[batch][instance]` where a batch is one instanced draw call.
// So "which unit is this" is answered by "which draw call, and which slot of its instance
// buffer", and a unit list cannot be walked without knowing the render batching (PLAN2.md
// §1.1). This replaces that.
//
// STRUCTURE OF ARRAYS, NOT AN ARRAY OF STRUCTS, and this is a deliberate deviation from the
// `Unit` sketch in PLAN2.md §2. Three reasons, in order of weight:
//
//   1. The passes already take spans of exactly these arrays, so the migration in P1.4 is a
//      change of WHICH span rather than a rewrite of every pass. That is the difference
//      between a phase that can be verified step by step and one that cannot.
//   2. The arrays are what the renderer's upload wants: a gather over one contiguous array
//      per frame rather than a strided walk over an array of structs. (This reason used to
//      read "keeping `UnitInstance` contiguous means the upload stays a memcpy". P2.2 ended
//      that: the store holds `Transform` now, the instance is built at draw time, and the
//      gather it was trying to avoid is the gather P1.4 introduced anyway.)
//   3. It matches how the sim already treats death, which is the property the golden replay
//      log depends on — see below.
//
// An `Unit` view over these arrays is the right shape for callers that want to talk about
// one unit, and is cheap to add on top. It is not needed yet.
//
// DEATH IS A TOMBSTONE, NOT A REMOVAL. `retireDead` (Skirmish.cpp) zeroes a dead unit's
// radius and leaves it where it is; nothing is ever erased and no unit ever moves slot. That
// is not an accident to be tidied up — it is what makes iteration order stable, and the
// replay hash is order-sensitive on purpose (PLAN2.md §7 P1.0). A store that compacted on
// death would reorder the survivors, change every subsequent hash, and turn the one tool
// that makes this phase safe into noise. So: slots are permanent for the life of the match,
// `IdPool` decides which are live, and a pass skips the dead.
//
// The store owns no definitions. A unit carries a `UnitTypeIndex` and the catalog holds the
// rest, so the store never touches the VFS and a test can build one from two structs.
class UnitStore {
public:
    /// What a new unit needs. Grouped rather than passed as eight arguments because the
    /// order of eight floats is exactly the kind of thing a caller gets silently wrong.
    struct Spawn {
        UnitTypeIndex type = 0;
        Transform transform{};
        MoveState motion{};
        Health health{};
    };

    /// Adds a unit and returns its handle.
    ///
    /// Reuses a dead unit's slot when one is free, which is what keeps a long match from
    /// growing an array per unit it has ever had. The handle is distinct from every handle
    /// ever issued for that slot (`IdPool`).
    [[nodiscard]] UnitId spawn(const Spawn& request);

    /// Marks a unit dead. Its slot stays put and its arrays keep their last values — see
    /// the note on tombstones above. Killing an already-dead unit does nothing.
    void kill(UnitId id);

    [[nodiscard]] bool alive(UnitId id) const noexcept { return ids_.alive(id); }

    /// Whether the unit in this slot is live. The form a pass wants, since a pass walks
    /// slots rather than carrying handles.
    [[nodiscard]] bool slotAlive(UnitIndex slot) const noexcept {
        return slot < generations_.size() && ids_.alive(UnitId{slot, generations_[slot]});
    }

    /// The handle currently occupying a slot. Stale-safe: for an empty slot the generation
    /// will not match, so `alive()` on the result is false.
    [[nodiscard]] UnitId idAt(UnitIndex slot) const noexcept {
        return slot < generations_.size() ? UnitId{slot, generations_[slot]} : UnitId{};
    }

    // --- The arrays, for the passes ------------------------------------------
    //
    // All the same length, all indexed by slot, all sparse. Mutable where a pass writes and
    // const where it reads, which is the same split `CombatGroup` and `SkirmishGroup`
    // already make and for the same reason.

    [[nodiscard]] std::span<Transform> transforms() noexcept { return transforms_; }
    [[nodiscard]] std::span<const Transform> transforms() const noexcept {
        return transforms_;
    }
    [[nodiscard]] std::span<MoveState> motion() noexcept { return motion_; }
    [[nodiscard]] std::span<const MoveState> motion() const noexcept { return motion_; }
    [[nodiscard]] std::span<Health> health() noexcept { return health_; }
    [[nodiscard]] std::span<const Health> health() const noexcept { return health_; }
    [[nodiscard]] std::span<const UnitTypeIndex> types() const noexcept { return types_; }

    /// The orders each unit still has to carry out (PLAN2.md §6.4, §7 P4.1).
    ///
    /// ON THE UNIT, which is where the plan puts it and where Recoil puts it too — the
    /// alternative, one table keyed by handle, would need clearing on death and would make
    /// "walk every unit's queue" a hash lookup per slot in a pass that already has the slot.
    /// A slot's queue is cleared when the slot is reused, not when the unit dies: a corpse's
    /// arrays are deliberately left intact (see the note on tombstones above).
    [[nodiscard]] std::span<CommandQueue> orders() noexcept { return orders_; }
    [[nodiscard]] std::span<const CommandQueue> orders() const noexcept { return orders_; }

    /// Assigns the immutable creation clock carried by an accepted command. Match-global, as in
    /// retail: recycling a unit slot or clearing one queue must not restart the clock.
    [[nodiscard]] CommandSerial allocateCommandSerial() noexcept {
        return nextCommandSerial_++;
    }
    [[nodiscard]] CommandSerial nextCommandSerial() const noexcept { return nextCommandSerial_; }

    // --- The spatial index (PLAN2.md §6.5, §7 P5.2) ---------------------------
    //
    // HERE RATHER THAN THREADED THROUGH SIX SIGNATURES. `nearestTarget`, `nearestStruck`,
    // `damageArea`, `aimAtTargets` and `resolveCollisions` all ask "which units are near this
    // place", and all five already take the store. An index derived from the store's own
    // positions belongs with them — the alternative was a `SpatialGrid&` parameter on every
    // pass and on every one of their forty-odd test call sites, for no gain in clarity.
    //
    // IT IS NOT SELF-MAINTAINING, and that is the price. A pass that moves units invalidates
    // it, so `reindex` is called explicitly at the points in the tick where positions have
    // settled — `tickSkirmish` owns that, the same way it owns the pass order. A query against
    // a stale index answers about where units WERE, which is a wrong answer rather than a
    // crash, so the rebuild points are documented where they happen.

    /// Rebuilds the spatial index from the current positions.
    ///
    /// Idempotent and cheap: one pass over the slots and a sort. Called twice per tick — once
    /// before collisions and once after, because collisions move things.
    void reindex(Fx cellSize);

    /// The index. Const because a query is a read — the answer buffer inside is a cache.
    [[nodiscard]] const SpatialGrid& space() const noexcept { return space_; }

    /// Slots that exist, live or dead. The length of every array above.
    [[nodiscard]] std::size_t slotCount() const noexcept { return transforms_.size(); }

    /// Units currently alive.
    [[nodiscard]] std::size_t liveCount() const noexcept { return ids_.liveCount(); }

    /// The type of the unit in a slot, or 0 for a slot that has never held one. Bounds
    /// checked because a pass indexing past the end is a bug worth failing loudly on rather
    /// than reading whatever is next in memory.
    [[nodiscard]] UnitTypeIndex typeAt(UnitIndex slot) const noexcept {
        return slot < types_.size() ? types_[slot] : UnitTypeIndex{0};
    }

private:
    IdPool ids_;

    /// The generation currently in each slot, mirrored from the pool so that a slot can be
    /// turned back into a handle without asking the pool for its internals.
    std::vector<Generation> generations_;

    /// The sim's authority on where units are. `UnitInstance` — the GPU's layout — is built
    /// from these at draw time and never read back; see `core/sim/Transform.hpp`.
    std::vector<Transform> transforms_;
    std::vector<MoveState> motion_;
    std::vector<Health> health_;
    std::vector<UnitTypeIndex> types_;
    std::vector<CommandQueue> orders_;

    CommandSerial nextCommandSerial_ = 0;

    /// Not parallel to the arrays above: a sorted index INTO them, rebuilt by `reindex`.
    SpatialGrid space_;
};

} // namespace rm::sim
