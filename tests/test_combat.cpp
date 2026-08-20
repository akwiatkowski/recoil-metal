// Targeting, ballistics and damage.
//
// All pure, and all invisible when subtly wrong: a falloff curve that is off still kills
// things, just not the right ones, and a reload measured in the wrong unit reads as a
// balance complaint rather than a bug. So the numbers are worked out by hand here.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "core/sim/Combat.hpp"

#include <cstdint>
#include <numbers>
#include <vector>

using Catch::Approx;
using rm::sim::Army;
using rm::sim::CombatGroup;
using rm::sim::Health;
using rm::sim::Projectile;
using rm::sim::UnitRef;
using rm::unitdef::BallisticArc;
using rm::unitdef::UnitDef;
using rm::unitdef::Weapon;
using rm::unitdef::WeaponRole;

namespace {

/// A flat field, so a projectile's landing height is predictable and a test about
/// ballistics is not also a test about terrain.
[[nodiscard]] rm::HeightField flatField(float height = 0.0f) {
    rm::HeightField field;
    field.squaresX = 100;
    field.squaresZ = 100;
    field.baseHeight = height;
    field.heightScale = 1.0f;
    field.raw.assign(field.sampleCount(), std::uint16_t{0});
    return field;
}

[[nodiscard]] Weapon directFire(float damage, float rangeElmos, float radiusElmos = 0.0f) {
    Weapon weapon;
    weapon.label = "test gun";
    weapon.role = WeaponRole::DirectFire;
    weapon.damage = damage;
    weapon.maxRangeElmos = rangeElmos;
    weapon.damageRadiusElmos = radiusElmos;
    weapon.rateOfFire = 1.0f;                        // one shot a second
    weapon.muzzleVelocityElmosPerSecond = 100.0f;    // fast, but not instant
    return weapon;
}

/// A unit at a place, owned by an army, with a definition. Held by the caller because
/// the spans in a CombatGroup point at it.
struct Squad {
    std::vector<rm::UnitInstance> instances;
    std::vector<rm::sim::MoveState> motion;
    std::vector<Health> health;
    UnitDef def;

    void add(float x, float z, int army, float hp) {
        rm::UnitInstance instance{};
        instance.position = {x, 0.0f, z};
        instance.scale = 1.0f;
        instances.push_back(instance);

        rm::sim::MoveState state;
        state.armyIndex = army;
        state.radiusElmos = 4.0f;
        motion.push_back(state);

        health.push_back(Health{.current = hp, .maximum = hp, .reloadRemaining = {}});
    }

    [[nodiscard]] CombatGroup group() {
        return CombatGroup{
            .instances = instances, .motion = motion, .health = health, .def = &def};
    }
};

} // namespace

TEST_CASE("range is measured on the ground, so high ground is not cover") {
    // A weapon's range is a footprint on the map, not a sphere. Using the 3-D distance
    // would make a unit on a cliff harder to shoot than the same unit on the flat, which
    // is a stealth field nobody asked for.
    CHECK(rm::sim::groundDistanceElmos({0, 0, 0}, {30, 0, 40}) == Approx(50.0f));
    CHECK(rm::sim::groundDistanceElmos({0, 0, 0}, {30, 900, 40}) == Approx(50.0f));
}

TEST_CASE("a reload is a whole number of ticks, and never zero") {
    Weapon weapon = directFire(10.0f, 100.0f);

    weapon.rateOfFire = 1.0f;  // one a second, at 10 ticks a second
    CHECK(weapon.reloadTicks() == 10);

    weapon.rateOfFire = 2.0f;
    CHECK(weapon.reloadTicks() == 5);

    // The fastest weapon in the corpus fires ten times a second, which at this tick rate
    // is exactly one shot per tick — a sim cannot fire between two ticks.
    weapon.rateOfFire = 10.0f;
    CHECK(weapon.reloadTicks() == 1);

    // And a rate faster than the tick rate clamps rather than rounding to nothing, which
    // would be a weapon that fires every tick AND an infinite loop for anything counting
    // down from zero.
    weapon.rateOfFire = 50.0f;
    CHECK(weapon.reloadTicks() == 1);
}

