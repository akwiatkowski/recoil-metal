#include "core/sim/UnitCensus.hpp"

#include "core/sim/Movement.hpp"
#include "core/sim/UnitStore.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <map>
#include <vector>

using rm::sim::UnitCensus;
using rm::sim::UnitId;
using rm::sim::UnitStore;

namespace {

[[nodiscard]] UnitStore::Spawn unitOf(int army, rm::UnitTypeIndex type) {
    UnitStore::Spawn s;
    s.type = type;
    s.instance.scale = 1.0f;
    s.motion.armyIndex = army;
    s.health = rm::sim::Health{.current = 100.0f, .maximum = 100.0f};
    return s;
}

/// A small deterministic generator. Not `std::mt19937`, because a test that has to agree
/// with itself across runs and platforms should not depend on a library's choice of
/// engine — the same reason the sim does not.
class Lcg {
public:
    explicit Lcg(std::uint32_t seed) : state_{seed} {}
    std::uint32_t next() {
        state_ = state_ * 1664525u + 1013904223u;
        return state_;
    }
    std::uint32_t below(std::uint32_t bound) { return next() % bound; }

private:
    std::uint32_t state_;
};

} // namespace

TEST_CASE("a census counts what each team owns of each type") {
    UnitStore store;
    UnitCensus census;

    (void)store.spawn(unitOf(0, 5));
    (void)store.spawn(unitOf(0, 5));
    (void)store.spawn(unitOf(0, 9));
    (void)store.spawn(unitOf(1, 5));

    census.rebuild(store);

    REQUIRE(census.count(0, 5) == 2);
    REQUIRE(census.count(0, 9) == 1);
    REQUIRE(census.count(1, 5) == 1);
    REQUIRE(census.count(1, 9) == 0);
    REQUIRE(census.size() == 4);
}

TEST_CASE("a team's units are one contiguous run, whatever their types") {
    // The reason the key puts team in the high half: "everything army N owns" is the same
    // binary search over a wider bound, not a second structure to keep in step.
    UnitStore store;
    UnitCensus census;
    for (int i = 0; i < 5; ++i) {
        (void)store.spawn(unitOf(2, static_cast<rm::UnitTypeIndex>(i)));
    }
    (void)store.spawn(unitOf(3, 0));
    census.rebuild(store);

    REQUIRE(census.countOfTeam(2) == 5);
    REQUIRE(census.countOfTeam(3) == 1);
    REQUIRE(census.countOfTeam(4) == 0);
}

TEST_CASE("the dead leave the census on the next rebuild") {
    UnitStore store;
    UnitCensus census;
    const UnitId a = store.spawn(unitOf(0, 1));
    const UnitId b = store.spawn(unitOf(0, 1));
    census.rebuild(store);
    REQUIRE(census.count(0, 1) == 2);

    store.kill(a);
    census.rebuild(store);

    REQUIRE(census.count(0, 1) == 1);
    REQUIRE(census.of(0, 1).size() == 1);
    REQUIRE(census.of(0, 1)[0] == b);
}

TEST_CASE("a unit with no army is left out rather than bucketed under a made-up team") {
    // A `--units` crowd owns nothing and is owned by nobody. Giving it a team would put it
    // in somebody's unit census, and from there into the win condition.
    UnitStore store;
    UnitCensus census;
    (void)store.spawn(unitOf(rm::sim::kNoArmy, 4));
    (void)store.spawn(unitOf(rm::sim::kNoArmy, 4));
    const UnitId owned = store.spawn(unitOf(0, 4));

    census.rebuild(store);

    REQUIRE(census.size() == 1);
    REQUIRE(census.count(0, 4) == 1);
    REQUIRE(census.of(0, 4)[0] == owned);
}

TEST_CASE("every handle the census returns is alive and of the type asked for") {
    UnitStore store;
    UnitCensus census;
    for (int army = 0; army < 4; ++army) {
        for (int type = 1; type <= 3; ++type) {
            for (int n = 0; n < 3; ++n) {
                (void)store.spawn(unitOf(army, static_cast<rm::UnitTypeIndex>(type)));
            }
        }
    }
    census.rebuild(store);

    for (rm::TeamIndex army = 0; army < 4; ++army) {
        for (rm::UnitTypeIndex type = 1; type <= 3; ++type) {
            for (const UnitId id : census.of(army, type)) {
                REQUIRE(store.alive(id));
                REQUIRE(store.typeAt(id.index) == type);
                REQUIRE(store.motion()[id.index].armyIndex == static_cast<int>(army));
            }
        }
    }
}

