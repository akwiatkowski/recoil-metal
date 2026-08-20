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
    weapon.turreted = true;
    weapon.damage = damage;
    weapon.maxRangeElmos = rangeElmos;
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

    const rm::sim::UnitId tank = roster.add(roster.addType(tankDef), 0.0f, 0.0f, 0, 500.0f);
    (void)roster.add(roster.addType(targetDef), 200.0f, 0.0f, 1, 500.0f);

    // Ordered somewhere, so the movement half has something to do.
    rm::sim::orderTo(roster.motion(tank), field, 100.0f, 0.0f);
    const std::array<float, 3> started = roster.instance(tank).position;

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

    const TickReport report = rm::sim::tickSkirmish(roster.store, roster.catalog, match, field);

    CHECK(roster.instance(tank).position != started);  // it moved
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

    const rm::sim::UnitId tank = roster.add(roster.addType(tankDef), 0.0f, 0.0f, 0, 500.0f);
    (void)roster.add(roster.addType(targetDef), 900.0f, 0.0f, 1,
                     500.0f);  // far out of range

    rm::sim::orderTo(roster.motion(tank), field, 100.0f, 0.0f);

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

    const TickReport report = rm::sim::tickSkirmish(roster.store, roster.catalog, match, field);

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
    roster.health(victim).current = 0.0f;  // already destroyed, this tick retires it

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

    const TickReport report = rm::sim::tickSkirmish(roster.store, roster.catalog, match, field);

    REQUIRE(report.died.size() == 1);
    // The HANDLE of the unit that died, which is a stronger claim than the pair it replaced:
    // a (batch, instance) pair named a place, and a place can be refilled.
    CHECK(report.died[0].ref == victim);

    // The wreck's size and place are carried in the report, sampled BEFORE the unit was
    // retired — going back to the slot for them would find a radius of zero, because
    // retiring is what zeroes it.
    CHECK(report.died[0].radiusElmos == Approx(4.0f));
    CHECK(report.died[0].at[0] == Approx(50.0f));

    // Retired: it no longer shoves the living, it no longer drives anywhere, and the store
    // agrees it is gone — which the old pair could not express at all.
    CHECK(roster.motion(victim).radiusElmos == 0.0f);
    CHECK(roster.motion(victim).moving == false);
    CHECK_FALSE(roster.store.alive(victim));
    CHECK(roster.store.slotCount() == 2);  // the slot stays: death is a tombstone

    // Reported ONCE. A corpse sits in its slot for the rest of the match, and a tick
    // that kept reporting it would fire its death explosion every tick forever.
    const TickReport again = rm::sim::tickSkirmish(roster.store, roster.catalog, match, field);
    CHECK(again.died.empty());
}

TEST_CASE("losing the last commander defeats an army and ends the match") {
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
    TickReport report = rm::sim::tickSkirmish(roster.store, roster.catalog, match, field);
    CHECK(report.defeated == 0);
    CHECK(report.matchEnded == false);

    // Army 1's commander dies.
    roster.health(theirs).current = 0.0f;
    report = rm::sim::tickSkirmish(roster.store, roster.catalog, match, field);

    CHECK(report.defeated == 1);
    CHECK(armies[1].defeated);
    CHECK(report.matchEnded);
    REQUIRE(report.winner.has_value());
    CHECK(*report.winner == armies[0].team);
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

    const TickReport report = rm::sim::tickSkirmish(roster.store, roster.catalog, match, field);

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
    blast.damage = 400.0f;
    blast.damageRadiusElmos = 100.0f;

    Roster roster;
    UnitDef bombDef;
    bombDef.name = "test_bomb";
    bombDef.weapons.push_back(blast);
    UnitDef targetDef;
    targetDef.name = "test_target";

    const rm::sim::UnitId bomb = roster.add(roster.addType(bombDef), 0.0f, 0.0f, 0, 100.0f);
    roster.health(bomb).current = 0.0f;  // dies this tick

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

    TickReport report = rm::sim::tickSkirmish(roster.store, roster.catalog, match, field);

    CHECK(report.deathBlasts == 1);
    CHECK(report.deathBlastDamage > 0.0f);
    CHECK(roster.health(bystander).current < 500.0f);  // it felt it

    // ONCE. The corpse sits in its slot for the rest of the match, and a blast that
    // repeated every tick would be both a wrong answer and an unbounded one.
    const float afterOne = roster.health(bystander).current;
    report = rm::sim::tickSkirmish(roster.store, roster.catalog, match, field);
    CHECK(report.deathBlasts == 0);
    CHECK(roster.health(bystander).current == Approx(afterOne));
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
    (void)roster.add(roster.addType(tankDef), 0.0f, 0.0f, 0, 500.0f);

    std::vector<Army> armies = twoSides();
    std::vector<Projectile> projectiles;
    std::vector<Economy> economies(2);
    const std::vector<int> commandersEver(2, 0);

    // One construction, nearly done, with the mass and energy banked to finish it.
    std::vector<Construction> building;
    Construction work;
    work.armyIndex = 0;
    work.position = {100.0f, 0.0f, 100.0f};
    work.cost = {.mass = 10.0f, .energy = 10.0f};
    work.buildTimeRemaining = 1.0f;
    work.totalBuildTime = 1.0f;
    work.buildRate = 100.0f;  // finishes well within one tick
    building.push_back(work);

    economies[0].stored = {.mass = 1000.0f, .energy = 1000.0f};

    // The cap has to come from `baseStorage`: the tick recomputes storage from it plus
    // whatever is standing, so setting `economies[0].storage` here would be overwritten
    // before a single build unit was funded.
    Match match{.armies = armies,
                .economies = economies,
                .projectiles = &projectiles,
                .building = &building,
                .commandersEver = commandersEver,
                .baseStorage = {.mass = 1000.0f, .energy = 1000.0f}};

    TickReport report = rm::sim::tickSkirmish(roster.store, roster.catalog, match, field);

    REQUIRE(report.finished.size() == 1);
    CHECK(report.finished[0].armyIndex == 0);
    CHECK(building.size() == 1);      // still there
    CHECK(building[0].finished());

    // And reported once: a second tick must not spawn the same building again.
    report = rm::sim::tickSkirmish(roster.store, roster.catalog, match, field);
    CHECK(report.finished.empty());
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

    (void)rm::sim::tickSkirmish(roster.store, roster.catalog, match, field);

    CHECK(economies[0].incomePerSecond.mass == Approx(2.0f));
    CHECK(economies[0].upkeepPerSecond.energy == Approx(2.0f));

    // Destroyed, and the income goes with it.
    roster.health(extractor).current = 0.0f;
    (void)rm::sim::tickSkirmish(roster.store, roster.catalog, match, field);

    CHECK(economies[0].incomePerSecond.mass == Approx(0.0f));
    CHECK(economies[0].upkeepPerSecond.energy == Approx(0.0f));
}
