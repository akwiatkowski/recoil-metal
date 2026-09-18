// The fork-join pool the sim's parallel passes run on (ADR-036 / D15).
//
// The contract under test is the one determinism leans on: `parallelFor`
// covers `[0, count)` exactly once, each worker writes only the slots it was
// handed, and the ANSWER cannot depend on how many workers there were — the
// pool may reorder work, never results.

#include "core/TaskPool.hpp"

#include "core/sim/Command.hpp"
#include "core/sim/Pathfinding.hpp"
#include "core/sim/Skirmish.hpp"
#include "core/sim/StateHash.hpp"
#include "core/sim/Terrain.hpp"

#include "support/FxMatchers.hpp"
#include "support/TestRoster.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <numeric>
#include <thread>
#include <vector>

namespace {

/// Runs `fn(first, last)` over [0, count) under the requested pool size and
/// restores the caller's setting afterwards — a pool size is process state,
/// and a test that leaves it changed leaks into every later case.
struct PoolSize {
    explicit PoolSize(std::size_t workers) { rm::setSimPoolSize(workers); }
    ~PoolSize() { rm::setSimPoolSize(0); }  // 0 asks for the machine default
};

} // namespace

TEST_CASE("parallelFor covers every index exactly once at any pool size") {
    for (const std::size_t workers : {1u, 2u, 8u}) {
        const PoolSize pool{workers};
        std::vector<int> seen(1000, 0);
        rm::parallelFor(seen.size(), [&seen](std::size_t first, std::size_t last) {
            for (std::size_t i = first; i < last; ++i) {
                ++seen[i];
            }
        });
        CHECK(std::ranges::all_of(seen, [](int hits) { return hits == 1; }));
    }
}

TEST_CASE("parallelFor computes the same result at every pool size") {
    // Per-slot outputs combined by the caller AFTER the join: the pattern the
    // sim passes use, so the test asserts the pattern's property.
    const auto run = [](std::size_t workers) {
        const PoolSize pool{workers};
        std::vector<std::uint64_t> out(997, 0);
        rm::parallelFor(out.size(), [&out](std::size_t first, std::size_t last) {
            for (std::size_t i = first; i < last; ++i) {
                out[i] = i * i + 7;
            }
        });
        return out;
    };
    const std::vector<std::uint64_t> serial = run(1);
    CHECK(run(4) == serial);
    CHECK(run(16) == serial);
}

TEST_CASE("parallelFor runs inline when the pool is a single worker") {
    const PoolSize pool{1};
    const std::thread::id caller = std::this_thread::get_id();
    std::thread::id worker{};
    rm::parallelFor(64, [&worker](std::size_t first, std::size_t last) {
        worker = std::this_thread::get_id();
        (void)first;
        (void)last;
    });
    CHECK(worker == caller);
}

TEST_CASE("parallelFor shares a non-trivial range across workers") {
    const PoolSize pool{4};
    std::atomic<std::size_t> distinct{0};
    std::atomic<bool> ran{false};
    rm::parallelFor(4096, [&distinct, &ran](std::size_t first, std::size_t last) {
        if (first != last) {
            ++distinct;
        }
        ran = true;
    });
    CHECK(ran);
    CHECK(distinct > 1);
}

TEST_CASE("parallelFor tolerates an empty range") {
    const PoolSize pool{8};
    bool called = false;
    rm::parallelFor(0, [&called](std::size_t, std::size_t) { called = true; });
    CHECK_FALSE(called);
}

