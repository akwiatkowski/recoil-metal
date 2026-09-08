// One tick of a match, end to end.
//
// This file exists because of a bug it could not have caught before: every piece of the
// match — movement, collisions, aiming, firing, projectiles, death, defeat, economy —
// was tested on its own and passing, while the ASSEMBLY of them lived in an anonymous
// namespace inside main.mm and could not be linked from here at all. The windowed loop
// ran movement and collisions and nothing else, so a unit in the interactive game never
// fired a shot, and 511 green tests had nothing to say about it.
//
// So what is tested here is deliberately not any single rule. It is that ONE call
// advances the whole match, because that is the property the two callers — the headless
// pre-run and the frame loop — depend on to stay in step.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "core/sim/Skirmish.hpp"

#include "support/TestRoster.hpp"

#include <cstdint>
#include <vector>

#include "support/FxMatchers.hpp"

using Catch::Approx;
using rm::sim::Army;
using rm::sim::Construction;
using rm::sim::Economy;
using rm::sim::Health;
using rm::sim::Match;
using rm::sim::Projectile;
using rm::test::Roster;
using rm::sim::TickReport;
using rm::unitdef::UnitDef;
using rm::unitdef::Weapon;
using rm::unitdef::WeaponRole;

namespace {

/// A flat field: a test about the match should not also be a test about terrain.
[[nodiscard]] rm::HeightField flatField() {
    rm::HeightField field;
    field.squaresX = 100;
    field.squaresZ = 100;
    field.baseHeight = 0.0f;
    field.heightScale = 1.0f;
    field.raw.assign(field.sampleCount(), std::uint16_t{0});
    return field;
}

/// A turreted gun, so a test about the match is not also a test about facing —
/// `canFireAt` lets a turreted weapon fire whatever the hull is doing.
[[nodiscard]] Weapon turretedGun(float damage, float rangeElmos) {
    Weapon weapon;
    weapon.label = "test gun";
    weapon.role = WeaponRole::DirectFire;
    weapon.targetPriorities = {{"LAND"}};
    weapon.turreted = true;
    weapon.damage = rm::test::mag(damage);
    weapon.maxRange = rm::test::fx(rangeElmos);
    weapon.rateOfFire = 1.0f;                      // one shot a second
    weapon.muzzleVelocityElmosPerSecond = 200.0f;  // crosses the range in a tick or two
    return weapon;
}

/// Two armies at war with each other, free-for-all.
[[nodiscard]] std::vector<Army> twoSides() { return rm::sim::freeForAll(2); }

} // namespace

TEST_CASE("one tick both moves a unit and fires its gun") {
    // THE REGRESSION. The frame loop used to call the movement tick directly and nothing
    // else, so a unit crossed the map perfectly and never shot. Whatever else changes,
    // one call to the match tick has to do both.
    const rm::HeightField field = flatField();

    Roster roster;
    UnitDef tankDef;
    tankDef.name = "test_tank";
    tankDef.weapons.push_back(turretedGun(100.0f, 400.0f));
    UnitDef targetDef;
    targetDef.name = "test_target";
    targetDef.categories = {"LAND"};

    const rm::sim::UnitId tank = roster.add(roster.addType(tankDef), 0.0f, 0.0f, 0, 500.0f);
    (void)roster.add(roster.addType(targetDef), 200.0f, 0.0f, 1, 500.0f);

    // Ordered somewhere, so the movement half has something to do.
    rm::sim::orderTo(roster.motion(tank), rm::sim::Terrain{field}, rm::test::fx(100.0f),
                     rm::test::fx(0.0f));
    const std::array<rm::sim::Fx, 3> started = rm::sim::positionOf(roster.transform(tank));

    std::vector<Army> armies = twoSides();
    std::vector<Projectile> projectiles;
    std::vector<Construction> building;
    std::vector<Economy> economies(2);
    const std::vector<int> commandersEver(2, 0);

    Match match{.armies = armies,
                .economies = economies,
                .projectiles = &projectiles,
                .building = &building,
                .commandersEver = commandersEver};

    const TickReport report = rm::sim::tickSkirmish(roster.store, roster.catalog, match, rm::sim::Terrain{field});

    CHECK(rm::sim::positionOf(roster.transform(tank)) != started);  // it moved
    CHECK(report.shotsFired == 1);                     // and it shot
    CHECK(projectiles.size() == 1);
}

