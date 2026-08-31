#include "core/map/HeightField.hpp"
#include "core/sim/Army.hpp"
#include "core/sim/Combat.hpp"
#include "core/sim/Economy.hpp"
#include "core/sim/Skirmish.hpp"
#include "core/unit/UnitDef.hpp"

#include "support/TestRoster.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <set>
#include <utility>
#include <vector>

#include "support/FxMatchers.hpp"

// Properties a match must have however its units are STORED.
//
// WHY THIS FILE EXISTS. `docs/golden-p1.log` verifies that a change altered nothing, by
// comparing per-tick state hashes. It is the right tool for a refactor that preserves
// behaviour, and it is useless for one that does not — and P1.4 does not: moving from
// per-batch storage to a flat store changes which unit wins a targeting tie
// (`nearestTarget` breaks ties on the lower batch then the lower instance), which changes
// the match. The hash will disagree, correctly, and so cannot tell "reordered as expected"
// from "broke".
//
// So this is the other half of the net. Everything asserted here is true of ANY correct
// match — no assertion mentions an order, a batch, a slot, or a count of units in a
// particular place. A migration that reorders units keeps every one of them; a migration
// that loses a unit, resurrects a corpse, double-reports a death, leaks a projectile or
// lets health go negative breaks one immediately.
//
// Written before P1.4 rather than after, because a net built after the fall is a post-mortem.
// It caught the migration in the shape it was built for: the assertions below now run against
// a flat `UnitStore`, and the only edits were to how a unit is NAMED — a handle instead of a
// (batch, instance) pair. Nothing about what a correct match does had to move.

using rm::sim::Army;
using rm::sim::Health;
using rm::sim::UnitId;
using rm::test::Roster;
using rm::unitdef::UnitDef;
using rm::unitdef::Weapon;
using rm::unitdef::WeaponRole;

namespace {

[[nodiscard]] rm::HeightField flatField() {
    rm::HeightField field;
    field.squaresX = 200;
    field.squaresZ = 200;
    field.baseHeight = 0.0f;
    field.heightScale = 1.0f;
    field.raw.assign(field.sampleCount(), std::uint16_t{0});
    return field;
}

[[nodiscard]] Weapon turretedGun(float damage, float rangeElmos) {
    Weapon weapon;
    weapon.label = "test gun";
    weapon.role = WeaponRole::DirectFire;
    weapon.turreted = true;
    weapon.damage = rm::test::mag(damage);
    weapon.maxRange = rm::test::fx(rangeElmos);
    weapon.rateOfFire = 1.0f;
    weapon.muzzleVelocityElmosPerSecond = 200.0f;
    return weapon;
}

/// A fight that actually resolves: two armies of gunners inside each other's range.
struct Fight {
    Roster roster;
    std::vector<Army> armies = rm::sim::freeForAll(2);
    std::vector<rm::sim::Projectile> projectiles;
    std::vector<rm::sim::Construction> building;
    std::vector<rm::sim::Economy> economies;
    std::vector<int> commandersEver;

    Fight() {
        UnitDef redDef;
        redDef.name = "red_gun";
        redDef.weapons.push_back(turretedGun(40.0f, 300.0f));
        UnitDef blueDef;
        blueDef.name = "blue_gun";
        blueDef.weapons.push_back(turretedGun(40.0f, 300.0f));

        const rm::UnitTypeIndex red = roster.addType(redDef);
        const rm::UnitTypeIndex blue = roster.addType(blueDef);

        // INTERLEAVED on purpose. Two batches put all five reds before all five blues; one
        // flat store spawns them alternately, so slot order is neither side's order. Any
        // assertion below that quietly depended on the old grouping would have broken here.
        for (int i = 0; i < 5; ++i) {
            (void)roster.add(red, 0.0f, static_cast<float>(i) * 30.0f, 0, 300.0f);
            (void)roster.add(blue, 200.0f, static_cast<float>(i) * 30.0f, 1, 300.0f);
        }

        economies.assign(2, rm::sim::Economy{});
        commandersEver.assign(2, 0);  // no commanders: the win condition sits out
    }

