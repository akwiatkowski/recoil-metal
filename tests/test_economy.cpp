// Income, storage, and things being built with them.
//
// Numbers taken from the blueprints so the arithmetic is checked against the game rather
// than against itself: a UEF Mass Extractor (UEB1103) costs 36 mass and 360 energy over a
// BuildTime of 60, and a commander (UEL0001) builds at a rate of 10 — so six seconds, at
// 6 mass and 60 energy a second.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "core/sim/Economy.hpp"

#include <vector>

using Catch::Approx;
using rm::sim::Construction;
using rm::sim::Economy;
using rm::sim::Resources;

namespace {

/// UEB1103, a UEF Mass Extractor, built by a rate-10 commander.
[[nodiscard]] Construction massExtractor() {
    return Construction{
        .armyIndex = 0,
        .position = {100.0f, 0.0f, 100.0f},
        .cost = {.mass = 36.0f, .energy = 360.0f},
        .buildTimeRemaining = 60.0f,
        .totalBuildTime = 60.0f,
        .buildRate = 10.0f,
        .blueprintIndex = 0,
    };
}

/// An economy with more than enough of everything, so a test about one thing is not also
/// a test about stalling.
[[nodiscard]] Economy rich() {
    return Economy{
        .stored = {.mass = 10000.0f, .energy = 10000.0f},
        .storage = {.mass = 10000.0f, .energy = 10000.0f},
        .incomePerSecond = {},
        .fundedFraction = 1.0f,
    };
}

} // namespace

TEST_CASE("a build's drain is its cost spread over how long it will take") {
    // 60 build units at a rate of 10 is six seconds; 36 mass over six seconds is 6 a
    // second. This is what `BuildRate` MEANS, and why two engineers on one structure drain
    // twice as fast for the same total.
    const Resources rate = rm::sim::drainPerSecond(massExtractor());
    CHECK(rate.mass == Approx(6.0f));
    CHECK(rate.energy == Approx(60.0f));

    Construction faster = massExtractor();
    faster.buildRate = 20.0f;  // three seconds
    const Resources quick = rm::sim::drainPerSecond(faster);
    CHECK(quick.mass == Approx(12.0f));
    CHECK(quick.energy == Approx(120.0f));
}

TEST_CASE("a funded build finishes in the time the blueprint implies") {
    Economy economy = rich();
    std::vector<Construction> building{massExtractor()};

    // Six seconds at ten ticks a second.
    for (int tick = 0; tick < 60; ++tick) {
        rm::sim::tickEconomy(economy, building);
    }

    REQUIRE(building.size() == 1);
    CHECK(building.front().finished());
    CHECK(building.front().fraction() == Approx(1.0f));

    // And it cost exactly what the blueprint says, not a tick more or less.
    CHECK(economy.stored.mass == Approx(10000.0f - 36.0f).margin(0.01));
    CHECK(economy.stored.energy == Approx(10000.0f - 360.0f).margin(0.1));
}

TEST_CASE("a build is not nearly done half way through, it is exactly half done") {
    Economy economy = rich();
    std::vector<Construction> building{massExtractor()};

    for (int tick = 0; tick < 30; ++tick) {  // three of the six seconds
        rm::sim::tickEconomy(economy, building);
    }
    CHECK(building.front().fraction() == Approx(0.5f));
    CHECK_FALSE(building.front().finished());
}

TEST_CASE("running out slows everything by the same fraction, it does not refuse") {
    // The mechanic the game is built around: a stall is a slowdown. Half the mass means
    // half the progress, not a build that stops and waits.
    Economy economy = rich();
    economy.stored.mass = 0.3f;   // enough for half of one tick's 0.6
    economy.incomePerSecond.mass = 3.0f;  // half of the 6 a second the build wants
    economy.incomePerSecond.energy = 10000.0f;
    economy.storage = {.mass = 10000.0f, .energy = 10000.0f};
    economy.stored.energy = 10000.0f;

    std::vector<Construction> building{massExtractor()};
    rm::sim::tickEconomy(economy, building);

    CHECK(economy.fundedFraction == Approx(1.0f));  // the first tick is affordable

    // Now run it dry and watch the fraction fall rather than the build stop.
    economy.stored.mass = 0.0f;
    economy.incomePerSecond.mass = 3.0f;
    const float before = building.front().buildTimeRemaining;
    rm::sim::tickEconomy(economy, building);

    CHECK(economy.fundedFraction == Approx(0.5f).margin(0.01));
    // Progress was made, but half as much as a funded tick's full 1.0 build units.
    CHECK(before - building.front().buildTimeRemaining == Approx(0.5f).margin(0.01));
}