TEST_CASE("a death explosion is not a weapon that fires") {
    // 99 of the 494 shipped "weapons" are the unit's own destruction. Firing one would
    // give a hundred units an unmissable, unreloadable cannon.
    Weapon death = directFire(1000.0f, 100.0f);
    death.role = WeaponRole::Death;
    CHECK_FALSE(death.fires());

    // Nor is a table that lacks what a gun must have.
    Weapon noRange = directFire(10.0f, 0.0f);
    CHECK_FALSE(noRange.fires());
    Weapon noDamage = directFire(0.0f, 100.0f);
    CHECK_FALSE(noDamage.fires());

    CHECK(directFire(10.0f, 100.0f).fires());
}

TEST_CASE("a unit shoots the nearest enemy and never a friend") {
    const std::vector<Army> armies = rm::sim::freeForAll(2);

    Squad mine;
    mine.add(0.0f, 0.0f, 0, 100.0f);
    mine.add(10.0f, 0.0f, 0, 100.0f);  // an ALLY, nearer than any enemy

    Squad theirs;
    theirs.add(0.0f, 200.0f, 1, 100.0f);  // far
    theirs.add(0.0f, 50.0f, 1, 100.0f);   // near
    theirs.add(0.0f, 900.0f, 1, 100.0f);  // out of range

    const std::vector<CombatGroup> groups{mine.group(), theirs.group()};
    const Weapon weapon = directFire(10.0f, 300.0f);

    const auto target = rm::sim::nearestTarget({0, 0, 0}, 0, weapon, groups, armies);
    REQUIRE(target.has_value());
    CHECK(target->batch == 1);
    CHECK(target->instance == 1);  // the near enemy, not the nearer ally
}

TEST_CASE("a dead enemy is not a target, and neither is a defeated army's unit") {
    std::vector<Army> armies = rm::sim::freeForAll(2);

    Squad mine;
    mine.add(0.0f, 0.0f, 0, 100.0f);
    Squad theirs;
    theirs.add(0.0f, 50.0f, 1, 0.0f);    // already dead
    theirs.add(0.0f, 80.0f, 1, 100.0f);  // alive, further away

    const std::vector<CombatGroup> groups{mine.group(), theirs.group()};
    const Weapon weapon = directFire(10.0f, 300.0f);

    auto target = rm::sim::nearestTarget({0, 0, 0}, 0, weapon, groups, armies);
    REQUIRE(target.has_value());
    CHECK(target->instance == 1);  // skipped the corpse

    // And once the army has lost, nothing it owns draws fire — otherwise a winning force
    // keeps shooting a side that is already out.
    armies[1].defeated = true;
    CHECK_FALSE(rm::sim::nearestTarget({0, 0, 0}, 0, weapon, groups, armies).has_value());
}

TEST_CASE("a minimum range is a hole a unit can stand in") {
    // 87 weapons state one. Without it a unit walks up to an artillery piece and stands
    // in the one place it cannot be shot from — which is correct, and only correct if the
    // dead zone is honoured.
    Squad mine;
    mine.add(0.0f, 0.0f, 0, 100.0f);
    Squad theirs;
    theirs.add(0.0f, 20.0f, 1, 100.0f);  // inside the dead zone
    const std::vector<CombatGroup> groups{mine.group(), theirs.group()};
    const std::vector<Army> armies = rm::sim::freeForAll(2);

    Weapon artillery = directFire(100.0f, 500.0f);
    artillery.minRangeElmos = 100.0f;

    CHECK_FALSE(rm::sim::nearestTarget({0, 0, 0}, 0, artillery, groups, armies).has_value());

    // ...and the same weapon does reach something outside it.
    theirs.instances[0].position = {0.0f, 0.0f, 200.0f};
    const std::vector<CombatGroup> further{mine.group(), theirs.group()};
    CHECK(rm::sim::nearestTarget({0, 0, 0}, 0, artillery, further, armies).has_value());
}