TEST_CASE("a unit with no enemy in range moves without firing") {
    // The other half of the same property: the tick must not invent a shot. An
    // always-firing tick would make the test above pass for the wrong reason.
    const rm::HeightField field = flatField();

    Roster roster;
    UnitDef tankDef;
    tankDef.name = "test_tank";
    tankDef.weapons.push_back(turretedGun(100.0f, 50.0f));  // short gun
    UnitDef targetDef;
    targetDef.name = "test_target";
    targetDef.categories = {"LAND"};

    const rm::sim::UnitId tank = roster.add(roster.addType(tankDef), 0.0f, 0.0f, 0, 500.0f);
    (void)roster.add(roster.addType(targetDef), 900.0f, 0.0f, 1,
                     500.0f);  // far out of range

    rm::sim::orderTo(roster.motion(tank), rm::sim::Terrain{field}, rm::test::fx(100.0f),
                     rm::test::fx(0.0f));

    std::vector<Army> armies = twoSides();
    std::vector<Projectile> projectiles;
    std::vector<Construction> building;
    std::vector<Economy> economies(2);
    const std::vector<int> commandersEver(2, 0);

    Match match{.armies = armies,
                .economies = economies,
                .projectiles = &projectiles,
                .building = &building,
                .commandersEver = commandersEver};

    const TickReport report = rm::sim::tickSkirmish(roster.store, roster.catalog, match, rm::sim::Terrain{field});

    CHECK(report.shotsFired == 0);
    CHECK(projectiles.empty());
}

TEST_CASE("the tick reports its dead, and retires them from the fight") {
    // Death is reported rather than acted on here, because what a death MEANS is split:
    // the sim stops the unit moving and colliding, and the caller leaves a scorch mark,
    // which needs a decal buffer the sim has no business owning.
    const rm::HeightField field = flatField();

    Roster roster;
    UnitDef tankDef;
    tankDef.name = "test_tank";
    UnitDef targetDef;
    targetDef.name = "test_target";

    (void)roster.add(roster.addType(tankDef), 0.0f, 0.0f, 0, 500.0f);
    const rm::sim::UnitId victim =
        roster.add(roster.addType(targetDef), 50.0f, 0.0f, 1, 10.0f);
    roster.health(victim).current = rm::test::mag(0.0f);  // already destroyed, this tick retires it

    std::vector<Army> armies = twoSides();
    std::vector<Projectile> projectiles;
    std::vector<Construction> building;
    std::vector<Economy> economies(2);
    const std::vector<int> commandersEver(2, 0);

    Match match{.armies = armies,
                .economies = economies,
                .projectiles = &projectiles,
                .building = &building,
                .commandersEver = commandersEver};

    const TickReport report = rm::sim::tickSkirmish(roster.store, roster.catalog, match, rm::sim::Terrain{field});

    REQUIRE(report.died.size() == 1);
    // The HANDLE of the unit that died, which is a stronger claim than the pair it replaced:
    // a (batch, instance) pair named a place, and a place can be refilled.
    CHECK(report.died[0].ref == victim);

    // The wreck's size and place are carried in the report, sampled BEFORE the unit was
    // retired — going back to the slot for them would find a radius of zero, because
    // retiring is what zeroes it.
    CHECK(rm::test::asFloat(report.died[0].radiusElmos) == Approx(4.0f));
    CHECK(rm::test::asFloat(report.died[0].at[0]) == Approx(50.0f));

    // Retired: it no longer shoves the living, it no longer drives anywhere, and the store
    // agrees it is gone — which the old pair could not express at all.
    CHECK(rm::test::asFloat(roster.motion(victim).radiusElmos) == 0.0f);
    CHECK(roster.motion(victim).moving == false);
    CHECK_FALSE(roster.store.alive(victim));
    CHECK(roster.store.slotCount() == 2);  // the slot stays: death is a tombstone

    // Reported ONCE. A corpse sits in its slot for the rest of the match, and a tick
    // that kept reporting it would fire its death explosion every tick forever.
    const TickReport again = rm::sim::tickSkirmish(roster.store, roster.catalog, match, rm::sim::Terrain{field});
    CHECK(again.died.empty());
}