TEST_CASE("a shortfall slows every build equally, not the last one in the list") {
    // Paying in list order would fund whoever came first and starve the rest, which makes
    // progress depend on array order — invisible until two identical bases behave
    // differently.
    Economy economy = rich();
    economy.stored.mass = 0.0f;
    economy.incomePerSecond.mass = 6.0f;  // half of what two extractors want
    economy.stored.energy = 100000.0f;
    economy.storage.energy = 100000.0f;

    std::vector<Construction> building{massExtractor(), massExtractor()};
    rm::sim::tickEconomy(economy, building);

    CHECK(economy.fundedFraction == Approx(0.5f).margin(0.01));
    CHECK(building[0].buildTimeRemaining == Approx(building[1].buildTimeRemaining));
}

TEST_CASE("energy can be the thing that stalls, not only mass") {
    // Whichever is scarcer decides, because a build needs both.
    Economy economy = rich();
    economy.stored.energy = 0.0f;
    economy.incomePerSecond.energy = 30.0f;  // half of the 60 a second wanted
    economy.storage.energy = 10000.0f;

    std::vector<Construction> building{massExtractor()};
    rm::sim::tickEconomy(economy, building);

    CHECK(economy.fundedFraction == Approx(0.5f).margin(0.01));
}

TEST_CASE("nothing being built means nothing is spent and nothing stalls") {
    Economy economy = rich();
    economy.incomePerSecond = {.mass = 2.0f, .energy = 20.0f};
    std::vector<Construction> nothing;

    const float massBefore = economy.stored.mass;
    rm::sim::tickEconomy(economy, nothing);

    // Income still arrives, and an idle economy is fully funded rather than divided by
    // zero.
    CHECK(economy.fundedFraction == Approx(1.0f));
    CHECK(economy.stored.mass >= massBefore);
}

TEST_CASE("income beyond storage is lost, which is the pressure to build something") {
    Economy economy;
    economy.storage = {.mass = 100.0f, .energy = 100.0f};
    economy.stored = {.mass = 99.0f, .energy = 99.0f};
    economy.incomePerSecond = {.mass = 100.0f, .energy = 100.0f};

    std::vector<Construction> nothing;
    rm::sim::tickEconomy(economy, nothing);

    CHECK(economy.stored.mass == Approx(100.0f));
    CHECK(economy.stored.energy == Approx(100.0f));
}

TEST_CASE("a store never goes negative, however the arithmetic falls") {
    // Floating point can leave a hair below zero after the subtraction, and a negative
    // store would make the next tick's ratio negative and run every build BACKWARDS.
    Economy economy;
    economy.storage = {.mass = 1000.0f, .energy = 1000.0f};
    economy.stored = {.mass = 0.6f, .energy = 6.0f};

    std::vector<Construction> building{massExtractor()};
    for (int tick = 0; tick < 20; ++tick) {
        rm::sim::tickEconomy(economy, building);
        CHECK(economy.stored.mass >= 0.0f);
        CHECK(economy.stored.energy >= 0.0f);
        CHECK(building.front().buildTimeRemaining <= 60.0f);
    }
}

TEST_CASE("a build with no rate never progresses and costs nothing") {
    // A builder with no BuildRate is not building. Dividing by it would be a NaN that
    // spreads through the store and makes every later tick meaningless.
    Economy economy = rich();
    Construction stalled = massExtractor();
    stalled.buildRate = 0.0f;

    std::vector<Construction> building{stalled};
    const float massBefore = economy.stored.mass;
    rm::sim::tickEconomy(economy, building);

    CHECK(economy.stored.mass == Approx(massBefore));
    CHECK(building.front().buildTimeRemaining == Approx(60.0f));
    CHECK_FALSE(building.front().finished());
}