TEST_CASE("a flat shot flies straight at its target") {
    const Weapon weapon = directFire(10.0f, 500.0f);  // 100 elmos/s
    const Projectile shot = rm::sim::launch({0, 0, 0}, {0, 0, 200}, weapon, 0);

    // Two seconds of flight over 200 elmos, so 100 elmos a second down +Z and nothing
    // sideways.
    CHECK(shot.velocity[0] == Approx(0.0f));
    CHECK(shot.velocity[2] == Approx(100.0f));
    CHECK(shot.arc == BallisticArc::None);

    // It leaves the MUZZLE, four elmos above the shooter's feet, and aims at the target's
    // middle two elmos above its own — so a flat shot at a target on the same ground
    // angles slightly DOWN rather than travelling level. Two elmos of drop over two
    // seconds is one a second.
    CHECK(shot.position[1] == Approx(rm::sim::kMuzzleHeightElmos));
    CHECK(shot.velocity[1] == Approx(-1.0f));
}

TEST_CASE("an arced shot rises, and comes down where the target is") {
    Weapon artillery = directFire(100.0f, 1000.0f);
    artillery.arc = BallisticArc::High;
    artillery.muzzleVelocityElmosPerSecond = 100.0f;

    const std::array<float, 3> from{0, 0, 0};
    const std::array<float, 3> to{0, 0, 300};
    const Projectile shot = rm::sim::launch(from, to, artillery, 0);

    // It must LEAVE going up, which is the whole point of an arc — a flat shot at the
    // same target has a vertical velocity of zero.
    CHECK(shot.velocity[1] > 0.0f);

    // And gravity must bring it down exactly there. Simulated tick by tick rather than
    // asserted from the formula, so the test checks the integration and not the algebra
    // it was derived from.
    std::vector<Projectile> flight{shot};
    Squad empty;
    std::vector<CombatGroup> none;
    const rm::HeightField field = flatField();

    int ticks = 0;
    while (!flight.empty() && ticks < 1000) {
        rm::sim::advanceProjectiles(flight, none, {}, field);
        ++ticks;
    }

    // It landed (the list is empty) rather than expiring at the lifetime cap.
    CHECK(flight.empty());
    CHECK(ticks < rm::sim::kProjectileLifetimeTicks);
}

TEST_CASE("damage falls off linearly to nothing at the rim") {
    const std::vector<Army> armies = rm::sim::freeForAll(2);

    Squad theirs;
    theirs.add(0.0f, 0.0f, 1, 100.0f);     // at the centre
    theirs.add(0.0f, 50.0f, 1, 100.0f);    // halfway out
    theirs.add(0.0f, 100.0f, 1, 100.0f);   // at the rim
    theirs.add(0.0f, 200.0f, 1, 100.0f);   // outside

    std::vector<CombatGroup> groups{theirs.group()};
    const float dealt =
        rm::sim::damageArea({0, 0, 0}, 100.0f, 80.0f, 0, groups, armies);

    CHECK(theirs.health[0].current == Approx(20.0f));   // took all 80
    CHECK(theirs.health[1].current == Approx(60.0f));   // took half
    CHECK(theirs.health[2].current == Approx(100.0f));  // at the rim: nothing
    CHECK(theirs.health[3].current == Approx(100.0f));  // outside: nothing
    CHECK(dealt == Approx(120.0f));
}

TEST_CASE("a blast does not hurt the army that fired it") {
    const std::vector<Army> armies = rm::sim::freeForAll(2);
    Squad mine;
    mine.add(0.0f, 0.0f, 0, 100.0f);
    std::vector<CombatGroup> groups{mine.group()};

    // Fired by army 0, centred on army 0's own unit.
    const float dealt = rm::sim::damageArea({0, 0, 0}, 100.0f, 80.0f, 0, groups, armies);
    CHECK(dealt == Approx(0.0f));
    CHECK(mine.health[0].current == Approx(100.0f));
}