TEST_CASE("losing the last commander defeats an army and starts winner confirmation") {
    const rm::HeightField field = flatField();

    Roster roster;
    // A real commander id, and spelled the way the blueprints spell it: `isCommanderId`
    // is an exact match against the four, so lowercasing it here would leave both sides
    // with no commander and declare an instant draw.
    UnitDef commanderDef;
    commanderDef.name = "UEL0001";
    const rm::UnitTypeIndex commander = roster.addType(commanderDef);
    (void)roster.add(commander, 0.0f, 0.0f, 0, 12000.0f);
    const rm::sim::UnitId theirs = roster.add(commander, 400.0f, 0.0f, 1, 12000.0f);

    std::vector<Army> armies = twoSides();
    std::vector<Projectile> projectiles;
    std::vector<Construction> building;
    std::vector<Economy> economies(2);
    const std::vector<int> commandersEver{1, 1};  // both STARTED with one

    Match match{.armies = armies,
                .economies = economies,
                .projectiles = &projectiles,
                .building = &building,
                .commandersEver = commandersEver};

    // Nothing has happened yet: both sides are whole.
    TickReport report = rm::sim::tickSkirmish(roster.store, roster.catalog, match, rm::sim::Terrain{field});
    CHECK(report.defeated == 0);
    CHECK(report.matchEnded == false);

    // Army 1's commander dies.
    roster.health(theirs).current = rm::test::mag(0.0f);
    report = rm::sim::tickSkirmish(roster.store, roster.catalog, match, rm::sim::Terrain{field});

    const rm::sim::TickRate rate{};
    const rm::TickCount pollTicks = rate.ticks(rm::sim::seconds(3.0f));
    for (rm::TickCount tick = 1; tick < pollTicks - 1; ++tick) {
        report = rm::sim::tickSkirmish(roster.store, roster.catalog, match,
                                       rm::sim::Terrain{field}, rate);
    }

    CHECK(report.defeated == 1);
    CHECK(armies[1].defeated);
    CHECK_FALSE(report.matchEnded);
    CHECK_FALSE(match.over);
    REQUIRE(match.pendingWinner.has_value());
    CHECK(*match.pendingWinner == armies[0].alliance);
}

TEST_CASE("defeating the other alliance starts team-match winner confirmation") {
    const rm::HeightField field = flatField();

    Roster roster;
    UnitDef commanderDef;
    commanderDef.name = "UEL0001";
    const rm::UnitTypeIndex commander = roster.addType(commanderDef);
    (void)roster.add(commander, 0.0f, 0.0f, 0, 12000.0f);
    (void)roster.add(commander, 100.0f, 0.0f, 1, 12000.0f);
    const rm::sim::UnitId enemyA = roster.add(commander, 400.0f, 0.0f, 2, 12000.0f);
    const rm::sim::UnitId enemyB = roster.add(commander, 500.0f, 0.0f, 3, 12000.0f);

    std::vector<Army> armies = rm::sim::freeForAll(4);
    armies[0].alliance = 0;
    armies[1].alliance = 0;
    armies[2].alliance = 1;
    armies[3].alliance = 1;
    std::vector<Projectile> projectiles;
    std::vector<Construction> building;
    std::vector<Economy> economies(4);
    const std::vector<int> commandersEver(4, 1);
    Match match{.armies = armies,
                .economies = economies,
                .projectiles = &projectiles,
                .building = &building,
                .commandersEver = commandersEver};

    roster.health(enemyA).current = rm::test::mag(0.0f);
    roster.health(enemyB).current = rm::test::mag(0.0f);
    TickReport report =
        rm::sim::tickSkirmish(roster.store, roster.catalog, match, rm::sim::Terrain{field});
    const rm::sim::TickRate rate{};
    const rm::TickCount pollTicks = rate.ticks(rm::sim::seconds(3.0f));
    for (rm::TickCount tick = 1; tick < pollTicks; ++tick) {
        report = rm::sim::tickSkirmish(roster.store, roster.catalog, match,
                                       rm::sim::Terrain{field}, rate);
    }

    CHECK(report.defeated == 2);
    CHECK(rm::sim::survivorCount(armies) == 2);
    CHECK_FALSE(report.matchEnded);
    CHECK_FALSE(match.over);
    REQUIRE(match.pendingWinner.has_value());
    CHECK(*match.pendingWinner == 0);
}