    /// One tick, which is now one call: there is nothing to rebuild between ticks.
    rm::sim::TickReport tick(const rm::HeightField& field) {
        rm::sim::Match m = match();
        return rm::sim::tickSkirmish(roster.store, roster.catalog, m, rm::sim::Terrain{field});
    }

    [[nodiscard]] rm::sim::Match match() {
        return rm::sim::Match{
            .armies = armies,
            .economies = economies,
            .projectiles = &projectiles,
            .building = &building,
            .commandersEver = commandersEver,
        };
    }
};

/// Every unit's health, summed — a total stated without naming where any unit lives.
[[nodiscard]] float totalHealth(const rm::sim::UnitStore& store) {
    rm::sim::Mag total{};
    for (const Health& h : store.health()) {
        total += h.current;
    }
    return rm::test::asFloat(total);
}

[[nodiscard]] std::size_t livingUnits(const rm::sim::UnitStore& store) {
    std::size_t alive = 0;
    for (const Health& h : store.health()) {
        alive += h.alive() ? 1u : 0u;
    }
    return alive;
}

} // namespace

TEST_CASE("health stays within its bounds for every unit, every tick") {
    // The cheapest invariant and the one most likely to catch an indexing mistake: if a
    // migration crosses two units' health with each other's maximum, or applies damage to
    // the wrong slot, a value leaves its range.
    const rm::HeightField field = flatField();
    Fight fight;

    for (int tick = 0; tick < 200; ++tick) {
        (void)fight.tick(field);

        for (const Health& h : fight.roster.store.health()) {
            REQUIRE(rm::test::asFloat(h.current) >= 0.0f);
            REQUIRE(h.current <= h.maximum);
        }
    }
}

TEST_CASE("the dead stay dead") {
    // A resurrected corpse is the signature failure of a storage migration: a slot reused
    // or an index crossed puts health back into a unit that had none.
    const rm::HeightField field = flatField();
    Fight fight;
    std::set<rm::UnitIndex> everDead;

    for (int tick = 0; tick < 300; ++tick) {
        (void)fight.tick(field);

        const rm::sim::UnitStore& store = fight.roster.store;
        for (rm::UnitIndex slot = 0; slot < store.slotCount(); ++slot) {
            if (!store.health()[slot].alive()) {
                everDead.insert(slot);
                // The STORE must agree, which the health value alone could not say: a
                // slot whose health hit zero and whose handle is still live would be a
                // unit the passes keep working on.
                REQUIRE_FALSE(store.slotAlive(slot));
            } else {
                REQUIRE_FALSE(everDead.contains(slot));
            }
        }
    }
    REQUIRE_FALSE(everDead.empty());  // the fight actually killed somebody
}

TEST_CASE("a death is reported exactly once, ever") {
    // The caller leaves a permanent scorch mark per entry, so a corpse reported twice
    // scorches the same ground twice — and a corpse reported never leaves none at all.
    const rm::HeightField field = flatField();
    Fight fight;
    // Keyed by the whole HANDLE, not by the slot. Two reports of one corpse carry the same
    // handle, so the set catches them; two different units that happen to share a recycled
    // slot carry different generations, so the set does not accuse them of it. A slot-keyed
    // set would be wrong in a match long enough to reuse one.
    std::set<std::pair<rm::UnitIndex, rm::Generation>> reported;

    for (int tick = 0; tick < 300; ++tick) {
        const rm::sim::TickReport report = fight.tick(field);

        for (const rm::sim::Death& death : report.died) {
            const auto key = std::pair{death.ref.index, death.ref.generation};
            REQUIRE(reported.insert(key).second);  // never seen before
        }
    }

    REQUIRE(reported.size() == 10 - livingUnits(fight.roster.store));
}

