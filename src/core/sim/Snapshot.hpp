#pragma once

#include "core/Types.hpp"
#include "core/sim/Army.hpp"
#include "core/sim/Fx.hpp"
#include "core/sim/IdPool.hpp"
#include "core/sim/Transform.hpp"

#include <cstddef>
#include <vector>

namespace rm::sim {

class UnitStore;

// What the renderer is allowed to see, as of one tick.
//
// WHY THIS EXISTS (PLAN2.md §4.1, §7 P7.1). The draw gather reads the live store: it walks
// `store.transforms()` and `store.motion()` and builds `UnitInstance` from them, every frame.
// So the renderer's input is the sim's working state, and three things follow from that which
// are all bad:
//
//   1. **There is nothing to interpolate between.** A frame draws wherever the sim happens to
//      be, so motion steps at the tick rate — 10 times a second by default, and `--tick-rate 5`
//      makes it twice as bad. Interpolation needs two states, and there is only ever one.
//   2. **The seam is a discipline rather than a type.** Nothing stops a renderer reading a
//      field it should not, or worse writing one. Recoil enforces this through its type system
//      (`System/Sync/SyncedPrimitive.h`, `SyncedFloat3.h`); a copy the renderer cannot write to
//      is the cheaper version of the same guarantee.
//   3. **The sim's type index is the renderer's batch index.** `resolveUnits` asserts they are
//      equal, which is why a buildable type cannot be registered with the catalog before
//      something of it spawns — and why build orders cannot go through `applyCommand` (§11).
//      A snapshot is where those two numbers stop having to be the same one.
//
// WHY A COPY RATHER THAN RECOIL'S `drawPos`. Recoil derives `drawPos = pos + speed * timeOffset`
// (`Sim/Objects/SolidObject.h:431`, annotated `unsynced`) and copies nothing, which is cheaper.
// §4.1 makes the case for the copy and D2 sharpened it: the tick rate is a knob now, and at
// 5 Hz the interpolation window is 200 ms — long enough that reading half-updated sim state
// would be visible on screen. A per-player snapshot is also where fog of war attaches later,
// and a derived `drawPos` has nowhere to put it.
//
// WHAT IT COSTS, measured rather than assumed: one `UnitView` is 56 bytes, so a 5,000-unit
// match copies 280 KB per tick. At 10 Hz that is 2.7 MB/s of memcpy-shaped work against a
// frame budget already spending 2.5 ms on the GPU. The capacity is kept between ticks
// (`snapshotInto`), so the steady state allocates nothing.

/// One unit, as the renderer may see it.
///
/// FIXED POINT STILL, and that is deliberate: this is the sim's account of where things are,
/// and converting to float belongs one layer further out where the interpolation happens. A
/// snapshot that had already converted would make "same sim, same bytes" a claim about float
/// determinism instead of about the copy.
struct UnitView {
    /// Which unit. The renderer's handle on it too — selection, the HUD and picking all name
    /// units by id, and a snapshot entry is the only thing they will have.
    UnitId id{};

    UnitTypeIndex type = 0;
    int armyIndex = kNoArmy;

    Transform transform{};

    /// Ground covered since it spawned, which is what paces a walk cycle. Presentation reads
    /// it; the sim only ever adds to it.
    Fx distanceTravelledElmos{};

    /// How fast it moves, per tick. Here because the walk cycle needs BOTH — distance covered
    /// divided by the stride the animation implies, and the stride is speed times the clip's
    /// duration. Without it the renderer would have to reach back into the store for a number
    /// the snapshot exists to stop it reaching for.
    Fx speedPerTick{};

    /// What it can still take, for the health bars and the strategic-icon tint.
    Mag health{};
    Mag maxHealth{};
};

/// Every live unit, as of one tick.
///
/// LIVE ONLY, and packed — not one entry per slot. The store is sparse by design (death is a
/// tombstone), and the renderer has no use for the holes: it draws what exists. That also makes
/// the snapshot the natural place for the sim's slot numbering to stop being the renderer's
/// business, which is the third reason above.
struct Snapshot {
    /// The tick this is the state as of. Carried so a caller interpolating two of them knows
    /// which is older without keeping a convention, and so a stale one is detectable.
    TickIndex tick = 0;

    /// In SLOT ORDER, which makes the sequence reproducible and makes matching two snapshots
    /// by id a merge rather than a lookup — a unit's slot never changes while it lives.
    std::vector<UnitView> units;

    [[nodiscard]] std::size_t size() const noexcept { return units.size(); }
    [[nodiscard]] bool empty() const noexcept { return units.empty(); }
};

/// Takes a snapshot.
///
/// PURE, which is §7 P7.1's stated test: the same store gives the same bytes, and the store is
/// not touched. `const UnitStore&` says the second half at compile time; the first half is what
/// the test asserts, because "pure" is easy to lose to a cache or a lazily-built index.
[[nodiscard]] Snapshot snapshot(const UnitStore& store, TickIndex tick);

/// The same, reusing `out`'s capacity.
///
/// What the frame loop calls. A snapshot per tick would otherwise be an allocation per tick,
/// and the whole point of the copy is that it is cheap.
void snapshotInto(const UnitStore& store, TickIndex tick, Snapshot& out);

} // namespace rm::sim