TEST_CASE("a crowd with no commanders is not a draw on the first tick") {
    // `--units` scatters a decorative crowd that never had a commander. Reading "no
    // commander alive" as "lost its commander" would declare a draw before anything
    // has happened — which is why the tick is told what each army STARTED with.
    const rm::HeightField field = flatField();

    Roster roster;
    UnitDef tankDef;
    tankDef.name = "test_tank";
    const rm::UnitTypeIndex tank = roster.addType(tankDef);
    (void)roster.add(tank, 0.0f, 0.0f, 0, 500.0f);
    (void)roster.add(tank, 400.0f, 0.0f, 1, 500.0f);

    std::vector<Army> armies = twoSides();
    std::vector<Projectile> projectiles;
    std::vector<Construction> building;
    std::vector<Economy> economies(2);
    const std::vector<int> commandersEver(2, 0);  // neither side ever had one

    Match match{.armies = armies,
                .economies = economies,
                .projectiles = &projectiles,
                .building = &building,
                .commandersEver = commandersEver};

    const TickReport report = rm::sim::tickSkirmish(roster.store, roster.catalog, match, rm::sim::Terrain{field});

    CHECK(report.defeated == 0);
    CHECK(report.matchEnded == false);
}

TEST_CASE("a death explosion goes off once, and hurts what is standing nearby") {
    // 99 of the 494 shipped weapons are a death blast. They were parsed for two
    // milestones before anything set one off, and the thing that finally does is the
    // tick — not the caller, because a blast deals damage and damage is the sim's.
    const rm::HeightField field = flatField();

    Weapon blast;
    blast.label = "test death";
    blast.role = WeaponRole::Death;
    blast.damage = rm::test::mag(400.0f);
    blast.damageRadius = rm::test::fx(100.0f);

    Roster roster;
    UnitDef bombDef;
    bombDef.name = "test_bomb";
    bombDef.weapons.push_back(blast);
    UnitDef targetDef;
    targetDef.name = "test_target";

    const rm::sim::UnitId bomb = roster.add(roster.addType(bombDef), 0.0f, 0.0f, 0, 100.0f);
    roster.health(bomb).current = rm::test::mag(0.0f);  // dies this tick

    // Half the blast radius away.
    const rm::sim::UnitId bystander =
        roster.add(roster.addType(targetDef), 50.0f, 0.0f, 1, 500.0f);

    std::vector<Army> armies = twoSides();
    std::vector<Projectile> projectiles;
    std::vector<Construction> building;
    std::vector<Economy> economies(2);
    const std::vector<int> commandersEver(2, 0);

    Match match{.armies = armies,
                .economies = economies,
                .projectiles = &projectiles,
                .building = &building,
                .commandersEver = commandersEver};

    TickReport report = rm::sim::tickSkirmish(roster.store, roster.catalog, match, rm::sim::Terrain{field});

    CHECK(report.deathBlasts == 1);
    CHECK(rm::test::asFloat(report.deathBlastDamage) > 0.0f);
    CHECK(rm::test::asFloat(roster.health(bystander).current) < 500.0f);  // it felt it

    // ONCE. The corpse sits in its slot for the rest of the match, and a blast that
    // repeated every tick would be both a wrong answer and an unbounded one.
    const rm::sim::Mag afterOne = roster.health(bystander).current;
    report = rm::sim::tickSkirmish(roster.store, roster.catalog, match, rm::sim::Terrain{field});
    CHECK(report.deathBlasts == 0);
    CHECK(rm::test::asFloat(roster.health(bystander).current) == Approx(rm::test::asFloat(afterOne)));
}