TEST_CASE("a point hit lands on what it was aimed at") {
    // 222 of the 494 weapons state no damage radius. They are point hits at full
    // strength, not weapons that cannot hurt anything — a falloff over a zero radius
    // would divide by zero and damage nobody.
    const std::vector<Army> armies = rm::sim::freeForAll(2);
    Squad theirs;
    theirs.add(0.0f, 0.0f, 1, 100.0f);
    theirs.add(0.0f, 60.0f, 1, 100.0f);
    std::vector<CombatGroup> groups{theirs.group()};

    const float dealt = rm::sim::damageArea({0, 0, 0}, 0.0f, 40.0f, 0, groups, armies);
    CHECK(theirs.health[0].current == Approx(60.0f));
    CHECK(theirs.health[1].current == Approx(100.0f));
    CHECK(dealt == Approx(40.0f));
}

TEST_CASE("damage never takes more than a unit has, so overkill is not negative health") {
    const std::vector<Army> armies = rm::sim::freeForAll(2);
    Squad theirs;
    theirs.add(0.0f, 0.0f, 1, 30.0f);
    std::vector<CombatGroup> groups{theirs.group()};

    const float dealt = rm::sim::damageArea({0, 0, 0}, 0.0f, 5000.0f, 0, groups, armies);
    CHECK(theirs.health[0].current == Approx(0.0f));
    CHECK(dealt == Approx(30.0f));  // what was actually taken, not what was thrown
    CHECK_FALSE(theirs.health[0].alive());
}

TEST_CASE("a shot fired reloads, and does not fire again until it has") {
    const std::vector<Army> armies = rm::sim::freeForAll(2);

    Squad mine;
    mine.add(0.0f, 0.0f, 0, 100.0f);
    mine.def.weapons.push_back(directFire(10.0f, 300.0f));  // one shot a second = 10 ticks

    Squad theirs;
    theirs.add(0.0f, 100.0f, 1, 1000.0f);

    std::vector<Projectile> shots;
    const std::vector<CombatGroup> groups{mine.group(), theirs.group()};

    CHECK(rm::sim::fireWeapons(groups, armies, shots) == 1);
    CHECK(shots.size() == 1);

    // Nine ticks of reload, during which nothing more is fired.
    for (int tick = 0; tick < 9; ++tick) {
        CHECK(rm::sim::fireWeapons(groups, armies, shots) == 0);
    }
    CHECK(shots.size() == 1);

    // ...and then it fires again.
    CHECK(rm::sim::fireWeapons(groups, armies, shots) == 1);
    CHECK(shots.size() == 2);
}

TEST_CASE("a unit with nothing to shoot at holds its fire and stays loaded") {
    // The reload runs whether or not there is a target, so a unit that comes into
    // contact fires at once rather than starting a fresh reload on sighting — the
    // difference between an ambush working and not.
    const std::vector<Army> armies = rm::sim::freeForAll(2);

    Squad mine;
    mine.add(0.0f, 0.0f, 0, 100.0f);
    mine.def.weapons.push_back(directFire(10.0f, 100.0f));

    Squad theirs;
    theirs.add(0.0f, 5000.0f, 1, 100.0f);  // far out of range

    std::vector<Projectile> shots;
    {
        const std::vector<CombatGroup> groups{mine.group(), theirs.group()};
        for (int tick = 0; tick < 50; ++tick) {
            CHECK(rm::sim::fireWeapons(groups, armies, shots) == 0);
        }
    }
    CHECK(shots.empty());

    // Now it walks into range and shoots on the very first tick.
    theirs.instances[0].position = {0.0f, 0.0f, 50.0f};
    const std::vector<CombatGroup> closed{mine.group(), theirs.group()};
    CHECK(rm::sim::fireWeapons(closed, armies, shots) == 1);
}

TEST_CASE("the dead neither shoot nor are shot") {
    const std::vector<Army> armies = rm::sim::freeForAll(2);

    Squad mine;
    mine.add(0.0f, 0.0f, 0, 0.0f);  // dead on arrival
    mine.def.weapons.push_back(directFire(10.0f, 300.0f));
    Squad theirs;
    theirs.add(0.0f, 100.0f, 1, 100.0f);

    std::vector<Projectile> shots;
    const std::vector<CombatGroup> groups{mine.group(), theirs.group()};
    CHECK(rm::sim::fireWeapons(groups, armies, shots) == 0);
    CHECK(shots.empty());
}

