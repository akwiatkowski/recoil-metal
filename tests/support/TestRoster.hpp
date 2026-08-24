#pragma once

// Every unit in a test match, plus the definitions they point at.
//
// WHY THIS IS SHARED. Three test files each grew their own copy of this — `Batch` in
// test_skirmish, `Squad` in test_combat, `Batch` again in test_match_invariants — and all
// three had to change in the same way when the sim stopped taking batches. Three copies of
// one fixture is three chances to migrate it differently and then disagree about which
// behaviour is correct.
//
// It is deliberately thin: a store, a catalog, and somewhere to keep the definitions. It
// makes no match, chooses no armies and runs no tick, because those differ per file and a
// fixture that decided them would be a second sim.

#include "core/Types.hpp"
#include "core/sim/Health.hpp"
#include "core/sim/UnitCatalog.hpp"
#include "core/sim/UnitStore.hpp"
#include "core/unit/UnitDef.hpp"

#include <algorithm>
#include <deque>
#include <utility>
#include <vector>

namespace rm::test {

/// A store, a catalog, and the definitions the catalog points at.
///
/// One store and one catalog, because that is what the sim takes: a test that wants a tank
/// and a target registers two TYPES rather than building two batches. The def moved from
/// "a property of the batch" to "a property of the unit" (`sim::UnitCatalog`), and that is
/// the change that let the batches go.
///
/// The defs live in a `deque` because the catalog holds POINTERS to them: a `vector` would
/// move its elements on growth and leave every registered type dangling.
struct Roster {
    sim::UnitStore store;
    sim::UnitCatalog catalog;
    std::deque<unitdef::UnitDef> defs;

    /// The clock this roster's derived rates were computed against. A member rather than a
    /// parameter so that a test cannot register a type at one rate and spawn at another,
    /// which would give a unit a speed its weapons disagreed with.
    sim::TickRate rate{};

    /// Registers a definition and returns the type index units of it will carry.
    [[nodiscard]] UnitTypeIndex addType(unitdef::UnitDef def) {
        defs.push_back(std::move(def));
        return catalog.add(&defs.back(), rate);
    }

    /// A unit of a type, at a place, owned by an army, with a health pool.
    ///
    /// The radius is fixed at 4 elmos for everything: separation and blast falloff are
    /// tested on their own, and a per-unit radius here would let one test's crowding
    /// change another test's answer.
    sim::UnitId add(UnitTypeIndex type, float x, float z, int army, float hp) {
        const unitdef::UnitDef* def = catalog.def(type);

        // Authored in decimals, stored in fixed point: a test says "at x = 400" and the
        // conversion happens here rather than at every call.
        const sim::Transform transform{.x = sim::fxFromFloat(x), .z = sim::fxFromFloat(z)};

        sim::MoveState state = sim::defaultMotion(rate);
        state.armyIndex = army;
        state.airborne = def != nullptr && def->motion == unitdef::MotionType::Air;
        state.surfaceWater = def != nullptr && def->motion == unitdef::MotionType::Water;
        state.radiusElmos = sim::Fx::fromInt(4);

        // One reload counter per weapon, starting at zero so the first shot is available on
        // the first tick rather than a reload later.
        const sim::UnitId id = store.spawn({
            .type = type,
            .transform = transform,
            .motion = state,
            .health = [&] {
                sim::Health health = sim::initialHealth(
                    sim::magFromFloat(hp), catalog.shield(type).maximum);
                health.reloadRemaining =
                    std::vector<int>(def != nullptr ? def->weapons.size() : 0u, 0);
                return health;
            }(),
        });
        // The new unit has to be in the spatial index before anything asks what is near it.
        // A test that spawns and then queries is the common shape, and making the fixture do
        // this is what keeps every one of those cases from carrying a reindex call.
        reindex();
        return id;
    }

    /// Rebuilds the store's spatial index, the way `tickSkirmish` does.
    ///
    /// CALLED BY `add`, so a test that only spawns units never has to think about it. A test
    /// that MOVES a unit by writing its transform does — the index is not self-maintaining
    /// (`UnitStore::reindex`), so a targeting query against a stale one answers about where the
    /// unit was. Several cases here do exactly that on purpose, and call this afterwards.
    ///
    /// The cell size mirrors the sim's own rule: a preferred 64 elmos, floored at the collision
    /// reach. Not shared with `Skirmish.cpp` because a test fixture reproducing the engine's
    /// tuning constant would hide a change to it rather than reveal one.
    void reindex() {
        sim::Fx largest{};
        for (const sim::MoveState& state : store.motion()) {
            largest = std::max(largest, state.radiusElmos);
        }
        store.reindex(std::max(sim::Fx::fromInt(64), largest * 2));
    }

    // Reached by HANDLE, which is how a test names a unit now — a slot index would be an
    // invitation to assume spawn order, and assuming spawn order is what the flat store
    // exists to stop.
    [[nodiscard]] sim::Transform& transform(sim::UnitId id) {
        return store.transforms()[id.index];
    }
    [[nodiscard]] sim::MoveState& motion(sim::UnitId id) { return store.motion()[id.index]; }
    [[nodiscard]] sim::Health& health(sim::UnitId id) { return store.health()[id.index]; }
};

} // namespace rm::test
