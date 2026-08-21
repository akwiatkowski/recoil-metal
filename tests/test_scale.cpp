// Five thousand units, at the configured rate.
//
// §7 P8.1's stated test is "a perf regression gate", and this is the one test in the suite whose
// assertion is about TIME. That makes it different in kind from everything else here, so the
// design is worth stating:
//
//   IT ASSERTS A RATIO, NOT A DURATION. The claim is "the sim keeps up with its own clock, with
//   room to spare" — 100 ticks at 10 Hz is ten seconds of match, and the gate is that it takes
//   a fraction of that in wall time. A gate written as "under 800 ms" would be a statement about
//   this machine.
//
//   THE MARGIN IS WIDE ON PURPOSE. Measured here the sim runs **12.9x real time** at 5,000
//   units — 100 ticks of a ten-second match in 0.77 s, firing 50,000 shots — and the gate is 2x.
//   That is not a weak test: the thing it exists to catch is a pass going quadratic again, which
//   at this unit count is a factor of ten, not a factor of two. A tight gate would fail on a
//   laptop under thermal load and get deleted, which is worse than a loose one that holds.
//
//   IT SKIPS IN AN UNOPTIMISED BUILD. A Debug build is several times slower and would fail this
//   for no reason anybody should act on.
//
// WHAT MADE IT PASSABLE. Before P5 this was not close: the same 5,008-unit match took 73.7 s of
// user time against 7.0 s after, because `nearestTarget` and the aim sweep scanned every unit for
// every shooter. §7 put this item behind a Linux build and a cross-architecture hash; it is first
// in P8 now because P5 turned it from a project into a measurement.
#include <catch2/catch_test_macros.hpp>

#include "core/sim/Skirmish.hpp"

#include "support/TestRoster.hpp"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <vector>

namespace {

/// How many units the criterion names.
constexpr std::size_t kUnits = 5000;

/// How many ticks to time. A hundred is a second of match at the slowest supported rate and ten
/// at the default — long enough to average over a scheduler hiccup, short enough that the whole
/// suite stays under half a minute.
constexpr int kTicks = 100;

/// The margin. See the note above: the gate is 2x real time against 12.9x measured.
constexpr double kRequiredSpeedup = 2.0;

[[nodiscard]] rm::HeightField bigField() {
    rm::HeightField field;
    field.squaresX = 512;  // 4,096 elmos across, a real map's size
    field.squaresZ = 512;
    field.baseHeight = 0.0f;
    field.heightScale = 1.0f;
    field.raw.assign(field.sampleCount(), std::uint16_t{0});
    return field;
}

/// An armed unit, so the timing includes what actually costs: targeting, aiming and firing.
///
/// A crowd of unarmed units would time the movement and collision passes only, and those were
/// never the quadratic ones.
[[nodiscard]] rm::unitdef::UnitDef fighterDef() {
    rm::unitdef::UnitDef def;
    def.name = "test_fighter";
    def.speedElmosPerSecond = 30.0f;
    def.turnRateRadiansPerSecond = 2.0f;

    rm::unitdef::Weapon weapon;
    weapon.label = "test gun";
    weapon.role = rm::unitdef::WeaponRole::DirectFire;
    weapon.damage = rm::sim::magFromFloat(5.0f);
    weapon.maxRange = rm::sim::fxFromFloat(200.0f);
    weapon.muzzleVelocityElmosPerSecond = 300.0f;
    weapon.rateOfFire = 1.0f;
    weapon.turreted = true;
    def.weapons.push_back(weapon);
    return def;
}

} // namespace

TEST_CASE("five thousand units keep up with the clock") {
#ifndef NDEBUG
    SKIP("a perf gate in an unoptimised build measures the build, not the engine");
#endif

    const rm::HeightField field = bigField();
    const rm::sim::Terrain terrain{field};
    const rm::sim::PassabilityGrid grid = rm::sim::buildPassability(field, 0.0f, 60.0f, 0.0f);

    rm::test::Roster roster;
    const rm::UnitTypeIndex type = roster.addType(fighterDef());

    // TWO ARMIES, INTERLEAVED AND SPREAD over the map, which is the expensive arrangement: every
    // unit has enemies in range of something, so the targeting passes do real work rather than
    // finding nothing and returning early.
    //
    // Spawned in a lattice rather than at random, because this test measures rather than
    // asserts an answer — a layout that changed run to run would make the number change with it.
    const auto side = static_cast<int>(std::sqrt(static_cast<double>(kUnits)) + 1.0);
    int placed = 0;
    for (int z = 0; z < side && placed < static_cast<int>(kUnits); ++z) {
        for (int x = 0; x < side && placed < static_cast<int>(kUnits); ++x) {
            (void)roster.add(type, static_cast<float>(x) * 52.0f + 40.0f,
                             static_cast<float>(z) * 52.0f + 40.0f, (x + z) % 2, 200.0f);
            ++placed;
        }
    }
    REQUIRE(static_cast<std::size_t>(placed) == kUnits);

    std::vector<rm::sim::Army> armies = rm::sim::freeForAll(2);
    std::vector<rm::sim::Projectile> shots;
    std::vector<rm::sim::Economy> economies(2);
    const std::vector<int> commandersEver{0, 0};
    const std::vector<const rm::sim::PassabilityGrid*> grids(roster.catalog.size(), &grid);

    rm::sim::Match match{.armies = armies,
                         .economies = economies,
                         .projectiles = &shots,
                         .passability = grids,
                         .commandersEver = commandersEver};

    // One tick before timing, so the first tick's costs — the spatial index's first sort growing
    // its buffer, the projectile list's first allocation — are not counted as the steady state.
    (void)rm::sim::tickSkirmish(roster.store, roster.catalog, match, terrain, roster.rate);

    const auto started = std::chrono::steady_clock::now();
    std::size_t shotsFired = 0;
    for (int tick = 0; tick < kTicks; ++tick) {
        shotsFired += rm::sim::tickSkirmish(roster.store, roster.catalog, match, terrain,
                                            roster.rate)
                          .shotsFired;
    }
    const auto elapsed = std::chrono::duration<double>(
                             std::chrono::steady_clock::now() - started)
                             .count();

    const double simulated = static_cast<double>(kTicks) * roster.rate.secondsPerTick();
    const double speedup = simulated / elapsed;

    // Reported whether or not it passes, because the number is the useful part — a gate that
    // only says pass or fail cannot tell "we are near the edge" from "we have ten times the
    // headroom we need".
    WARN("5,000 units: " << kTicks << " ticks of " << simulated << "s took " << elapsed
                         << "s — " << speedup << "x real time, " << shotsFired << " shots");

    CHECK(speedup >= kRequiredSpeedup);

    // AND THE WORK HAPPENED. A gate that passed because nothing was being simulated would be
    // the easiest of all to satisfy: 5,000 armed units next to enemies fire.
    CHECK(shotsFired > 0);
}
