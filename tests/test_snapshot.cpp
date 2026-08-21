// What the renderer may see, as of one tick.
//
// §7 P7.1's claim is that a snapshot is PURE: the same store gives the same bytes, and the store
// is not touched. The first half needs a byte comparison rather than a field-by-field one; the
// second needs an instrument that can see the whole sim, which is the state hash. Interpolation
// between two of these is P7.2 and lives in `test_interpolate.cpp`.
#include <catch2/catch_test_macros.hpp>

#include "core/sim/Snapshot.hpp"
#include "core/sim/StateHash.hpp"

#include "support/FxMatchers.hpp"
#include "support/TestRoster.hpp"

#include <cstring>
#include <vector>

using rm::sim::Snapshot;
using rm::sim::UnitId;
using rm::sim::UnitView;

namespace {

/// Every field of a snapshot, byte for byte.
///
/// `std::memcmp` over the entries rather than a field-by-field compare, because the claim is
/// "the same BYTES": a field-by-field compare would pass while padding differed, and padding is
/// exactly what a lazily-built cache would show up in. `UnitView` is trivially copyable, which
/// is what makes this legal to ask.
[[nodiscard]] bool identical(const Snapshot& a, const Snapshot& b) {
    static_assert(std::is_trivially_copyable_v<UnitView>,
                  "a snapshot entry must be trivially copyable — the seam is a memcpy");
    if (a.tick != b.tick || a.units.size() != b.units.size()) {
        return false;
    }
    return a.units.empty()
           || std::memcmp(a.units.data(), b.units.data(),
                          a.units.size() * sizeof(UnitView)) == 0;
}

[[nodiscard]] rm::unitdef::UnitDef plainDef() {
    rm::unitdef::UnitDef def;
    def.name = "test_thing";
    return def;
}

} // namespace

// --- P7.1: the snapshot ------------------------------------------------------------------

TEST_CASE("a snapshot holds every live unit, in slot order, and no tombstones") {
    rm::test::Roster roster;
    const rm::UnitTypeIndex type = roster.addType(plainDef());
    const UnitId first = roster.add(type, 100.0f, 100.0f, 0, 50.0f);
    const UnitId second = roster.add(type, 200.0f, 300.0f, 1, 80.0f);
    const UnitId third = roster.add(type, 400.0f, 500.0f, 1, 80.0f);

    roster.store.kill(second);

    const Snapshot taken = rm::sim::snapshot(roster.store, 42);
    CHECK(taken.tick == 42);
    REQUIRE(taken.size() == 2);

    CHECK(taken.units[0].id == first);
    CHECK(taken.units[1].id == third);  // the dead one's slot is skipped, not left as a hole
    CHECK(taken.units[0].armyIndex == 0);
    CHECK(taken.units[1].armyIndex == 1);
    CHECK(rm::test::asFloat(taken.units[1].transform.x) == 400.0f);
    CHECK(rm::test::asFloat(taken.units[1].transform.z) == 500.0f);
    CHECK(rm::test::asFloat(taken.units[0].health) == 50.0f);
    CHECK(rm::test::asFloat(taken.units[0].maxHealth) == 50.0f);
}

TEST_CASE("the same store gives the same bytes") {
    // §7 P7.1's stated test, half one. Taken three times, including once into a buffer that
    // already held a bigger snapshot — because `snapshotInto` reuses capacity, and a stale tail
    // left behind by a shorter second call is exactly the bug this catches.
    rm::test::Roster roster;
    const rm::UnitTypeIndex type = roster.addType(plainDef());
    for (int i = 0; i < 12; ++i) {
        (void)roster.add(type, static_cast<float>(i * 37), static_cast<float>(i * 53), i % 3,
                         100.0f);
    }

    const Snapshot once = rm::sim::snapshot(roster.store, 7);
    const Snapshot twice = rm::sim::snapshot(roster.store, 7);
    CHECK(identical(once, twice));

    Snapshot reused = rm::sim::snapshot(roster.store, 7);
    roster.store.kill(roster.store.idAt(3));
    rm::sim::snapshotInto(roster.store, 7, reused);
    CHECK(reused.size() == 11);
    // And it is what a fresh one would be, not the old one with an entry blanked.
    CHECK(identical(reused, rm::sim::snapshot(roster.store, 7)));
}

TEST_CASE("taking a snapshot does not touch the sim") {
    // Half two, and the sharper half: `const UnitStore&` says it at compile time, but a cache
    // or a lazily-built index would satisfy the compiler and break the seam. The state hash is
    // the instrument that can tell — it reads every field the sim has.
    rm::test::Roster roster;
    const rm::UnitTypeIndex type = roster.addType(plainDef());
    for (int i = 0; i < 6; ++i) {
        (void)roster.add(type, static_cast<float>(i * 20), 0.0f, 0, 100.0f);
    }

    std::vector<rm::sim::Army> armies = rm::sim::freeForAll(1);
    std::vector<rm::sim::Economy> economies(1);
    const std::vector<int> commandersEver{0};
    rm::sim::Match match{
        .armies = armies, .economies = economies, .commandersEver = commandersEver};

    const rm::StateHash before = rm::sim::hashMatch(roster.store, match);
    for (int i = 0; i < 10; ++i) {
        (void)rm::sim::snapshot(roster.store, static_cast<rm::TickIndex>(i));
    }
    CHECK(rm::sim::hashMatch(roster.store, match) == before);
}