TEST_CASE("a shot in flight lands and kills, and is then gone") {
    const std::vector<Army> armies = rm::sim::freeForAll(2);
    const rm::HeightField field = flatField();

    Squad theirs;
    theirs.add(0.0f, 100.0f, 1, 25.0f);  // frail

    Weapon weapon = directFire(40.0f, 300.0f, 30.0f);
    std::vector<Projectile> shots{
        rm::sim::launch({0, 0, 0}, {0, 0, 100}, weapon, 0)};

    std::vector<CombatGroup> groups{theirs.group()};
    for (int tick = 0; tick < 100 && !shots.empty(); ++tick) {
        rm::sim::advanceProjectiles(shots, groups, armies, field);
    }

    CHECK(shots.empty());                       // spent
    CHECK_FALSE(theirs.health[0].alive());      // and it landed on something

    const auto dead = rm::sim::deadUnits(groups);
    REQUIRE(dead.size() == 1);
    CHECK(dead.front() == UnitRef{.batch = 0, .instance = 0});
}

TEST_CASE("a shot that hits nothing expires instead of flying forever") {
    // Nothing here despawns on leaving the map, so a missed shot would otherwise
    // accumulate — a leak whose symptom is a falling frame rate rather than a wrong
    // number.
    const rm::HeightField field = flatField(-100000.0f);  // ground far below: never lands

    Weapon weapon = directFire(10.0f, 300.0f);
    weapon.muzzleVelocityElmosPerSecond = 1000.0f;
    std::vector<Projectile> shots{rm::sim::launch({0, 0, 0}, {0, 0, 100}, weapon, 0)};

    std::vector<CombatGroup> none;
    for (int tick = 0; tick <= rm::sim::kProjectileLifetimeTicks; ++tick) {
        rm::sim::advanceProjectiles(shots, none, {}, field);
    }
    CHECK(shots.empty());
}

TEST_CASE("an unowned unit takes no part in a fight") {
    // kNoArmy is -1 rather than 0 precisely so this holds: a decorative instance, or one
    // whose owner was never set, is neither a target nor a shooter. With a default of 0 it
    // would belong to the first player and read as the enemy fielding units it never
    // built.
    const std::vector<Army> armies = rm::sim::freeForAll(2);

    Squad mine;
    mine.add(0.0f, 0.0f, 0, 100.0f);
    mine.def.weapons.push_back(directFire(10.0f, 300.0f));

    Squad nobodys;
    nobodys.add(0.0f, 50.0f, rm::sim::kNoArmy, 100.0f);

    std::vector<Projectile> shots;
    const std::vector<CombatGroup> groups{mine.group(), nobodys.group()};
    CHECK(rm::sim::fireWeapons(groups, armies, shots) == 0);

    std::vector<CombatGroup> mutableGroups{mine.group(), nobodys.group()};
    CHECK(rm::sim::damageArea({0, 0, 50}, 100.0f, 500.0f, 0, mutableGroups, armies)
          == Approx(0.0f));
    CHECK(nobodys.health[0].alive());
}

TEST_CASE("a unit's death weapon is found, and is not the gun it fired with") {
    UnitDef def;
    def.weapons.push_back(directFire(10.0f, 300.0f));  // its cannon
    Weapon death = directFire(900.0f, 100.0f, 60.0f);
    death.role = WeaponRole::Death;
    death.label = "death explosion";
    def.weapons.push_back(death);

    const Weapon* found = rm::sim::deathWeapon(def);
    REQUIRE(found != nullptr);
    CHECK(found->label == "death explosion");
    CHECK(found->damage == Approx(900.0f));

    // A unit with no death weapon is ordinary, not an error: most have one, some do not.
    UnitDef unarmed;
    CHECK(rm::sim::deathWeapon(unarmed) == nullptr);
}