TEST_CASE("a reported death carries the radius it had while alive") {
    // The wreck is sized from it, and retiring a unit is what zeroes it — so a report that
    // sampled after retirement would leave a scorch mark of size zero. Stated as an
    // invariant because it is easy to reintroduce by moving one line.
    const rm::HeightField field = flatField();
    Fight fight;
    bool sawADeath = false;

    for (int tick = 0; tick < 300; ++tick) {
        const rm::sim::TickReport report = fight.tick(field);
        for (const rm::sim::Death& death : report.died) {
            sawADeath = true;
            REQUIRE(rm::test::asFloat(death.radiusElmos) > 0.0f);
        }
    }
    REQUIRE(sawADeath);
}

TEST_CASE("a corpse stops shoving the living") {
    // `retireDead` zeroes the radius so the dead stop participating in collisions. If a
    // migration retires the wrong slot, a living unit is un-shoved and a dead one is not.
    const rm::HeightField field = flatField();
    Fight fight;

    for (int tick = 0; tick < 300; ++tick) {
        (void)fight.tick(field);

        const rm::sim::UnitStore& store = fight.roster.store;
        for (rm::UnitIndex slot = 0; slot < store.slotCount(); ++slot) {
            if (!store.health()[slot].alive()) {
                REQUIRE(store.motion()[slot].radiusElmos == rm::sim::Fx{});
            }
        }
    }
}

TEST_CASE("total health only ever falls") {
    // No unit heals in this engine yet, so the sum is monotonic. A migration that read a
    // stale copy of health and wrote it back would show up here as a rise.
    const rm::HeightField field = flatField();
    Fight fight;
    float previous = totalHealth(fight.roster.store);

    for (int tick = 0; tick < 200; ++tick) {
        (void)fight.tick(field);

        const float now = totalHealth(fight.roster.store);
        REQUIRE(now <= previous);
        previous = now;
    }
    REQUIRE(previous < 5 * 300.0f * 2);  // damage actually happened
}

TEST_CASE("projectiles do not leak") {
    // Every shot either lands or expires. A list that only grows is a frame-rate bug with a
    // long fuse, and the lifetime cap exists precisely because nothing despawns on leaving
    // the map.
    const rm::HeightField field = flatField();
    Fight fight;
    std::size_t highWater = 0;

    for (int tick = 0; tick < 400; ++tick) {
        (void)fight.tick(field);
        highWater = std::max(highWater, fight.projectiles.size());
    }

    // Ten gunners firing once a second cannot have hundreds in flight at 10 Hz unless
    // something is failing to retire them.
    REQUIRE(highWater < 100);

    // And once the fight is over, the sky clears.
    for (int tick = 0; tick < 400; ++tick) {
        (void)fight.tick(field);
    }
    REQUIRE(fight.projectiles.empty());
}

TEST_CASE("shots fired are only ever attributed to a live shooter") {
    // A dead unit that keeps firing is what you get when retirement and the firing pass
    // disagree about which slot is which.
    const rm::HeightField field = flatField();
    Fight fight;

    std::size_t totalShots = 0;
    for (int tick = 0; tick < 300; ++tick) {
        const rm::sim::TickReport report = fight.tick(field);
        totalShots += report.shotsFired;

        if (livingUnits(fight.roster.store) == 0) {
            // Nobody left: no further shot may be attributed to anyone.
            REQUIRE(fight.tick(field).shotsFired == 0);
            break;
        }
    }
    REQUIRE(totalShots > 0);
}

TEST_CASE("the match is decided at most once") {
    // `matchEnded` is true on the ONE tick that decided it, and `Match::over` is what keeps
    // it that way. A migration that rebuilt the match state each tick would re-announce.
    const rm::HeightField field = flatField();
    Fight fight;
    fight.commandersEver.assign(2, 1);  // give the win condition something to decide

    int endings = 0;
    for (int tick = 0; tick < 400; ++tick) {
        rm::sim::Match match = fight.match();
        const rm::sim::TickReport report =
            rm::sim::tickSkirmish(fight.roster.store, fight.roster.catalog, match, rm::sim::Terrain{field});
        if (report.matchEnded) {
            ++endings;
        }
        if (match.over) {
            break;
        }
    }
    REQUIRE(endings <= 1);
}

