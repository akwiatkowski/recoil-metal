// Income, storage, and things being built with them.
//
// Numbers taken from the blueprints so the arithmetic is checked against the game rather
// than against itself: a UEF Mass Extractor (UEB1103) costs 36 mass and 360 energy over a
// BuildTime of 60, and a commander (UEL0001) builds at a rate of 10 — so six seconds, at
// 6 mass and 60 energy a second.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "core/sim/Economy.hpp"

#include "support/EconomyTick.hpp"
#include "support/FxMatchers.hpp"

#include <vector>

using Catch::Approx;
using rm::sim::Construction;
using rm::sim::Economy;
using rm::sim::Resources;

namespace {

/// The clock these cases are written against. Every assertion below is stated in SECONDS,
/// because that is how the blueprints state their numbers and how a reader thinks — so the
/// helpers convert, and the conversion is visible in one place rather than at fifty.
const rm::sim::TickRate kRate{10};

/// Resources from the decimals a blueprint states.
[[nodiscard]] Resources res(float mass, float energy) {
    return Resources{.mass = rm::test::mag(mass), .energy = rm::test::mag(energy)};
}

/// A per-SECOND rate as the per-tick amount the economy now holds.
[[nodiscard]] Resources perTick(float massPerSecond, float energyPerSecond) {
    return Resources{.mass = kRate.magPerTick(massPerSecond),
                     .energy = kRate.magPerTick(energyPerSecond)};
}

/// A stored magnitude back as the decimal an assertion is written in.
[[nodiscard]] float amount(rm::sim::Mag value) { return rm::test::asFloat(value); }

/// A per-tick rate back as the per-second figure an assertion is written in.
[[nodiscard]] float rateOf(rm::sim::Mag perTickValue) {
    return rm::test::asFloat(perTickValue) * static_cast<float>(kRate.ticksPerSecond());
}

/// UEB1103, a UEF Mass Extractor, built by a rate-10 commander.
[[nodiscard]] Construction massExtractor() {
    return Construction{
        .armyIndex = 0,
        .position = rm::test::at(100, 0, 100),
        .cost = res(36.0f, 360.0f),
        .buildTimeRemaining = rm::test::mag(60.0f),
        .totalBuildTime = rm::test::mag(60.0f),
        .buildPerTick = kRate.magPerTick(10.0f),
        .blueprintIndex = 0,
    };
}

/// An economy with more than enough of everything, so a test about one thing is not also
/// a test about stalling.
[[nodiscard]] Economy rich() {
    return Economy{
        .stored = res(10000.0f, 10000.0f),
        .storage = res(10000.0f, 10000.0f),
        .incomePerTick = {},
        .fundedFraction = rm::sim::kFxOne,
    };
}

} // namespace

TEST_CASE("a build's drain is its cost spread over how long it will take") {
    // 60 build units at a rate of 10 is six seconds; 36 mass over six seconds is 6 a
    // second. This is what `BuildRate` MEANS, and why two engineers on one structure drain
    // twice as fast for the same total.
    // Asserted per SECOND, converted back from the per-tick figure the function now returns:
    // the claim is about what `BuildRate` means, which is a per-second fact.
    const Resources rate = rm::sim::drainPerTick(massExtractor());
    CHECK(rateOf(rate.mass) == Approx(6.0f).margin(0.01));
    CHECK(rateOf(rate.energy) == Approx(60.0f).margin(0.1));

    Construction faster = massExtractor();
    faster.buildPerTick = kRate.magPerTick(20.0f);  // three seconds
    const Resources quick = rm::sim::drainPerTick(faster);
    CHECK(rateOf(quick.mass) == Approx(12.0f).margin(0.01));
    CHECK(rateOf(quick.energy) == Approx(120.0f).margin(0.1));
}

