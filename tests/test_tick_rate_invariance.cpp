// The same match at 5, 10, 20 and 50 Hz.
//
// PLAN2.md §5.1's STATED TEST, and the only thing that keeps its rule true a year from now:
// "run the same scripted match at 5, 10, 20 and 50 Hz and assert every observable duration —
// projectile lifetime, reload cadence, build completion, income accrued — lands within one
// tick of its authored value in wall-clock seconds. If a rate change moves any of them by more
// than a tick, a literal survived somewhere."
//
// WHY THIS IS THE TEST THAT MATTERS. `check_no_tick_literals.sh` catches a constant whose name
// admits what it is. This catches the ones that do not: a division by ten written inline, a
// per-second value used as though it were per-tick, a threshold tuned at one rate. Every such
// mistake produces a duration that changes when the rate does, and nothing else here will
// notice.
//
// The tolerance is ONE TICK, measured in seconds, and it has to be: a duration is rounded to a
// whole number of ticks, so at 5 Hz a reload can be up to 200 ms out by construction. What is
// forbidden is being out by a FACTOR, which is what a surviving literal produces.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "core/sim/Combat.hpp"
#include "core/sim/Economy.hpp"

#include "support/FxMatchers.hpp"
#include "support/TestRoster.hpp"

#include <cstdint>
#include <vector>

using Catch::Approx;
using rm::sim::TickRate;

namespace {

/// The four rates: both ends of the permitted range and the two in between that content is
/// likely to be authored against.
constexpr std::uint32_t kRates[] = {5, 10, 20, 50};

[[nodiscard]] rm::HeightField flatField() {
    rm::HeightField field;
    field.squaresX = 200;
    field.squaresZ = 200;
    field.baseHeight = 0.0f;
    field.heightScale = 1.0f;
    field.raw.assign(field.sampleCount(), std::uint16_t{0});
    return field;
}

[[nodiscard]] rm::unitdef::Weapon gun(float shotsPerSecond, float rangeElmos) {
    rm::unitdef::Weapon weapon;
    weapon.label = "test gun";
    weapon.role = rm::unitdef::WeaponRole::DirectFire;
    weapon.turreted = true;
    weapon.damage = rm::test::mag(1.0f);  // harmless: this is about cadence, not killing
    weapon.maxRange = rm::test::fx(rangeElmos);
    weapon.rateOfFire = shotsPerSecond;
    weapon.muzzleVelocityElmosPerSecond = 200.0f;
    return weapon;
}

} // namespace

TEST_CASE("a projectile's lifetime is thirty seconds at every rate") {
    // The constant PLAN2 §5.1 names first. It was `300` with "thirty seconds at 10 Hz" in a
    // comment; at 20 Hz that comment would have been a lie and nothing would have failed.
    const rm::HeightField field = flatField();

    for (const std::uint32_t hz : kRates) {
        const TickRate rate{hz};

        // A shot with nowhere to land: the ground is far below, so it flies until it expires.
        rm::HeightField bottomless = field;
        bottomless.baseHeight = -100000.0f;

        rm::unitdef::Weapon weapon = gun(1.0f, 300.0f);
        weapon.muzzleVelocityElmosPerSecond = 1000.0f;
        std::vector<rm::sim::Projectile> shots{
            rm::sim::launch(rm::test::at(0, 0, 0), rm::test::at(0, 0, 100), weapon, 0, rate,
                            rate.perTick(weapon.muzzleVelocityElmosPerSecond),
                            rm::unitdef::flatDamage(weapon.damage))};

        rm::sim::UnitStore nobody;
        int ticks = 0;
        while (!shots.empty() && ticks < 100000) {
            rm::sim::advanceProjectiles(shots, nobody, {}, rm::sim::Terrain{bottomless}, rate);
            ++ticks;
        }

        const double seconds = static_cast<double>(ticks) / static_cast<double>(hz);
        REQUIRE(seconds == Approx(30.0).margin(1.0 / static_cast<double>(hz)));
    }
}

TEST_CASE("a weapon's cadence is its authored shots per second, at every rate") {
    const rm::HeightField field = flatField();

    for (const std::uint32_t hz : kRates) {
        const TickRate rate{hz};

        rm::test::Roster roster;
        roster.rate = rate;

        rm::unitdef::UnitDef gunner;
        gunner.name = "test_gunner";
        gunner.weapons.push_back(gun(2.0f, 400.0f));  // two shots a second
        rm::unitdef::UnitDef target;
        target.name = "test_target";

        (void)roster.add(roster.addType(gunner), 0.0f, 0.0f, 0, 500.0f);
        (void)roster.add(roster.addType(target), 0.0f, 100.0f, 1, 100000.0f);

        const std::vector<rm::sim::Army> armies = rm::sim::freeForAll(2);
        std::vector<rm::sim::Projectile> shots;

        // Ten seconds of wall-clock time, however many ticks that is.
        std::size_t fired = 0;
        for (std::uint32_t tick = 0; tick < 10 * hz; ++tick) {
            fired += rm::sim::fireWeapons(roster.store, roster.catalog, armies, shots, rate);
        }

        // Two a second for ten seconds is twenty. One tick of slack at each end, because the
        // reload is a whole number of ticks: at 5 Hz a half-second interval is 2.5 ticks and
        // rounds, so the cadence is 2 or 3 ticks rather than exactly 2.5.
        const double expected = 20.0;
        const double slack = 2.0 + 10.0 / static_cast<double>(hz);
        REQUIRE(static_cast<double>(fired) == Approx(expected).margin(slack));
    }
}