TEST_CASE("a winner must remain stable for fifteen seconds before the match ends") {
    // C-210: retail polls defeat every three seconds and requires the same winner for fifteen.
    // Keep Match alive across those ticks: the confirmation belongs to the match, not a caller's
    // temporary TickReport.
    const rm::HeightField field = flatField();
    Fight fight;
    fight.armies[1].defeated = true;
    rm::sim::Match match = fight.match();
    const rm::sim::TickRate rate{};
    const rm::TickCount confirmationTicks = rate.ticks(rm::sim::seconds(15.0f));

    for (rm::TickCount tick = 0; tick < confirmationTicks - 1; ++tick) {
        (void)rm::sim::tickSkirmish(fight.roster.store, fight.roster.catalog, match,
                                    rm::sim::Terrain{field}, rate);
    }
    REQUIRE_FALSE(match.over);

    const rm::sim::TickReport report =
        rm::sim::tickSkirmish(fight.roster.store, fight.roster.catalog, match,
                              rm::sim::Terrain{field}, rate);
    REQUIRE(match.over);
    REQUIRE(report.matchEnded);
    REQUIRE(report.winner == 0);
}

TEST_CASE("commander defeat is polled, then clears its army except walls") {
    // C-210: retail checks commanders every three seconds. OnDefeat then schedules an
    // ALLUNITS cleanup twenty seconds later, but WALL units are deliberately left standing.
    const rm::HeightField field = flatField();
    Roster roster;

    UnitDef commanderDef;
    commanderDef.name = "UEL0001";
    UnitDef unitDef;
    unitDef.name = "test_unit";
    UnitDef wallDef;
    wallDef.name = "test_wall";
    wallDef.categories = {"STRUCTURE", "WALL"};
    const rm::UnitTypeIndex commander = roster.addType(commanderDef);
    const rm::UnitTypeIndex unit = roster.addType(unitDef);
    const rm::UnitTypeIndex wall = roster.addType(wallDef);

    const rm::sim::UnitId defeatedCommander = roster.add(commander, 0.0f, 0.0f, 1, 100.0f);
    const rm::sim::UnitId defeatedUnit = roster.add(unit, 20.0f, 0.0f, 1, 100.0f);
    const rm::sim::UnitId defeatedWall = roster.add(wall, 40.0f, 0.0f, 1, 100.0f);
    (void)roster.add(commander, 100.0f, 0.0f, 0, 100.0f);

    std::vector<Army> armies = rm::sim::freeForAll(2);
    std::vector<rm::sim::Economy> economies(2);
    std::vector<rm::sim::Projectile> projectiles;
    const std::vector<int> commandersEver{1, 1};
    rm::sim::Match match{.armies = armies,
                         .economies = economies,
                         .projectiles = &projectiles,
                         .commandersEver = commandersEver};
    const rm::sim::TickRate rate{};

    roster.health(defeatedCommander).current = rm::sim::Mag{};
    const rm::TickCount pollTicks = rate.ticks(rm::sim::seconds(3.0f));
    for (rm::TickCount tick = 0; tick < pollTicks - 1; ++tick) {
        (void)rm::sim::tickSkirmish(roster.store, roster.catalog, match, rm::sim::Terrain{field}, rate);
    }
    REQUIRE_FALSE(armies[1].defeated);

    (void)rm::sim::tickSkirmish(roster.store, roster.catalog, match, rm::sim::Terrain{field}, rate);
    REQUIRE(armies[1].defeated);
    REQUIRE(roster.health(defeatedUnit).alive());
    REQUIRE(roster.health(defeatedWall).alive());

    const rm::TickCount cleanupTicks = rate.ticks(rm::sim::seconds(20.0f));
    for (rm::TickCount tick = 0; tick < cleanupTicks - 1; ++tick) {
        (void)rm::sim::tickSkirmish(roster.store, roster.catalog, match, rm::sim::Terrain{field}, rate);
    }
    REQUIRE(roster.health(defeatedUnit).alive());

    (void)rm::sim::tickSkirmish(roster.store, roster.catalog, match, rm::sim::Terrain{field}, rate);
    REQUIRE_FALSE(roster.health(defeatedUnit).alive());
    REQUIRE(roster.health(defeatedWall).alive());

    // Cleanup only sets health to zero; the ordinary retirement pass observes it next tick.
    REQUIRE(roster.store.slotAlive(defeatedUnit.index));
    (void)rm::sim::tickSkirmish(roster.store, roster.catalog, match, rm::sim::Terrain{field}, rate);
    REQUIRE_FALSE(roster.store.slotAlive(defeatedUnit.index));
}