TEST_CASE("a funded build finishes in the time the blueprint implies") {
    Economy economy = rich();
    std::vector<Construction> building{massExtractor()};

    // Six seconds at ten ticks a second.
    for (int tick = 0; tick < 60; ++tick) {
        rm::test::tickBuild(economy, building);
    }

    REQUIRE(building.size() == 1);
    CHECK(building.front().finished());
    CHECK(rm::test::asFloat(building.front().fraction()) == Approx(1.0f));

    // And it cost exactly what the blueprint says, not a tick more or less.
    CHECK(amount(economy.stored.mass) == Approx(10000.0f - 36.0f).margin(0.01));
    CHECK(amount(economy.stored.energy) == Approx(10000.0f - 360.0f).margin(0.1));
}

TEST_CASE("a build is not nearly done half way through, it is exactly half done") {
    Economy economy = rich();
    std::vector<Construction> building{massExtractor()};

    for (int tick = 0; tick < 30; ++tick) {  // three of the six seconds
        rm::test::tickBuild(economy, building);
    }
    CHECK(rm::test::asFloat(building.front().fraction()) == Approx(0.5f));
    CHECK_FALSE(building.front().finished());
}

TEST_CASE("a shortfall reaches the build one tick late") {
    // The mechanic the game is built around: a stall is a slowdown, not a refusal. But the
    // slowdown ARRIVES A TICK LATE, and that lag is retail's, not an artifact. The engine
    // caches the funding ratio on the builder in the motion stage — which runs last — and
    // reads it in command dispatch, which runs first (`C-142`, `C-162`). A builder therefore
    // always advances on the previous beat's fraction.
    Economy economy = rich();
    economy.stored.mass = rm::test::mag(0.3f);   // enough for half of one tick's 0.6
    economy.incomePerTick.mass = kRate.magPerTick(3.0f);  // half of the 6 a second the build wants
    economy.incomePerTick.energy = kRate.magPerTick(10000.0f);
    economy.storage = res(10000.0f, 10000.0f);
    economy.stored.energy = rm::test::mag(10000.0f);

    std::vector<Construction> building{massExtractor()};
    rm::test::tickBuild(economy, building);
    CHECK(rm::test::asFloat(economy.fundedFraction) == Approx(1.0f));  // the first tick is affordable

    // Run it dry. The RATIO falls immediately...
    economy.stored.mass = rm::test::mag(0.0f);
    economy.incomePerTick.mass = kRate.magPerTick(3.0f);
    const float beforeLagged = amount(building.front().buildTimeRemaining);
    rm::test::tickBuild(economy, building);
    CHECK(rm::test::asFloat(economy.fundedFraction) == Approx(0.5f).margin(0.01));
    // ...but this tick still advances on the fraction cached while the economy was rich.
    CHECK(beforeLagged - amount(building.front().buildTimeRemaining) == Approx(1.0f).margin(0.01));

    // Only now does the build feel it.
    const float before = amount(building.front().buildTimeRemaining);
    rm::test::tickBuild(economy, building);
    CHECK(before - amount(building.front().buildTimeRemaining) == Approx(0.5f).margin(0.01));
}

TEST_CASE("a shortfall slows every build equally, not the last one in the list") {
    // Paying in list order would fund whoever came first and starve the rest, which makes
    // progress depend on array order — invisible until two identical bases behave
    // differently.
    Economy economy = rich();
    economy.stored.mass = rm::test::mag(0.0f);
    economy.incomePerTick.mass = kRate.magPerTick(6.0f);  // half of what two extractors want
    economy.stored.energy = rm::test::mag(100000.0f);
    economy.storage.energy = rm::test::mag(100000.0f);

    std::vector<Construction> building{massExtractor(), massExtractor()};
    rm::test::tickBuild(economy, building);

    CHECK(rm::test::asFloat(economy.fundedFraction) == Approx(0.5f).margin(0.01));
    CHECK(amount(building[0].buildTimeRemaining)
          == Approx(amount(building[1].buildTimeRemaining)));
}