TEST_CASE("10,000 random spawns and deaths: every count matches a brute-force scan") {
    // PLAN2 §7 P1.3's stated test. The census exists to replace a scan, so the scan is the
    // oracle: anything it says the census must agree with, or the O(1) answer is worthless.
    UnitStore store;
    UnitCensus census;
    Lcg rng{0x5EED1234u};

    constexpr int kTeams = 6;
    constexpr int kTypes = 12;
    std::vector<UnitId> everSpawned;

    for (int step = 0; step < 10000; ++step) {
        // Spawn most of the time, kill sometimes — which grows the store and then churns it,
        // the shape a real match has.
        if (everSpawned.empty() || rng.below(100) < 65) {
            const auto army = static_cast<int>(rng.below(kTeams));
            const auto type = static_cast<rm::UnitTypeIndex>(1 + rng.below(kTypes));
            everSpawned.push_back(store.spawn(unitOf(army, type)));
        } else {
            const UnitId victim = everSpawned[rng.below(
                static_cast<std::uint32_t>(everSpawned.size()))];
            store.kill(victim);  // may already be dead; that is a no-op
        }
    }

    census.rebuild(store);

    // Brute force: walk every slot, count live units per (army, type).
    std::map<std::pair<int, rm::UnitTypeIndex>, std::size_t> expected;
    std::size_t expectedTotal = 0;
    for (rm::UnitIndex slot = 0; slot < store.slotCount(); ++slot) {
        if (!store.slotAlive(slot)) {
            continue;
        }
        const int army = store.motion()[slot].armyIndex;
        if (army < 0) {
            continue;
        }
        ++expected[{army, store.typeAt(slot)}];
        ++expectedTotal;
    }

    REQUIRE(census.size() == expectedTotal);
    REQUIRE(expectedTotal > 100);  // the churn actually left a population to check

    for (int army = 0; army < kTeams; ++army) {
        std::size_t teamTotal = 0;
        for (rm::UnitTypeIndex type = 0; type <= kTypes; ++type) {
            const auto it = expected.find({army, type});
            const std::size_t want = it == expected.end() ? 0 : it->second;
            REQUIRE(census.count(static_cast<rm::TeamIndex>(army), type) == want);
            teamTotal += want;
        }
        REQUIRE(census.countOfTeam(static_cast<rm::TeamIndex>(army)) == teamTotal);
    }
}

TEST_CASE("a rebuild is reproducible, down to the order within a bucket") {
    // Determinism: the sort breaks ties on slot so the order is total. Without that,
    // two units of the same team and type could come back either way round, and anything
    // that iterated the census — an opponent picking a target, say — would stop being
    // replayable.
    auto build = [] {
        UnitStore store;
        UnitCensus census;
        for (int i = 0; i < 200; ++i) {
            (void)store.spawn(unitOf(i % 3, static_cast<rm::UnitTypeIndex>(1 + (i % 5))));
        }
        census.rebuild(store);
        std::vector<UnitId> flat;
        for (rm::TeamIndex team = 0; team < 3; ++team) {
            for (const UnitId id : census.ofTeam(team)) {
                flat.push_back(id);
            }
        }
        return flat;
    };

    REQUIRE(build() == build());
}

TEST_CASE("an empty store gives an empty census rather than an error") {
    UnitStore store;
    UnitCensus census;
    census.rebuild(store);

    REQUIRE(census.size() == 0);
    REQUIRE(census.count(0, 0) == 0);
    REQUIRE(census.of(3, 7).empty());
    REQUIRE(census.ofTeam(3).empty());
}

TEST_CASE("rebuilding twice in a row changes nothing") {
    UnitStore store;
    UnitCensus census;
    (void)store.spawn(unitOf(1, 2));
    (void)store.spawn(unitOf(1, 2));

    census.rebuild(store);
    const std::size_t once = census.count(1, 2);
    census.rebuild(store);

    REQUIRE(census.count(1, 2) == once);
    REQUIRE(census.size() == 2);
}