TEST_CASE("a defeated army stays defeated") {
    const rm::HeightField field = flatField();
    Fight fight;
    fight.commandersEver.assign(2, 1);

    std::vector<bool> everDefeated(2, false);
    for (int tick = 0; tick < 400; ++tick) {
        (void)fight.tick(field);

        for (const Army& army : fight.armies) {
            const auto i = static_cast<std::size_t>(army.index);
            if (army.defeated) {
                everDefeated[i] = true;
            }
            REQUIRE_FALSE((everDefeated[i] && !army.defeated));
        }
    }
}

TEST_CASE("an economy never banks more than it can store, nor funds more than it has") {
    const rm::HeightField field = flatField();
    Fight fight;
    const rm::sim::TickRate rate{};
    const rm::sim::Resources cap{.mass = rm::test::mag(650.0f),
                                 .energy = rm::test::mag(5000.0f)};
    for (rm::sim::Economy& economy : fight.economies) {
        economy.storage = cap;
        economy.incomePerTick = rm::sim::Resources{.mass = rate.magPerTick(5.0f),
                                                   .energy = rate.magPerTick(20.0f)};
    }

    for (int tick = 0; tick < 300; ++tick) {
        rm::sim::Match match = fight.match();
        match.baseStorage = cap;
        (void)rm::sim::tickSkirmish(fight.roster.store, fight.roster.catalog, match, rm::sim::Terrain{field});

        for (const rm::sim::Economy& economy : fight.economies) {
            REQUIRE(economy.stored.mass >= rm::sim::Mag{});
            REQUIRE(economy.stored.energy >= rm::sim::Mag{});
            // No slack needed any more, and that is the point: fixed point cannot leave a
            // store "a hair above" its cap the way float could, so the invariant is now an
            // exact bound rather than one with a tolerance bolted on.
            REQUIRE(economy.stored.mass <= economy.storage.mass);
            REQUIRE(economy.stored.energy <= economy.storage.energy);
            REQUIRE(economy.fundedFraction >= rm::sim::Fx{});
            REQUIRE(economy.fundedFraction <= rm::sim::kFxOne);
        }
    }
}

TEST_CASE("a match with nobody in it does nothing, rather than deciding something") {
    // The empty case, which bit us once already: with no armies, survivorCount is zero,
    // which is also <= 1, and a naive win condition declares a draw on tick one.
    const rm::HeightField field = flatField();

    std::vector<Army> none;
    std::vector<rm::sim::Economy> noEconomies;
    std::vector<int> noCommanders;
    std::vector<rm::sim::Projectile> noShots;
    rm::sim::UnitStore noUnits;
    const rm::sim::UnitCatalog noTypes;

    rm::sim::Match match{
        .armies = none,
        .economies = noEconomies,
        .projectiles = &noShots,
        .building = nullptr,
        .commandersEver = noCommanders,
    };

    const rm::sim::TickReport report =
        rm::sim::tickSkirmish(noUnits, noTypes, match, rm::sim::Terrain{field});
    REQUIRE(report.died.empty());
    REQUIRE(report.shotsFired == 0);
    REQUIRE(report.finished.empty());
}