TEST_CASE("energy can be the thing that stalls, not only mass") {
    // Whichever is scarcer decides, because a build needs both.
    Economy economy = rich();
    economy.stored.energy = rm::test::mag(0.0f);
    economy.incomePerTick.energy = kRate.magPerTick(30.0f);  // half of the 60 a second wanted
    economy.storage.energy = rm::test::mag(10000.0f);

    std::vector<Construction> building{massExtractor()};
    rm::test::tickBuild(economy, building);

    CHECK(rm::test::asFloat(economy.fundedFraction) == Approx(0.5f).margin(0.01));
}

TEST_CASE("nothing being built means nothing is spent and nothing stalls") {
    Economy economy = rich();
    economy.incomePerTick = perTick(2.0f, 20.0f);
    std::vector<Construction> nothing;

    const float massBefore = amount(economy.stored.mass);
    rm::sim::tickEconomy(economy, nothing);

    // Income still arrives, and an idle economy is fully funded rather than divided by
    // zero.
    CHECK(rm::test::asFloat(economy.fundedFraction) == Approx(1.0f));
    CHECK(amount(economy.stored.mass) >= massBefore);
}

TEST_CASE("income beyond storage is lost, which is the pressure to build something") {
    Economy economy;
    economy.storage = res(100.0f, 100.0f);
    economy.stored = res(99.0f, 99.0f);
    economy.incomePerTick = perTick(100.0f, 100.0f);

    std::vector<Construction> nothing;
    rm::sim::tickEconomy(economy, nothing);

    CHECK(amount(economy.stored.mass) == Approx(100.0f));
    CHECK(amount(economy.stored.energy) == Approx(100.0f));
}

TEST_CASE("a full store spends current income before overflow is clamped") {
    Economy economy;
    economy.storage = res(100.0f, 100.0f);
    economy.stored = economy.storage;
    economy.incomePerTick = res(1.0f, 1.0f);

    std::vector<Construction> building{Construction{
        .cost = res(2.0f, 2.0f),
        .buildTimeRemaining = rm::test::mag(2.0f),
        .totalBuildTime = rm::test::mag(2.0f),
        .buildPerTick = rm::test::mag(1.0f),
    }};

    rm::test::tickBuild(economy, building);

    CHECK(amount(economy.stored.mass) == Approx(100.0f));
    CHECK(amount(economy.stored.energy) == Approx(100.0f));
    CHECK(amount(building[0].buildTimeRemaining) == Approx(1.0f));
}

TEST_CASE("current income is spendable with no storage headroom") {
    Economy economy;
    economy.incomePerTick = res(1.0f, 1.0f);

    std::vector<Construction> building{Construction{
        .cost = res(2.0f, 2.0f),
        .buildTimeRemaining = rm::test::mag(2.0f),
        .totalBuildTime = rm::test::mag(2.0f),
        .buildPerTick = rm::test::mag(1.0f),
    }};

    rm::test::tickBuild(economy, building);

    CHECK(rm::test::asFloat(economy.fundedFraction) == Approx(1.0f));
    CHECK(amount(building[0].buildTimeRemaining) == Approx(1.0f));
    CHECK(amount(economy.stored.mass) == Approx(0.0f));
    CHECK(amount(economy.stored.energy) == Approx(0.0f));
}

TEST_CASE("carried overflow cannot fund this tick") {
    Economy economy;
    economy.storage = res(100.0f, 100.0f);
    economy.stored = res(101.0f, 101.0f);

    std::vector<Construction> building{Construction{
        .cost = res(2.0f, 2.0f),
        .buildTimeRemaining = rm::test::mag(2.0f),
        .totalBuildTime = rm::test::mag(2.0f),
        .buildPerTick = rm::test::mag(1.0f),
    }};

    rm::test::tickBuild(economy, building);

    CHECK(amount(economy.stored.mass) == Approx(99.0f));
    CHECK(amount(economy.stored.energy) == Approx(99.0f));
}

TEST_CASE("a negative capacity cannot make storage negative") {
    Economy economy;
    economy.storage = res(-1.0f, -1.0f);
    economy.stored = res(10.0f, 10.0f);

    std::vector<Construction> nothing;
    rm::sim::tickEconomy(economy, nothing);

    CHECK(amount(economy.stored.mass) == Approx(0.0f));
    CHECK(amount(economy.stored.energy) == Approx(0.0f));
}