TEST_CASE("a death explosion goes off where the unit stood") {
    // 99 of the 494 shipped weapons are exactly this, parsed and never fired until now. An
    // ACU's is enormous, which is the most characteristic thing about the game.
    const std::vector<Army> armies = rm::sim::freeForAll(2);

    UnitDef def;
    Weapon death = directFire(500.0f, 100.0f, 80.0f);  // 80-elmo blast
    death.role = WeaponRole::Death;
    def.weapons.push_back(death);

    Squad theirs;
    theirs.add(0.0f, 0.0f, 1, 1000.0f);    // at the centre
    theirs.add(0.0f, 40.0f, 1, 1000.0f);   // halfway out
    theirs.add(0.0f, 500.0f, 1, 1000.0f);  // well clear
    std::vector<CombatGroup> groups{theirs.group()};

    const float dealt = rm::sim::explodeOnDeath(def, {0, 0, 0}, 0, groups, armies);

    CHECK(dealt > 0.0f);
    CHECK(theirs.health[0].current == Approx(500.0f));   // took the full 500
    CHECK(theirs.health[1].current == Approx(750.0f));   // half of it
    CHECK(theirs.health[2].current == Approx(1000.0f));  // untouched
}

TEST_CASE("a unit with no death weapon detonates harmlessly") {
    const std::vector<Army> armies = rm::sim::freeForAll(2);
    UnitDef def;
    def.weapons.push_back(directFire(10.0f, 300.0f));  // a gun, not a death blast

    Squad theirs;
    theirs.add(0.0f, 0.0f, 1, 100.0f);
    std::vector<CombatGroup> groups{theirs.group()};

    CHECK(rm::sim::explodeOnDeath(def, {0, 0, 0}, 0, groups, armies) == Approx(0.0f));
    CHECK(theirs.health[0].current == Approx(100.0f));
}

TEST_CASE("a bearing is measured the way a unit's yaw is") {
    // atan2(dx, dz), NOT atan2(dz, dx): yaw is measured from +Z toward +X because that is
    // what the vertex shader does with it. Swapping the arguments compiles, runs, and points
    // every turret ninety degrees off.
    CHECK(rm::sim::bearingTo({0, 0, 0}, {0, 0, 100}) == Approx(0.0f));                    // +Z
    CHECK(rm::sim::bearingTo({0, 0, 0}, {100, 0, 0})
          == Approx(std::numbers::pi_v<float> / 2.0f));                                   // +X
}

TEST_CASE("a heading error takes the shorter way round") {
    // A unit one degree the wrong side of north must read as one degree off, not 359 — or it
    // turns the long way and looks broken.
    constexpr float pi = std::numbers::pi_v<float>;
    CHECK(rm::sim::headingError(0.0f, 0.1f) == Approx(0.1f));
    CHECK(rm::sim::headingError(0.1f, 0.0f) == Approx(0.1f));  // symmetric
    CHECK(rm::sim::headingError(-0.05f, 0.05f) == Approx(0.1f));
    CHECK(rm::sim::headingError(0.0f, 2.0f * pi - 0.1f) == Approx(0.1f).margin(1e-4));
    CHECK(rm::sim::headingError(0.0f, pi) == Approx(pi));  // the furthest possible
}

TEST_CASE("a turreted weapon fires whatever the hull is doing") {
    // 284 of the 399 weapons that say either way are turreted, and this engine does not
    // animate turrets — so gating them on the hull would leave two thirds of the corpus
    // unable to shoot at all.
    Weapon turret = directFire(10.0f, 300.0f);
    turret.turreted = true;
    turret.firingToleranceDegrees = 1.0f;

    CHECK(rm::sim::canFireAt(turret, 0.0f, 0.0f));
    CHECK(rm::sim::canFireAt(turret, 0.0f, std::numbers::pi_v<float>));  // directly behind
}

TEST_CASE("an unturreted weapon must be pointed at what it shoots") {
    // The fix for a tank firing out of its side armour.
    Weapon fixed = directFire(10.0f, 300.0f);
    fixed.turreted = false;
    fixed.firingToleranceDegrees = 2.0f;  // the corpus's own mode

    CHECK(rm::sim::canFireAt(fixed, 0.0f, 0.0f));
    CHECK(rm::sim::canFireAt(fixed, 0.0f, 0.03f));  // just under two degrees
    CHECK_FALSE(rm::sim::canFireAt(fixed, 0.0f, 0.5f));
    CHECK_FALSE(rm::sim::canFireAt(fixed, 0.0f, std::numbers::pi_v<float> / 2.0f));
}

