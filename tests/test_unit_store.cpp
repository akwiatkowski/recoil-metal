#include "core/sim/UnitStore.hpp"

#include <catch2/catch_test_macros.hpp>

#include <vector>

#include "support/FxMatchers.hpp"

using rm::sim::UnitId;
using rm::sim::UnitStore;

namespace {

[[nodiscard]] UnitStore::Spawn tankAt(float x, float z, int army, rm::UnitTypeIndex type = 1) {
    UnitStore::Spawn s;
    s.type = type;
    s.instance.position = {x, 0.0f, z};
    s.instance.scale = 1.0f;
    s.motion.armyIndex = army;
    s.motion.radiusElmos = 16.0f;
    s.health = rm::sim::Health{.current = rm::test::mag(500.0f), .maximum = rm::test::mag(500.0f)};
    return s;
}

} // namespace

TEST_CASE("a spawned unit is alive, findable, and carries what it was given") {
    UnitStore store;
    const UnitId id = store.spawn(tankAt(100.0f, 200.0f, 3, 7));

    REQUIRE(store.alive(id));
    REQUIRE(store.liveCount() == 1);
    REQUIRE(store.slotCount() == 1);
    REQUIRE(store.typeAt(id.index) == 7);
    REQUIRE(store.instances()[id.index].position[0] == 100.0f);
    REQUIRE(store.instances()[id.index].position[2] == 200.0f);
    REQUIRE(store.motion()[id.index].armyIndex == 3);
    REQUIRE(store.health()[id.index].current == rm::test::mag(500.0f));
}

TEST_CASE("every array is the same length and indexed by slot") {
    // The invariant every pass relies on. Three parallel deques that must stay index-locked
    // is what this type exists to replace; if they can drift, nothing is gained.
    UnitStore store;
    for (int i = 0; i < 50; ++i) {
        (void)store.spawn(tankAt(static_cast<float>(i), 0.0f, i % 4));
    }
    REQUIRE(store.instances().size() == store.slotCount());
    REQUIRE(store.motion().size() == store.slotCount());
    REQUIRE(store.health().size() == store.slotCount());
    REQUIRE(store.types().size() == store.slotCount());
}

TEST_CASE("a killed unit stops being alive but keeps its slot") {
    // The tombstone property, which the golden replay log depends on: nothing is erased and
    // no survivor moves, so iteration order never changes.
    UnitStore store;
    const UnitId a = store.spawn(tankAt(10.0f, 0.0f, 0));
    const UnitId b = store.spawn(tankAt(20.0f, 0.0f, 0));
    const UnitId c = store.spawn(tankAt(30.0f, 0.0f, 0));

    store.kill(b);

    REQUIRE_FALSE(store.alive(b));
    REQUIRE(store.alive(a));
    REQUIRE(store.alive(c));
    REQUIRE(store.liveCount() == 2);
    REQUIRE(store.slotCount() == 3);              // nothing was removed
    REQUIRE(a.index == 0);
    REQUIRE(c.index == 2);                        // ...and nothing moved
    REQUIRE(store.instances()[2].position[0] == 30.0f);
}

TEST_CASE("a dead unit's last values are left where they were") {
    // Deliberate: `retireDead` in the tick is what zeroes a corpse's collision radius, and
    // the wreck it leaves is sized from the radius it had while alive. A store that cleared
    // the slot on death would take that away before the tick could read it.
    UnitStore store;
    const UnitId a = store.spawn(tankAt(42.0f, 24.0f, 1));
    store.kill(a);

    REQUIRE(store.instances()[a.index].position[0] == 42.0f);
    REQUIRE(store.motion()[a.index].radiusElmos == 16.0f);
}

TEST_CASE("1,000 units, kill every third — iteration and lookup both hold") {
    // PLAN2 §7 P1.2's stated test.
    UnitStore store;
    std::vector<UnitId> ids;
    for (int i = 0; i < 1000; ++i) {
        ids.push_back(store.spawn(tankAt(static_cast<float>(i), 0.0f, i % 8)));
    }
    REQUIRE(store.liveCount() == 1000);

    for (std::size_t i = 0; i < ids.size(); i += 3) {
        store.kill(ids[i]);
    }

    const std::size_t expectedDead = (1000 + 2) / 3;  // 0, 3, 6, ... 999
    REQUIRE(store.liveCount() == 1000 - expectedDead);
    REQUIRE(store.slotCount() == 1000);

    // Lookup: every handle answers correctly about itself.
    for (std::size_t i = 0; i < ids.size(); ++i) {
        const bool shouldBeDead = (i % 3) == 0;
        REQUIRE(store.alive(ids[i]) == !shouldBeDead);
    }

    // Iteration: walking slots and skipping the dead visits exactly the survivors, in
    // order, with their values intact.
    std::size_t visited = 0;
    float lastX = -1.0f;
    for (rm::UnitIndex slot = 0; slot < store.slotCount(); ++slot) {
        if (!store.slotAlive(slot)) {
            continue;
        }
        ++visited;
        const float x = store.instances()[slot].position[0];
        REQUIRE(x > lastX);  // still in spawn order
        lastX = x;
    }
    REQUIRE(visited == store.liveCount());
}