TEST_CASE("a store never goes negative, however the arithmetic falls") {
    // Floating point can leave a hair below zero after the subtraction, and a negative
    // store would make the next tick's ratio negative and run every build BACKWARDS.
    Economy economy;
    economy.storage = res(1000.0f, 1000.0f);
    economy.stored = res(0.6f, 6.0f);

    std::vector<Construction> building{massExtractor()};
    for (int tick = 0; tick < 20; ++tick) {
        rm::test::tickBuild(economy, building);
        CHECK(amount(economy.stored.mass) >= 0.0f);
        CHECK(amount(economy.stored.energy) >= 0.0f);
        CHECK(amount(building.front().buildTimeRemaining) <= 60.0f);
    }
}

TEST_CASE("a build with no rate never progresses and costs nothing") {
    // A builder with no BuildRate is not building. Dividing by it would be a NaN that
    // spreads through the store and makes every later tick meaningless.
    Economy economy = rich();
    Construction stalled = massExtractor();
    stalled.buildPerTick = {};

    std::vector<Construction> building{stalled};
    const float massBefore = amount(economy.stored.mass);
    rm::test::tickBuild(economy, building);

    CHECK(amount(economy.stored.mass) == Approx(massBefore));
    CHECK(amount(building.front().buildTimeRemaining) == Approx(60.0f));
    CHECK_FALSE(building.front().finished());
}

TEST_CASE("finished work is taken out and handed back") {
    std::vector<Construction> building{massExtractor(), massExtractor()};
    building[0].buildTimeRemaining = {};

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
    economy.storage = res(650.0f, 5000.0f);  // one extractor's own storage
    economy.incomePerTick = perTick(rm::sim::kCommanderTrickleMassPerSecond,
                                    rm::sim::kCommanderTrickleEnergyPerSecond);

    std::vector<Construction> building{massExtractor()};

    int ticks = 0;
    while (!building.front().finished() && ticks < 10000) {
        rm::test::tickBuild(economy, building);
        ++ticks;
    }

    CHECK(building.front().finished());
    // Slower than the six seconds a rich army manages — it is a trickle — but it finishes,
    // which is the whole requirement.
    CHECK(ticks > 60);
    CHECK(ticks < 1000);
}

TEST_CASE("upkeep competes with construction rather than preceding it") {
    // **This test changed sides.** It used to assert that upkeep is charged FIRST and that the
    // ordering "is the mechanic" — a base short of power stops building rather than stops
    // running. That was ours, not Forged Alliance's. Retail cannot prioritise between them
    // even in principle: `Unit.lua` sums maintenance and build cost into one consumption
    // figure before the native setter ever sees it, so the engine has a single number per
    // consumer and no way to tell the halves apart (`C-103`, `C-161`).
    Economy economy;
    economy.storage = res(1000.0f, 1000.0f);
    economy.stored = res(100.0f, 6.0f);
    economy.upkeepPerTick = perTick(0.0f, 60.0f);

    std::vector<Construction> building{massExtractor()};
    rm::test::tickBuild(economy, building);

    // Upkeep is energy-only, so it sits in the single-resource bucket; the build wants both
    // and sits in the multi-resource one. Energy binds, and neither is served first.
    CHECK_FALSE(economy.massIsBinding);
    CHECK(amount(economy.requestedLastTick.energy) == Approx(12.0f).margin(0.01));
    // What was actually paid is strictly less than what was asked for, and the store empties.
    CHECK(amount(economy.usageLastTick.energy) < 12.0f);
    CHECK(amount(economy.stored.energy) == Approx(0.0f).margin(0.01));
}

