// What the sim says happened.
//
// THE STATED TEST IS ONE LINE OF §7 P6.1 — "a kill emits exactly one `UnitDestroyed` with the
// right instigator" — and both halves are load-bearing. EXACTLY ONE, because a corpse sits in
// its slot for the rest of the match and a naive emit would report it every tick forever. THE
// RIGHT INSTIGATOR, because a death is noticed a pass later than the damage that caused it, so
// the killer has to be recorded on the victim at the moment the blow lands or it is gone.
#include <catch2/catch_test_macros.hpp>

#include "core/sim/Events.hpp"
#include "core/sim/Skirmish.hpp"

#include "support/FxMatchers.hpp"
#include "support/TestRoster.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

using rm::sim::Event;
using rm::sim::EventKind;
using rm::sim::EventQueue;
using rm::sim::UnitId;

namespace {

[[nodiscard]] rm::HeightField flatField() {
    rm::HeightField field;
    field.squaresX = 128;
    field.squaresZ = 128;
    field.baseHeight = 0.0f;
    field.heightScale = 1.0f;
    field.raw.assign(field.sampleCount(), std::uint16_t{0});
    return field;
}

[[nodiscard]] rm::unitdef::UnitDef gunnerDef() {
    rm::unitdef::UnitDef def;
    def.name = "test_gunner";

    rm::unitdef::Weapon weapon;
    weapon.label = "test gun";
    weapon.role = rm::unitdef::WeaponRole::DirectFire;
    weapon.damage = rm::sim::magFromFloat(1000.0f);  // one shot kills
    weapon.maxRange = rm::sim::fxFromFloat(300.0f);
    weapon.muzzleVelocityElmosPerSecond = 400.0f;
    weapon.rateOfFire = 1.0f;
    weapon.turreted = true;  // no aiming to wait for
    def.weapons.push_back(weapon);
    return def;
}

[[nodiscard]] rm::unitdef::UnitDef targetDef() {
    rm::unitdef::UnitDef def;
    def.name = "test_target";
    return def;
}

/// A shooter and a victim, one army each, with a queue listening.
struct Duel {
    rm::HeightField field = flatField();
    rm::sim::Terrain terrain{field};
    rm::test::Roster roster;
    std::vector<rm::sim::Army> armies = rm::sim::freeForAll(2);
    std::vector<rm::sim::Projectile> shots;
    std::vector<rm::sim::Economy> economies{2};
    std::vector<int> commandersEver{0, 0};
    EventQueue events;

    UnitId shooter;
    UnitId victim;

    Duel() {
        shooter = roster.add(roster.addType(gunnerDef()), 0.0f, 0.0f, 0, 500.0f);
        victim = roster.add(roster.addType(targetDef()), 0.0f, 100.0f, 1, 100.0f);
    }

    rm::sim::TickReport step() {
        rm::sim::Match match{.armies = armies,
                             .economies = economies,
                             .projectiles = &shots,
                             .events = &events,
                             .commandersEver = commandersEver};
        return rm::sim::tickSkirmish(roster.store, roster.catalog, match, terrain,
                                     roster.rate);
    }
};

} // namespace

TEST_CASE("a kill emits exactly one UnitDestroyed, naming the killer") {
    // §7 P6.1's stated test.
    Duel duel;

    // Run until the victim dies, accumulating events across ticks — the caller owns the
    // queue's lifetime, so nothing clears it here.
    bool died = false;
    for (int tick = 0; tick < 60 && !died; ++tick) {
        const rm::sim::TickReport report = duel.step();
        died = !report.died.empty();
    }
    REQUIRE(died);

    // EXACTLY ONE, however many more ticks run. The corpse stays in its slot, so a report
    // that keyed off "health is zero" would fire again every tick.
    for (int tick = 0; tick < 20; ++tick) {
        (void)duel.step();
    }
    CHECK(duel.events.count(EventKind::UnitDestroyed) == 1);

    // And it names the shooter, not merely the shooter's army.
    const Event* death = nullptr;
    for (const Event& event : duel.events.all()) {
        if (event.kind == EventKind::UnitDestroyed) {
            death = &event;
        }
    }
    REQUIRE(death != nullptr);
    CHECK(death->unit == duel.victim);
    CHECK(death->instigator == duel.shooter);
    CHECK(death->army == 1);  // the victim's army, not the killer's
}

TEST_CASE("damage is reported before the death it causes") {
    // The ordering is part of the contract — events come out in tick-pass order — and it is
    // what lets a consumer accumulate damage and then attribute a kill without looking ahead.
    Duel duel;
    for (int tick = 0; tick < 60; ++tick) {
        if (!duel.step().died.empty()) {
            break;
        }
    }

    std::size_t firstDamage = duel.events.size();
    std::size_t firstDeath = duel.events.size();
    for (std::size_t i = 0; i < duel.events.all().size(); ++i) {
        const EventKind kind = duel.events.all()[i].kind;
        if (kind == EventKind::UnitDamaged && firstDamage == duel.events.size()) {
            firstDamage = i;
        }
        if (kind == EventKind::UnitDestroyed && firstDeath == duel.events.size()) {
            firstDeath = i;
        }
    }
    REQUIRE(firstDamage < duel.events.size());
    REQUIRE(firstDeath < duel.events.size());
    CHECK(firstDamage < firstDeath);
}

