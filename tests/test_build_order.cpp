// The scripted opponent's build order — milestone 20.
//
// NOT an AI, and tested like the clockwork it is: a fixed sequence (power, a second
// extractor, a factory, then tanks) and one attack wave whose size comes from the
// blueprints' own arithmetic rather than taste. The commander and the factory are
// separate actors — each has its own question — which is why this is three small
// functions rather than one state machine.
#include <catch2/catch_test_macros.hpp>

#include "core/sim/BuildOrder.hpp"
#include "core/unit/UnitDef.hpp"

#include <cmath>

#include "support/FxMatchers.hpp"

using rm::sim::ArmyView;
using rm::sim::Opponent;
using rm::sim::StructureOrder;

namespace {

/// The wave size these cases reason about. A local constant now that the engine's own is in
/// `data/opening.lua` — the tests are about the RULE ("launch once, at N"), and the number is a
/// balance decision the data file owns. `defaultOpening().waveSize` is checked separately, in
/// test_opening.cpp, against the file.
constexpr std::size_t kWaveSize = 20;

/// An army mid-game: first extractor standing, commander idle, nothing else yet.
[[nodiscard]] ArmyView afterFirstExtractor() {
    return ArmyView{
        .commanderAlive = true,
        .commanderBusy = false,
        .factoryBusy = false,
        .extractorsStanding = 1,
        .powerGeneratorsStanding = 0,
        .factoriesStanding = 0,
        .tanksAlive = 0,
    };
}

} // namespace

TEST_CASE("the commander follows the fixed order: power, second extractor, factory") {
    ArmyView view = afterFirstExtractor();

    SECTION("power generator first — everything after it is energy-bound") {
        CHECK(rm::sim::nextStructure(view) == StructureOrder::PowerGenerator);
    }

    SECTION("then a second extractor, because tanks are mass-bound") {
        view.powerGeneratorsStanding = 1;
        CHECK(rm::sim::nextStructure(view) == StructureOrder::Extractor);
    }

    SECTION("then the factory, and after it the commander is done") {
        view.powerGeneratorsStanding = 1;
        view.extractorsStanding = 2;
        CHECK(rm::sim::nextStructure(view) == StructureOrder::Factory);

        view.factoriesStanding = 1;
        CHECK(rm::sim::nextStructure(view) == StructureOrder::None);
    }
}

TEST_CASE("the commander starts nothing while it should wait") {
    ArmyView view = afterFirstExtractor();

    SECTION("not before the first extractor stands — that build is ordered at spawn") {
        view.extractorsStanding = 0;
        CHECK(rm::sim::nextStructure(view) == StructureOrder::None);
    }

    SECTION("not while something is already under construction") {
        view.commanderBusy = true;
        CHECK(rm::sim::nextStructure(view) == StructureOrder::None);
    }

    SECTION("not once it is dead — a defeated army orders nothing") {
        view.commanderAlive = false;
        CHECK(rm::sim::nextStructure(view) == StructureOrder::None);
    }
}

TEST_CASE("the factory produces tanks whenever it stands idle") {
    ArmyView view = afterFirstExtractor();

    SECTION("no factory, no tank") { CHECK_FALSE(rm::sim::wantsTank(view)); }

    SECTION("a standing, idle factory always starts the next tank") {
        view.factoriesStanding = 1;
        CHECK(rm::sim::wantsTank(view));
    }

    SECTION("but only one at a time") {
        view.factoriesStanding = 1;
        view.factoryBusy = true;
        CHECK_FALSE(rm::sim::wantsTank(view));
    }

    SECTION("and production never stops — the stream after the wave is the win condition") {
        view.factoriesStanding = 1;
        view.tanksAlive = kWaveSize + 5;
        CHECK(rm::sim::wantsTank(view));
    }
}

TEST_CASE("one attack wave, launched at strength and never re-launched") {
    ArmyView view = afterFirstExtractor();
    view.factoriesStanding = 1;
    Opponent script;

    SECTION("not before the wave is big enough to survive the commander's return fire") {
        view.tanksAlive = kWaveSize - 1;
        CHECK_FALSE(rm::sim::launchesAttack(script, view, kWaveSize));
    }

    SECTION("at strength, it launches") {
        view.tanksAlive = kWaveSize;
        CHECK(rm::sim::launchesAttack(script, view, kWaveSize));
    }

    SECTION("once launched, it stays launched — reinforcements join, waves do not reform") {
        script.attackLaunched = true;
        view.tanksAlive = kWaveSize * 2;
        CHECK_FALSE(rm::sim::launchesAttack(script, view, kWaveSize));
    }
}