TEST_CASE("finished work is taken out and handed back") {
    std::vector<Construction> building{massExtractor(), massExtractor()};
    building[0].buildTimeRemaining = 0.0f;

    const std::vector<Construction> done = rm::sim::takeFinished(building);

    REQUIRE(done.size() == 1);
    CHECK(done.front().blueprintIndex == 0);
    CHECK(building.size() == 1);  // the unfinished one is still going
    CHECK_FALSE(building.front().finished());
}

TEST_CASE("a commander's trickle is enough to afford the first extractor") {
    // The bootstrap, and the thing that makes a match possible at all: without any income
    // an army cannot afford its first extractor and the game never begins. This is a check
    // that the chosen trickle actually clears that bar.
    Economy economy;
    economy.storage = {.mass = 650.0f, .energy = 5000.0f};  // one extractor's own storage
    economy.incomePerSecond = rm::sim::kCommanderTrickle;

    std::vector<Construction> building{massExtractor()};

    int ticks = 0;
    while (!building.front().finished() && ticks < 10000) {
        rm::sim::tickEconomy(economy, building);
        ++ticks;
    }

    CHECK(building.front().finished());
    // Slower than the six seconds a rich army manages — it is a trickle — but it finishes,
    // which is the whole requirement.
    CHECK(ticks > 60);
    CHECK(ticks < 1000);
}

TEST_CASE("upkeep is charged before construction is funded") {
    // Not optional, and the ORDER is the mechanic: a base short of power stops BUILDING
    // rather than stopping running. Funding builds first and letting upkeep take the
    // remainder would invert that and make a brownout invisible.
    Economy economy;
    economy.storage = {.mass = 1000.0f, .energy = 1000.0f};
    economy.stored = {.mass = 100.0f, .energy = 6.0f};
    // A tick's upkeep is 6 energy, which is exactly what is banked — so nothing is left
    // for the 6 the build wants.
    economy.upkeepPerSecond = {.mass = 0.0f, .energy = 60.0f};

    std::vector<Construction> building{massExtractor()};
    rm::sim::tickEconomy(economy, building);

    CHECK(economy.stored.energy == Approx(0.0f));
    CHECK(economy.fundedFraction == Approx(0.0f));
    CHECK(building.front().buildTimeRemaining == Approx(60.0f));  // no progress at all
}

TEST_CASE("upkeep alone can empty a store, and never past zero") {
    Economy economy;
    economy.storage = {.mass = 1000.0f, .energy = 1000.0f};
    economy.stored = {.mass = 0.0f, .energy = 1.0f};
    economy.upkeepPerSecond = {.mass = 0.0f, .energy = 500.0f};

    std::vector<Construction> nothing;
    for (int tick = 0; tick < 5; ++tick) {
        rm::sim::tickEconomy(economy, nothing);
        CHECK(economy.stored.energy >= 0.0f);
    }
    CHECK(economy.stored.energy == Approx(0.0f));

    // And an idle economy is still fully funded: there is nothing asking to be paid.
    CHECK(economy.fundedFraction == Approx(1.0f));
}

TEST_CASE("an extractor's own upkeep eats the energy it needed to be built") {
    // The real numbers: UEB1103 makes 2 mass a second and burns 2 energy doing it. An
    // economy that ignored the second would run richer than the game's.
    Economy economy;
    economy.storage = {.mass = 650.0f, .energy = 5000.0f};
    economy.incomePerSecond = rm::sim::kCommanderTrickle;  // 0.5 mass, 5 energy

    // One extractor standing: +2 mass, -2 energy.
    economy.incomePerSecond.mass += 2.0f;
    economy.upkeepPerSecond.energy += 2.0f;

    std::vector<Construction> nothing;
    for (int tick = 0; tick < 10; ++tick) {  // one second
        rm::sim::tickEconomy(economy, nothing);
    }

    // 2.5 mass a second in, and 5 energy in against 2 out.
    CHECK(economy.stored.mass == Approx(2.5f).margin(0.01));
    CHECK(economy.stored.energy == Approx(3.0f).margin(0.01));
}