TEST_CASE("a finished construction is reported but left in the list") {
    // Reported, because only the caller can turn one into a unit — that needs a model
    // out of the VFS. Left in the list, because a finished construction is what marks
    // its ground as taken: the scripted opponent scans the list for a free mass deposit,
    // and removing the extractor it just finished would invite a second one on top.
    const rm::HeightField field = flatField();

    Roster roster;
    UnitDef tankDef;
    tankDef.name = "test_tank";
    const rm::sim::UnitId builder = roster.add(roster.addType(tankDef), 0.0f, 0.0f, 0, 500.0f);

    std::vector<Army> armies = twoSides();
    std::vector<Projectile> projectiles;
    std::vector<Economy> economies(2);
    const std::vector<int> commandersEver(2, 0);

    // One construction, nearly done, with the mass and energy banked to finish it.
    std::vector<Construction> building;
    Construction work;
    work.armyIndex = 0;
    work.position = rm::test::at(100, 0, 100);
    work.cost = {.mass = rm::test::mag(10.0f), .energy = rm::test::mag(10.0f)};
    work.buildTimeRemaining = rm::test::mag(1.0f);
    work.totalBuildTime = rm::test::mag(1.0f);
    // A hundred build units a second against one unit of work: it finishes well within a
    // tick, whatever the rate.
    work.buildPerTick = rm::sim::TickRate{}.magPerTick(100.0f);
    work.builder = builder;
    building.push_back(work);

    // AND A FOUNDER HOLDING THE ORDER, because a construction advances inside its builder's
    // own dispatch tick now (`C-112`) rather than out of the economy pass. A record with no
    // live builder standing over it is inert — which is the same rule as retail's, where the
    // helper dies with the builder and the half-built thing simply stops.
    rm::sim::Command order;
    order.kind = rm::sim::CommandKind::Build;
    order.unit = builder;
    order.targetX = rm::test::at(100, 0, 100)[0];
    order.targetZ = rm::test::at(100, 0, 100)[2];
    (void)roster.store.orders()[builder.index].give(order, /*queued=*/false);
    roster.store.orders()[builder.index].markCurrentActive();

    economies[0].stored = {.mass = rm::test::mag(1000.0f),
                           .energy = rm::test::mag(1000.0f)};

    // The cap has to come from `baseStorage`: the tick recomputes storage from it plus
    // whatever is standing, so setting `economies[0].storage` here would be overwritten
    // before a single build unit was funded.
    Match match{.armies = armies,
                .economies = economies,
                .projectiles = &projectiles,
                .building = &building,
                .commandersEver = commandersEver,
                .baseStorage = {.mass = rm::test::mag(1000.0f),
                                .energy = rm::test::mag(1000.0f)}};

    TickReport report = rm::sim::tickSkirmish(roster.store, roster.catalog, match, rm::sim::Terrain{field});

    REQUIRE(report.finished.size() == 1);
    CHECK(report.finished[0].armyIndex == 0);
    CHECK(building.size() == 1);      // still there
    CHECK(building[0].finished());
    CHECK(building[0].advancedLastTick);  // presentation can draw the completing beam

    // And reported once: a second tick must not spawn the same building again.
    report = rm::sim::tickSkirmish(roster.store, roster.catalog, match, rm::sim::Terrain{field});
    CHECK(report.finished.empty());
    CHECK_FALSE(building[0].advancedLastTick);
}