TEST_CASE("the wave size is the blueprints' arithmetic, not taste") {
    // Sequential-kill model: the commander (100 dps, UEL0001 zephyr — Damage 100,
    // RateOfFire 1) kills one 300 hp tank (UEL0201) every 3 seconds while every tank
    // still alive deals its 24 dps. A wave of N therefore lands about
    // 24 * 3 * N(N+1)/2 damage before it dies, and that must clear 12000 hp.
    const float tankDps = 24.0f;
    const float secondsPerTankKilled = 300.0f / 100.0f;
    const auto damageOfWave = [&](std::size_t n) {
        const auto nf = static_cast<float>(n);
        return tankDps * secondsPerTankKilled * nf * (nf + 1.0f) / 2.0f;
    };

    // The smallest wave the model says clears the commander's 12000 hp...
    std::size_t minimal = 1;
    while (damageOfWave(minimal) < 12000.0f) {
        ++minimal;
    }

    // ...and the script's wave is that plus a stated margin of two — the model is
    // optimistic (no travel time, no dead tanks blocking the living) — but no more:
    // a bigger margin is the script sitting on tanks it should have used.
    CHECK(kWaveSize >= minimal);
    CHECK(kWaveSize == minimal + 2);
}

TEST_CASE("structures fan out from the start position, toward the map centre") {
    const std::array<rm::sim::Fx, 3> start = rm::test::at(512, 20, 512);
    const rm::sim::Fx centreX = rm::sim::Fx::fromInt(4096);
    const rm::sim::Fx centreZ = rm::sim::Fx::fromInt(4096);

    const auto power = rm::sim::structureSite(start, centreX, centreZ, 0);
    const auto factory = rm::sim::structureSite(start, centreX, centreZ, 1);

    // Distances read back as decimals, because the claims are about elmos on the ground.
    const auto apart = [](const std::array<rm::sim::Fx, 3>& a,
                          const std::array<rm::sim::Fx, 3>& b) {
        return rm::test::asFloat(rm::sim::fxHypot(a[0] - b[0], a[2] - b[2]));
    };

    SECTION("every slot is its own place, far enough apart not to overlap") {
        CHECK(apart(power, factory) >= 24.0f);
    }

    SECTION("both sit toward the centre, off the commander's own spot") {
        for (const auto& site : {power, factory}) {
            const float away = apart(site, start);
            CHECK(away >= 16.0f);   // not on the commander
            CHECK(away <= 100.0f);  // still inside the start plateau
            // Toward the centre: the offset's dot product with the centre
            // direction is positive.
            const float dx = rm::test::asFloat(site[0] - start[0]);
            const float dz = rm::test::asFloat(site[2] - start[2]);
            CHECK(dx * rm::test::asFloat(centreX - start[0])
                      + dz * rm::test::asFloat(centreZ - start[2])
                  > 0.0f);
        }
    }

    SECTION("deterministic — the same inputs place the same base") {
        const auto again = rm::sim::structureSite(start, centreX, centreZ, 0);
        CHECK(power == again);
    }
}

TEST_CASE("tanks roll off past the factory, not into it") {
    const std::array<rm::sim::Fx, 3> factory = rm::test::at(600, 20, 600);
    const rm::sim::Fx centre = rm::sim::Fx::fromInt(4096);
    const auto rally = rm::sim::rolloffPoint(factory, centre, centre);

    const float dx = rm::test::asFloat(rally[0] - factory[0]);
    const float dz = rm::test::asFloat(rally[1] - factory[2]);
    CHECK(std::sqrt(dx * dx + dz * dz) >= 20.0f);  // clear of the factory's own footprint
    CHECK(dx * rm::test::asFloat(centre - factory[0])
              + dz * rm::test::asFloat(centre - factory[2])
          > 0.0f);
}

TEST_CASE("the wave size is the unit's own arithmetic, and the tank still earns twenty") {
    // The model data/opening.lua documents, now computed per unit. The 300 hp / 24 dps
    // tank must derive to exactly the hand-derived twenty — that is the regression pin —
    // while a fragile bot earns a bigger wave and a toothless def falls back rather than
    // dividing by nothing.
    rm::unitdef::UnitDef tank;
    tank.health = rm::sim::magFromFloat(300.0f);
    rm::unitdef::Weapon gun;
    gun.label = "gun";
    gun.role = rm::unitdef::WeaponRole::DirectFire;
    gun.damage = rm::sim::magFromFloat(24.0f);
    gun.rateOfFire = 1.0f;
    gun.maxRange = rm::sim::fxFromFloat(100.0f);
    tank.weapons.push_back(gun);
    CHECK(rm::unitdef::waveSizeFor(tank) == 20);

    rm::unitdef::UnitDef bot = tank;
    bot.health = rm::sim::magFromFloat(29.0f);
    bot.weapons[0].damage = rm::sim::magFromFloat(7.0f);
    CHECK(rm::unitdef::waveSizeFor(bot) > 30);

    rm::unitdef::UnitDef unarmed;
    unarmed.health = rm::sim::magFromFloat(300.0f);
    CHECK(rm::unitdef::waveSizeFor(unarmed) == 20);
}