TEST_CASE("a build takes the time its blueprint implies, at every rate") {
    // UEB1103, a UEF Mass Extractor: 36 mass and 360 energy over a BuildTime of 60, built by
    // a rate-10 commander — six seconds. The numbers are the game's; the six seconds is what
    // must not move.
    for (const std::uint32_t hz : kRates) {
        const TickRate rate{hz};

        rm::sim::Economy economy;
        economy.stored = {.mass = rm::test::mag(10000.0f), .energy = rm::test::mag(10000.0f)};
        economy.storage = economy.stored;

        std::vector<rm::sim::Construction> building{rm::sim::Construction{
            .armyIndex = 0,
            .position = {},
            .cost = {.mass = rm::test::mag(36.0f), .energy = rm::test::mag(360.0f)},
            .buildTimeRemaining = rm::test::mag(60.0f),
            .totalBuildTime = rm::test::mag(60.0f),
            .buildPerTick = rate.magPerTick(10.0f),
            .blueprintIndex = 0,
        }};

        std::uint32_t ticks = 0;
        while (!building.front().finished() && ticks < 100000) {
            rm::sim::tickEconomy(economy, building);
            ++ticks;
        }

        const double seconds = static_cast<double>(ticks) / static_cast<double>(hz);
        REQUIRE(seconds == Approx(6.0).margin(1.0 / static_cast<double>(hz)));

        // And it cost what the blueprint says, whatever the rate — the per-tick share of the
        // cost is derived, so the total must not drift with how many shares there were.
        REQUIRE(rm::test::asFloat(economy.stored.mass) == Approx(10000.0f - 36.0f).margin(0.5f));
    }
}

TEST_CASE("income accrues at its authored rate, at every rate") {
    for (const std::uint32_t hz : kRates) {
        const TickRate rate{hz};

        rm::sim::Economy economy;
        economy.storage = {.mass = rm::test::mag(100000.0f),
                           .energy = rm::test::mag(100000.0f)};
        // Two mass a second: a T1 mass extractor, the slowest producer in the corpus and so
        // the one where a per-tick rounding error shows up most.
        economy.incomePerTick = {.mass = rate.magPerTick(2.0f), .energy = rate.magPerTick(20.0f)};

        std::vector<rm::sim::Construction> nothing;
        for (std::uint32_t tick = 0; tick < 60 * hz; ++tick) {  // one minute
            rm::sim::tickEconomy(economy, nothing);
        }

        // 120 mass in a minute. The slack is one tick's worth of income, which is the most a
        // rounded per-tick rate can be out by per tick — times the number of ticks, since the
        // error is systematic rather than random.
        const double perTick = rm::test::asFloat(economy.incomePerTick.mass);
        const double expected = perTick * 60.0 * static_cast<double>(hz);
        REQUIRE(rm::test::asFloat(economy.stored.mass) == Approx(expected).margin(0.5));

        // And that expectation is itself close to the authored 120 — this is the assertion
        // that would catch a per-second value used as a per-tick one.
        REQUIRE(expected == Approx(120.0).margin(1.0));
    }
}

TEST_CASE("a unit covers the same ground per second at every rate") {
    // The movement pass takes per-tick speeds, so this is where a stray division would show
    // up as a unit that walks five times too fast at 50 Hz.
    const rm::HeightField field = flatField();
    const rm::sim::Terrain terrain{field};

    for (const std::uint32_t hz : kRates) {
        const TickRate rate{hz};

        std::vector<rm::sim::Transform> units{
            rm::sim::Transform{.x = rm::test::fx(100.0f), .z = rm::test::fx(100.0f)}};
        std::vector<rm::sim::MoveState> motion{rm::sim::defaultMotion(rate)};

        // Straight ahead, so no time is lost to turning — the claim is about speed.
        rm::sim::orderTo(motion[0], terrain, rm::test::fx(100.0f), rm::test::fx(1500.0f));

        for (std::uint32_t tick = 0; tick < 5 * hz; ++tick) {  // five seconds
            rm::sim::tick(units, motion, terrain);
        }

        // 87 elmos a second for five seconds is 435. One tick of travel of slack, which at 5 Hz
        // is 17 elmos — the coarsest the rule permits.
        const double travelled = static_cast<double>(rm::test::asFloat(
            motion[0].distanceTravelledElmos));
        const double perTick = static_cast<double>(rm::test::asFloat(motion[0].speedPerTick));
        REQUIRE(travelled == Approx(5.0 * 87.0).margin(perTick + 1.0));
    }
}
