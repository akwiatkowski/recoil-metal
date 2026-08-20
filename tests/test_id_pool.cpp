#include "core/sim/IdPool.hpp"

#include <catch2/catch_test_macros.hpp>

#include <set>
#include <vector>

using rm::sim::IdPool;
using rm::sim::UnitId;

TEST_CASE("a default-constructed handle is never alive") {
    // Generations start at 1 precisely so this holds. Without it, a struct that forgot to
    // initialise a UnitId member would name unit zero, and name it convincingly.
    const IdPool pool;
    REQUIRE_FALSE(pool.alive(UnitId{}));

    IdPool used;
    (void)used.acquire();
    REQUIRE_FALSE(used.alive(UnitId{}));
}

TEST_CASE("a fresh handle is alive and the pool counts it") {
    IdPool pool;
    const UnitId a = pool.acquire();
    REQUIRE(pool.alive(a));
    REQUIRE(pool.liveCount() == 1);
    REQUIRE(pool.capacity() == 1);
}

TEST_CASE("releasing a handle makes it stale") {
    IdPool pool;
    const UnitId a = pool.acquire();
    pool.release(a);
    REQUIRE_FALSE(pool.alive(a));
    REQUIRE(pool.liveCount() == 0);
}

TEST_CASE("a dead unit's handle never resolves to its successor") {
    // The whole reason the generation exists. Without it the reused slot would answer to
    // the old handle, and a weapon still aiming at a dead target would silently retarget
    // whoever inherited the slot — a match that plays differently for no visible reason.
    IdPool pool;
    const UnitId dead = pool.acquire();
    pool.release(dead);

    const UnitId successor = pool.acquire();

    REQUIRE(successor.index == dead.index);       // the slot IS reused
    REQUIRE(successor.generation != dead.generation);
    REQUIRE(pool.alive(successor));
    REQUIRE_FALSE(pool.alive(dead));              // ...and the old handle stays dead
    REQUIRE_FALSE(dead == successor);
}

TEST_CASE("an order held across a death fails cleanly rather than hitting a stranger") {
    // The scenario stated in PLAN2 §7 P1.1, end to end: something remembers a target, the
    // target dies, the slot is refilled, and the remembering thing must not act on it.
    IdPool pool;
    const UnitId target = pool.acquire();
    const UnitId orderTarget = target;  // a weapon, an order queue, a script — anything

    pool.release(target);
    const UnitId newcomer = pool.acquire();
    REQUIRE(pool.alive(newcomer));

    // The holder asks, and gets a straight answer.
    REQUIRE_FALSE(pool.alive(orderTarget));
}

TEST_CASE("slots are reused rather than grown, so a long match does not leak them") {
    IdPool pool;
    for (int i = 0; i < 1000; ++i) {
        const UnitId id = pool.acquire();
        pool.release(id);
    }
    REQUIRE(pool.capacity() == 1);
    REQUIRE(pool.liveCount() == 0);
}

TEST_CASE("every live handle is distinct, across churn") {
    IdPool pool;
    std::vector<UnitId> live;
    std::set<std::pair<rm::UnitIndex, rm::Generation>> seen;

    // Grow, then kill every third, then refill — the shape of an actual battle.
    for (int i = 0; i < 300; ++i) {
        live.push_back(pool.acquire());
    }
    for (std::size_t i = 0; i < live.size(); i += 3) {
        pool.release(live[i]);
    }
    for (int i = 0; i < 100; ++i) {
        live.push_back(pool.acquire());
    }

    std::size_t aliveSeen = 0;
    for (const UnitId id : live) {
        if (!pool.alive(id)) {
            continue;
        }
        ++aliveSeen;
        // No two live handles may be equal.
        REQUIRE(seen.insert({id.index, id.generation}).second);
    }
    REQUIRE(aliveSeen == pool.liveCount());
}

TEST_CASE("releasing a stale handle twice is a no-op, not a corruption") {
    // Two systems each tidying up the same dead unit is ordinary. If the second release
    // decremented the count or pushed the slot onto the free list again, the pool would
    // hand the same slot to two live units.
    IdPool pool;
    const UnitId a = pool.acquire();
    pool.release(a);
    pool.release(a);
    pool.release(a);

    REQUIRE(pool.liveCount() == 0);

    const UnitId b = pool.acquire();
    const UnitId c = pool.acquire();
    REQUIRE_FALSE(b == c);
    REQUIRE(b.index != c.index);
    REQUIRE(pool.liveCount() == 2);
}

TEST_CASE("releasing a handle from another pool does nothing") {
    IdPool a;
    IdPool b;
    const UnitId fromA = a.acquire();

    b.release(fromA);  // b has never heard of it
    REQUIRE(b.liveCount() == 0);
    REQUIRE(a.alive(fromA));
}

TEST_CASE("a handle with an out-of-range index is not alive") {
    IdPool pool;
    (void)pool.acquire();
    REQUIRE_FALSE(pool.alive(UnitId{9999, 1}));
}

TEST_CASE("the same sequence of calls yields the same ids, every run") {
    // Determinism, which for this type is the whole point: the replay hash is the project's
    // success criterion, and it is computed over state keyed by these ids. A pool that
    // handed out slots in a different order on a different run would make two identical
    // matches hash differently.
    auto run = [] {
        IdPool pool;
        std::vector<UnitId> out;
        std::vector<UnitId> held;
        for (int i = 0; i < 50; ++i) {
            held.push_back(pool.acquire());
        }
        for (std::size_t i = 0; i < held.size(); i += 2) {
            pool.release(held[i]);
        }
        for (int i = 0; i < 40; ++i) {
            out.push_back(pool.acquire());
        }
        return out;
    };

    const std::vector<UnitId> first = run();
    const std::vector<UnitId> second = run();
    REQUIRE(first == second);
}

TEST_CASE("reuse is most-recently-released-first, which is what makes it predictable") {
    // Stated as a test because the store above will depend on it: LIFO over a vector is
    // deterministic and cache-friendly. If this ever becomes FIFO or ordered, the golden
    // replay logs are invalidated and that has to be a deliberate, re-baselined change.
    IdPool pool;
    const UnitId a = pool.acquire();
    const UnitId b = pool.acquire();
    const UnitId c = pool.acquire();

    pool.release(a);
    pool.release(b);
    pool.release(c);

    REQUIRE(pool.acquire().index == c.index);
    REQUIRE(pool.acquire().index == b.index);
    REQUIRE(pool.acquire().index == a.index);
}