TEST_CASE("a shot fired and a shot landing are separate events") {
    // `WeaponFired` is the trigger-pull and `ProjectileImpact` is the arrival, and the gap
    // between them is flight time. Collapsing them would make a sound layer play the hit
    // before the shell had gone anywhere.
    Duel duel;
    for (int tick = 0; tick < 60; ++tick) {
        if (!duel.step().died.empty()) {
            break;
        }
    }

    CHECK(duel.events.count(EventKind::WeaponFired) >= 1);
    CHECK(duel.events.count(EventKind::ProjectileImpact) >= 1);

    // The first shot is fired strictly before the first thing lands.
    std::size_t fired = duel.events.size();
    std::size_t landed = duel.events.size();
    for (std::size_t i = 0; i < duel.events.all().size(); ++i) {
        const EventKind kind = duel.events.all()[i].kind;
        if (kind == EventKind::WeaponFired && fired == duel.events.size()) {
            fired = i;
        }
        if (kind == EventKind::ProjectileImpact && landed == duel.events.size()) {
            landed = i;
        }
    }
    CHECK(fired < landed);
}

TEST_CASE("damage reports what was dealt, not what was thrown") {
    // A 1000-damage shot into 100 health reports 100. The distinction matters to anything
    // summing damage — overkill would otherwise inflate every total — and it is the same
    // figure `damageArea` returns.
    Duel duel;
    for (int tick = 0; tick < 60; ++tick) {
        if (!duel.step().died.empty()) {
            break;
        }
    }

    rm::sim::Mag total{};
    for (const Event& event : duel.events.all()) {
        if (event.kind == EventKind::UnitDamaged) {
            total += event.amount;
        }
    }
    CHECK(rm::test::asFloat(total) == 100.0f);
}

TEST_CASE("losing the last commander is a defeat and then a game over") {
    // Both events, in that order, and the game-over names the winning alliance.
    rm::HeightField field = flatField();
    const rm::sim::Terrain terrain{field};
    rm::test::Roster roster;

    rm::unitdef::UnitDef acu;
    acu.name = "UEL0001";  // `isCommanderId` reads the name
    const rm::UnitTypeIndex type = roster.addType(acu);
    const UnitId theirs = roster.add(type, 0.0f, 100.0f, 1, 100.0f);
    (void)roster.add(type, 0.0f, 0.0f, 0, 100.0f);

    std::vector<rm::sim::Army> armies = rm::sim::freeForAll(2);
    std::vector<rm::sim::Projectile> shots;
    std::vector<rm::sim::Economy> economies(2);
    const std::vector<int> commandersEver{1, 1};
    EventQueue events;

    // Killed outright rather than shot, so this case is about the defeat chain and not about
    // ballistics.
    roster.health(theirs).current = rm::sim::Mag{};

    rm::sim::Match match{.armies = armies,
                         .economies = economies,
                         .projectiles = &shots,
                         .events = &events,
                         .commandersEver = commandersEver};
    (void)rm::sim::tickSkirmish(roster.store, roster.catalog, match, terrain, roster.rate);

    CHECK(events.count(EventKind::TeamDefeated) == 1);
    CHECK(events.count(EventKind::GameOver) == 1);

    std::size_t defeated = events.size();
    std::size_t over = events.size();
    for (std::size_t i = 0; i < events.all().size(); ++i) {
        if (events.all()[i].kind == EventKind::TeamDefeated) {
            defeated = i;
            CHECK(events.all()[i].army == 1);
        }
        if (events.all()[i].kind == EventKind::GameOver) {
            over = i;
            CHECK(events.all()[i].army == 0);  // the surviving alliance
        }
    }
    CHECK(defeated < over);

    // And neither repeats on the next tick: `match.over` latches, and a defeated army is
    // already flagged.
    events.clear();
    (void)rm::sim::tickSkirmish(roster.store, roster.catalog, match, terrain, roster.rate);
    CHECK(events.count(EventKind::TeamDefeated) == 0);
    CHECK(events.count(EventKind::GameOver) == 0);
}

TEST_CASE("a scene with no listener runs the same tick") {
    // The null queue is the ordinary case — a `--units` crowd, a pathfinding harness, most
    // tests — so it must be a branch and not a special path.
    Duel duel;
    rm::sim::Match silent{.armies = duel.armies,
                          .economies = duel.economies,
                          .projectiles = &duel.shots,
                          .events = nullptr,
                          .commandersEver = duel.commandersEver};
    CHECK_NOTHROW(
        (void)rm::sim::tickSkirmish(duel.roster.store, duel.roster.catalog, silent,
                                    duel.terrain, duel.roster.rate));
}

TEST_CASE("every kind has a name, and the names are distinct") {
    // The names are what `--print-events` shows and what a log would carry, so a duplicate or
    // a missing one is a silent loss of information rather than a compile error.
    const std::array kinds{EventKind::UnitCreated,         EventKind::UnitFinished,
                           EventKind::UnitDamaged,         EventKind::UnitDestroyed,
                           EventKind::WeaponFired,         EventKind::ProjectileImpact,
                           EventKind::ConstructionStarted, EventKind::ConstructionFinished,
                           EventKind::TeamDefeated,        EventKind::GameOver};

    std::vector<std::string_view> names;
    for (const EventKind kind : kinds) {
        const std::string_view name = rm::sim::eventKindName(kind);
        CHECK(name != "unknown");
        names.push_back(name);
    }
    std::sort(names.begin(), names.end());
    CHECK(std::adjacent_find(names.begin(), names.end()) == names.end());
    CHECK(names.size() == 10);
}