TEST_CASE("income is what is standing, and a destroyed producer stops paying") {
    // Recomputed from the living every tick rather than accumulated when a build
    // finishes, so a structure that dies takes its production with it.
    const rm::HeightField field = flatField();

    Roster roster;
    UnitDef extractorDef;
    extractorDef.name = "test_extractor";
    extractorDef.producesMassPerSecond = 2.0f;
    extractorDef.upkeepEnergyPerSecond = 2.0f;
    const rm::sim::UnitId extractor =
        roster.add(roster.addType(extractorDef), 0.0f, 0.0f, 0, 100.0f);

    std::vector<Army> armies = twoSides();
    std::vector<Projectile> projectiles;
    std::vector<Construction> building;
    std::vector<Economy> economies(2);
    const std::vector<int> commandersEver(2, 0);

    Match match{.armies = armies,
                .economies = economies,
                .projectiles = &projectiles,
                .building = &building,
                .commandersEver = commandersEver};

    (void)rm::sim::tickSkirmish(roster.store, roster.catalog, match, rm::sim::Terrain{field});

    // Asserted per SECOND — the blueprint's own unit — converted back from the per-tick
    // figure the economy now holds.
    const auto perSecond = [](rm::sim::Mag perTick) {
        return rm::test::asFloat(perTick)
               * static_cast<float>(rm::sim::TickRate{}.ticksPerSecond());
    };
    CHECK(perSecond(economies[0].incomePerTick.mass) == Approx(2.0f).margin(0.01));
    CHECK(perSecond(economies[0].upkeepPerTick.energy) == Approx(2.0f).margin(0.01));

    // Destroyed, and the income goes with it.
    roster.health(extractor).current = rm::test::mag(0.0f);
    (void)rm::sim::tickSkirmish(roster.store, roster.catalog, match, rm::sim::Terrain{field});

    CHECK(perSecond(economies[0].incomePerTick.mass) == Approx(0.0f));
    CHECK(perSecond(economies[0].upkeepPerTick.energy) == Approx(0.0f));
}

TEST_CASE("a dead silo's record is reaped before the economy pass and a recycled slot inherits nothing") {
    // SiloAmmo is keyed by a generational UnitId (`C-081`). A silo destroyed mid-production must
    // stop drawing the same tick, and the UnitStore reusing its slot for a LATER unit must not
    // resurrect the record — the reaping compares the generation, not the index.
    const rm::HeightField field = flatField();

    Roster roster;
    UnitDef siloDef;
    siloDef.name = "test_silo";
    const rm::UnitTypeIndex siloType = roster.addType(siloDef);
    const rm::sim::UnitId silo = roster.add(siloType, 0.0f, 0.0f, 0, 500.0f);
    // A second army standing around, so the match has an opponent and the victory logic stays
    // out of a test that is about the economy stage.
    (void)roster.add(siloType, 900.0f, 900.0f, 1, 500.0f);

    std::vector<Army> armies = twoSides();
    std::vector<Projectile> projectiles;
    std::vector<Construction> building;
    std::vector<Economy> economies(2);
    economies[0].stored = {.mass = rm::test::mag(10000.0f), .energy = rm::test::mag(1000000.0f)};
    economies[0].storage = economies[0].stored;
    const std::vector<int> commandersEver(2, 0);

    // ART-S013/ART-S001 values, as in test_economy.cpp: 3600 mass, 360000 energy, 2400 ticks.
    std::vector<rm::sim::SiloAmmo> siloAmmo{rm::sim::makeSiloAmmo(
        silo, 0, false, 7,
        {.mass = rm::test::mag(3600.0f), .energy = rm::test::mag(360000.0f)},
        rm::test::mag(259200.0f), rm::test::mag(108.0f))};

    Match match{.armies = armies,
                .economies = economies,
                .projectiles = &projectiles,
                .building = &building,
                .siloAmmo = &siloAmmo,
                .commandersEver = commandersEver,
                // The tick REBUILDS storage from baseStorage plus per-unit contributions
                // (C-069/C-234), so a bare def would clamp an army's bank to zero.
                .baseStorage = {.mass = rm::test::mag(10000.0f),
                                .energy = rm::test::mag(1000000.0f)}};

    (void)rm::sim::tickSkirmish(roster.store, roster.catalog, match, rm::sim::Terrain{field});
    REQUIRE(siloAmmo.size() == 1);
    CHECK(siloAmmo.front().elapsedTicks == 1);  // fully funded, so one production beat landed
    const rm::sim::Mag massAfterFirstTick = economies[0].stored.mass;

    // Kill the silo and IMMEDIATELY refill its slot before the next tick: the record's owner
    // generation is now stale while the index is live again, which is exactly the case an
    // index-only ownership check would get wrong.
    roster.store.kill(silo);
    const rm::sim::UnitId recycled = roster.add(siloType, 50.0f, 0.0f, 0, 500.0f);
    INFO("recycled slot: " << recycled.index << " was " << silo.index);
    CHECK(recycled.index == silo.index);
    CHECK(recycled.generation != silo.generation);
    REQUIRE(roster.store.alive(recycled));

    (void)rm::sim::tickSkirmish(roster.store, roster.catalog, match, rm::sim::Terrain{field});
    CHECK(siloAmmo.empty());  // neither the dead silo nor its slot's new tenant owns a record
    CHECK(economies[0].stored.mass == massAfterFirstTick);  // and nothing was charged for it
}