TEST_CASE("an idle unit turns to bring its gun to bear, at its own rate") {
    const std::vector<Army> armies = rm::sim::freeForAll(2);

    Squad mine;
    mine.add(0.0f, 0.0f, 0, 100.0f);
    mine.motion[0].turnRateRadiansPerSecond = 1.0f;  // one radian a second
    mine.motion[0].moving = false;
    Weapon fixed = directFire(10.0f, 300.0f);
    fixed.turreted = false;
    mine.def.weapons.push_back(fixed);

    Squad theirs;
    theirs.add(100.0f, 0.0f, 1, 100.0f);  // due +X, so a bearing of pi/2

    const std::vector<CombatGroup> groups{mine.group(), theirs.group()};

    // One tick is a tenth of a radian, so it does not snap round — a slow hull is slow to
    // aim, which is why the turn rate is read off the blueprint at all.
    CHECK(rm::sim::aimAtTargets(mine.instances, mine.motion, &mine.def, groups, armies) == 1);
    CHECK(mine.instances[0].rotationY == Approx(0.1f));

    // ...and it gets there eventually.
    for (int tick = 0; tick < 100; ++tick) {
        (void)rm::sim::aimAtTargets(mine.instances, mine.motion, &mine.def, groups, armies);
    }
    CHECK(mine.instances[0].rotationY
          == Approx(std::numbers::pi_v<float> / 2.0f).margin(0.01));
}

TEST_CASE("a moving unit is not turned by aiming, and a turreted one has no reason to") {
    const std::vector<Army> armies = rm::sim::freeForAll(2);
    Squad theirs;
    theirs.add(100.0f, 0.0f, 1, 100.0f);

    SECTION("moving: its order decides where it points") {
        Squad mine;
        mine.add(0.0f, 0.0f, 0, 100.0f);
        mine.motion[0].moving = true;
        Weapon fixed = directFire(10.0f, 300.0f);
        fixed.turreted = false;
        mine.def.weapons.push_back(fixed);

        const std::vector<CombatGroup> groups{mine.group(), theirs.group()};
        CHECK(rm::sim::aimAtTargets(mine.instances, mine.motion, &mine.def, groups, armies) == 0);
        CHECK(mine.instances[0].rotationY == Approx(0.0f));
    }
    SECTION("turreted: the turret aims, not the hull") {
        Squad mine;
        mine.add(0.0f, 0.0f, 0, 100.0f);
        Weapon turret = directFire(10.0f, 300.0f);
        turret.turreted = true;
        mine.def.weapons.push_back(turret);

        const std::vector<CombatGroup> groups{mine.group(), theirs.group()};
        CHECK(rm::sim::aimAtTargets(mine.instances, mine.motion, &mine.def, groups, armies) == 0);
    }
}

TEST_CASE("a unit facing the wrong way holds its shot rather than spending it") {
    // The reload must NOT be consumed while turning: a unit that spent its shot waiting to
    // line up would fire far more slowly than its blueprint says.
    const std::vector<Army> armies = rm::sim::freeForAll(2);

    Squad mine;
    mine.add(0.0f, 0.0f, 0, 100.0f);
    mine.instances[0].rotationY = std::numbers::pi_v<float>;  // facing away
    Weapon fixed = directFire(10.0f, 300.0f);
    fixed.turreted = false;
    fixed.firingToleranceDegrees = 2.0f;
    mine.def.weapons.push_back(fixed);

    Squad theirs;
    theirs.add(0.0f, 100.0f, 1, 100.0f);  // due +Z, a bearing of 0

    std::vector<Projectile> shots;
    const std::vector<CombatGroup> groups{mine.group(), theirs.group()};

    for (int tick = 0; tick < 30; ++tick) {
        CHECK(rm::sim::fireWeapons(groups, armies, shots) == 0);
    }
    CHECK(shots.empty());

    // Turn it round and it fires on the very next tick, its reload never having been spent.
    mine.instances[0].rotationY = 0.0f;
    CHECK(rm::sim::fireWeapons(groups, armies, shots) == 1);
}
