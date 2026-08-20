// Periodic work, spread across the ticks of its own period.
//
// THE GUARANTEE IS THE TEST: over any window of one period, every index is due exactly once —
// not at most once, not on average once. Everything else here is a consequence of that or a
// check that the period came from the clock rather than from a literal.
#include <catch2/catch_test_macros.hpp>

#include "core/sim/SlowUpdate.hpp"
#include "core/sim/UnitStore.hpp"

#include <map>
#include <vector>

using rm::sim::seconds;
using rm::sim::SlowUpdate;
using rm::sim::TickRate;

TEST_CASE("the period is derived from the tick rate, not written down") {
    // The same authored second is a different number of ticks at every rate — which is the
    // whole of §5.1 and the reason Recoil's hardcoded 16 frames is not copied. At 30 Hz its 16
    // frames are 0.533 s; a period written as 16 here would be 3.2 s at our slowest rate.
    CHECK(SlowUpdate{TickRate{5}, seconds(1.0f)}.period() == 5);
    CHECK(SlowUpdate{TickRate{10}, seconds(1.0f)}.period() == 10);
    CHECK(SlowUpdate{TickRate{20}, seconds(1.0f)}.period() == 20);
    CHECK(SlowUpdate{TickRate{50}, seconds(1.0f)}.period() == 50);

    // And a shorter period is a smaller rotation at the same rate.
    CHECK(SlowUpdate{TickRate{10}, seconds(0.5f)}.period() == 5);
}

TEST_CASE("a period shorter than a tick degrades to every tick") {
    // `TickRate::ticks` floors at one, so work paced faster than the clock is simply done every
    // tick. The alternative is a modulo by zero, and the honest answer is that a sim cannot
    // pace anything more finely than its own step.
    const SlowUpdate every{TickRate{10}, seconds(0.001f)};
    CHECK(every.period() == 1);
    for (rm::TickIndex tick = 0; tick < 5; ++tick) {
        CHECK(every.due(0, tick));
        CHECK(every.due(7, tick));
    }
}

TEST_CASE("every index is due exactly once per period") {
    // The guarantee, over a window of one period and with more indices than the period holds —
    // so several share each residue and the count per tick is more than one.
    const SlowUpdate pacing{TickRate{10}, seconds(1.0f)};
    const std::size_t total = 37;

    std::vector<int> visits(total, 0);
    for (rm::TickIndex tick = 0; tick < pacing.period(); ++tick) {
        for (std::size_t index = 0; index < total; ++index) {
            if (pacing.due(index, tick)) {
                ++visits[index];
            }
        }
    }
    for (std::size_t index = 0; index < total; ++index) {
        CHECK(visits[index] == 1);
    }
}

TEST_CASE("the work is spread, not merely delayed") {
    // A period WITHOUT a stagger would put every index on one tick in ten, which is a stutter
    // rather than a saving. This is the numeric form of §7 P3.6's `--bench` check: the busiest
    // tick does a period's fraction of the work, not all of it.
    const SlowUpdate pacing{TickRate{10}, seconds(1.0f)};
    const std::size_t total = 1000;

    std::size_t busiest = 0;
    std::size_t counted = 0;
    for (rm::TickIndex tick = 0; tick < pacing.period(); ++tick) {
        const std::size_t due = pacing.dueCount(total, tick);
        busiest = std::max(busiest, due);
        counted += due;
    }
    CHECK(counted == total);   // the whole set, once
    CHECK(busiest == 100);     // and never more than a tenth of it at a time
}

TEST_CASE("dueCount agrees with counting by hand") {
    // The closed form is an optimisation, and an optimisation that disagrees with the thing it
    // replaces is a bug rather than a saving. Checked over a size that does not divide evenly,
    // which is where the remainder term earns its place.
    const SlowUpdate pacing{TickRate{10}, seconds(0.7f)};  // 7 ticks
    for (std::size_t total : {0u, 1u, 6u, 7u, 8u, 23u}) {
        for (rm::TickIndex tick = 0; tick < 14; ++tick) {
            std::size_t byHand = 0;
            for (std::size_t index = 0; index < total; ++index) {
                if (pacing.due(index, tick)) {
                    ++byHand;
                }
            }
            CHECK(pacing.dueCount(total, tick) == byHand);
        }
    }
}

TEST_CASE("the rotation survives units coming and going") {
    // THE CASE THE PLAN NAMES — "even per-tick load". An index's residue is a property of the
    // index, so a unit's place in the rotation is fixed for as long as it exists, and units
    // spawning or dying cannot shift anybody else's turn.
    //
    // That is a claim about the STORE as much as about this class: it holds because the flat
    // store never moves a live unit's slot and reuses a dead one's in place. A store that
    // compacted on death would renumber the survivors and could skip or double-visit whoever
    // moved — which is the bug this test would catch if tombstones were ever traded away.
    const SlowUpdate pacing{TickRate{10}, seconds(1.0f)};
    rm::sim::UnitStore store;

    std::vector<rm::sim::UnitId> ids;
    for (int i = 0; i < 15; ++i) {
        ids.push_back(store.spawn(rm::sim::UnitStore::Spawn{
            .health = rm::sim::Health{.current = rm::sim::Mag::fromInt(100),
                                      .maximum = rm::sim::Mag::fromInt(100)}}));
    }

    // A unit in the middle dies and its slot is taken by a newcomer.
    store.kill(ids[4]);
    const rm::sim::UnitId replacement = store.spawn(rm::sim::UnitStore::Spawn{
        .health = rm::sim::Health{.current = rm::sim::Mag::fromInt(100),
                                  .maximum = rm::sim::Mag::fromInt(100)}});
    CHECK(replacement.index == ids[4].index);  // the slot, reused in place

    // Every LIVE slot is still visited exactly once across a period, and the recycled one
    // inherits the turn its slot always had rather than being appended to the end.
    std::map<rm::UnitIndex, int> visits;
    for (rm::TickIndex tick = 0; tick < pacing.period(); ++tick) {
        for (rm::UnitIndex slot = 0; slot < store.slotCount(); ++slot) {
            if (store.slotAlive(slot) && pacing.due(slot, tick)) {
                ++visits[slot];
            }
        }
    }
    CHECK(visits.size() == 15);
    for (const auto& [slot, count] : visits) {
        CHECK(count == 1);
    }
    CHECK(pacing.due(replacement.index, 4));
}

TEST_CASE("two subsystems pacing differently are two of these") {
    // No mode flag and no shared clock: a `SlowUpdate` holds nothing but its period, so a pass
    // that thinks once a second and a pass that thinks twice can each own one and neither has
    // to know about the other.
    const SlowUpdate slow{TickRate{10}, seconds(1.0f)};
    const SlowUpdate quick{TickRate{10}, seconds(0.5f)};

    CHECK(slow.period() == 10);
    CHECK(quick.period() == 5);
    CHECK(slow.due(3, 3));
    CHECK(slow.due(3, 13));
    CHECK_FALSE(slow.due(3, 8));
    CHECK(quick.due(3, 8));  // the same index, due twice as often
}