TEST_CASE("a dead redirector's record is reaped before projectiles fly") {
    // Same generational keying as silo ammunition (`C-081`): a destroyed Loyalist stops
    // diverting the same tick, and a recycled slot inherits no redirector.
    const rm::HeightField field = flatField();

    Roster roster;
    UnitDef loyalistDef;
    loyalistDef.name = "loyalist";
    const rm::UnitTypeIndex loyalistType = roster.addType(loyalistDef);
    const rm::sim::UnitId loyalist = roster.add(loyalistType, 0.0f, 0.0f, 0, 500.0f);
    (void)roster.add(loyalistType, 900.0f, 900.0f, 1, 500.0f);

    std::vector<Army> armies = twoSides();
    std::vector<Projectile> projectiles;
    std::vector<Construction> building;
    std::vector<Economy> economies(2);
    const std::vector<int> commandersEver(2, 0);
    std::vector<rm::sim::MissileRedirect> redirects{
        rm::sim::MissileRedirect{.owner = loyalist,
                                 .radiusElmos = rm::sim::fxFromFloat(120.0f),
                                 .cooldownTicks = 10,
                                 .remaining = 0}};

    Match match{.armies = armies,
                .economies = economies,
                .projectiles = &projectiles,
                .building = &building,
                .commandersEver = commandersEver,
                .redirects = &redirects,
                .baseStorage = {.mass = rm::test::mag(10000.0f),
                                .energy = rm::test::mag(1000000.0f)}};

    roster.store.kill(loyalist);
    const rm::sim::UnitId recycled = roster.add(loyalistType, 50.0f, 0.0f, 0, 500.0f);
    REQUIRE(recycled.index == loyalist.index);
    REQUIRE(recycled.generation != loyalist.generation);

    (void)rm::sim::tickSkirmish(roster.store, roster.catalog, match, rm::sim::Terrain{field});
    CHECK(redirects.empty());
}

TEST_CASE("match funds enhancement work only from its living owner's army", "[enhancement][skirmish]") {
    using namespace rm::sim;
    const auto field = flatField();
    Roster roster;
    UnitDef def;
    def.name = "enhancing_commander";
    const auto owner = roster.add(roster.addType(def), 0, 0, 1, 500);
    auto armies = twoSides();
    std::vector<Economy> economies(2);
    economies[0].stored = {Mag::fromInt(80), Mag::fromInt(800)};
    economies[1].stored = economies[0].stored;
    std::vector<EnhancementWork> work{{
        .owner=owner, .name="AdvancedEngineering",
        .cost={Mag::fromInt(8),Mag::fromInt(80)},
        .totalBuildTime=Mag::fromInt(8), .buildTimeRemaining=Mag::fromInt(8),
        .buildPerTick=Mag::fromInt(1),
    }};
    Match match{.armies=armies, .economies=economies, .enhancements=&work,
                .baseStorage={Mag::fromInt(1000),Mag::fromInt(1000)}};
    (void)tickSkirmish(roster.store, roster.catalog, match, Terrain{field});
    CHECK(economies[0].stored.mass == Mag::fromInt(80));
    CHECK(economies[1].stored.mass == Mag::fromInt(79));
    CHECK(economies[1].stored.energy == Mag::fromInt(790));
    CHECK(work[0].fundedLastTick == Fx::fromInt(1));
    CHECK(work[0].buildTimeRemaining == Mag::fromInt(8)); // Dispatch owns progress.
    work[0].owner.generation += 1; // Stale/recycled handles must never bill an army.
    (void)tickSkirmish(roster.store, roster.catalog, match, Terrain{field});
    CHECK(economies[1].stored.mass == Mag::fromInt(79));
}