TEST_CASE("upkeep alone can empty a store, and never past zero") {
    Economy economy;
    economy.storage = res(1000.0f, 1000.0f);
    economy.stored = res(0.0f, 1.0f);
    economy.upkeepPerTick = perTick(0.0f, 500.0f);

    std::vector<Construction> nothing;
    rm::sim::tickEconomy(economy, nothing);
    CHECK(amount(economy.requestedLastTick.energy) == Approx(50.0f));
    // Not exactly 1.0: the ratio is a fixed-point division and multiplying it back through
    // the demand loses a raw step. Retail loses the same step in binary32 at the same place.
    CHECK(amount(economy.usageLastTick.energy) == Approx(1.0f).margin(0.01));

    for (int tick = 1; tick < 5; ++tick) {
        rm::sim::tickEconomy(economy, nothing);
        CHECK(amount(economy.stored.energy) >= 0.0f);
    }
    CHECK(amount(economy.stored.energy) == Approx(0.0f).margin(0.01));

    // And the fraction reports ZERO, not one. An unpayable upkeep bill is unmet demand now
    // that upkeep is a request (`C-161`); under the old lump-subtraction model it was
    // invisible and this asserted "fully funded: there is nothing asking to be paid".
    CHECK(rm::test::asFloat(economy.fundedFraction) == Approx(0.0f).margin(0.01));
}

TEST_CASE("an extractor's own upkeep eats the energy it needed to be built") {
    // The real numbers: UEB1103 makes 2 mass a second and burns 2 energy doing it. An
    // economy that ignored the second would run richer than the game's.
    Economy economy;
    economy.storage = res(650.0f, 5000.0f);
    economy.incomePerTick = perTick(rm::sim::kCommanderTrickleMassPerSecond,
                                    rm::sim::kCommanderTrickleEnergyPerSecond);

    // One extractor standing: +2 mass, -2 energy.
    economy.incomePerTick.mass += kRate.magPerTick(2.0f);
    economy.upkeepPerTick.energy += kRate.magPerTick(2.0f);

    std::vector<Construction> nothing;
    for (int tick = 0; tick < 10; ++tick) {  // one second
        rm::sim::tickEconomy(economy, nothing);
    }

    // 2.5 mass a second in, and 5 energy in against 2 out.
    CHECK(amount(economy.stored.mass) == Approx(2.5f).margin(0.01));
    CHECK(amount(economy.stored.energy) == Approx(3.0f).margin(0.01));
}

TEST_CASE("allied overflow is a progressive split, not an equal one") {
    // Retail divides the REMAINING excess by the REMAINING recipient count, caps each share
    // by that ally's headroom, and subtracts what was actually taken (`C-163`). So an ally
    // with no room passes its share along rather than wasting it, and a later recipient can
    // receive far more than an equal split would give. A flat 1/n is the obvious reading and
    // is wrong wherever any ally is near cap.
    std::vector<rm::sim::Army> armies(3);
    for (int index = 0; index < 3; ++index) {
        armies[static_cast<std::size_t>(index)].index = index;
        armies[static_cast<std::size_t>(index)].alliance = 0;  // one team
    }

    std::vector<Economy> economies(3);
    for (Economy& economy : economies) {
        economy.storage = res(100.0f, 100.0f);
    }
    // The giver is 60 mass over its cap.
    economies[0].stored = res(160.0f, 0.0f);
    // The first ally is FULL, so it can take nothing and must pass its share on.
    economies[1].stored = res(100.0f, 0.0f);
    economies[2].stored = res(0.0f, 0.0f);

    rm::sim::shareOverflow(economies, armies);

    CHECK(amount(economies[1].sharedIn.mass) == Approx(0.0f));
    // An equal split would have handed the second ally 30 and destroyed the other 30. The
    // progressive one gives it everything the first could not hold.
    CHECK(amount(economies[2].sharedIn.mass) == Approx(60.0f).margin(0.01));
    // And the giver keeps only its cap either way.
    CHECK(amount(economies[0].stored.mass) == Approx(100.0f));
}

