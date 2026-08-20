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

    /// Registers a definition and returns the type index units of it will carry.
    [[nodiscard]] UnitTypeIndex addType(unitdef::UnitDef def) {
        defs.push_back(std::move(def));
        return catalog.add(&defs.back());
    }

    /// A unit of a type, at a place, owned by an army, with a health pool.
    ///
    /// The radius is fixed at 4 elmos for everything: separation and blast falloff are
    /// tested on their own, and a per-unit radius here would let one test's crowding
    /// change another test's answer.
    sim::UnitId add(UnitTypeIndex type, float x, float z, int army, float hp) {
        const unitdef::UnitDef* def = catalog.def(type);

        UnitInstance instance{};
        instance.position = {x, 0.0f, z};
        instance.scale = 1.0f;

        sim::MoveState state;
        state.armyIndex = army;
        state.radiusElmos = 4.0f;

        // One reload counter per weapon, starting at zero so the first shot is available on
        // the first tick rather than a reload later.
        return store.spawn({
            .type = type,
            .instance = instance,
            .motion = state,
            .health = sim::Health{.current = hp,
                                  .maximum = hp,
                                  .reloadRemaining = std::vector<int>(
                                      def != nullptr ? def->weapons.size() : 0u, 0)},
        });
    }

    // Reached by HANDLE, which is how a test names a unit now — a slot index would be an
    // invitation to assume spawn order, and assuming spawn order is what the flat store
    // exists to stop.
    [[nodiscard]] UnitInstance& instance(sim::UnitId id) { return store.instances()[id.index]; }
    [[nodiscard]] sim::MoveState& motion(sim::UnitId id) { return store.motion()[id.index]; }
    [[nodiscard]] sim::Health& health(sim::UnitId id) { return store.health()[id.index]; }
};

} // namespace rm::test
