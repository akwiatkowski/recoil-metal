#include "core/sim/UnitStore.hpp"
#include "core/sim/StateHash.hpp"

#include <catch2/catch_test_macros.hpp>

#include <vector>

#include "support/FxMatchers.hpp"

using rm::sim::UnitId;
using rm::sim::UnitStore;

namespace {

[[nodiscard]] UnitStore::Spawn tankAt(float x, float z, int army, rm::UnitTypeIndex type = 1) {
    UnitStore::Spawn s;
    s.type = type;
    s.transform.x = rm::test::fx(x);
    s.transform.z = rm::test::fx(z);
    s.motion.armyIndex = army;
    s.motion.radiusElmos = rm::sim::Fx::fromInt(16);
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
    REQUIRE(store.transforms()[id.index].x == rm::test::fx(100.0f));
    REQUIRE(store.transforms()[id.index].z == rm::test::fx(200.0f));
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
    REQUIRE(store.transforms().size() == store.slotCount());
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
    REQUIRE(store.transforms()[2].x == rm::test::fx(30.0f));
}

TEST_CASE("a dead unit's last values are left where they were") {
    // Deliberate: `retireDead` in the tick is what zeroes a corpse's collision radius, and
    // the wreck it leaves is sized from the radius it had while alive. A store that cleared
    // the slot on death would take that away before the tick could read it.
    UnitStore store;
    const UnitId a = store.spawn(tankAt(42.0f, 24.0f, 1));
    store.kill(a);

    REQUIRE(store.transforms()[a.index].x == rm::test::fx(42.0f));
    REQUIRE(store.motion()[a.index].radiusElmos == rm::sim::Fx::fromInt(16));
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
    rm::sim::Fx lastX = rm::sim::Fx::fromInt(-1);
    for (rm::UnitIndex slot = 0; slot < store.slotCount(); ++slot) {
        if (!store.slotAlive(slot)) {
            continue;
        }
        ++visited;
        const rm::sim::Fx x = store.transforms()[slot].x;
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
    REQUIRE(store.transforms()[second.index].x == rm::test::fx(999.0f));
    REQUIRE(store.motion()[second.index].armyIndex == 1);
}

TEST_CASE("a reused slot is fully overwritten, never partly inherited") {
    // The bug this guards against: a slot keeping a field from its previous occupant — a
    // health bar, a route, a reload timer — which would make a fresh unit behave like a
    // ghost of the one before it.
    UnitStore store;
    UnitStore::Spawn wounded = tankAt(5.0f, 5.0f, 0, 3);
    wounded.health = rm::sim::Health{.current = rm::test::mag(1.0f), .maximum = rm::test::mag(500.0f)};
    wounded.motion.path.push_back({rm::test::fx(70.0f), rm::test::fx(80.0f)});
    wounded.motion.moving = true;
    wounded.motion.distanceTravelledElmos = rm::test::fx(1234.0f);

    const UnitId old = store.spawn(wounded);
    store.kill(old);

    const UnitId fresh = store.spawn(tankAt(6.0f, 6.0f, 2, 9));

    REQUIRE(fresh.index == old.index);
    REQUIRE(store.health()[fresh.index].current == rm::test::mag(500.0f));
    REQUIRE(store.motion()[fresh.index].path.empty());
    REQUIRE_FALSE(store.motion()[fresh.index].moving);
    REQUIRE(store.motion()[fresh.index].distanceTravelledElmos == rm::sim::Fx{});
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

TEST_CASE("a unit store snapshot restores live units, tombstones, and allocator reuse") {
    UnitStore original;
    UnitStore::Spawn moving = tankAt(12.0f, 34.0f, 2, 7);
    moving.motion.moving = true;
    moving.motion.path.push_back({rm::test::fx(56.0f), rm::test::fx(78.0f)});
    moving.health = rm::sim::Health{.current = rm::test::mag(123.0f),
                                    .maximum = rm::test::mag(500.0f)};

    const UnitId live = original.spawn(moving);
    const UnitId firstOccupant = original.spawn(tankAt(90.0f, 10.0f, 3, 9));
    original.kill(firstOccupant);
    const UnitId reusedLive = original.spawn(tankAt(91.0f, 11.0f, 3, 10));

    UnitStore::Spawn olderTombstone = tankAt(20.0f, 30.0f, 4, 11);
    olderTombstone.health = rm::sim::Health{.current = rm::test::mag(321.0f),
                                             .maximum = rm::test::mag(600.0f)};
    olderTombstone.motion.moving = true;
    const UnitId olderDead = original.spawn(olderTombstone);

    UnitStore::Spawn newerTombstone = tankAt(40.0f, 50.0f, 5, 12);
    newerTombstone.health = rm::sim::Health{.current = rm::test::mag(222.0f),
                                             .maximum = rm::test::mag(700.0f)};
    newerTombstone.motion.path.push_back({rm::test::fx(60.0f), rm::test::fx(70.0f)});
    const UnitId newerDead = original.spawn(newerTombstone);
    original.kill(olderDead);
    original.kill(newerDead);
    REQUIRE(original.attach(live, reusedLive));

    const UnitStore::Snapshot saved = original.snapshot();
    UnitStore restored{saved};

    REQUIRE(restored.alive(live));
    REQUIRE(restored.alive(reusedLive));
    REQUIRE(reusedLive.generation > 1);
    REQUIRE_FALSE(restored.alive(olderDead));
    REQUIRE_FALSE(restored.alive(newerDead));
    REQUIRE(restored.typeAt(live.index) == 7);
    REQUIRE(restored.transforms()[live.index].x == rm::test::fx(12.0f));
    REQUIRE(restored.transforms()[live.index].z == rm::test::fx(34.0f));
    REQUIRE(restored.health()[live.index].current == rm::test::mag(123.0f));
    REQUIRE(restored.health()[live.index].maximum == rm::test::mag(500.0f));
    REQUIRE(restored.motion()[live.index].moving);
    REQUIRE(restored.motion()[live.index].path.size() == 1);
    REQUIRE(restored.motion()[live.index].path[0]
            == std::array<rm::sim::Fx, 2>{rm::test::fx(56.0f), rm::test::fx(78.0f)});
    REQUIRE(restored.parentOf(reusedLive) == live);
    REQUIRE(restored.childrenOf(live) == std::vector<UnitId>{reusedLive});

    REQUIRE(restored.typeAt(olderDead.index) == 11);
    REQUIRE(restored.transforms()[olderDead.index].x == rm::test::fx(20.0f));
    REQUIRE(restored.health()[olderDead.index].current == rm::test::mag(321.0f));
    REQUIRE(restored.motion()[olderDead.index].moving);
    REQUIRE(restored.typeAt(newerDead.index) == 12);
    REQUIRE(restored.transforms()[newerDead.index].z == rm::test::fx(50.0f));
    REQUIRE(restored.health()[newerDead.index].maximum == rm::test::mag(700.0f));
    REQUIRE(restored.motion()[newerDead.index].path
            == std::vector<std::array<rm::sim::Fx, 2>>{{rm::test::fx(60.0f), rm::test::fx(70.0f)}});

    const auto spawnThree = [](UnitStore& store) {
        return std::array{store.spawn(tankAt(1.0f, 2.0f, 4)),
                          store.spawn(tankAt(3.0f, 4.0f, 4)),
                          store.spawn(tankAt(5.0f, 6.0f, 4))};
    };
    const auto originalNext = spawnThree(original);
    const auto restoredNext = spawnThree(restored);

    REQUIRE(originalNext == restoredNext);
    REQUIRE(originalNext[0].index == newerDead.index);
    REQUIRE(originalNext[1].index == olderDead.index);
}

TEST_CASE("attachments keep one parent per child and reject duplicate or cyclic links") {
    UnitStore store;
    const UnitId parent = store.spawn(tankAt(1.0f, 0.0f, 0));
    const UnitId child = store.spawn(tankAt(2.0f, 0.0f, 0));
    const UnitId otherParent = store.spawn(tankAt(3.0f, 0.0f, 0));

    REQUIRE(store.attach(parent, child));
    REQUIRE(store.parentOf(child) == parent);
    REQUIRE(store.childrenOf(parent) == std::vector<UnitId>{child});
    REQUIRE_FALSE(store.attach(otherParent, child));
    REQUIRE_FALSE(store.attach(child, parent));
    REQUIRE(store.attach(child, otherParent));
    REQUIRE_FALSE(store.attach(otherParent, parent));
    REQUIRE(store.detach(otherParent));
    REQUIRE_FALSE(store.attach(parent, UnitId{}));

    REQUIRE(store.detach(child));
    REQUIRE_FALSE(store.parentOf(child).has_value());
    REQUIRE(store.childrenOf(parent).empty());

    REQUIRE(store.attach(parent, child));
    store.kill(parent);
    REQUIRE_FALSE(store.parentOf(child).has_value());
    CHECK(store.attachmentOffsetOf(child) == std::array<rm::sim::Fx, 2>{});
    REQUIRE(store.childrenOf(parent).empty());
    REQUIRE_FALSE(store.attach(parent, child));

    const UnitId newParent = store.spawn(tankAt(4.0f, 0.0f, 0));
    REQUIRE(store.attach(newParent, child));
    CHECK(store.attachmentOffsetOf(child)
          == std::array<rm::sim::Fx, 2>{rm::test::fx(-2.0f), rm::test::fx(0.0f)});
    store.kill(child);
    REQUIRE(store.childrenOf(newParent).empty());
}

TEST_CASE("attached children follow captured offsets until detached") {
    UnitStore store;
    const UnitId parent = store.spawn(tankAt(10.0f, 20.0f, 0));
    const UnitId child = store.spawn(tankAt(13.0f, 25.0f, 0));
    const UnitId grandchild = store.spawn(tankAt(15.0f, 28.0f, 0));
    REQUIRE(store.attach(parent, child));
    REQUIRE(store.attach(child, grandchild));

    store.transforms()[parent.index].x = rm::test::fx(30.0f);
    store.transforms()[parent.index].z = rm::test::fx(40.0f);
    store.propagateAttachments();
    CHECK(store.transforms()[child.index].x == rm::test::fx(33.0f));
    CHECK(store.transforms()[child.index].z == rm::test::fx(45.0f));
    CHECK(store.transforms()[grandchild.index].x == rm::test::fx(35.0f));
    CHECK(store.transforms()[grandchild.index].z == rm::test::fx(48.0f));

    REQUIRE(store.detach(child));
    store.transforms()[parent.index].x = rm::test::fx(50.0f);
    store.transforms()[parent.index].z = rm::test::fx(60.0f);
    store.propagateAttachments();
    CHECK(store.transforms()[child.index].x == rm::test::fx(33.0f));
    CHECK(store.transforms()[child.index].z == rm::test::fx(45.0f));
}

TEST_CASE("attachments change the match hash") {
    UnitStore detached;
    UnitStore attached;
    const UnitId detachedParent = detached.spawn(tankAt(1.0f, 0.0f, 0));
    const UnitId detachedChild = detached.spawn(tankAt(2.0f, 0.0f, 0));
    const UnitId attachedParent = attached.spawn(tankAt(1.0f, 0.0f, 0));
    const UnitId attachedChild = attached.spawn(tankAt(2.0f, 0.0f, 0));
    (void)detachedParent;
    (void)detachedChild;

    REQUIRE(attached.attach(attachedParent, attachedChild));
    std::vector<rm::sim::Army> armies(1);
    std::vector<rm::sim::Economy> economies(1);
    std::vector<int> commandersEver(1);
    const rm::sim::Match match{.armies = armies,
                                .economies = economies,
                                .commandersEver = commandersEver};
    REQUIRE(rm::sim::hashMatch(detached, match) != rm::sim::hashMatch(attached, match));
}

TEST_CASE("attachment offsets change the match hash") {
    UnitStore left;
    UnitStore right;
    const UnitId leftParent = left.spawn(tankAt(1.0f, 1.0f, 0));
    const UnitId leftChild = left.spawn(tankAt(2.0f, 2.0f, 0));
    const UnitId rightParent = right.spawn(tankAt(1.0f, 1.0f, 0));
    const UnitId rightChild = right.spawn(tankAt(3.0f, 3.0f, 0));
    REQUIRE(left.attach(leftParent, leftChild));
    REQUIRE(right.attach(rightParent, rightChild));
    right.transforms()[rightChild.index] = left.transforms()[leftChild.index];

    std::vector<rm::sim::Army> armies(1);
    std::vector<rm::sim::Economy> economies(1);
    std::vector<int> commandersEver(1);
    const rm::sim::Match match{.armies = armies,
                                .economies = economies,
                                .commandersEver = commandersEver};
    CHECK(rm::sim::hashMatch(left, match) != rm::sim::hashMatch(right, match));
}

TEST_CASE("different valid attachment topologies hash differently") {
    UnitStore chain;
    UnitStore siblings;
    const UnitId chainA = chain.spawn(tankAt(1.0f, 0.0f, 0));
    const UnitId chainB = chain.spawn(tankAt(2.0f, 0.0f, 0));
    const UnitId chainC = chain.spawn(tankAt(3.0f, 0.0f, 0));
    const UnitId siblingsA = siblings.spawn(tankAt(1.0f, 0.0f, 0));
    const UnitId siblingsB = siblings.spawn(tankAt(2.0f, 0.0f, 0));
    const UnitId siblingsC = siblings.spawn(tankAt(3.0f, 0.0f, 0));

    REQUIRE(chain.attach(chainA, chainB));
    REQUIRE(chain.attach(chainB, chainC));
    REQUIRE(siblings.attach(siblingsA, siblingsB));
    REQUIRE(siblings.attach(siblingsA, siblingsC));

    std::vector<rm::sim::Army> armies(1);
    std::vector<rm::sim::Economy> economies(1);
    std::vector<int> commandersEver(1);
    const rm::sim::Match match{.armies = armies,
                                .economies = economies,
                                .commandersEver = commandersEver};
    REQUIRE(rm::sim::hashMatch(chain, match) != rm::sim::hashMatch(siblings, match));
}