TEST_CASE("a gift arrives as income, so a full ally gains nothing from it") {
    // Retail credits a share to the recipient's INCOME accumulator, which the allocator
    // consumes at the start of the next beat (`C-163`) — not to its store. So the gift is
    // spendable rather than banked, and it cannot push anyone over their cap.
    std::vector<rm::sim::Army> armies(2);
    armies[1].index = 1;
    armies[0].alliance = armies[1].alliance = 0;

    std::vector<Economy> economies(2);
    economies[0].storage = res(100.0f, 100.0f);
    economies[1].storage = res(100.0f, 100.0f);
    economies[0].stored = res(150.0f, 0.0f);
    economies[1].stored = res(90.0f, 0.0f);

    rm::sim::shareOverflow(economies, armies);
    // Capped by the ally's 10 of headroom, not by the 50 on offer.
    CHECK(amount(economies[1].sharedIn.mass) == Approx(10.0f).margin(0.01));
    CHECK(amount(economies[1].stored.mass) == Approx(90.0f));  // not yet arrived

    std::vector<Construction> nothing;
    rm::sim::tickEconomy(economies[1], nothing);
    CHECK(amount(economies[1].stored.mass) == Approx(100.0f).margin(0.01));
    CHECK(amount(economies[1].sharedIn.mass) == Approx(0.0f));  // consumed, not re-credited
}

TEST_CASE("storage capacity truncates per structure, not on the sum") {
    // The economy's ONE rounding point (`C-069`, `C-104`(e), `C-160`). Retail adds each
    // structure's contribution through a truncating conversion, so three structures offering
    // 105.6 contribute 315 rather than 316.8. Truncating the sum instead agrees for
    // whole-numbered blueprints and diverges for every other, which is what decides it.
    const rm::sim::Mag one = rm::test::mag(105.6f);
    rm::sim::Mag perStructure{};
    for (int index = 0; index < 3; ++index) {
        perStructure += rm::sim::Mag::fromInt(one.floorToInt());
    }
    CHECK(amount(perStructure) == Approx(315.0f));
    CHECK(amount(perStructure) != Approx(316.8f));
}

TEST_CASE("the per-unit consumed ratio is recovered from the demand shape") {
    // Retail keeps this on each unit's own request (`Unit+0x53c`); we recover it from the
    // bucket sums, which is equivalent because the allocator is linear in them (`C-159`).
    // Shields and intel read it as a rate multiplier, so a brownout has to reach them.
    Economy economy;
    economy.storage = res(1000.0f, 1000.0f);
    economy.stored = res(1000.0f, 3.0f);   // plenty of mass, energy is short
    economy.upkeepPerTick = perTick(0.0f, 60.0f);  // energy-only: single-resource bucket

    std::vector<Construction> building{massExtractor()};  // wants both: multi-resource
    rm::test::tickBuild(economy, building);

    CHECK_FALSE(economy.massIsBinding);  // energy binds

    // A consumer that wants nothing is unaffected, so the ratio stays neutral.
    CHECK(rm::test::asFloat(economy.consumedRatio(res(0.0f, 0.0f))) == Approx(1.0f));

    // **Which BUCKET a request is summed into and which RATIO it is granted at are different
    // questions**, and conflating them is the easy mistake here — this test was first written
    // asserting that an energy-only consumer gets `singleResourceFunded`, and it does not.
    // Retail buckets by how many resources are outstanding, but grants at `r1` whenever the
    // request is outstanding **on the binding resource** (`C-159`). Energy binds here, so an
    // energy-only consumer is on the binding resource and takes `r1` — the same ratio as a
    // consumer wanting both.
    CHECK(rm::test::asFloat(economy.consumedRatio(perTick(0.0f, 60.0f)))
          == Approx(rm::test::asFloat(economy.multiResourceFunded)));
    CHECK(rm::test::asFloat(economy.consumedRatio(perTick(10.0f, 10.0f)))
          == Approx(rm::test::asFloat(economy.multiResourceFunded)));
    // A MASS-only consumer is not outstanding on the binding resource, so it takes `r2` —
    // and with mass in plentiful supply that means it is funded in full while the energy
    // payers are throttled. A single global ratio cannot express that.
    CHECK(rm::test::asFloat(economy.consumedRatio(perTick(10.0f, 0.0f)))
          == Approx(rm::test::asFloat(economy.singleResourceFunded)));
}