TEST_CASE("a scripted match hashes identically at every pool size") {
    // P10.6's acceptance test, end to end rather than at the pool boundary:
    // the tick may reorder WORK inside a pass, never a RESULT. Both fork-joined
    // sites are exercised — the move orders put a live search on each army's
    // PathService lane, and the two firing lines spread automatic acquisition
    // across the fire pass's per-slot writes — so a hash that only survives at
    // one pool size is a real determinism break, not a vacuous pass.
    const auto runHash = [](std::size_t workers) {
        const PoolSize pool{workers};

        rm::HeightField field;
        field.squaresX = 128;
        field.squaresZ = 128;
        field.baseHeight = 0.0f;
        field.heightScale = 1.0f;
        field.raw.assign(field.sampleCount(), std::uint16_t{0});

        rm::sim::Terrain terrain{field, false, 0.0f, nullptr, {},
                                 rm::sim::PlacementMode::Free};
        rm::sim::PassabilityGrid grid = rm::sim::buildPassability(field, 0.0f);
        rm::test::Roster roster;
        std::vector<rm::sim::Army> armies = rm::sim::freeForAll(2);
        std::vector<rm::sim::Player> players = rm::sim::onePlayerPerArmy(2, 0);
        rm::sim::PathService paths;
        std::vector<rm::sim::Construction> building;
        std::vector<rm::sim::Projectile> shots;
        std::vector<rm::sim::Economy> economies(2);
        const std::vector<int> commandersEver(2, 0);

        rm::unitdef::Weapon weapon{.trackingRadius = rm::sim::Fx::fromInt(1)};
        weapon.label = "test gun";
        weapon.role = rm::unitdef::WeaponRole::DirectFire;
        weapon.targetPriorities = {{"LAND"}};
        weapon.damage = rm::test::mag(10.0f);
        weapon.maxRange = rm::test::fx(120.0f);
        weapon.rateOfFire = 1.0f;
        weapon.muzzleVelocityElmosPerSecond = 100.0f;
        rm::unitdef::UnitDef gunner;
        gunner.name = "test_gunner";
        gunner.categories = {"LAND"};
        gunner.weapons.push_back(weapon);
        rm::unitdef::UnitDef target;
        target.name = "test_target";
        target.categories = {"LAND"};

        const rm::UnitTypeIndex gunnerType = roster.addType(gunner);
        const rm::UnitTypeIndex targetType = roster.addType(target);

        // Movers feed PathService (one search queue per army — two lanes of
        // path work); the firing line feeds the acquisition fan-out, with
        // targets parked inside maxRange so the pass has real queries to make.
        std::vector<rm::sim::CommandIssue> issues;
        std::uint32_t serial = 0;
        const auto moveTo = [&](rm::sim::UnitId unit, rm::PlayerIndex player,
                                float x, float z) {
            issues.push_back(rm::sim::CommandIssue{
                .tick = 0, .source = static_cast<rm::CommandSource>(player),
                .id = rm::commandId(static_cast<rm::CommandSource>(player), serial++),
                .player = player, .kind = rm::sim::CommandKind::Move,
                .units = {unit}, .targetX = rm::test::fx(x), .targetZ = rm::test::fx(z)});
        };
        rm::sim::UnitId firstMover{};
        for (int i = 0; i < 16; ++i) {
            const float f = static_cast<float>(i);
            const auto unit = roster.add(gunnerType, 100.0f + f * 8.0f, 100.0f, 0, 500.0f);
            if (i == 0) {
                firstMover = unit;
            }
            moveTo(unit, 0, 400.0f, 100.0f + f * 8.0f);
        }
        for (int i = 0; i < 16; ++i) {
            const float f = static_cast<float>(i);
            const auto unit = roster.add(targetType, 500.0f + f * 8.0f, 500.0f, 1, 500.0f);
            moveTo(unit, 1, 200.0f, 500.0f + f * 8.0f);
        }
        for (int i = 0; i < 32; ++i) {
            const float f = static_cast<float>(i);
            (void)roster.add(gunnerType, 400.0f + f * 4.0f, 300.0f, 0, 500.0f);
        }
        for (int i = 0; i < 32; ++i) {
            const float f = static_cast<float>(i);
            (void)roster.add(targetType, 400.0f + f * 4.0f, 380.0f, 1, 500.0f);
        }

        for (const rm::sim::CommandIssue& issue : issues) {
            REQUIRE(rm::sim::applyCommand(
                issue, roster.store, roster.catalog, players, armies, terrain,
                [&](rm::sim::UnitId) { return &grid; }, roster.rate, &building, nullptr,
                nullptr, &paths));
        }
        std::size_t shotsFired = 0;
        for (rm::TickIndex tick = 0; tick < 120; ++tick) {
            const std::vector<const rm::sim::PassabilityGrid*> grids(roster.catalog.size(),
                                                                     &grid);
            rm::sim::Match match{.armies = armies,
                                 .economies = economies,
                                 .projectiles = &shots,
                                 .building = &building,
                                 .passability = grids,
                                 .pathService = &paths,
                                 .commandersEver = commandersEver};
            shotsFired += rm::sim::tickSkirmish(roster.store, roster.catalog, match, terrain)
                              .shotsFired;
        }
        // Not vacuous: the run only counts if both parallel sites actually
        // had work — shots fired means acquisition ran, and the mover reaching
        // its ordered x means a search completed and published. The count is
        // cumulative: an unturreted hull slews between shots, so the in-flight
        // set can legitimately be empty on the last tick.
        REQUIRE(shotsFired > 0);
        REQUIRE(roster.store.transforms()[firstMover.index].x > rm::test::fx(300.0f));

        rm::sim::Match match{.armies = armies,
                             .economies = economies,
                             .projectiles = &shots,
                             .building = &building,
                             .pathService = &paths,
                             .commandersEver = commandersEver};
        return rm::sim::hashMatch(roster.store, match);
    };

    const rm::StateHash serial = runHash(1);
    CHECK(runHash(2) == serial);
    CHECK(runHash(4) == serial);
    CHECK(runHash(8) == serial);
}