TEST_CASE("a freed slot is reused, and the old handle does not follow it") {
    UnitStore store;
    const UnitId first = store.spawn(tankAt(1.0f, 0.0f, 0));
    store.kill(first);

    const UnitId second = store.spawn(tankAt(999.0f, 0.0f, 1));

    REQUIRE(second.index == first.index);          // the slot came back
    REQUIRE(store.slotCount() == 1);               // ...without growing the arrays
    REQUIRE(store.alive(second));
    REQUIRE_FALSE(store.alive(first));             // ...and the old handle stayed dead
    REQUIRE(store.instances()[second.index].position[0] == 999.0f);
    REQUIRE(store.motion()[second.index].armyIndex == 1);
}

TEST_CASE("a reused slot is fully overwritten, never partly inherited") {
    // The bug this guards against: a slot keeping a field from its previous occupant — a
    // health bar, a route, a reload timer — which would make a fresh unit behave like a
    // ghost of the one before it.
    UnitStore store;
    UnitStore::Spawn wounded = tankAt(5.0f, 5.0f, 0, 3);
    wounded.health = rm::sim::Health{.current = rm::test::mag(1.0f), .maximum = rm::test::mag(500.0f)};
    wounded.motion.path.push_back({70.0f, 80.0f});
    wounded.motion.moving = true;
    wounded.motion.distanceTravelledElmos = 1234.0f;

    const UnitId old = store.spawn(wounded);
    store.kill(old);

    const UnitId fresh = store.spawn(tankAt(6.0f, 6.0f, 2, 9));

    REQUIRE(fresh.index == old.index);
    REQUIRE(store.health()[fresh.index].current == rm::test::mag(500.0f));
    REQUIRE(store.motion()[fresh.index].path.empty());
    REQUIRE_FALSE(store.motion()[fresh.index].moving);
    REQUIRE(store.motion()[fresh.index].distanceTravelledElmos == 0.0f);
    REQUIRE(store.typeAt(fresh.index) == 9);
}

TEST_CASE("slotAlive and idAt agree with the handles") {
    UnitStore store;
    const UnitId a = store.spawn(tankAt(1.0f, 0.0f, 0));
    const UnitId b = store.spawn(tankAt(2.0f, 0.0f, 0));
    store.kill(a);

    REQUIRE_FALSE(store.slotAlive(a.index));
    REQUIRE(store.slotAlive(b.index));
    REQUIRE(store.idAt(b.index) == b);
    REQUIRE_FALSE(store.alive(store.idAt(a.index)));
}

TEST_CASE("killing twice, or killing a handle from another store, does nothing") {
    UnitStore store;
    UnitStore other;
    const UnitId a = store.spawn(tankAt(1.0f, 0.0f, 0));
    const UnitId elsewhere = other.spawn(tankAt(1.0f, 0.0f, 0));

    store.kill(a);
    store.kill(a);
    store.kill(elsewhere);

    REQUIRE(store.liveCount() == 0);
    REQUIRE(other.liveCount() == 1);

    // ...and the store is still coherent afterwards.
    const UnitId b = store.spawn(tankAt(2.0f, 0.0f, 0));
    const UnitId c = store.spawn(tankAt(3.0f, 0.0f, 0));
    REQUIRE_FALSE(b == c);
    REQUIRE(b.index != c.index);
    REQUIRE(store.liveCount() == 2);
}

TEST_CASE("out-of-range queries answer rather than read past the end") {
    UnitStore store;
    (void)store.spawn(tankAt(1.0f, 0.0f, 0));

    REQUIRE_FALSE(store.slotAlive(500));
    REQUIRE(store.typeAt(500) == 0);
    REQUIRE_FALSE(store.alive(store.idAt(500)));
}

TEST_CASE("the same sequence of spawns and kills gives the same store, every run") {
    // Determinism: the store is keyed by ids from a LIFO pool, so a replay that spawns and
    // kills in the same order must land on the same slots. If it did not, two identical
    // matches would hash differently.
    auto run = [] {
        UnitStore store;
        std::vector<UnitId> ids;
        for (int i = 0; i < 100; ++i) {
            ids.push_back(store.spawn(tankAt(static_cast<float>(i), 0.0f, i % 3)));
        }
        for (std::size_t i = 0; i < ids.size(); i += 4) {
            store.kill(ids[i]);
        }
        std::vector<UnitId> refills;
        for (int i = 0; i < 20; ++i) {
            refills.push_back(store.spawn(tankAt(1000.0f + static_cast<float>(i), 0.0f, 0)));
        }
        return refills;
    };

    REQUIRE(run() == run());
}
