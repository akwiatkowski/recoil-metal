// Targeting, ballistics and damage.
//
// All pure, and all invisible when subtly wrong: a falloff curve that is off still kills
// things, just not the right ones, and a reload measured in the wrong unit reads as a
// balance complaint rather than a bug. So the numbers are worked out by hand here.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "core/sim/Combat.hpp"

#include "support/TestRoster.hpp"

#include <cstdint>
#include <cmath>
#include <numbers>
#include <vector>

#include "support/FxMatchers.hpp"

using Catch::Approx;
using rm::sim::Army;
using rm::sim::Health;
using rm::sim::Projectile;
using rm::sim::UnitId;
using rm::test::Roster;
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

[[nodiscard]] Weapon directFire(float damage, float rangeElmos, float radiusElmos = 0.0f,
                                rm::sim::Fx trackingRadius = rm::sim::Fx::fromInt(1)) {
    Weapon weapon{.trackingRadius = trackingRadius};
    weapon.label = "test gun";
    weapon.role = WeaponRole::DirectFire;
    weapon.targetPriorities = {{"LAND"}};
    weapon.damage = rm::test::mag(damage);
    weapon.maxRange = rm::test::fx(rangeElmos);
    weapon.damageRadius = rm::test::fx(radiusElmos);
    weapon.rateOfFire = 1.0f;                        // one shot a second
    weapon.muzzleVelocityElmosPerSecond = 100.0f;    // fast, but not instant
    return weapon;
}

/// An unarmed thing to shoot at: a type with no weapons, which is what most of these cases
/// want on the receiving end.
[[nodiscard]] UnitDef targetDef() {
    UnitDef def;
    def.name = "test_target";
    def.categories = {"LAND"};
    return def;
}

/// A type that carries one gun.
[[nodiscard]] UnitDef gunnerDef(const Weapon& weapon) {
    UnitDef def;
    def.name = "test_gunner";
    def.weapons.push_back(weapon);
    return def;
}

/// Whether one raw-unit-sized target is struck by a projectile moving by the supplied Q18.14
/// delta. Tiny dimensions keep the test about C-168's radius-one fallback rather than the
/// ordinary four-elmo test body.
[[nodiscard]] bool tinyProjectileStrikes(std::array<rm::FxRaw, 3> deltaRaw,
                                         std::array<rm::FxRaw, 3> targetRaw) {
    const std::vector<Army> armies = rm::sim::freeForAll(2);
    Roster roster;
    const UnitId target =
        roster.add(roster.addType(targetDef()), 0.0f, 0.0f, 1, 100.0f);
    roster.transform(target) = {
        .x = rm::sim::Fx::fromRaw(targetRaw[0]),
        .y = rm::sim::Fx::fromRaw(targetRaw[1]),
        .z = rm::sim::Fx::fromRaw(targetRaw[2]),
    };
    roster.motion(target).radiusElmos = rm::sim::Fx::fromRaw(1);
    roster.reindex();

    Projectile shot;
    shot.position = {};
    shot.velocity = {
        rm::sim::Fx::fromRaw(deltaRaw[0]),
        rm::sim::Fx::fromRaw(deltaRaw[1]),
        rm::sim::Fx::fromRaw(deltaRaw[2]),
    };
    shot.damage = rm::unitdef::flatDamage(rm::test::mag(40.0f));
    shot.targetLayers = rm::unitdef::TargetLayerMask::Surface;
    shot.firedByArmy = 0;
    shot.ticksRemaining = 2;
    std::vector<Projectile> shots{shot};

    rm::sim::advanceProjectiles(shots, roster.store, armies,
                                rm::sim::Terrain{flatField(-100.0f)}, roster.rate,
                                nullptr, &roster.catalog);
    rm::sim::advanceProjectiles(shots, roster.store, armies,
                                rm::sim::Terrain{flatField(-100.0f)}, roster.rate,
                                nullptr, &roster.catalog);
    return roster.health(target).current < rm::test::mag(100.0f);
}

} // namespace

TEST_CASE("projectile splash reaches wrecks on the deferred impact beat only", "[wreck-combat]") {
    using rm::sim::Fx;
    using rm::sim::Mag;
    const auto armies = rm::sim::freeForAll(2);
    Roster roster;
    const auto target = roster.add(roster.addType(targetDef()), 100, 100, 1, 100);
    roster.reindex();
    rm::sim::FeatureStore features;
    const rm::sim::Feature body{.at = {Fx::fromInt(100), {}, Fx::fromInt(100)},
        .radiusElmos = Fx::fromInt(2), .health = Mag::fromInt(100),
        .maximumHealth = Mag::fromInt(100), .maximumMassReclaim = Mag::fromInt(50),
        .massRemaining = Mag::fromInt(50), .reclaimWorkRemaining = Mag::fromInt(50),
        .reclaimWorkTotal = Mag::fromInt(50)};
    const auto wreck = features.add(body);
    REQUIRE(wreck == target);
    Projectile shot;
    shot.position = {Fx::fromInt(100), Fx::fromInt(10), Fx::fromInt(100)};
    shot.velocity = {Fx{}, Fx::fromInt(-20), Fx{}};
    shot.damage = rm::unitdef::flatDamage(Mag::fromInt(40));
    shot.damageRadiusElmos = Fx::fromInt(5);
    shot.firedByArmy = 0;
    shot.ticksRemaining = 5;
    std::vector<Projectile> shots{shot};
    const auto tick = [&] {
        rm::sim::advanceProjectiles(shots, roster.store, armies, rm::sim::Terrain{flatField()},
            roster.rate, nullptr, &roster.catalog, {}, &features);
    };
    tick();
    REQUIRE(shots.size() == 1);
    REQUIRE(shots.front().pendingImpact != rm::sim::ImpactType::Invalid);
    CHECK(features.find(wreck)->health == Mag::fromInt(100));
    SECTION("area damage includes both distinct pools") {
        tick();
        REQUIRE(features.find(wreck));
        CHECK(features.find(wreck)->health == Mag::fromInt(60));
        CHECK(rm::test::asFloat(features.find(wreck)->massRemaining) == Approx(30).margin(0.01));
        CHECK(roster.health(target).current == Mag::fromInt(60));
    }
    SECTION("point damage names the unit even if a feature has the same ID") {
        shots.front().damageRadiusElmos = {};
        tick();
        CHECK(features.find(wreck)->health == Mag::fromInt(100));
        CHECK(roster.health(target).current == Mag::fromInt(60));
    }
    SECTION("splash queries current geometry after a feature slot is reused") {
        features.remove(wreck);
        auto distant = body;
        distant.at[0] = Fx::fromInt(200);
        const auto replacement = features.add(distant);
        REQUIRE(replacement.index == wreck.index);
        REQUIRE(replacement.generation != wreck.generation);
        tick();
        CHECK(features.find(replacement)->health == Mag::fromInt(100));
    }
    CHECK(shots.empty());
}

TEST_CASE("wreck blast bounds and ordinary projectile sweeps use different candidate sets", "[wreck-combat]") {
    using rm::sim::Fx;
    using rm::sim::Mag;
    Roster roster;
    const auto armies = rm::sim::freeForAll(2);
    rm::sim::FeatureStore features;
    const auto wreck = features.add({.at = {Fx::fromInt(100), {}, Fx::fromInt(100)},
        .radiusElmos = Fx::fromInt(2), .health = Mag::fromInt(50), .maximumHealth = Mag::fromInt(50)});
    Projectile shot;
    shot.position = {Fx::fromInt(90), Fx::fromInt(1), Fx::fromInt(100)};
    shot.velocity = {Fx::fromInt(20), {}, {}};
    shot.damage = rm::unitdef::flatDamage(Mag::fromInt(100));
    shot.firedByArmy = 0;
    shot.ticksRemaining = 5;
    std::vector<Projectile> shots{shot};
    rm::sim::advanceProjectiles(shots, roster.store, armies, rm::sim::Terrain{flatField()},
        roster.rate, nullptr, nullptr, {}, &features);
    REQUIRE(shots.size() == 1);
    CHECK(shots.front().pendingImpact == rm::sim::ImpactType::Invalid);
    CHECK(features.find(wreck)->health == Mag::fromInt(50));
    const auto blast = [&](int x) {
        return rm::sim::damageArea({Fx::fromInt(x), {}, Fx::fromInt(100)}, Fx::fromInt(5),
            Mag::fromInt(100), 0, roster.store, armies, {}, nullptr, nullptr, &features);
    };
    CHECK(blast(108) == Mag{}); // one elmo beyond the box-plus-blast reach
    CHECK(blast(107) == Mag::fromInt(50)); // tangent is included, damage capped at remaining health
    CHECK(features.find(wreck) == nullptr);
    CHECK(blast(100) == Mag{}); // a dead feature cannot be damaged twice
}

TEST_CASE("range is measured on the ground, so high ground is not cover") {
    // A weapon's range is a footprint on the map, not a sphere. Using the 3-D distance
    // would make a unit on a cliff harder to shoot than the same unit on the flat, which
    // is a stealth field nobody asked for.
    CHECK(rm::test::asFloat(rm::sim::groundDistanceElmos(rm::test::at(0, 0, 0), rm::test::at(30, 0, 40))) == Approx(50.0f));
    CHECK(rm::test::asFloat(rm::sim::groundDistanceElmos(rm::test::at(0, 0, 0), rm::test::at(30, 900, 40))) == Approx(50.0f));
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

TEST_CASE("a manual weapon and an upgrade's weapon wait for orders that never come") {
    // The commander's OverCharge states `ManualFire = true` and 12000 damage
    // (uel0001_unit.bp) — in the game it costs energy and a click, and neither exists
    // here. Auto-firing it one-shots everything that ever walks into range.
    Weapon overcharge = directFire(12000.0f, 176.0f);
    overcharge.manualFire = true;
    CHECK_FALSE(overcharge.fires());

    // The commander's TacMissile states `EnabledByEnhancement = 'TacticalMissile'`
    // and a 2048-elmo range. Enhancements do not exist in this engine, so a weapon
    // gated on one does not exist either — read it as a gun and the commander snipes
    // whole columns from a fifth of the map away, which is how this line was found.
    Weapon missile = directFire(250.0f, 2048.0f);
    missile.enabledByEnhancement = true;
    CHECK_FALSE(missile.fires());
}

TEST_CASE("point defence does not acquire or fire at units") {
    const std::vector<Army> armies = rm::sim::freeForAll(2);
    Roster roster;

    Weapon pointDefence = directFire(10.0f, 300.0f);
    pointDefence.role = WeaponRole::DirectFire;  // `Defense` parses to this ordinary role.
    pointDefence.targetsProjectiles = true;
    (void)roster.add(roster.addType(gunnerDef(pointDefence)), 0.0f, 0.0f, 0, 100.0f);
    (void)roster.add(roster.addType(targetDef()), 0.0f, 100.0f, 1, 100.0f);

    std::vector<Projectile> shots;
    CHECK_FALSE(rm::sim::nearestTarget(rm::test::at(0, 0, 0), 0, pointDefence,
                                       roster.store, armies, nullptr, &roster.catalog));
    CHECK(rm::sim::fireWeapons(roster.store, roster.catalog, armies, shots,
                               rm::sim::TickRate{}) == 0);
    CHECK(shots.empty());
}

TEST_CASE("point defence fires an interceptor at the nearest hostile projectile") {
    const std::vector<Army> armies = rm::sim::freeForAll(2);
    Roster roster;
    Weapon pointDefence = directFire(10.0f, 300.0f);
    pointDefence.targetsProjectiles = true;
    (void)roster.add(roster.addType(gunnerDef(pointDefence)), 0.0f, 0.0f, 0, 100.0f);

    Projectile friendly{.position = rm::test::at(0, 4, 20), .firedByArmy = 0,
                        .ticksRemaining = 10};
    Projectile farHostile{.position = rm::test::at(100, 4, 0), .firedByArmy = 1,
                          .ticksRemaining = 10};
    Projectile nearHostile{.position = rm::test::at(0, 4, 50), .firedByArmy = 1,
                           .ticksRemaining = 10};
    std::vector<Projectile> shots{friendly, farHostile, nearHostile};

    REQUIRE(rm::sim::fireWeapons(roster.store, roster.catalog, armies, shots,
                                  rm::sim::TickRate{}) == 1);
    REQUIRE(shots.size() == 4);
    CHECK(shots.back().interceptor);
    CHECK(shots.back().velocity[0] == rm::sim::Fx{});
    CHECK(shots.back().velocity[2] == rm::test::fx(10.0f));
}

TEST_CASE("an interceptor launch leads a crossing target", "[interception][lead]") {
    const std::vector<Army> armies = rm::sim::freeForAll(2);
    Roster roster;
    Weapon pointDefence = directFire(10.0f, 300.0f);
    pointDefence.targetsProjectiles = true;
    pointDefence.firingToleranceBrads = 32768; // the crossing lead leaves the bow
    (void)roster.add(roster.addType(gunnerDef(pointDefence)), 0.0f, 0.0f, 0, 100.0f);

    // Crossing at 20 elmos a tick through (0, 4, 50). A current-position solution
    // would leave the muzzle dead along +z; the lead bends toward +x instead.
    std::vector<Projectile> shots{{.position = rm::test::at(0, 4, 50),
                                   .velocity = rm::test::at(20, 0, 0),
                                   .firedByArmy = 1,
                                   .ticksRemaining = 100}};
    REQUIRE(rm::sim::fireWeapons(roster.store, roster.catalog, armies, shots,
                                  roster.rate)
            == 1);
    REQUIRE(shots.size() == 2);
    const auto launched = shots.back().velocity;
    CHECK(launched[0] > rm::sim::Fx{});
    // Independent pursuit estimate in doubles: two iterations at 10 elmos a tick
    // from a 4-elmo muzzle give (9.759, 0, 2.182). The test computes the contract;
    // the sim computes it in fixed point.
    CHECK(rm::test::asFloat(launched[0]) == Approx(9.759).margin(0.05));
    CHECK(rm::test::asFloat(launched[1]) == Approx(0.0).margin(0.05));
    CHECK(rm::test::asFloat(launched[2]) == Approx(2.182).margin(0.05));
}

TEST_CASE("a led interceptor passes close by a crossing missile", "[interception][lead]") {
    const std::vector<Army> armies = rm::sim::freeForAll(2);
    Roster roster;
    Weapon pointDefence = directFire(10.0f, 500.0f);
    pointDefence.targetsProjectiles = true;
    pointDefence.firingToleranceBrads = 32768;
    pointDefence.muzzleVelocityElmosPerSecond = 400.0f;
    (void)roster.add(roster.addType(gunnerDef(pointDefence)), 0.0f, 0.0f, 0, 100.0f);

    // A tactical crossing at 16 elmos a tick, 460 out. The interceptor flies
    // ballistically (no homing target), so its closest approach measures the launch
    // solution alone. The control below computes what a current-position aim would
    // do in doubles; the sim must beat it by an order of magnitude, in fixed point.
    std::vector<Projectile> shots{{.position = rm::test::at(-300, 60, 350),
                                   .velocity = rm::test::at(16, 0, 0),
                                   .firedByArmy = 1,
                                   .ticksRemaining = 300,
                                   .maxHealth = rm::sim::magFromFloat(1.0f),
                                   .health = rm::sim::magFromFloat(1.0f)}};
    REQUIRE(rm::sim::fireWeapons(roster.store, roster.catalog, armies, shots,
                                  roster.rate)
            == 1);
    REQUIRE(shots.size() == 2);
    double closest = 1.0e9;
    for (int tick = 0; tick < 60 && shots.size() == 2; ++tick) {
        const auto& hostile = shots[0];
        const auto& interceptor = shots[1];
        const double dx = rm::test::asFloat(interceptor.position[0])
            - rm::test::asFloat(hostile.position[0]);
        const double dy = rm::test::asFloat(interceptor.position[1])
            - rm::test::asFloat(hostile.position[1]);
        const double dz = rm::test::asFloat(interceptor.position[2])
            - rm::test::asFloat(hostile.position[2]);
        closest = std::min(closest, std::sqrt(dx * dx + dy * dy + dz * dz));
        rm::sim::advanceProjectiles(shots, roster.store, armies,
                                    rm::sim::Terrain{flatField()}, roster.rate, nullptr,
                                    &roster.catalog);
    }
    // No-lead control: a 40-elmo straight flight at (-300, 60, 350) from (0, 4, 0)
    // while the missile slides +x at 16 a tick misses by over a hundred elmos.
    double aimX = -300.0, aimY = 60.0, aimZ = 350.0;
    double length = std::sqrt(aimX * aimX + aimY * aimY + aimZ * aimZ);
    const double vx = 40.0 * aimX / length, vy = 40.0 * aimY / length,
                 vz = 40.0 * aimZ / length;
    double uncontrolled = 1.0e9;
    for (int tick = 0; tick < 60; ++tick) {
        const double mx = -300.0 + 16.0 * tick, my = 60.0, mz = 350.0;
        const double ix = vx * tick, iy = 4.0 + vy * tick, iz = vz * tick;
        const double dx = ix - mx, dy = iy - my, dz = iz - mz;
        uncontrolled = std::min(uncontrolled, std::sqrt(dx * dx + dy * dy + dz * dz));
    }
    CHECK(uncontrolled > 100.0);
    CHECK(closest < 25.0);
}
TEST_CASE("a counted projectile launches before its guarded silo-ammo consume") {
    const std::vector<Army> armies = rm::sim::freeForAll(2);
    Roster roster;
    Weapon interceptor = directFire(10.0f, 300.0f);
    interceptor.targetsProjectiles = true;
    interceptor.countedProjectile = true;
    const UnitId silo = roster.add(roster.addType(gunnerDef(interceptor)), 0.0f, 0.0f, 0, 100.0f);
    std::vector<Projectile> shots{{.position = rm::test::at(0, 4, 50), .firedByArmy = 1, .ticksRemaining = 10}};
    rm::sim::SiloAmmo ammo{.owner = silo, .weapon = 0, .stored = 1, .capacity = 7};
    REQUIRE(rm::sim::fireWeapons(roster.store, roster.catalog, armies, shots, roster.rate,
                                  nullptr, nullptr, nullptr, 0,
                                  std::span<rm::sim::SiloAmmo>{&ammo, 1}) == 1);
    CHECK(shots.size() == 2);
    CHECK(ammo.stored == 0);
}

TEST_CASE("a counted projectile with an empty silo does not launch") {
    // The gate behind the fire is retail's `UnitWeapon::CanFire` AND-ing `HasSiloAmmo`
    // (ART-E001 `0x006E01D9`: false pushes false), reached from Lua's `OnGotTarget` early
    // return for counted weapons (ART-S010 `lua/sim/defaultweapons.lua:416`). No launch
    // means no consume either — and a counted weapon on a unit with NO silo record still
    // fires, because `HasSiloAmmo` is true when the unit has no silo object (`C-085`).
    const std::vector<Army> armies = rm::sim::freeForAll(2);
    Roster roster;
    Weapon interceptor = directFire(10.0f, 300.0f);
    interceptor.targetsProjectiles = true;
    interceptor.countedProjectile = true;
    const UnitId silo = roster.add(roster.addType(gunnerDef(interceptor)), 0.0f, 0.0f, 0, 100.0f);

    SECTION("an empty stored count holds the shot") {
        std::vector<Projectile> shots{{.position = rm::test::at(0, 4, 50), .firedByArmy = 1,
                                       .ticksRemaining = 10}};
        rm::sim::SiloAmmo ammo{.owner = silo, .weapon = 0, .stored = 0, .capacity = 7};
        CHECK(rm::sim::fireWeapons(roster.store, roster.catalog, armies, shots, roster.rate,
                                    nullptr, nullptr, nullptr, 0,
                                    std::span<rm::sim::SiloAmmo>{&ammo, 1}) == 0);
        CHECK(shots.size() == 1);
        CHECK(ammo.stored == 0);
    }

    SECTION("a counted weapon with no silo record still fires") {
        std::vector<Projectile> shots{{.position = rm::test::at(0, 4, 50), .firedByArmy = 1,
                                       .ticksRemaining = 10}};
        CHECK(rm::sim::fireWeapons(roster.store, roster.catalog, armies, shots,
                                    roster.rate) == 1);
        CHECK(shots.size() == 2);
    }
}

TEST_CASE("point defence rejects friendly projectiles") {
    const std::vector<Army> armies = rm::sim::freeForAll(2);
    Roster roster;
    Weapon pointDefence = directFire(10.0f, 300.0f);
    pointDefence.targetsProjectiles = true;
    (void)roster.add(roster.addType(gunnerDef(pointDefence)), 0.0f, 0.0f, 0, 100.0f);

    std::vector<Projectile> shots{{.position = rm::test::at(0, 4, 50),
                                   .firedByArmy = 0,
                                   .ticksRemaining = 10}};
    CHECK(rm::sim::fireWeapons(roster.store, roster.catalog, armies, shots,
                               rm::sim::TickRate{}) == 0);
    CHECK(shots.size() == 1);
}

TEST_CASE("point defence applies TrackingRadius only to projectile acquisition") {
    const std::vector<Army> armies = rm::sim::freeForAll(2);
    Roster roster;
    Weapon pointDefence = directFire(10.0f, 100.0f, 0.0f, rm::sim::Fx::fromInt(2));
    pointDefence.targetsProjectiles = true;
    (void)roster.add(roster.addType(gunnerDef(pointDefence)), 0.0f, 0.0f, 0, 100.0f);

    SECTION("it reaches a hostile projectile beyond MaxRadius") {
        std::vector<Projectile> shots{{.position = rm::test::at(0, 4, 150),
                                       .firedByArmy = 1,
                                       .ticksRemaining = 10}};
        CHECK(rm::sim::fireWeapons(roster.store, roster.catalog, armies, shots,
                                   rm::sim::TickRate{}) == 1);
    }

    SECTION("it rejects a hostile projectile beyond the expanded reach") {
        std::vector<Projectile> shots{{.position = rm::test::at(0, 4, 201),
                                       .firedByArmy = 1,
                                       .ticksRemaining = 10}};
        CHECK(rm::sim::fireWeapons(roster.store, roster.catalog, armies, shots,
                                   rm::sim::TickRate{}) == 0);
    }

    SECTION("it does not expand ordinary unit acquisition") {
        const rm::UnitTypeIndex type = roster.addType(targetDef());
        (void)roster.add(type, 0.0f, 150.0f, 1, 100.0f);
        Weapon ordinary = directFire(10.0f, 100.0f, 0.0f, rm::sim::Fx::fromInt(2));
        CHECK_FALSE(rm::sim::nearestTarget(rm::test::at(0, 0, 0), 0, ordinary,
                                            roster.store, armies));
    }
}

TEST_CASE("a low TrackingRadius does not shorten point defence MaxRadius") {
    const std::vector<Army> armies = rm::sim::freeForAll(2);
    Roster roster;
    Weapon pointDefence = directFire(10.0f, 100.0f, 0.0f, rm::test::fx(0.75f));
    pointDefence.targetsProjectiles = true;
    (void)roster.add(roster.addType(gunnerDef(pointDefence)), 0.0f, 0.0f, 0, 100.0f);

    std::vector<Projectile> shots{{.position = rm::test::at(0, 4, 100),
                                   .firedByArmy = 1,
                                   .ticksRemaining = 10}};
    CHECK(rm::sim::fireWeapons(roster.store, roster.catalog, armies, shots,
                               rm::sim::TickRate{}) == 1);
}

TEST_CASE("an interceptor damages a projectile's health rather than force-destroying it") {
    // C-087: interception damages; it does not force-destroy. 34 projectile blueprints
    // declare Defense.MaxHealth — tacticals 1-3, nukes 25 (ART-S013) — so an SMD
    // interceptor's 30 damage one-shots a nuke while the UEF TMD's 1 needs several hits.
    // A projectile with no authored pool still dies to any damage (`OnImpactDestroy`
    // otherwise), and the interceptor itself is consumed on contact either way.
    const std::vector<Army> armies = rm::sim::freeForAll(2);

    const auto flyInterceptor = [&](float damage, float targetMaxHealth,
                                    const std::array<rm::sim::Fx, 3>& at) {
        Roster roster;
        Projectile target{.position = at,
                          .firedByArmy = 1,
                          .ticksRemaining = 10,
                          .maxHealth = rm::test::mag(targetMaxHealth),
                          .health = rm::test::mag(targetMaxHealth)};
        Projectile interceptor{.position = rm::test::at(0, 4, 0),
                               .velocity = rm::test::at(0, 0, 20),
                               .damage = rm::unitdef::flatDamage(rm::test::mag(damage)),
                               .firedByArmy = 0,
                               .interceptor = true,
                               .ticksRemaining = 10};
        std::vector<Projectile> shots{target, interceptor};
        rm::sim::advanceProjectiles(shots, roster.store, armies,
                                    rm::sim::Terrain{flatField()}, roster.rate, nullptr,
                                    &roster.catalog);
        return shots;
    };

    SECTION("a 30-damage interceptor one-shots a 25-health nuke") {
        CHECK(flyInterceptor(30.0f, 25.0f, rm::test::at(0, 4, 10)).empty());
    }

    SECTION("a 1-damage interceptor leaves a 2-health missile alive at 1") {
        const std::vector<Projectile> survivors = flyInterceptor(1.0f, 2.0f, rm::test::at(0, 4, 10));
        REQUIRE(survivors.size() == 1);
        CHECK_FALSE(survivors.front().interceptor);
        CHECK(rm::sim::magToFloat(survivors.front().health) == Approx(1.0f));
    }
}

TEST_CASE("point defence applies target restrictions to projectile acquisition") {
    // C-088's TMD/SMD split: the TMD (UEB4201) carries
    // `TargetRestrictOnlyAllow = 'TACTICAL MISSILE'` and no ammo, the SMD (UEB4302)
    // `TargetRestrictOnlyAllow = 'STRATEGIC MISSILE'` — both also disallow UNTARGETABLE.
    // A restriction admits a shot only when it carries every listed tag, so a defence
    // with an allow list never wastes a shot on an ordinary shell, which has no resolved
    // blueprint categories at all.
    const std::vector<Army> armies = rm::sim::freeForAll(2);

    const auto restrictedGun = [](std::optional<std::vector<std::string>> allow) {
        Weapon gun = directFire(10.0f, 300.0f);
        gun.targetsProjectiles = true;
        gun.targetRestrictOnlyAllow = std::move(allow);
        gun.targetRestrictOnlyDisallow = std::vector<std::string>{"UNTARGETABLE"};
        return gun;
    };
    // Sorted: the acquisition check is the same binary search the unit side uses.
    const std::vector<std::string> tactical{"MISSILE", "TACTICAL"};
    const std::vector<std::string> strategic{"MISSILE", "STRATEGIC"};

    SECTION("a TMD-pattern allow list takes a tactical and refuses a strategic") {
        Roster roster;
        (void)roster.add(roster.addType(gunnerDef(restrictedGun(
                            std::vector<std::string>{"TACTICAL", "MISSILE"}))),
                         0.0f, 0.0f, 0, 100.0f);
        std::vector<Projectile> tacticalShot{{.position = rm::test::at(0, 4, 50),
                                              .firedByArmy = 1,
                                              .ticksRemaining = 10,
                                              .categories = tactical}};
        CHECK(rm::sim::fireWeapons(roster.store, roster.catalog, armies, tacticalShot,
                                    roster.rate) == 1);
        std::vector<Projectile> strategicShot{{.position = rm::test::at(0, 4, 50),
                                               .firedByArmy = 1,
                                               .ticksRemaining = 10,
                                               .categories = strategic}};
        CHECK(rm::sim::fireWeapons(roster.store, roster.catalog, armies, strategicShot,
                                    roster.rate) == 0);
        CHECK(strategicShot.size() == 1);
    }

    SECTION("an untargetable tactical is refused despite the allow list") {
        Roster roster;
        (void)roster.add(roster.addType(gunnerDef(restrictedGun(
                            std::vector<std::string>{"TACTICAL", "MISSILE"}))),
                         0.0f, 0.0f, 0, 100.0f);
        // Sorted: MISSILE < TACTICAL < UNTARGETABLE.
        const std::vector<std::string> cloaked{"MISSILE", "TACTICAL", "UNTARGETABLE"};
        std::vector<Projectile> shots{{.position = rm::test::at(0, 4, 50),
                                       .firedByArmy = 1,
                                       .ticksRemaining = 10,
                                       .categories = cloaked}};
        CHECK(rm::sim::fireWeapons(roster.store, roster.catalog, armies, shots,
                                    roster.rate) == 0);
        CHECK(shots.size() == 1);
    }

    SECTION("an ordinary shell with no categories never triggers an allow list") {
        Roster roster;
        (void)roster.add(roster.addType(gunnerDef(restrictedGun(
                            std::vector<std::string>{"TACTICAL", "MISSILE"}))),
                         0.0f, 0.0f, 0, 100.0f);
        std::vector<Projectile> shots{{.position = rm::test::at(0, 4, 50),
                                       .firedByArmy = 1,
                                       .ticksRemaining = 10}};
        CHECK(rm::sim::fireWeapons(roster.store, roster.catalog, armies, shots,
                                    roster.rate) == 0);
        CHECK(shots.size() == 1);
    }

    SECTION("an SMD-pattern allow list takes the strategic and refuses the tactical") {
        Roster roster;
        (void)roster.add(roster.addType(gunnerDef(restrictedGun(
                            std::vector<std::string>{"STRATEGIC", "MISSILE"}))),
                         0.0f, 0.0f, 0, 100.0f);
        std::vector<Projectile> strategicShot{{.position = rm::test::at(0, 4, 50),
                                               .firedByArmy = 1,
                                               .ticksRemaining = 10,
                                               .categories = strategic}};
        CHECK(rm::sim::fireWeapons(roster.store, roster.catalog, armies, strategicShot,
                                    roster.rate) == 1);
        std::vector<Projectile> tacticalShot{{.position = rm::test::at(0, 4, 50),
                                              .firedByArmy = 1,
                                              .ticksRemaining = 10,
                                              .categories = tactical}};
        CHECK(rm::sim::fireWeapons(roster.store, roster.catalog, armies, tacticalShot,
                                    roster.rate) == 0);
        CHECK(tacticalShot.size() == 1);
    }
}

TEST_CASE("a positive interceptor contact consumes both projectiles") {
    const std::vector<Army> armies = rm::sim::freeForAll(2);
    Roster roster;
    Weapon pointDefence = directFire(10.0f, 300.0f);
    pointDefence.targetsProjectiles = true;
    (void)roster.add(roster.addType(gunnerDef(pointDefence)), 0.0f, 0.0f, 0, 100.0f);

    std::vector<Projectile> shots{{.position = rm::test::at(0, 4, 10),
                                   .damage = rm::unitdef::flatDamage(rm::test::mag(10.0f)),
                                   .firedByArmy = 1,
                                   .ticksRemaining = 10}};
    REQUIRE(rm::sim::fireWeapons(roster.store, roster.catalog, armies, shots,
                                  roster.rate) == 1);
    REQUIRE(shots.size() == 2);

    rm::sim::advanceProjectiles(shots, roster.store, armies,
                                rm::sim::Terrain{flatField()}, roster.rate, nullptr,
                                &roster.catalog);
    CHECK(shots.empty());
}

TEST_CASE("an interceptor removes the nearest projectile on its sweep") {
    const std::vector<Army> armies = rm::sim::freeForAll(2);
    Roster roster;

    Projectile far{.position = rm::test::at(0, 0, 15), .firedByArmy = 1, .ticksRemaining = 2};
    Projectile near{.position = rm::test::at(0, 0, 5), .firedByArmy = 1, .ticksRemaining = 2};
    Projectile interceptor{.position = rm::test::at(0, 0, 0),
                           .velocity = rm::test::at(0, 0, 20),
                           .damage = rm::unitdef::flatDamage(rm::test::mag(10.0f)),
                           .firedByArmy = 0,
                           .interceptor = true,
                           .ticksRemaining = 2};
    std::vector<Projectile> shots{far, near, interceptor};

    rm::sim::advanceProjectiles(shots, roster.store, armies,
                                rm::sim::Terrain{flatField(-100.0f)}, roster.rate, nullptr,
                                &roster.catalog);
    REQUIRE(shots.size() == 1);
    CHECK(shots.front().position == far.position);
}

TEST_CASE("interception uses projectile positions from the start of the tick") {
    const std::vector<Army> armies = rm::sim::freeForAll(2);
    Roster roster;
    Projectile hostile{.position = rm::test::at(0, 0, 5),
                       .velocity = rm::test::at(0, 0, 20),
                       .firedByArmy = 1,
                       .ticksRemaining = 2};
    Projectile interceptor{.position = rm::test::at(0, 0, 0),
                           .velocity = rm::test::at(0, 0, 20),
                           .damage = rm::unitdef::flatDamage(rm::test::mag(10.0f)),
                           .firedByArmy = 0,
                           .interceptor = true,
                           .ticksRemaining = 2};
    const auto advance = [&](std::vector<Projectile> shots) {
        rm::sim::advanceProjectiles(shots, roster.store, armies,
                                    rm::sim::Terrain{flatField(-100.0f)}, roster.rate, nullptr,
                                    &roster.catalog);
        return shots;
    };

    CHECK(advance({hostile, interceptor}).empty());
    CHECK(advance({interceptor, hostile}).empty());
}

TEST_CASE("interception considers a shot in flight before its terrain impact") {
    const std::vector<Army> armies = rm::sim::freeForAll(2);
    Roster roster;
    Projectile hostile{.position = rm::test::at(0, 10, 5),
                       .velocity = rm::test::at(0, -20, 0),
                       .firedByArmy = 1,
                       .ticksRemaining = 2};
    Projectile interceptor{.position = rm::test::at(0, 10, 0),
                           .velocity = rm::test::at(0, 0, 20),
                           .damage = rm::unitdef::flatDamage(rm::test::mag(10.0f)),
                           .firedByArmy = 0,
                           .interceptor = true,
                           .ticksRemaining = 2};
    const auto advance = [&](std::vector<Projectile> shots) {
        rm::sim::advanceProjectiles(shots, roster.store, armies,
                                    rm::sim::Terrain{flatField()}, roster.rate, nullptr,
                                    &roster.catalog);
        return shots;
    };

    CHECK(advance({hostile, interceptor}).empty());
    CHECK(advance({interceptor, hostile}).empty());
}

TEST_CASE("an already-consumed projectile cannot consume another interceptor") {
    const std::vector<Army> armies = rm::sim::freeForAll(2);
    Roster roster;
    Projectile hostile{.position = rm::test::at(0, 0, 5), .firedByArmy = 1,
                       .ticksRemaining = 2};
    Projectile first{.position = rm::test::at(0, 0, 0),
                     .velocity = rm::test::at(0, 0, 20),
                     .damage = rm::unitdef::flatDamage(rm::test::mag(10.0f)),
                     .firedByArmy = 0,
                     .interceptor = true,
                     .ticksRemaining = 2};
    Projectile second = first;
    std::vector<Projectile> shots{hostile, first, second};

    rm::sim::advanceProjectiles(shots, roster.store, armies,
                                rm::sim::Terrain{flatField(-100.0f)}, roster.rate, nullptr,
                                &roster.catalog);
    REQUIRE(shots.size() == 1);
    CHECK(shots.front().interceptor);
}

TEST_CASE("an unturreted point defence weapon aims before it fires") {
    const std::vector<Army> armies = rm::sim::freeForAll(2);
    Roster roster;
    Weapon pointDefence = directFire(10.0f, 300.0f);
    pointDefence.targetsProjectiles = true;
    pointDefence.turreted = false;
    pointDefence.firingToleranceBrads = rm::unitdef::firingToleranceBradsFromDegrees(2.0f);
    const UnitId gunner =
        roster.add(roster.addType(gunnerDef(pointDefence)), 0.0f, 0.0f, 0, 100.0f);
    roster.motion(gunner).turnPerTick = rm::sim::kBradQuarterTurn;

    std::vector<Projectile> shots{{.position = rm::test::at(50, 4, 0),
                                   .firedByArmy = 1,
                                   .ticksRemaining = 10}};
    CHECK(rm::sim::fireWeapons(roster.store, roster.catalog, armies, shots, roster.rate) == 0);
    CHECK(rm::sim::aimAtTargets(roster.store, roster.catalog, armies, nullptr, &shots) == 1);
    CHECK(rm::sim::fireWeapons(roster.store, roster.catalog, armies, shots, roster.rate) == 1);
}

TEST_CASE("a unit shoots the nearest enemy and never a friend") {
    const std::vector<Army> armies = rm::sim::freeForAll(2);

    Roster roster;
    const rm::UnitTypeIndex type = roster.addType(targetDef());
    (void)roster.add(type, 0.0f, 0.0f, 0, 100.0f);
    (void)roster.add(type, 10.0f, 0.0f, 0, 100.0f);  // an ALLY, nearer than any enemy

    (void)roster.add(type, 0.0f, 200.0f, 1, 100.0f);            // far
    const UnitId near = roster.add(type, 0.0f, 50.0f, 1, 100.0f);  // near
    (void)roster.add(type, 0.0f, 900.0f, 1, 100.0f);            // out of range

    const Weapon weapon = directFire(10.0f, 300.0f);

    const auto target = rm::sim::nearestTarget(rm::test::at(0, 0, 0), 0, weapon, roster.store, armies);
    REQUIRE(target.has_value());
    CHECK(*target == near);  // the near enemy, not the nearer ally
}

TEST_CASE("automatic acquisition skips BENIGN enemies") {
    const std::vector<Army> armies = rm::sim::freeForAll(2);
    Roster roster;

    UnitDef benign = targetDef();
    benign.name = "benign_target";
    benign.categories = {"BENIGN"};
    const UnitId nearBenign =
        roster.add(roster.addType(benign), 0.0f, 50.0f, 1, 100.0f);
    const UnitId farHostile =
        roster.add(roster.addType(targetDef()), 0.0f, 100.0f, 1, 100.0f);

    const Weapon weapon = directFire(10.0f, 300.0f);
    const auto target = rm::sim::nearestTarget(rm::test::at(0, 0, 0), 0, weapon,
                                                roster.store, armies, nullptr,
                                                &roster.catalog);

    REQUIRE(target.has_value());
    CHECK(*target == farHostile);
    CHECK(*target != nearBenign);
}

TEST_CASE("automatic acquisition skips DoNotTarget enemies") {
    const std::vector<Army> armies = rm::sim::freeForAll(2);
    Roster roster;
    const rm::UnitTypeIndex type = roster.addType(targetDef());
    const UnitId near = roster.add(type, 0.0f, 50.0f, 1, 100.0f);
    const UnitId far = roster.add(type, 0.0f, 100.0f, 1, 100.0f);
    const Weapon weapon = directFire(10.0f, 300.0f);

    REQUIRE(roster.store.setDoNotTarget(near, true));
    CHECK(rm::sim::nearestTarget(rm::test::at(0, 0, 0), 0, weapon, roster.store, armies) == far);

    REQUIRE(roster.store.setDoNotTarget(near, false));
    CHECK(rm::sim::nearestTarget(rm::test::at(0, 0, 0), 0, weapon, roster.store, armies) == near);
}

TEST_CASE("automatic acquisition skips a unit an own-side engineer is reclaiming (C-157)") {
    // Retail's IsTargetExempt, step 11 of FindBestEnemy: what my side is taking apart is not
    // shot, whether it would be the new target or the incumbent. An enemy's own reclaim of
    // its unit exempts nothing for us.
    std::vector<Army> armies = rm::sim::freeForAll(3);
    armies[2].alliance = armies[0].alliance;  // army 2 is our ally
    Roster roster;
    const rm::UnitTypeIndex type = roster.addType(targetDef());
    const UnitId near = roster.add(type, 0.0f, 50.0f, 1, 100.0f);
    const UnitId far = roster.add(type, 0.0f, 100.0f, 1, 100.0f);
    const Weapon weapon = directFire(10.0f, 300.0f);
    const auto pick = [&](std::span<const rm::sim::WorkClaim> claims,
                          std::optional<UnitId> incumbent = std::nullopt) {
        return rm::sim::nearestTarget(rm::test::at(0, 0, 0), 0, weapon, roster.store, armies,
                                      nullptr, nullptr, std::nullopt, incumbent, nullptr, claims);
    };

    const std::array<rm::sim::WorkClaim, 1> ours{{{.target = near.index, .workerArmy = 0}}};
    CHECK(pick(ours) == far);
    const std::array<rm::sim::WorkClaim, 1> allied{{{.target = near.index, .workerArmy = 2}}};
    CHECK(pick(allied) == far);
    const std::array<rm::sim::WorkClaim, 1> theirs{{{.target = near.index, .workerArmy = 1}}};
    CHECK(pick(theirs) == near);
    CHECK(pick({}) == near);
    // The incumbent cannot remain one once our engineer starts on it.
    CHECK(pick({}, near) == near);
    CHECK(pick(ours, near) == far);
}

TEST_CASE("automatic acquisition rejects a closer target outside the playable rectangle") {
    const std::vector<Army> armies = rm::sim::freeForAll(2);
    Roster roster;
    const rm::UnitTypeIndex type = roster.addType(targetDef());
    const UnitId outside = roster.add(type, 20.0f, 0.0f, 1, 100.0f);
    const UnitId inside = roster.add(type, 0.0f, 50.0f, 1, 100.0f);
    const rm::sim::PlayableRect playable{
        .minX = rm::test::fx(-10.0f),
        .maxX = rm::test::fx(10.0f),
        .minZ = rm::test::fx(-10.0f),
        .maxZ = rm::test::fx(100.0f),
    };

    CHECK(rm::sim::nearestTarget(rm::test::at(0, 0, 0), 0, directFire(10.0f, 300.0f),
                                 roster.store, armies, nullptr, &roster.catalog, std::nullopt,
                                 std::nullopt, &playable)
          == inside);
    CHECK(outside != inside);
}

TEST_CASE("automatic acquisition denies weapons with no target priorities") {
    const std::vector<Army> armies = rm::sim::freeForAll(2);
    Roster roster;
    const rm::UnitTypeIndex type = roster.addType(targetDef());
    (void)roster.add(type, 0.0f, 50.0f, 1, 100.0f);
    Weapon weapon = directFire(10.0f, 300.0f);
    weapon.targetPriorities.clear();

    CHECK_FALSE(rm::sim::nearestTarget(rm::test::at(0, 0, 0), 0, weapon, roster.store, armies,
                                        nullptr, &roster.catalog));
    CHECK_FALSE(rm::sim::nearestTarget(rm::test::at(0, 0, 0), 0, weapon, roster.store, armies));
}

TEST_CASE("automatic acquisition penalizes targets outside a weapon firing arc") {
    const std::vector<Army> armies = rm::sim::freeForAll(2);
    Roster roster;
    const rm::UnitTypeIndex type = roster.addType(targetDef());
    const UnitId outside = roster.add(type, 10.0f, 0.0f, 1, 100.0f);
    const UnitId within = roster.add(type, 0.0f, 15.0f, 1, 100.0f);
    Weapon weapon = directFire(10.0f, 300.0f);
    weapon.arcRangeDegrees = 30.0f;
    weapon.arcRangeBrads = rm::unitdef::arcRangeBradsFromDegrees(weapon.arcRangeDegrees);

    // The nearer side target scores 4 * 10^2 outside the arc; the 15-elmo forward target
    // scores 15^2 in it, so retail's class penalty makes the farther target win.
    CHECK(rm::sim::nearestTarget(rm::test::at(0, 0, 0), 0, weapon, roster.store, armies,
                                 nullptr, nullptr, rm::Brad{})
          == within);

    weapon.arcRangeDegrees = 180.0f;
    weapon.arcRangeBrads = rm::unitdef::arcRangeBradsFromDegrees(weapon.arcRangeDegrees);
    CHECK(rm::sim::nearestTarget(rm::test::at(0, 0, 0), 0, weapon, roster.store, armies,
                                 nullptr, nullptr, rm::Brad{})
          == outside);
}

TEST_CASE("a firing arc centre rotates automatic acquisition around the hull heading") {
    const std::vector<Army> armies = rm::sim::freeForAll(2);
    Roster roster;
    const rm::UnitTypeIndex type = roster.addType(targetDef());
    const UnitId forward = roster.add(type, 0.0f, 10.0f, 1, 100.0f);
    const UnitId right = roster.add(type, 15.0f, 0.0f, 1, 100.0f);
    Weapon weapon = directFire(10.0f, 300.0f);
    weapon.arcCentreDegrees = 90.0f;
    weapon.arcRangeDegrees = 30.0f;
    weapon.arcCentreBrads = rm::unitdef::arcCentreBradsFromDegrees(weapon.arcCentreDegrees);
    weapon.arcRangeBrads = rm::unitdef::arcRangeBradsFromDegrees(weapon.arcRangeDegrees);

    CHECK(rm::sim::nearestTarget(rm::test::at(0, 0, 0), 0, weapon, roster.store, armies,
                                 nullptr, nullptr, rm::Brad{})
          == right);
    CHECK(rm::sim::nearestTarget(rm::test::at(0, 0, 0), 0, weapon, roster.store, armies,
                                 nullptr, nullptr, rm::Brad{})
          != forward);

    weapon.arcCentreDegrees = 0.0f;
    weapon.arcCentreBrads = rm::unitdef::arcCentreBradsFromDegrees(weapon.arcCentreDegrees);
    CHECK(rm::sim::nearestTarget(rm::test::at(0, 0, 0), 0, weapon, roster.store, armies,
                                 nullptr, nullptr, static_cast<rm::Brad>(16384))
          == right);  // +90 degrees of hull heading points along +X.

    weapon.arcCentreDegrees = 45.0f;
    weapon.arcCentreBrads = rm::unitdef::arcCentreBradsFromDegrees(weapon.arcCentreDegrees);
    CHECK(rm::sim::nearestTarget(rm::test::at(0, 0, 0), 0, weapon, roster.store, armies,
                                 nullptr, nullptr, static_cast<rm::Brad>(8192))
          == right);  // 45 degrees of hull heading plus 45 degrees of weapon offset is +X.
}

TEST_CASE("automatic acquisition applies category target restrictions before ranking") {
    const std::vector<Army> armies = rm::sim::freeForAll(2);
    Roster roster;

    UnitDef naval = targetDef();
    naval.name = "naval_target";
    naval.categories = {"NAVAL"};
    UnitDef land = targetDef();
    land.name = "land_target";
    land.categories = {"LAND"};
    const UnitId nearNaval = roster.add(roster.addType(naval), 0.0f, 50.0f, 1, 100.0f);
    const UnitId farLand = roster.add(roster.addType(land), 0.0f, 100.0f, 1, 100.0f);

    Weapon weapon = directFire(10.0f, 300.0f);
    weapon.targetPriorities = {{"NAVAL"}, {"LAND"}};

    SECTION("only-allow selects an eligible NAVAL target over a nearer non-NAVAL target") {
        roster.transform(nearNaval).z = rm::test::fx(100.0f);
        roster.transform(farLand).z = rm::test::fx(50.0f);
        roster.reindex();
        weapon.targetPriorities = {{"LAND"}, {"NAVAL"}};
        weapon.targetRestrictOnlyAllow = std::vector<std::string>{"NAVAL"};
        CHECK(rm::sim::nearestTarget(rm::test::at(0, 0, 0), 0, weapon, roster.store, armies,
                                     nullptr, &roster.catalog)
              == nearNaval);
    }

    SECTION("only-disallow rejects a higher-ranked NAVAL target for another eligible target") {
        weapon.targetRestrictOnlyDisallow = std::vector<std::string>{"NAVAL"};
        CHECK(rm::sim::nearestTarget(rm::test::at(0, 0, 0), 0, weapon, roster.store, armies,
                                     nullptr, &roster.catalog)
              == farLand);
    }
}

TEST_CASE("target ranking preserves distances below hypotenuse quantization") {
    const std::vector<Army> armies = rm::sim::freeForAll(2);
    Roster roster;
    const rm::UnitTypeIndex type = roster.addType(targetDef());

    // At fifty elmos, the fixed-point hypotenuse rounds this 1/64-elmo offset away.
    // Retail compares squared distances before any square root, so the later slot wins.
    const UnitId farther = roster.add(type, 1.0f / 64.0f, 50.0f, 1, 100.0f);
    const UnitId nearer = roster.add(type, 0.0f, 50.0f, 1, 100.0f);

    const Weapon weapon = directFire(10.0f, 100.0f);
    const auto target =
        rm::sim::nearestTarget(rm::test::at(0, 0, 0), 0, weapon, roster.store, armies);

    REQUIRE(target.has_value());
    CHECK(*target == nearer);

    roster.transform(farther).x = rm::test::fx(0.0f);
    roster.reindex();
    const auto tied =
        rm::sim::nearestTarget(rm::test::at(0, 0, 0), 0, weapon, roster.store, armies);
    REQUIRE(tied.has_value());
    CHECK(*tied == farther);
}

TEST_CASE("a weapon acquires targets only on its allowed movement layer") {
    const std::vector<Army> armies = rm::sim::freeForAll(2);
    Roster roster;

    UnitDef surface = targetDef();
    surface.name = "surface_target";
    UnitDef aircraft = targetDef();
    aircraft.name = "air_target";
    aircraft.motion = rm::unitdef::MotionType::Air;
    const rm::UnitTypeIndex surfaceType = roster.addType(surface);
    const rm::UnitTypeIndex airType = roster.addType(aircraft);
    const UnitId nearSurface = roster.add(surfaceType, 0.0f, 50.0f, 1, 100.0f);
    const UnitId farAir = roster.add(airType, 0.0f, 100.0f, 1, 100.0f);

    Weapon weapon = directFire(10.0f, 300.0f);
    weapon.targetLayers = rm::unitdef::TargetLayerMask::Air;
    CHECK(rm::sim::nearestTarget(rm::test::at(0, 0, 0), 0, weapon, roster.store, armies)
          == farAir);

    weapon.targetLayers = rm::unitdef::TargetLayerMask::Surface;
    CHECK(rm::sim::nearestTarget(rm::test::at(0, 0, 0), 0, weapon, roster.store, armies)
          == nearSurface);
}

TEST_CASE("hull aiming does not combine one weapon's range with another's target layer") {
    const std::vector<Army> armies = rm::sim::freeForAll(2);
    Roster roster;

    Weapon longSurface = directFire(10.0f, 300.0f);
    longSurface.targetLayers = rm::unitdef::TargetLayerMask::Surface;
    Weapon shortAir = directFire(10.0f, 100.0f);
    shortAir.targetLayers = rm::unitdef::TargetLayerMask::Air;
    UnitDef gunner = gunnerDef(longSurface);
    gunner.weapons.push_back(shortAir);
    const UnitId shooter = roster.add(roster.addType(gunner), 0.0f, 0.0f, 0, 100.0f);

    UnitDef aircraft = targetDef();
    aircraft.motion = rm::unitdef::MotionType::Air;
    (void)roster.add(roster.addType(aircraft), 200.0f, 0.0f, 1, 100.0f);

    CHECK(rm::sim::aimAtTargets(roster.store, roster.catalog, armies) == 0);
    CHECK(roster.transform(shooter).heading == rm::Brad{0});
}

TEST_CASE("a unit does not shoot what its side cannot see") {
    // The bug ADR-037 was written to fix, now a test. Until intel existed this pass picked
    // from the whole store filtered by hostility and range, so every unit in the match
    // engaged targets on the far side of a hill it had no way of knowing were there.
    const std::vector<Army> armies = rm::sim::freeForAll(2);

    Roster roster;
    const rm::UnitTypeIndex type = roster.addType(targetDef());
    (void)roster.add(type, 0.0f, 0.0f, 0, 100.0f);
    const UnitId enemy = roster.add(type, 0.0f, 50.0f, 1, 100.0f);

    const Weapon weapon = directFire(10.0f, 300.0f);

    // With no intel at all — every scene that predates this — the enemy is a target.
    const auto blind = rm::sim::nearestTarget(rm::test::at(0, 0, 0), 0, weapon, roster.store,
                                              armies, nullptr);
    REQUIRE(blind.has_value());
    CHECK(*blind == enemy);

    // Configured but with nothing lighting the enemy's ground: no target, even though it is
    // hostile, alive, and well within range.
    rm::sim::Intel intel;
    intel.configure(2, rm::sim::Fx::fromInt(512), rm::sim::Fx::fromInt(512),
                    rm::sim::VisionStyle::ForgedAlliance);
    const auto unseen = rm::sim::nearestTarget(rm::test::at(0, 0, 0), 0, weapon, roster.store,
                                               armies, &intel, &roster.catalog);
    CHECK_FALSE(unseen.has_value());

    // And once army 0 puts something out there that CAN see, it engages. Through the pass
    // rather than by poking the grid: what is being checked is the path a match takes.
    UnitDef scoutDef = targetDef();
    scoutDef.name = "test_scout";
    scoutDef.visionRadiusElmos = 120.0f;
    const rm::UnitTypeIndex scoutType = roster.addType(scoutDef);
    (void)roster.add(scoutType, 0.0f, 20.0f, 0, 100.0f);

    intel.update(roster.store, roster.catalog, armies, nullptr);
    const auto seen = rm::sim::nearestTarget(rm::test::at(0, 0, 0), 0, weapon, roster.store,
                                             armies, &intel, &roster.catalog);
    REQUIRE(seen.has_value());
    CHECK(*seen == enemy);
}

TEST_CASE("radar contacts acquire by score until vision has identified them") {
    const std::vector<Army> armies = rm::sim::freeForAll(2);
    Roster roster;

    UnitDef radar = targetDef();
    radar.name = "radar";
    radar.radarRadiusElmos = 300.0f;
    (void)roster.add(roster.addType(radar), 0.0f, 0.0f, 0, 100.0f);

    UnitDef highPriority = targetDef();
    highPriority.name = "high_priority";
    highPriority.categories = {"HIGH", "LAND"};
    const UnitId farHigh = roster.add(roster.addType(highPriority), 0.0f, 200.0f, 1, 100.0f);
    const UnitId nearLand = roster.add(roster.addType(targetDef()), 0.0f, 50.0f, 1, 100.0f);

    Weapon weapon = directFire(10.0f, 300.0f);
    weapon.targetPriorities = {{"HIGH"}, {"LAND"}};

    rm::sim::Intel intel;
    intel.configure(2, rm::sim::Fx::fromInt(512), rm::sim::Fx::fromInt(512),
                    rm::sim::VisionStyle::ForgedAlliance);
    intel.update(roster.store, roster.catalog, armies, nullptr);

    // Radar is enough to acquire a real unit, but the blip has not revealed whether it is HIGH.
    CHECK(rm::sim::nearestTarget(rm::test::at(0, 0, 0), 0, weapon, roster.store, armies,
                                 &intel, &roster.catalog)
          == nearLand);

    UnitDef scout = targetDef();
    scout.name = "scout";
    scout.visionRadiusElmos = 60.0f;
    const UnitId observer = roster.add(roster.addType(scout), 0.0f, 200.0f, 0, 100.0f);
    intel.update(roster.store, roster.catalog, armies, nullptr);

    // Once the high-priority target leaves sight, its current radar contact still carries the
    // visual-identification latch that retail calls RECON_LOSEver.
    roster.transform(observer).z = rm::test::fx(400.0f);
    intel.update(roster.store, roster.catalog, armies, nullptr);
    CHECK(rm::sim::nearestTarget(rm::test::at(0, 0, 0), 0, weapon, roster.store, armies,
                                 &intel, &roster.catalog)
          == farHigh);
}

TEST_CASE("vision and radar do not see a submerged submarine", "[intel][naval]") {
    const std::vector<Army> armies = rm::sim::freeForAll(2);
    Roster roster;

    UnitDef watcher = targetDef();
    watcher.name = "watcher";
    watcher.visionRadiusElmos = 150.0f;
    watcher.radarRadiusElmos = 400.0f;
    watcher.sonarRadiusElmos = 400.0f;
    (void)roster.add(roster.addType(watcher), 0.0f, 0.0f, 0, 100.0f);

    const UnitId submerged =
        roster.add(roster.addType(targetDef()), 0.0f, 100.0f, 1, 100.0f);
    roster.motion(submerged).submersible = true;
    roster.motion(submerged).submerged = true;
    const UnitId surfaced =
        roster.add(roster.addType(targetDef()), 0.0f, 120.0f, 1, 100.0f);
    roster.motion(surfaced).submersible = true;

    rm::sim::Intel intel;
    intel.configure(2, rm::sim::Fx::fromInt(512), rm::sim::Fx::fromInt(512),
                    rm::sim::VisionStyle::ForgedAlliance);
    intel.update(roster.store, roster.catalog, armies, nullptr);

    // Both hulls stand inside vision and radar range; only sonar names the sunk one.
    CHECK(rm::sim::contactKindForUnit(0, submerged.index, roster.store, roster.catalog,
                                      armies, intel)
          == rm::sim::ContactKind::Sonar);
    CHECK(rm::sim::contactKindForUnit(0, surfaced.index, roster.store, roster.catalog,
                                      armies, intel)
          == rm::sim::ContactKind::Seen);
}

TEST_CASE("sonar hears only naval hulls", "[intel][naval]") {
    const std::vector<Army> armies = rm::sim::freeForAll(2);
    Roster roster;

    UnitDef watcher = targetDef();
    watcher.name = "watcher";
    watcher.visionRadiusElmos = 150.0f;
    watcher.sonarRadiusElmos = 400.0f;
    (void)roster.add(roster.addType(watcher), 0.0f, 0.0f, 0, 100.0f);

    const UnitId tank = roster.add(roster.addType(targetDef()), 0.0f, 300.0f, 1, 100.0f);
    const UnitId ship = roster.add(roster.addType(targetDef()), 0.0f, 320.0f, 1, 100.0f);
    roster.motion(ship).surfaceWater = true;

    rm::sim::Intel intel;
    intel.configure(2, rm::sim::Fx::fromInt(512), rm::sim::Fx::fromInt(512),
                    rm::sim::VisionStyle::ForgedAlliance);
    intel.update(roster.store, roster.catalog, armies, nullptr);

    CHECK_FALSE(rm::sim::contactKindForUnit(0, tank.index, roster.store, roster.catalog,
                                            armies, intel)
                    .has_value());
    CHECK(rm::sim::contactKindForUnit(0, ship.index, roster.store, roster.catalog,
                                      armies, intel)
          == rm::sim::ContactKind::Sonar);
}

TEST_CASE("torpedoes acquire sonar contacts, surface guns do not", "[intel][naval]") {
    const std::vector<Army> armies = rm::sim::freeForAll(2);
    Roster roster;

    UnitDef watcher = targetDef();
    watcher.name = "watcher";
    watcher.sonarRadiusElmos = 400.0f;
    (void)roster.add(roster.addType(watcher), 0.0f, 0.0f, 0, 100.0f);

    UnitDef hull = targetDef();
    hull.name = "hull";
    hull.categories = {"NAVAL"};
    const UnitId contact =
        roster.add(roster.addType(hull), 0.0f, 300.0f, 1, 100.0f);
    roster.motion(contact).submersible = true;
    roster.motion(contact).submerged = true;

    Weapon torpedo = directFire(10.0f, 500.0f);
    torpedo.targetPriorities = {{"NAVAL"}};
    torpedo.targetsSubmerged = true;
    Weapon surfaceGun = directFire(10.0f, 500.0f);
    surfaceGun.targetPriorities = {{"NAVAL"}};
    surfaceGun.targetsSubmerged = false;

    rm::sim::Intel intel;
    intel.configure(2, rm::sim::Fx::fromInt(512), rm::sim::Fx::fromInt(512),
                    rm::sim::VisionStyle::ForgedAlliance);
    intel.update(roster.store, roster.catalog, armies, nullptr);
    REQUIRE(rm::sim::contactKindForUnit(0, contact.index, roster.store, roster.catalog,
                                        armies, intel)
            == rm::sim::ContactKind::Sonar);

    CHECK(rm::sim::nearestTarget(rm::test::at(0, 0, 0), 0, torpedo, roster.store, armies,
                                 &intel, &roster.catalog)
          == contact);
    CHECK_FALSE(rm::sim::nearestTarget(rm::test::at(0, 0, 0), 0, surfaceGun, roster.store,
                                       armies, &intel, &roster.catalog)
                    .has_value());
}

TEST_CASE("automatic projectile fire aims at a live radar contact's deterministic blip") {
    const std::vector<Army> armies = rm::sim::freeForAll(2);
    Roster roster;

    Weapon weapon = directFire(10.0f, 300.0f);
    UnitDef gunner = gunnerDef(weapon);
    gunner.radarRadiusElmos = 300.0f;
    const UnitId shooter =
        roster.add(roster.addType(gunner), 0.0f, 0.0f, 0, 100.0f);
    // This case calls the separate aim and fire passes once. A full-turn allowance makes that
    // precondition independent of the blip's deterministic lateral offset.
    roster.motion(shooter).turnPerTick = rm::sim::kBradHalfTurn;
    const UnitId target =
        roster.add(roster.addType(targetDef()), 0.0f, 100.0f, 1, 100.0f);

    rm::sim::Intel intel;
    intel.configure(2, rm::sim::Fx::fromInt(512), rm::sim::Fx::fromInt(512),
                    rm::sim::VisionStyle::ForgedAlliance);
    intel.update(roster.store, roster.catalog, armies, nullptr);

    constexpr rm::TickIndex tick = 3;
    std::vector<Projectile> shots;
    REQUIRE(rm::sim::aimAtTargets(roster.store, roster.catalog, armies, &intel, nullptr, nullptr,
                                  tick, roster.rate)
            == 1);
    REQUIRE(rm::sim::fireWeapons(roster.store, roster.catalog, armies, shots, roster.rate,
                                  nullptr, &intel, nullptr, tick)
            == 1);
    REQUIRE(shots.size() == 1);
    REQUIRE(roster.health(shooter).automaticTargets[0] == target);

    const auto [blipX, blipZ] = rm::sim::radarBlipPosition(
        target, roster.transform(target).x, roster.transform(target).z, tick, roster.rate);
    const Projectile expected = rm::sim::launch(
        rm::sim::positionOf(roster.transform(shooter)),
        {blipX, roster.transform(target).y, blipZ}, weapon, 0, roster.rate,
        roster.catalog.weaponRates(roster.store.typeAt(shooter.index), 0).muzzlePerTick,
        roster.catalog.weaponRates(roster.store.typeAt(shooter.index), 0).damage, shooter);
    CHECK(shots.front().velocity == expected.velocity);
}

TEST_CASE("automatic targeting scores radar-only contacts at their blip, not truth",
          "[intel][targeting]") {
    const std::vector<Army> armies = rm::sim::freeForAll(2);
    Roster roster;

    Weapon weapon = directFire(10.0f, 500.0f);
    UnitDef gunner = gunnerDef(weapon);
    gunner.radarRadiusElmos = 400.0f; // vision stays zero: every contact is radar-only
    (void)roster.add(roster.addType(gunner), 0.0f, 0.0f, 0, 100.0f);
    const UnitId near =
        roster.add(roster.addType(targetDef()), 0.0f, 150.0f, 1, 100.0f);
    const UnitId far =
        roster.add(roster.addType(targetDef()), 0.0f, 170.0f, 1, 100.0f);

    rm::sim::Intel intel;
    intel.configure(2, rm::sim::Fx::fromInt(512), rm::sim::Fx::fromInt(512),
                    rm::sim::VisionStyle::ForgedAlliance);
    intel.update(roster.store, roster.catalog, armies, nullptr);
    REQUIRE(rm::sim::contactKindForUnit(0, near.index, roster.store, roster.catalog,
                                        armies, intel)
            == rm::sim::ContactKind::Radar);
    REQUIRE(rm::sim::contactKindForUnit(0, far.index, roster.store, roster.catalog,
                                        armies, intel)
            == rm::sim::ContactKind::Radar);

    // Truth always ranks the nearer contact first. The blip wanders up to 96 elmos,
    // so some tick ranks them the other way round — that tick proves the score reads
    // the blip, because nothing else moves. The search is deterministic: the blip is
    // a pure function of unit, tick and rate.
    rm::TickIndex flipTick = 0;
    bool flipped = false;
    for (rm::TickIndex tick = 0; tick < 400 && !flipped; ++tick) {
        const auto [nearX, nearZ] = rm::sim::radarBlipPosition(
            near, roster.transform(near).x, roster.transform(near).z, tick, roster.rate);
        const auto [farX, farZ] = rm::sim::radarBlipPosition(
            far, roster.transform(far).x, roster.transform(far).z, tick, roster.rate);
        if (farX * farX + farZ * farZ < nearX * nearX + nearZ * nearZ) {
            flipTick = tick;
            flipped = true;
        }
    }
    REQUIRE(flipped);

    const auto acquired = rm::sim::nearestTarget(
        rm::test::at(0, 0, 0), 0, weapon, roster.store, armies, &intel, &roster.catalog,
        std::nullopt, std::nullopt, nullptr, {}, std::nullopt, flipTick, roster.rate);
    REQUIRE(acquired.has_value());
    CHECK(*acquired == far); // truth-nearer loses: identity survives, rank follows the blip
}

TEST_CASE("automatic targeting does not acquire sonar-only contacts") {
    const std::vector<Army> armies = rm::sim::freeForAll(2);
    Roster roster;
    UnitDef sonar = targetDef();
    sonar.name = "sonar";
    sonar.sonarRadiusElmos = 300.0f;
    (void)roster.add(roster.addType(sonar), 0.0f, 0.0f, 0, 100.0f);
    const rm::UnitTypeIndex target = roster.addType(targetDef());
    (void)roster.add(target, 0.0f, 50.0f, 1, 100.0f);

    rm::sim::Intel intel;
    intel.configure(2, rm::sim::Fx::fromInt(512), rm::sim::Fx::fromInt(512),
                    rm::sim::VisionStyle::ForgedAlliance);
    intel.update(roster.store, roster.catalog, armies, nullptr);

    CHECK_FALSE(rm::sim::nearestTarget(rm::test::at(0, 0, 0), 0,
                                        directFire(10.0f, 300.0f), roster.store, armies,
                                        &intel, &roster.catalog));
}

TEST_CASE("automatic targeting obeys cloak, omni, and free-intel identity") {
    const std::vector<Army> armies = rm::sim::freeForAll(2);
    const Weapon weapon = directFire(10.0f, 300.0f);

    const auto targetFor = [&](bool omni, bool freeIntel) {
        Roster roster;
        UnitDef watcher = targetDef();
        watcher.visionRadiusElmos = 200.0f;
        watcher.omniRadiusElmos = omni ? 200.0f : 0.0f;
        (void)roster.add(roster.addType(watcher), 0.0f, 0.0f, 0, 100.0f);

        UnitDef hidden = targetDef();
        hidden.cloak = true;
        hidden.freeIntel = freeIntel;
        const UnitId enemy = roster.add(roster.addType(hidden), 0.0f, 50.0f, 1, 100.0f);

        rm::sim::Intel intel;
        intel.configure(2, rm::sim::Fx::fromInt(512), rm::sim::Fx::fromInt(512),
                        rm::sim::VisionStyle::ForgedAlliance);
        intel.update(roster.store, roster.catalog, armies, nullptr);
        return std::pair{rm::sim::nearestTarget(rm::test::at(0, 0, 0), 0, weapon,
                                                roster.store, armies, &intel, &roster.catalog),
                         enemy};
    };

    const auto cloaked = targetFor(false, false);
    const auto omni = targetFor(true, false);
    const auto freeIntel = targetFor(false, true);
    CHECK_FALSE(cloaked.first.has_value());
    CHECK(omni.first == omni.second);
    CHECK(freeIntel.first == freeIntel.second);
}

TEST_CASE("a dead enemy is not a target, and neither is a defeated army's unit") {
    std::vector<Army> armies = rm::sim::freeForAll(2);

    Roster roster;
    const rm::UnitTypeIndex type = roster.addType(targetDef());
    (void)roster.add(type, 0.0f, 0.0f, 0, 100.0f);
    (void)roster.add(type, 0.0f, 50.0f, 1, 0.0f);  // already dead
    const UnitId living = roster.add(type, 0.0f, 80.0f, 1, 100.0f);  // alive, further away

    const Weapon weapon = directFire(10.0f, 300.0f);

    auto target = rm::sim::nearestTarget(rm::test::at(0, 0, 0), 0, weapon, roster.store, armies);
    REQUIRE(target.has_value());
    CHECK(*target == living);  // skipped the corpse

    // And once the army has lost, nothing it owns draws fire — otherwise a winning force
    // keeps shooting a side that is already out.
    armies[1].defeated = true;
    CHECK_FALSE(
        rm::sim::nearestTarget(rm::test::at(0, 0, 0), 0, weapon, roster.store, armies).has_value());
}

TEST_CASE("a minimum range is a hole a unit can stand in") {
    // 87 weapons state one. Without it a unit walks up to an artillery piece and stands
    // in the one place it cannot be shot from — which is correct, and only correct if the
    // dead zone is honoured.
    Roster roster;
    const rm::UnitTypeIndex type = roster.addType(targetDef());
    (void)roster.add(type, 0.0f, 0.0f, 0, 100.0f);
    const UnitId hider = roster.add(type, 0.0f, 20.0f, 1, 100.0f);  // in the dead zone
    const std::vector<Army> armies = rm::sim::freeForAll(2);

    Weapon artillery = directFire(100.0f, 500.0f);
    artillery.minRange = rm::test::fx(100.0f);

    CHECK_FALSE(
        rm::sim::nearestTarget(rm::test::at(0, 0, 0), 0, artillery, roster.store, armies).has_value());

    // ...and the same weapon does reach something outside it.
    roster.transform(hider).z = rm::test::fx(200.0f);
    CHECK(rm::sim::nearestTarget(rm::test::at(0, 0, 0), 0, artillery, roster.store, armies).has_value());
}

TEST_CASE("a flat shot flies straight at its target") {
    const rm::sim::TickRate rate{};
    const Weapon weapon = directFire(10.0f, 500.0f);  // 100 elmos/s
    const Projectile shot =
        rm::sim::launch(rm::test::at(0, 0, 0), rm::test::at(0, 0, 200), weapon, 0, rate,
                        rate.perTick(weapon.muzzleVelocityElmosPerSecond),
                        rm::unitdef::flatDamage(weapon.damage));

    // Asserted PER SECOND, converted back from the per-tick velocity the projectile now
    // carries: "100 elmos a second" is the authored fact, and how far that is in a tick
    // depends on the clock.
    const auto perSecond = [&rate](rm::sim::Fx perTick) {
        return rm::test::asFloat(perTick) * static_cast<float>(rate.ticksPerSecond());
    };

    // Two seconds of flight over 200 elmos, so 100 elmos a second down +Z and nothing
    // sideways.
    CHECK(perSecond(shot.velocity[0]) == Approx(0.0f));
    CHECK(perSecond(shot.velocity[2]) == Approx(100.0f).margin(0.5f));
    CHECK(shot.arc == BallisticArc::None);

    // It leaves the MUZZLE, four elmos above the shooter's feet, and aims at the target's
    // middle two elmos above its own — so a flat shot at a target on the same ground
    // angles slightly DOWN rather than travelling level. Two elmos of drop over two
    // seconds is one a second.
    CHECK(rm::test::asFloat(shot.position[1])
          == Approx(rm::test::asFloat(rm::sim::kMuzzleHeight)));
    CHECK(perSecond(shot.velocity[1]) == Approx(-1.0f).margin(0.1f));
}

TEST_CASE("an arced shot rises, and comes down where the target is") {
    Weapon artillery = directFire(100.0f, 1000.0f);
    artillery.arc = BallisticArc::High;
    artillery.muzzleVelocityElmosPerSecond = 100.0f;

    const std::array<rm::sim::Fx, 3> from = rm::test::at(0, 0, 0);
    const std::array<rm::sim::Fx, 3> to = rm::test::at(0, 0, 300);
    const Projectile shot = rm::sim::launch(from, to, artillery, 0, rm::sim::TickRate{},
                        rm::sim::TickRate{}.perTick(artillery.muzzleVelocityElmosPerSecond),
                        rm::unitdef::flatDamage(artillery.damage));

    // It must LEAVE going up, which is the whole point of an arc — a flat shot at the
    // same target has a vertical velocity of zero.
    CHECK(shot.velocity[1] > rm::sim::Fx{});

    // And gravity must bring it down exactly there. Simulated tick by tick rather than
    // asserted from the formula, so the test checks the integration and not the algebra
    // it was derived from.
    std::vector<Projectile> flight{shot};
    rm::sim::UnitStore empty;
    const rm::HeightField field = flatField();

    int ticks = 0;
    while (!flight.empty() && ticks < 1000) {
        rm::sim::advanceProjectiles(flight, empty, {}, rm::sim::Terrain{field},
                                    rm::sim::TickRate{});
        ++ticks;
    }

    // It landed (the list is empty) rather than expiring at the lifetime cap.
    CHECK(flight.empty());
    CHECK(ticks < static_cast<int>(rm::sim::TickRate{}.ticks(
              rm::sim::kProjectileLifetime)));
}

TEST_CASE("an arced projectile advances by its average old and new velocity") {
    const rm::sim::TickRate rate{};
    const rm::sim::Fx gravity = rm::sim::projectileGravityPerTickSquared(rate);
    const rm::sim::Fx half = rm::sim::Fx::fromRatio(1, 2);

    Projectile shot;
    shot.position = rm::test::at(0, 100, 0);
    shot.velocity = rm::test::at(10, 20, 30);
    shot.arc = BallisticArc::High;
    shot.ticksRemaining = 2;
    std::vector<Projectile> flight{shot};
    rm::sim::UnitStore empty;

    rm::sim::advanceProjectiles(flight, empty, {}, rm::sim::Terrain{flatField()}, rate);

    REQUIRE(flight.size() == 1);
    // v_new = v_old - g; trapezoidal position uses (v_old + v_new) / 2,
    // which is v_old - g/2 for this constant-acceleration tick.
    const rm::sim::Fx oldVerticalVelocity = rm::test::fx(20.0f);
    const rm::sim::Fx newVerticalVelocity = oldVerticalVelocity - gravity;
    const rm::sim::Fx averageVerticalVelocity =
        oldVerticalVelocity + (newVerticalVelocity - oldVerticalVelocity) * half;
    CHECK(flight.front().velocity[1] == newVerticalVelocity);
    CHECK(flight.front().position[0] == rm::test::fx(10.0f));
    CHECK(flight.front().position[1]
          == rm::test::fx(100.0f) + averageVerticalVelocity);
    CHECK(flight.front().position[2] == rm::test::fx(30.0f));
}

TEST_CASE("area damage is uniform inside the radius, as retail's is") {
    // THIS TEST USED TO ASSERT THE OPPOSITE, and it was wrong. It read "damage falls off
    // linearly to nothing at the rim" and expected the unit halfway out to take half — the
    // Total Annihilation and Spring behaviour, which is what almost everyone assumes.
    //
    // Supreme Commander does not do that. In the retail executable the sphere worker
    // (`0x0073e100`) passes the amount to `DealDamage` (`0x0073dbc0`) untouched; the only
    // thing between the spatial query and the damage call is shield absorption, a flat
    // subtraction. The distance is computed and used as a DIRECTION, never as a scale.
    // `docs/fa-exe-analysis-plan.md`, claim `C-061`.
    //
    // Kept as one case with a victim at every interesting distance, because the failure this
    // guards against is a re-introduced curve, and a curve is only visible across a spread.
    const std::vector<Army> armies = rm::sim::freeForAll(2);

    Roster roster;
    const rm::UnitTypeIndex type = roster.addType(targetDef());
    const UnitId centre = roster.add(type, 0.0f, 0.0f, 1, 100.0f);
    const UnitId halfway = roster.add(type, 0.0f, 50.0f, 1, 100.0f);
    const UnitId rim = roster.add(type, 0.0f, 100.0f, 1, 100.0f);
    const UnitId outside = roster.add(type, 0.0f, 200.0f, 1, 100.0f);

    const rm::sim::Mag dealt =
        rm::sim::damageArea(rm::test::at(0, 0, 0), rm::test::fx(100.0f), rm::test::mag(80.0f), 0, roster.store, armies);

    CHECK(rm::test::asFloat(roster.health(centre).current) == Approx(20.0f));   // full 80
    CHECK(rm::test::asFloat(roster.health(halfway).current) == Approx(20.0f));  // also full 80
    CHECK(rm::test::asFloat(roster.health(rim).current) == Approx(20.0f));      // the rim is inside
    CHECK(rm::test::asFloat(roster.health(outside).current) == Approx(100.0f)); // outside: nothing

    // Three victims at 80 each. Under the old curve this was 120 — the same blast now deals
    // twice the damage, which is the whole point of the correction.
    CHECK(rm::test::asFloat(dealt) == Approx(240.0f));
}

TEST_CASE("a blast does not hurt the army that fired it") {
    const std::vector<Army> armies = rm::sim::freeForAll(2);
    Roster roster;
    const UnitId mine = roster.add(roster.addType(targetDef()), 0.0f, 0.0f, 0, 100.0f);

    // Fired by army 0, centred on army 0's own unit.
    const rm::sim::Mag dealt =
        rm::sim::damageArea(rm::test::at(0, 0, 0), rm::test::fx(100.0f), rm::test::mag(80.0f), 0, roster.store, armies);
    CHECK(rm::test::asFloat(dealt) == Approx(0.0f));
    CHECK(rm::test::asFloat(roster.health(mine).current) == Approx(100.0f));
}

TEST_CASE("a point hit lands on what it was aimed at") {
    // 222 of the 494 weapons state no damage radius. They are point hits at full
    // strength, not weapons that cannot hurt anything — a falloff over a zero radius
    // would divide by zero and damage nobody.
    const std::vector<Army> armies = rm::sim::freeForAll(2);
    Roster roster;
    const rm::UnitTypeIndex type = roster.addType(targetDef());
    const UnitId hit = roster.add(type, 0.0f, 0.0f, 1, 100.0f);
    const UnitId beside = roster.add(type, 0.0f, 60.0f, 1, 100.0f);

    const rm::sim::Mag dealt = rm::sim::damageArea(rm::test::at(0, 0, 0), rm::test::fx(0.0f), rm::test::mag(40.0f), 0, roster.store, armies);
    CHECK(rm::test::asFloat(roster.health(hit).current) == Approx(60.0f));
    CHECK(rm::test::asFloat(roster.health(beside).current) == Approx(100.0f));
    CHECK(rm::test::asFloat(dealt) == Approx(40.0f));
}

TEST_CASE("a point projectile damages only the body it struck") {
    // The swept collision has already selected the first body. C-111 found that throwing that
    // answer away and issuing a radius-zero spatial query made every overlapping body take a
    // separate full hit.
    const std::vector<Army> armies = rm::sim::freeForAll(2);
    Roster roster;
    const rm::UnitTypeIndex type = roster.addType(targetDef());
    const UnitId first = roster.add(type, 0.0f, 10.0f, 1, 100.0f);
    const UnitId overlappingA = roster.add(type, 0.0f, 12.0f, 1, 100.0f);
    const UnitId overlappingB = roster.add(type, 0.0f, 14.0f, 1, 100.0f);

    std::vector<Projectile> shots{Projectile{
        .position = rm::test::at(0, 1, 0),
        .velocity = rm::test::at(0, 0, 20),
        .damage = rm::unitdef::flatDamage(rm::test::mag(40.0f)),
        .damageRadiusElmos = {},
        .targetLayers = rm::unitdef::TargetLayerMask::Surface,
        .firedByArmy = 0,
        .ticksRemaining = 1,
    }};
    rm::sim::EventQueue events;
    const rm::HeightField field = flatField();
    rm::sim::advanceProjectiles(shots, roster.store, armies, rm::sim::Terrain{field},
                                roster.rate, &events, &roster.catalog);

    // Detection only records the impact. Retail invokes it at the start of the next
    // projectile tick, without moving the shot again.
    REQUIRE(shots.size() == 1);
    CHECK(shots.front().position == rm::test::at(0, 1, 6));
    CHECK(shots.front().pendingImpact == rm::sim::ImpactType::Unit);
    CHECK(shots.front().impactTarget == first);
    CHECK(rm::test::asFloat(roster.health(first).current) == Approx(100.0f));
    CHECK(events.count(rm::sim::EventKind::ProjectileImpact) == 0);
    CHECK(events.count(rm::sim::EventKind::UnitDamaged) == 0);

    rm::sim::advanceProjectiles(shots, roster.store, armies, rm::sim::Terrain{field},
                                roster.rate, &events, &roster.catalog);

    CHECK(shots.empty());
    CHECK(rm::test::asFloat(roster.health(first).current) == Approx(60.0f));
    CHECK(rm::test::asFloat(roster.health(overlappingA).current) == Approx(100.0f));
    CHECK(rm::test::asFloat(roster.health(overlappingB).current) == Approx(100.0f));
    CHECK(events.count(rm::sim::EventKind::ProjectileImpact) == 1);
    CHECK(events.count(rm::sim::EventKind::UnitDamaged) == 1);
    REQUIRE_FALSE(events.all().empty());
    CHECK(events.all().front().impactType == rm::sim::ImpactType::Unit);
}

TEST_CASE("a pending impact cannot follow a recycled unit slot") {
    const std::vector<Army> armies = rm::sim::freeForAll(2);
    Roster roster;
    const rm::UnitTypeIndex type = roster.addType(targetDef());
    const UnitId original = roster.add(type, 0.0f, 10.0f, 1, 100.0f);

    std::vector<Projectile> shots{Projectile{
        .position = rm::test::at(0, 1, 0),
        .velocity = rm::test::at(0, 0, 20),
        .damage = rm::unitdef::flatDamage(rm::test::mag(40.0f)),
        .targetLayers = rm::unitdef::TargetLayerMask::Surface,
        .firedByArmy = 0,
        .ticksRemaining = 1,
    }};
    rm::sim::EventQueue events;
    const rm::sim::Terrain terrain{flatField(-100.0f)};

    rm::sim::advanceProjectiles(shots, roster.store, armies, terrain, roster.rate,
                                &events, &roster.catalog);
    REQUIRE(shots.size() == 1);
    REQUIRE(shots.front().impactTarget == original);

    roster.store.kill(original);
    const UnitId replacement = roster.add(type, 0.0f, 10.0f, 1, 100.0f);
    REQUIRE(replacement.index == original.index);
    REQUIRE(replacement.generation != original.generation);

    rm::sim::advanceProjectiles(shots, roster.store, armies, terrain, roster.rate,
                                &events, &roster.catalog);

    CHECK(shots.empty());
    CHECK(rm::test::asFloat(roster.health(replacement).current) == Approx(100.0f));
    CHECK(events.count(rm::sim::EventKind::UnitDamaged) == 0);
    REQUIRE(events.count(rm::sim::EventKind::ProjectileImpact) == 1);
    CHECK(events.all().front().unit == UnitId{});
}

TEST_CASE("unit impact classification checks water level before movement layer") {
    const auto classify = [](float height) {
        const std::vector<Army> armies = rm::sim::freeForAll(2);
        Roster roster;
        UnitDef aircraft = targetDef();
        aircraft.motion = rm::unitdef::MotionType::Air;
        const UnitId target = roster.add(roster.addType(aircraft), 0.0f, 10.0f, 1, 100.0f);
        roster.transform(target).y = rm::test::fx(height);
        roster.reindex();

        std::vector<Projectile> shots{Projectile{
            .position = {rm::sim::Fx{}, rm::test::fx(height + 1.0f), rm::sim::Fx{}},
            .velocity = rm::test::at(0, 0, 20),
            .damage = rm::unitdef::flatDamage(rm::test::mag(40.0f)),
            .targetLayers = rm::unitdef::TargetLayerMask::Air,
            .firedByArmy = 0,
            .ticksRemaining = 1,
        }};

        rm::sim::advanceProjectiles(shots, roster.store, armies,
                                    rm::sim::Terrain{flatField(-100.0f), false, 0.0f},
                                    roster.rate, nullptr, &roster.catalog);
        REQUIRE(shots.size() == 1);
        return shots.front().pendingImpact;
    };

    CHECK(classify(10.0f) == rm::sim::ImpactType::UnitAir);
    CHECK(classify(-10.0f) == rm::sim::ImpactType::UnitUnderwater);
}

TEST_CASE("a splash projectile damages the body it struck at the contact point") {
    // The target is four elmos deep, but the blast radius is only one elmo. C-168 moves the
    // impact from the target centre to the collision-box surface, so a centre-only area query
    // would consume the projectile without damaging the body that caused the impact.
    const std::vector<Army> armies = rm::sim::freeForAll(2);
    Roster roster;
    const UnitId target =
        roster.add(roster.addType(targetDef()), 0.0f, 10.0f, 1, 100.0f);

    std::vector<Projectile> shots{Projectile{
        .position = rm::test::at(0, 1, 0),
        .velocity = rm::test::at(0, 0, 20),
        .damage = rm::unitdef::flatDamage(rm::test::mag(40.0f)),
        .damageRadiusElmos = rm::test::fx(1.0f),
        .targetLayers = rm::unitdef::TargetLayerMask::Surface,
        .firedByArmy = 0,
        .ticksRemaining = 1,
    }};
    rm::sim::EventQueue events;

    rm::sim::advanceProjectiles(shots, roster.store, armies,
                                rm::sim::Terrain{flatField(-100.0f)}, roster.rate,
                                &events, &roster.catalog);

    REQUIRE(shots.size() == 1);
    CHECK(rm::test::asFloat(roster.health(target).current) == Approx(100.0f));
    CHECK(events.count(rm::sim::EventKind::ProjectileImpact) == 0);
    rm::sim::advanceProjectiles(shots, roster.store, armies,
                                rm::sim::Terrain{flatField(-100.0f)}, roster.rate,
                                &events, &roster.catalog);

    CHECK(shots.empty());
    CHECK(rm::test::asFloat(roster.health(target).current) == Approx(60.0f));
    CHECK(events.count(rm::sim::EventKind::ProjectileImpact) == 1);
    CHECK(events.count(rm::sim::EventKind::UnitDamaged) == 1);
}

TEST_CASE("a pending splash stays at its contact point when bodies move") {
    const std::vector<Army> armies = rm::sim::freeForAll(2);
    Roster roster;
    const rm::UnitTypeIndex type = roster.addType(targetDef());
    const UnitId struck = roster.add(type, 0.0f, 10.0f, 1, 100.0f);
    const UnitId entering = roster.add(type, 0.0f, 30.0f, 1, 100.0f);

    std::vector<Projectile> shots{Projectile{
        .position = rm::test::at(0, 1, 0),
        .velocity = rm::test::at(0, 0, 20),
        .damage = rm::unitdef::flatDamage(rm::test::mag(40.0f)),
        .damageRadiusElmos = rm::test::fx(1.0f),
        .targetLayers = rm::unitdef::TargetLayerMask::Surface,
        .firedByArmy = 0,
        .ticksRemaining = 1,
    }};
    const rm::sim::Terrain terrain{flatField(-100.0f)};

    rm::sim::advanceProjectiles(shots, roster.store, armies, terrain, roster.rate,
                                nullptr, &roster.catalog);
    REQUIRE(shots.size() == 1);
    REQUIRE(shots.front().position == rm::test::at(0, 1, 6));

    // DamageArea runs on delivery and ignores OnImpact's target entity. What matters now is
    // which collision primitive overlaps the recorded blast sphere.
    roster.transform(struck).z = rm::test::fx(100.0f);
    roster.transform(entering).z = rm::test::fx(10.0f);
    roster.reindex();
    rm::sim::advanceProjectiles(shots, roster.store, armies, terrain, roster.rate,
                                nullptr, &roster.catalog);

    CHECK(rm::test::asFloat(roster.health(struck).current) == Approx(100.0f));
    CHECK(rm::test::asFloat(roster.health(entering).current) == Approx(60.0f));
}

TEST_CASE("non-positive damage neither heals nor reports a hit") {
    const std::vector<Army> armies = rm::sim::freeForAll(2);
    Roster roster;
    const UnitId target =
        roster.add(roster.addType(targetDef()), 0.0f, 0.0f, 1, 100.0f);
    rm::sim::EventQueue events;

    const rm::sim::Mag dealt = rm::sim::damageArea(
        rm::test::at(0, 0, 0), rm::test::fx(100.0f), rm::test::mag(-20.0f), 0,
        roster.store, armies, {}, &events);

    CHECK(rm::test::asFloat(roster.health(target).current) == Approx(100.0f));
    CHECK(dealt == rm::sim::Mag{});
    CHECK(events.count(rm::sim::EventKind::UnitDamaged) == 0);
}

TEST_CASE("damage never takes more than a unit has, so overkill is not negative health") {
    const std::vector<Army> armies = rm::sim::freeForAll(2);
    Roster roster;
    const UnitId frail = roster.add(roster.addType(targetDef()), 0.0f, 0.0f, 1, 30.0f);

    const rm::sim::Mag dealt =
        rm::sim::damageArea(rm::test::at(0, 0, 0), rm::test::fx(0.0f), rm::test::mag(5000.0f), 0, roster.store, armies);
    CHECK(rm::test::asFloat(roster.health(frail).current) == Approx(0.0f));
    CHECK(rm::test::asFloat(dealt) == Approx(30.0f));  // what was actually taken, not what was thrown
    CHECK_FALSE(roster.health(frail).alive());
}

TEST_CASE("a shot fired reloads, and does not fire again until it has") {
    const std::vector<Army> armies = rm::sim::freeForAll(2);

    Roster roster;
    // One shot a second = 10 ticks.
    (void)roster.add(roster.addType(gunnerDef(directFire(10.0f, 300.0f))), 0.0f, 0.0f, 0,
                     100.0f);
    (void)roster.add(roster.addType(targetDef()), 0.0f, 100.0f, 1, 1000.0f);

    std::vector<Projectile> shots;

    CHECK(rm::sim::fireWeapons(roster.store, roster.catalog, armies, shots, rm::sim::TickRate{}) == 1);
    CHECK(shots.size() == 1);

    // Nine ticks of reload, during which nothing more is fired.
    for (int tick = 0; tick < 9; ++tick) {
        CHECK(rm::sim::fireWeapons(roster.store, roster.catalog, armies, shots, rm::sim::TickRate{}) == 0);
    }
    CHECK(shots.size() == 1);

    // ...and then it fires again.
    CHECK(rm::sim::fireWeapons(roster.store, roster.catalog, armies, shots, rm::sim::TickRate{}) == 1);
    CHECK(shots.size() == 2);
}

TEST_CASE("a beam delivers the tick it fires: damage lands, nothing flies") {
    const std::vector<Army> armies = rm::sim::freeForAll(2);

    Roster roster;
    Weapon laser = directFire(25.0f, 300.0f);
    laser.beam = true;
    (void)roster.add(roster.addType(gunnerDef(laser)), 0.0f, 0.0f, 0, 100.0f);
    const UnitId victim = roster.add(roster.addType(targetDef()), 0.0f, 100.0f, 1, 1000.0f);

    std::vector<Projectile> shots;
    rm::sim::EventQueue events;

    CHECK(rm::sim::fireWeapons(roster.store, roster.catalog, armies, shots,
                               rm::sim::TickRate{}, &events) == 1);
    // The whole point, both halves: the damage is already dealt, and no projectile exists
    // for a later tick to deliver it again.
    CHECK(shots.empty());
    CHECK(rm::test::asFloat(roster.health(victim).current) == Approx(975.0f));

    // The event carries both ends of the line — the strike and the muzzle.
    bool beamSeen = false;
    for (const rm::sim::Event& event : events.all()) {
        if (event.kind == rm::sim::EventKind::BeamFired) {
            beamSeen = true;
            CHECK(rm::test::asFloat(event.at[2]) == Approx(100.0f));   // the victim
            CHECK(rm::test::asFloat(event.at2[2]) == Approx(0.0f));    // the muzzle
        }
    }
    CHECK(beamSeen);
}

TEST_CASE("area beams and death blasts deliver to the feature pool", "[wreck-combat]") {
    using rm::sim::Fx;
    using rm::sim::Mag;
    const auto armies = rm::sim::freeForAll(2);
    Roster roster;
    Weapon weapon = directFire(25, 300, 5);
    weapon.beam = true;
    (void)roster.add(roster.addType(gunnerDef(weapon)), 0, 0, 0, 100);
    (void)roster.add(roster.addType(targetDef()), 0, 100, 1, 1000);
    rm::sim::FeatureStore features;
    const auto wreck = features.add({.at = {Fx{}, Fx{}, Fx::fromInt(100)},
        .radiusElmos = Fx::fromInt(2), .health = Mag::fromInt(100), .maximumHealth = Mag::fromInt(100)});
    SECTION("beam area damage") {
        std::vector<Projectile> shots;
        REQUIRE(rm::sim::fireWeapons(roster.store, roster.catalog, armies, shots, roster.rate,
            nullptr, nullptr, nullptr, 0, {}, &features) == 1);
        CHECK(shots.empty());
    }
    SECTION("death area damage") {
        weapon.role = WeaponRole::Death;
        const auto def = gunnerDef(weapon);
        (void)rm::sim::explodeOnDeath(def, {Fx{}, Fx{}, Fx::fromInt(100)}, 0,
            roster.store, armies, {}, nullptr, &roster.catalog, &features);
    }
    REQUIRE(features.find(wreck));
    CHECK(features.find(wreck)->health == Mag::fromInt(75));
}

TEST_CASE("a unit with nothing to shoot at holds its fire and stays loaded") {
    // The reload runs whether or not there is a target, so a unit that comes into
    // contact fires at once rather than starting a fresh reload on sighting — the
    // difference between an ambush working and not.
    const std::vector<Army> armies = rm::sim::freeForAll(2);

    Roster roster;
    (void)roster.add(roster.addType(gunnerDef(directFire(10.0f, 100.0f))), 0.0f, 0.0f, 0,
                     100.0f);
    // Far out of range.
    const UnitId enemy = roster.add(roster.addType(targetDef()), 0.0f, 5000.0f, 1, 100.0f);

    std::vector<Projectile> shots;
    for (int tick = 0; tick < 50; ++tick) {
        CHECK(rm::sim::fireWeapons(roster.store, roster.catalog, armies, shots, rm::sim::TickRate{}) == 0);
    }
    CHECK(shots.empty());

    // Now it walks into range and shoots on the very first tick.
    //
    // The reindex is what the tick does after movement (§7 P5.2): targeting reads the store's
    // spatial index, so a unit teleported by writing its transform is still filed under its old
    // cell until the index is rebuilt. Without this the shooter finds nothing — a stale index
    // answers about where things were, and this test moves a unit without a movement pass.
    roster.transform(enemy).x = rm::test::fx(0.0f);
    roster.transform(enemy).z = rm::test::fx(50.0f);
    roster.reindex();
    CHECK(rm::sim::fireWeapons(roster.store, roster.catalog, armies, shots, rm::sim::TickRate{}) == 1);
}

TEST_CASE("the dead neither shoot nor are shot") {
    const std::vector<Army> armies = rm::sim::freeForAll(2);

    Roster roster;
    // Dead on arrival.
    (void)roster.add(roster.addType(gunnerDef(directFire(10.0f, 300.0f))), 0.0f, 0.0f, 0,
                     0.0f);
    (void)roster.add(roster.addType(targetDef()), 0.0f, 100.0f, 1, 100.0f);

    std::vector<Projectile> shots;
    CHECK(rm::sim::fireWeapons(roster.store, roster.catalog, armies, shots, rm::sim::TickRate{}) == 0);
    CHECK(shots.empty());
}

TEST_CASE("a shot in flight lands and kills, and is then gone") {
    const std::vector<Army> armies = rm::sim::freeForAll(2);
    const rm::HeightField field = flatField();

    Roster roster;
    const UnitId frail = roster.add(roster.addType(targetDef()), 0.0f, 100.0f, 1, 25.0f);

    Weapon weapon = directFire(40.0f, 300.0f, 30.0f);
    std::vector<Projectile> shots{rm::sim::launch(rm::test::at(0, 0, 0), rm::test::at(0, 0, 100), weapon, 0, rm::sim::TickRate{},
                        rm::sim::TickRate{}.perTick(weapon.muzzleVelocityElmosPerSecond),
                        rm::unitdef::flatDamage(weapon.damage))};

    for (int tick = 0; tick < 100 && !shots.empty(); ++tick) {
        rm::sim::advanceProjectiles(shots, roster.store, armies, rm::sim::Terrain{field},
                                    rm::sim::TickRate{});
    }

    CHECK(shots.empty());                        // spent
    CHECK_FALSE(roster.health(frail).alive());   // and it landed on something

    const auto dead = rm::sim::deadUnits(roster.store);
    REQUIRE(dead.size() == 1);
    CHECK(dead.front() == frail);
}

TEST_CASE("a swept projectile hits the first unit crossed in three dimensions") {
    const std::vector<Army> armies = rm::sim::freeForAll(2);
    const rm::HeightField field = flatField(-100.0f);
    Roster roster;
    const rm::UnitTypeIndex type = roster.addType(targetDef());
    const UnitId first = roster.add(type, 0.0f, 30.0f, 1, 100.0f);
    const UnitId second = roster.add(type, 0.0f, 80.0f, 1, 100.0f);
    roster.reindex();

    rm::sim::Projectile shot;
    shot.visualId = "/projectiles/TDFGauss01/TDFGauss01_proj.bp";
    shot.position = rm::test::at(0, 5, 0);
    shot.velocity = rm::test::at(0, 0, 100);
    shot.damage = rm::unitdef::flatDamage(rm::test::mag(40.0f));
    shot.firedByArmy = 0;
    shot.ticksRemaining = 2;
    std::vector<rm::sim::Projectile> shots{shot};
    rm::sim::EventQueue events;

    rm::sim::advanceProjectiles(shots, roster.store, armies, rm::sim::Terrain{field},
                                rm::sim::TickRate{}, &events, &roster.catalog);
    rm::sim::advanceProjectiles(shots, roster.store, armies, rm::sim::Terrain{field},
                                rm::sim::TickRate{}, &events, &roster.catalog);

    CHECK(rm::test::asFloat(roster.health(first).current) < 100.0f);
    CHECK(rm::test::asFloat(roster.health(second).current) == Approx(100.0f));
    CHECK(shots.empty());
    bool sawImpact = false;
    for (const rm::sim::Event& event : events.all()) {
        if (event.kind == rm::sim::EventKind::ProjectileImpact) {
            sawImpact = true;
            CHECK(event.at == rm::test::at(0, 5, 26));
            CHECK(event.visualId == shot.visualId);
            CHECK(event.visualDirection == shot.velocity);
        }
    }
    CHECK(sawImpact);
}

TEST_CASE("a projectile sweep reaches one tenth past both endpoints") {
    const std::vector<Army> armies = rm::sim::freeForAll(2);
    const rm::HeightField field = flatField(-100.0f);

    const auto healthAfterSweep = [&](rm::sim::Fx targetX, rm::sim::Fx targetZ) {
        Roster roster;
        const rm::UnitTypeIndex type = roster.addType(targetDef());
        const UnitId target = roster.add(type, 0.0f, 0.0f, 1, 100.0f);
        // More entries than covered cells force SpatialGrid's indexed path. Friendly fillers
        // are far away so they exercise only the broadphase, never collision selection.
        for (int filler = 0; filler < 20; ++filler) {
            (void)roster.add(type, 1000.0f + static_cast<float>(filler), 1000.0f, 0,
                             100.0f);
        }
        roster.transform(target).x = targetX;
        roster.transform(target).z = targetZ;
        roster.reindex();

        Projectile shot;
        shot.position = rm::test::at(0, 5, 0);
        shot.velocity = rm::test::at(0, 0, 100);
        shot.damage = rm::unitdef::flatDamage(rm::test::mag(40.0f));
        shot.firedByArmy = 0;
        shot.ticksRemaining = 2;
        std::vector<Projectile> shots{shot};

        rm::sim::advanceProjectiles(shots, roster.store, armies, rm::sim::Terrain{field},
                                    rm::sim::TickRate{}, nullptr, &roster.catalog);
        rm::sim::advanceProjectiles(shots, roster.store, armies, rm::sim::Terrain{field},
                                    rm::sim::TickRate{}, nullptr, &roster.catalog);
        return roster.health(target).current;
    };

    // Radius is four elmos. The first box ends exactly ten elmos behind the start; the second
    // begins exactly ten elmos past the end and touches the flight line at its X edge.
    CHECK(rm::test::asFloat(healthAfterSweep(rm::sim::Fx{}, rm::test::fx(-14.0f)))
          == Approx(60.0f));
    CHECK(rm::test::asFloat(
              healthAfterSweep(rm::test::fx(3.9f), rm::test::fx(114.0f)))
          == Approx(60.0f));

    // One tenth farther is outside: this rejects merely making the query generously larger.
    CHECK(rm::test::asFloat(healthAfterSweep(rm::sim::Fx{}, rm::test::fx(-14.1f)))
          == Approx(100.0f));
    CHECK(rm::test::asFloat(healthAfterSweep(rm::sim::Fx{}, rm::test::fx(114.1f)))
          == Approx(100.0f));

    // One raw position step beyond the exact endpoint is also outside. Q18.14 fractions used
    // to round this entry back onto the 1.1 boundary and manufacture a hit.
    CHECK(rm::test::asFloat(healthAfterSweep(
              rm::sim::Fx{}, rm::sim::Fx::fromInt(114) + rm::sim::Fx::fromRaw(1)))
          == Approx(100.0f));
}

TEST_CASE("sub-centielmo projectile motion uses a strict old-position sphere") {
    // C-168 compares the unrounded 3D length with 0.01. In Q18.14 raw units:
    //   10,000 * (16^2 + 163^2) = 268,250,000 < 16,384^2
    //   10,000 * (17^2 + 163^2) = 268,580,000 > 16,384^2
    // The target's one-raw-unit radius leaves its X face one step inside the sphere.
    CHECK(tinyProjectileStrikes({16, 0, 163}, {16384, 0, 0}));
    CHECK_FALSE(tinyProjectileStrikes({17, 0, 163}, {16384, 0, 0}));
    // A vertical component counts too: the threshold is a 3D length, not ground distance.
    CHECK_FALSE(tinyProjectileStrikes({0, 164, 0}, {16384, 0, 0}));

    // Exact tangency is out. Both targets would be inside a sphere centred at the pending
    // endpoint, so these also pin the query to the start-of-tick position.
    CHECK_FALSE(tinyProjectileStrikes({16, 0, 163}, {16385, 0, 0}));
    CHECK_FALSE(tinyProjectileStrikes({16, 0, 163}, {0, 0, 16385}));

    // The primitive test is fully 3D, not the grid's ground-only distance.
    CHECK(tinyProjectileStrikes({16, 0, 163}, {0, 16383, 0}));
    CHECK_FALSE(tinyProjectileStrikes({16, 0, 163}, {0, 16384, 0}));

    // The broadphase square is not the answer: 11,585^2 + 11,585^2 is just inside a
    // radius-one sphere in raw units, while increasing each nearest-face gap by one is out.
    CHECK(tinyProjectileStrikes({16, 0, 163}, {11586, 0, 11586}));
    CHECK_FALSE(tinyProjectileStrikes({16, 0, 163}, {11587, 0, 11587}));
}

TEST_CASE("a tiny-motion impact stays at the old projectile position") {
    const std::vector<Army> armies = rm::sim::freeForAll(2);
    Roster roster;
    const UnitId target =
        roster.add(roster.addType(targetDef()), 11.0f, 20.0f, 1, 100.0f);
    roster.transform(target).y = rm::test::fx(5.0f);
    roster.motion(target).radiusElmos = rm::sim::Fx::fromRaw(1);
    roster.reindex();

    const std::array<rm::sim::Fx, 3> from = rm::test::at(10, 5, 20);
    Projectile shot;
    shot.position = from;
    shot.velocity = {
        rm::sim::Fx::fromRaw(16), rm::sim::Fx{}, rm::sim::Fx::fromRaw(163)};
    shot.damage = rm::unitdef::flatDamage(rm::test::mag(40.0f));
    shot.targetLayers = rm::unitdef::TargetLayerMask::Surface;
    shot.firedByArmy = 0;
    shot.ticksRemaining = 2;
    std::vector<Projectile> shots{shot};
    rm::sim::EventQueue events;

    rm::sim::advanceProjectiles(shots, roster.store, armies,
                                rm::sim::Terrain{flatField(-100.0f)}, roster.rate,
                                &events, &roster.catalog);
    rm::sim::advanceProjectiles(shots, roster.store, armies,
                                rm::sim::Terrain{flatField(-100.0f)}, roster.rate,
                                &events, &roster.catalog);

    CHECK(rm::test::asFloat(roster.health(target).current) == Approx(60.0f));
    bool sawImpact = false;
    for (const rm::sim::Event& event : events.all()) {
        if (event.kind == rm::sim::EventKind::ProjectileImpact) {
            sawImpact = true;
            CHECK(event.at == from);
        }
    }
    CHECK(sawImpact);
}

TEST_CASE("terrain suppresses the tiny-motion entity fallback") {
    const auto healthAfter = [](float terrainHeight) {
        const std::vector<Army> armies = rm::sim::freeForAll(2);
        Roster roster;
        const UnitId target =
            roster.add(roster.addType(targetDef()), 0.0f, 0.0f, 1, 100.0f);

        Projectile shot;
        shot.position = {
            rm::sim::Fx{}, rm::sim::Fx::fromRaw(100), rm::sim::Fx{}};
        shot.velocity = {
            rm::sim::Fx{}, rm::sim::Fx::fromRaw(-150), rm::sim::Fx{}};
        shot.damage = rm::unitdef::flatDamage(rm::test::mag(40.0f));
        shot.targetLayers = rm::unitdef::TargetLayerMask::Surface;
        shot.firedByArmy = 0;
        shot.ticksRemaining = 2;
        std::vector<Projectile> shots{shot};

        rm::sim::advanceProjectiles(shots, roster.store, armies,
                                    rm::sim::Terrain{flatField(terrainHeight)}, roster.rate,
                                    nullptr, &roster.catalog);
        rm::sim::advanceProjectiles(shots, roster.store, armies,
                                    rm::sim::Terrain{flatField(terrainHeight)}, roster.rate,
                                    nullptr, &roster.catalog);
        return roster.health(target).current;
    };

    // The same old-position sphere finds the unit in both scenes. With no surface impact it
    // lands; when the tiny segment crosses terrain later in the tick, terrain wins outright.
    CHECK(rm::test::asFloat(healthAfter(-100.0f)) == Approx(60.0f));
    CHECK(rm::test::asFloat(healthAfter(0.0f)) == Approx(100.0f));
}

TEST_CASE("tiny-motion fallback preserves filters and ascending slot order on the grid") {
    const std::vector<Army> armies = rm::sim::freeForAll(2);
    Roster roster;
    const rm::UnitTypeIndex surfaceType = roster.addType(targetDef());
    UnitDef aircraft = targetDef();
    aircraft.name = "air_target";
    aircraft.motion = rm::unitdef::MotionType::Air;
    const rm::UnitTypeIndex airType = roster.addType(aircraft);

    const UnitId friendly = roster.add(surfaceType, 67.0f, 32.0f, 0, 100.0f);
    const UnitId air = roster.add(airType, 67.0f, 32.0f, 1, 100.0f);
    const UnitId shapeless = roster.add(surfaceType, 67.0f, 32.0f, 1, 100.0f);
    roster.motion(shapeless).radiusElmos = rm::sim::Fx{};
    for (int filler = 0; filler < 5; ++filler) {
        (void)roster.add(surfaceType, static_cast<float>(filler), 0.0f, 0, 100.0f);
    }
    const UnitId first = roster.add(surfaceType, 67.0f, 32.0f, 1, 100.0f);
    const UnitId second = roster.add(surfaceType, 67.0f, 32.0f, 1, 100.0f);

    // At x=62.5, reach five covers two 64-elmo cells. Two cells for ten entries forces
    // SpatialGrid's indexed path rather than its whole-array fallback.
    REQUIRE(roster.store.space().size() == 10);
    REQUIRE(roster.store.space().cellSize() == rm::sim::Fx::fromInt(64));

    Projectile shot;
    shot.position = {rm::test::fx(62.5f), rm::test::fx(1.0f), rm::test::fx(32.0f)};
    shot.velocity = {
        rm::sim::Fx::fromRaw(16), rm::sim::Fx{}, rm::sim::Fx::fromRaw(163)};
    shot.damage = rm::unitdef::flatDamage(rm::test::mag(40.0f));
    shot.targetLayers = rm::unitdef::TargetLayerMask::Surface;
    shot.firedByArmy = 0;
    shot.ticksRemaining = 2;
    std::vector<Projectile> shots{shot};

    rm::sim::advanceProjectiles(shots, roster.store, armies,
                                rm::sim::Terrain{flatField(-100.0f)}, roster.rate,
                                nullptr, &roster.catalog);
    rm::sim::advanceProjectiles(shots, roster.store, armies,
                                rm::sim::Terrain{flatField(-100.0f)}, roster.rate,
                                nullptr, &roster.catalog);

    CHECK(rm::test::asFloat(roster.health(friendly).current) == Approx(100.0f));
    CHECK(rm::test::asFloat(roster.health(air).current) == Approx(100.0f));
    CHECK(rm::test::asFloat(roster.health(shapeless).current) == Approx(100.0f));
    CHECK(rm::test::asFloat(roster.health(first).current) == Approx(60.0f));
    CHECK(rm::test::asFloat(roster.health(second).current) == Approx(100.0f));
}

TEST_CASE("a vertical projectile sweep includes target-box corners") {
    const std::vector<Army> armies = rm::sim::freeForAll(2);
    Roster roster;
    const UnitId target =
        roster.add(roster.addType(targetDef()), 3.0f, 3.0f, 1, 100.0f);
    roster.transform(target).y = rm::test::fx(50.0f);

    Projectile shot;
    shot.position = rm::test::at(0, 0, 0);
    shot.velocity = rm::test::at(0, 100, 0);
    shot.damage = rm::unitdef::flatDamage(rm::test::mag(40.0f));
    shot.firedByArmy = 0;
    shot.ticksRemaining = 2;
    std::vector<Projectile> shots{shot};

    rm::sim::advanceProjectiles(shots, roster.store, armies,
                                rm::sim::Terrain{flatField(-100.0f)},
                                rm::sim::TickRate{}, nullptr, &roster.catalog);
    rm::sim::advanceProjectiles(shots, roster.store, armies,
                                rm::sim::Terrain{flatField(-100.0f)},
                                rm::sim::TickRate{}, nullptr, &roster.catalog);

    CHECK(rm::test::asFloat(roster.health(target).current) == Approx(60.0f));
    CHECK(shots.empty());
}

TEST_CASE("a stationary projectile has no collision time before its tick") {
    const std::vector<Army> armies = rm::sim::freeForAll(2);
    Roster roster;
    const UnitId target =
        roster.add(roster.addType(targetDef()), 0.0f, 0.0f, 1, 100.0f);

    Projectile shot;
    shot.position = rm::test::at(0, 0, 0);
    shot.damage = rm::unitdef::flatDamage(rm::test::mag(40.0f));
    shot.firedByArmy = 0;
    shot.ticksRemaining = 2;
    std::vector<Projectile> shots{shot};

    rm::sim::advanceProjectiles(shots, roster.store, armies,
                                rm::sim::Terrain{flatField()}, rm::sim::TickRate{},
                                nullptr, &roster.catalog);
    rm::sim::advanceProjectiles(shots, roster.store, armies,
                                rm::sim::Terrain{flatField()}, rm::sim::TickRate{},
                                nullptr, &roster.catalog);

    // The unit and terrain both intersect at t=0, so the existing terrain-first tie remains.
    CHECK(rm::test::asFloat(roster.health(target).current) == Approx(100.0f));
    CHECK(shots.empty());
}

TEST_CASE("terrain beats an extended-sweep hit beyond the tick endpoint") {
    const auto healthAfter = [](float terrainHeight) {
        const std::vector<Army> armies = rm::sim::freeForAll(2);
        Roster roster;
        const UnitId target =
            roster.add(roster.addType(targetDef()), 0.0f, 106.0f, 1, 100.0f);
        // Put the collision box across the continued flight line after it passes through the
        // ground. Its unusual depth makes the ordering observable without inventing a ridge.
        roster.transform(target).y = rm::test::fx(-5.0f);

        Projectile shot;
        shot.position = rm::test::at(0, 100, 0);
        shot.velocity = rm::test::at(0, -100, 100);
        shot.damage = rm::unitdef::flatDamage(rm::test::mag(40.0f));
        shot.firedByArmy = 0;
        shot.ticksRemaining = 2;
        std::vector<Projectile> shots{shot};

        rm::sim::advanceProjectiles(shots, roster.store, armies,
                                    rm::sim::Terrain{flatField(terrainHeight)},
                                    rm::sim::TickRate{}, nullptr, &roster.catalog);
        rm::sim::advanceProjectiles(shots, roster.store, armies,
                                    rm::sim::Terrain{flatField(terrainHeight)},
                                    rm::sim::TickRate{}, nullptr, &roster.catalog);
        return roster.health(target).current;
    };

    CHECK(rm::test::asFloat(healthAfter(-100.0f)) == Approx(60.0f));
    CHECK(rm::test::asFloat(healthAfter(0.0f)) == Approx(100.0f));
}

TEST_CASE("terrain wins when its crossing and a buried body share one Fx time step") {
    // The flat surface is crossed at 10000/30000 of the tick. The target box starts one
    // position raw unit later, at 10001/30000. Those times fit inside one Q18.14 fraction
    // step, so comparing a Q31 body hit with a rounded-up Q18.14 terrain hit reverses them.
    const std::vector<Army> armies = rm::sim::freeForAll(2);
    Roster roster;
    UnitDef buried = targetDef();
    buried.sizeYElmos = 8.0f;
    const UnitId target = roster.add(roster.addType(buried), 0.0f, 0.0f, 1, 100.0f);
    roster.transform(target).y =
        -rm::sim::Fx::fromInt(8) - rm::sim::Fx::fromRaw(1);
    roster.reindex();

    Projectile shot;
    shot.position = {
        rm::sim::Fx{}, rm::sim::Fx::fromRaw(10000), rm::sim::Fx{}};
    shot.velocity = {
        rm::sim::Fx{}, rm::sim::Fx::fromRaw(-30000), rm::sim::Fx{}};
    shot.damage = rm::unitdef::flatDamage(rm::test::mag(40.0f));
    shot.targetLayers = rm::unitdef::TargetLayerMask::Surface;
    shot.firedByArmy = 0;
    shot.ticksRemaining = 2;
    std::vector<Projectile> shots{shot};

    rm::sim::advanceProjectiles(shots, roster.store, armies,
                                rm::sim::Terrain{flatField()}, roster.rate,
                                nullptr, &roster.catalog);
    rm::sim::advanceProjectiles(shots, roster.store, armies,
                                rm::sim::Terrain{flatField()}, roster.rate,
                                nullptr, &roster.catalog);

    CHECK(shots.empty());
    CHECK(rm::test::asFloat(roster.health(target).current) == Approx(100.0f));
}

TEST_CASE("terrain blocks a swept projectile before the unit behind it") {
    const std::vector<Army> armies = rm::sim::freeForAll(2);
    rm::HeightField field = flatField();
    // A single grid-line ridge at z=8. Both segment endpoints are below its 12-elmo peak but
    // above their local ground, so endpoint-only or one-square stepping tunnels through it.
    for (int x = 0; x < field.verticesX(); ++x) {
        field.raw[static_cast<std::size_t>(field.verticesX() + x)] = 12;
    }
    Roster roster;
    UnitDef wideTarget = targetDef();
    wideTarget.collisionRadiusElmos = 6.0f;
    wideTarget.sizeYElmos = 20.0f;
    const UnitId target = roster.add(roster.addType(wideTarget), 0.0f, 18.0f, 1, 100.0f);
    roster.reindex();

    rm::sim::Projectile shot;
    shot.position = {rm::test::fx(0.0f), rm::test::fx(11.9f), rm::test::fx(4.0f)};
    shot.velocity = rm::test::at(0, 0, 18);
    shot.damage = rm::unitdef::flatDamage(rm::test::mag(40.0f));
    shot.firedByArmy = 0;
    shot.ticksRemaining = 2;
    std::vector<rm::sim::Projectile> shots{shot};

    rm::sim::advanceProjectiles(shots, roster.store, armies, rm::sim::Terrain{field},
                                rm::sim::TickRate{}, nullptr, &roster.catalog);
    rm::sim::advanceProjectiles(shots, roster.store, armies, rm::sim::Terrain{field},
                                rm::sim::TickRate{}, nullptr, &roster.catalog);

    CHECK(rm::test::asFloat(roster.health(target).current) == Approx(100.0f));
    CHECK(shots.empty());
}

TEST_CASE("projectile collision respects altitude and does not use blast radius as a body") {
    const std::vector<Army> armies = rm::sim::freeForAll(2);
    const rm::HeightField field = flatField(-100.0f);

    SECTION("a shot above the unit keeps flying") {
        Roster roster;
        const UnitId target =
            roster.add(roster.addType(targetDef()), 0.0f, 50.0f, 1, 100.0f);
        roster.reindex();
        rm::sim::Projectile shot;
        shot.position = rm::test::at(0, 20, 0);
        shot.velocity = rm::test::at(0, 0, 100);
        shot.damage = rm::unitdef::flatDamage(rm::test::mag(40.0f));
        shot.firedByArmy = 0;
        shot.ticksRemaining = 2;
        std::vector<rm::sim::Projectile> shots{shot};

        rm::sim::advanceProjectiles(shots, roster.store, armies, rm::sim::Terrain{field},
                                    rm::sim::TickRate{}, nullptr, &roster.catalog);
        CHECK(rm::test::asFloat(roster.health(target).current) == Approx(100.0f));
        CHECK(shots.size() == 1);
    }

    SECTION("a wide blast does not detonate beside a missed body") {
        Roster roster;
        const UnitId target =
            roster.add(roster.addType(targetDef()), 10.0f, 50.0f, 1, 100.0f);
        roster.reindex();
        rm::sim::Projectile shot;
        shot.position = rm::test::at(0, 5, 0);
        shot.velocity = rm::test::at(0, 0, 100);
        shot.damage = rm::unitdef::flatDamage(rm::test::mag(40.0f));
        shot.damageRadiusElmos = rm::test::fx(30.0f);
        shot.firedByArmy = 0;
        shot.ticksRemaining = 2;
        std::vector<rm::sim::Projectile> shots{shot};

        rm::sim::advanceProjectiles(shots, roster.store, armies, rm::sim::Terrain{field},
                                    rm::sim::TickRate{}, nullptr, &roster.catalog);
        CHECK(rm::test::asFloat(roster.health(target).current) == Approx(100.0f));
        CHECK(shots.size() == 1);
    }
}

TEST_CASE("a surface shot passes aircraft and damages only its ground target") {
    const std::vector<Army> armies = rm::sim::freeForAll(2);
    const rm::HeightField field = flatField();
    Roster roster;

    UnitDef aircraft = targetDef();
    aircraft.name = "air_target";
    aircraft.motion = rm::unitdef::MotionType::Air;
    const UnitId air = roster.add(roster.addType(aircraft), 0.0f, 50.0f, 1, 100.0f);
    const UnitId surface =
        roster.add(roster.addType(targetDef()), 0.0f, 100.0f, 1, 100.0f);

    Weapon weapon = directFire(40.0f, 300.0f, 20.0f);
    weapon.targetLayers = rm::unitdef::TargetLayerMask::Surface;
    std::vector<Projectile> shots{rm::sim::launch(
        rm::test::at(0, 0, 0), rm::test::at(0, 0, 100), weapon, 0,
        rm::sim::TickRate{},
        rm::sim::TickRate{}.perTick(weapon.muzzleVelocityElmosPerSecond),
        rm::unitdef::flatDamage(weapon.damage))};

    for (int tick = 0; tick < 100 && !shots.empty(); ++tick) {
        rm::sim::advanceProjectiles(shots, roster.store, armies, rm::sim::Terrain{field},
                                    rm::sim::TickRate{});
    }

    CHECK(shots.empty());
    CHECK(rm::test::asFloat(roster.health(air).current) == Approx(100.0f));
    CHECK(rm::test::asFloat(roster.health(surface).current) < 100.0f);
}

TEST_CASE("a timeout becomes a next-tick targetless impact with splash-only damage") {
    const auto checkExpiry = [](rm::sim::Fx radius, float expectedHealth,
                                std::size_t expectedDamageEvents, float flightHeight = 1.0f,
                                bool hasWater = false) {
        const std::vector<Army> armies = rm::sim::freeForAll(2);
        Roster roster;
        // Ten elmos to the side of the endpoint: outside the four-elmo collision body but
        // inside the splash section's twelve-elmo damage radius.
        const UnitId target =
            roster.add(roster.addType(targetDef()), 10.0f, 20.0f, 1, 100.0f);

        Projectile shot;
        shot.position = {rm::sim::Fx{}, rm::test::fx(flightHeight), rm::sim::Fx{}};
        shot.velocity = rm::test::at(0, 0, 20);
        shot.damage = rm::unitdef::flatDamage(rm::test::mag(40.0f));
        shot.damageRadiusElmos = radius;
        shot.targetLayers = rm::unitdef::TargetLayerMask::Surface;
        shot.firedByArmy = 0;
        shot.ticksRemaining = 1;
        std::vector<Projectile> shots{shot};
        rm::sim::EventQueue events;
        const rm::sim::Terrain terrain{flatField(-100.0f), hasWater, 0.0f};
        const rm::sim::ImpactType expectedImpact = flightHeight < 0.0f
                                                        ? rm::sim::ImpactType::Underwater
                                                        : rm::sim::ImpactType::Air;
        const std::array<rm::sim::Fx, 3> endpoint{
            rm::sim::Fx{}, rm::test::fx(flightHeight), rm::sim::Fx::fromInt(20)};

        rm::sim::advanceProjectiles(shots, roster.store, armies, terrain, roster.rate,
                                    &events, &roster.catalog);

        REQUIRE(shots.size() == 1);
        CHECK(shots.front().position == endpoint);
        CHECK(shots.front().pendingImpact == expectedImpact);
        CHECK(rm::test::asFloat(roster.health(target).current) == Approx(100.0f));
        CHECK(events.count(rm::sim::EventKind::ProjectileImpact) == 0);

        rm::sim::advanceProjectiles(shots, roster.store, armies, terrain, roster.rate,
                                    &events, &roster.catalog);

        CHECK(shots.empty());
        CHECK(rm::test::asFloat(roster.health(target).current) == Approx(expectedHealth));
        CHECK(events.count(rm::sim::EventKind::ProjectileImpact) == 1);
        CHECK(events.count(rm::sim::EventKind::UnitDamaged) == expectedDamageEvents);
        for (const rm::sim::Event& event : events.all()) {
            if (event.kind == rm::sim::EventKind::ProjectileImpact) {
                CHECK(event.unit == UnitId{});
                CHECK(event.at == endpoint);
                CHECK(event.impactType == expectedImpact);
            }
        }
    };

    SECTION("positive-radius expiry airbursts") {
        checkExpiry(rm::test::fx(12.0f), 60.0f, 1);
    }
    SECTION("radius-zero expiry is harmless but still reports impact") {
        checkExpiry(rm::sim::Fx{}, 100.0f, 0);
    }
    SECTION("an expiry below water reports underwater rather than air") {
        // The native classifier compares against WaterLevel directly; map rendering's
        // HasWater flag is not part of this decision.
        checkExpiry(rm::sim::Fx{}, 100.0f, 0, -1.0f, false);
    }
    SECTION("an airburst is a sphere rather than an infinite vertical cylinder") {
        checkExpiry(rm::test::fx(12.0f), 100.0f, 0, 100.0f);
    }
}

TEST_CASE("a shot that hits nothing expires instead of flying forever") {
    // Nothing here despawns on leaving the map, so a missed shot would otherwise
    // accumulate — a leak whose symptom is a falling frame rate rather than a wrong
    // number.
    const rm::HeightField field = flatField(-100000.0f);  // ground far below: never lands

    Weapon weapon = directFire(10.0f, 300.0f);
    weapon.muzzleVelocityElmosPerSecond = 1000.0f;
    std::vector<Projectile> shots{rm::sim::launch(rm::test::at(0, 0, 0), rm::test::at(0, 0, 100), weapon, 0, rm::sim::TickRate{},
                        rm::sim::TickRate{}.perTick(weapon.muzzleVelocityElmosPerSecond),
                        rm::unitdef::flatDamage(weapon.damage))};

    rm::sim::UnitStore none;
    const auto lifetime =
        static_cast<int>(rm::sim::TickRate{}.ticks(rm::sim::kProjectileLifetime));
    for (int tick = 0; tick <= lifetime; ++tick) {
        rm::sim::advanceProjectiles(shots, none, {}, rm::sim::Terrain{field},
                                    rm::sim::TickRate{});
    }
    CHECK(shots.empty());
}

TEST_CASE("an unowned unit takes no part in a fight") {
    // kNoArmy is -1 rather than 0 precisely so this holds: a decorative instance, or one
    // whose owner was never set, is neither a target nor a shooter. With a default of 0 it
    // would belong to the first player and read as the enemy fielding units it never
    // built.
    const std::vector<Army> armies = rm::sim::freeForAll(2);

    Roster roster;
    (void)roster.add(roster.addType(gunnerDef(directFire(10.0f, 300.0f))), 0.0f, 0.0f, 0,
                     100.0f);
    const UnitId nobodys =
        roster.add(roster.addType(targetDef()), 0.0f, 50.0f, rm::sim::kNoArmy, 100.0f);

    std::vector<Projectile> shots;
    CHECK(rm::sim::fireWeapons(roster.store, roster.catalog, armies, shots, rm::sim::TickRate{}) == 0);

    CHECK(rm::test::asFloat(rm::sim::damageArea(rm::test::at(0, 0, 50), rm::test::fx(100.0f), rm::test::mag(500.0f), 0,
                                                roster.store, armies))
          == Approx(0.0f));
    CHECK(roster.health(nobodys).alive());
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
    CHECK(rm::test::asFloat(found->damage) == Approx(900.0f));

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

    Roster roster;
    const rm::UnitTypeIndex type = roster.addType(targetDef());
    const UnitId centre = roster.add(type, 0.0f, 0.0f, 1, 1000.0f);
    const UnitId halfway = roster.add(type, 0.0f, 40.0f, 1, 1000.0f);
    const UnitId clear = roster.add(type, 0.0f, 500.0f, 1, 1000.0f);

    const rm::sim::Mag dealt = rm::sim::explodeOnDeath(def, rm::test::at(0, 0, 0), 0, roster.store, armies);

    CHECK(rm::test::asFloat(dealt) > 0.0f);
    CHECK(rm::test::asFloat(roster.health(centre).current) == Approx(500.0f));   // took the full 500
    // Also the full 500: a death blast has no falloff either, for the same reason an ordinary
    // one does not — both go through `damageArea`. See `C-061`.
    CHECK(rm::test::asFloat(roster.health(halfway).current) == Approx(500.0f));
    CHECK(rm::test::asFloat(roster.health(clear).current) == Approx(1000.0f));   // untouched
}

TEST_CASE("a friendly death blast hurts allies but not its dying unit") {
    const std::vector<Army> armies = rm::sim::freeForAll(2);
    UnitDef def;
    Weapon death = directFire(40.0f, 100.0f, 80.0f);
    death.role = WeaponRole::Death;
    death.damageFriendly = true;
    def.weapons.push_back(death);

    Roster roster;
    const rm::UnitTypeIndex type = roster.addType(targetDef());
    const UnitId dying = roster.add(type, 0.0f, 0.0f, 0, 100.0f);
    const UnitId ally = roster.add(type, 0.0f, 40.0f, 0, 100.0f);

    (void)rm::sim::explodeOnDeath(def, rm::test::at(0, 0, 0), 0, roster.store, armies, dying);

    CHECK(rm::test::asFloat(roster.health(dying).current) == Approx(100.0f));
    CHECK(rm::test::asFloat(roster.health(ally).current) == Approx(60.0f));
}

TEST_CASE("a defeated army's friendly death blast cannot hurt a live enemy") {
    std::vector<Army> armies = rm::sim::freeForAll(2);
    armies[0].defeated = true;

    UnitDef def;
    Weapon death = directFire(40.0f, 100.0f, 80.0f);
    death.role = WeaponRole::Death;
    death.damageFriendly = true;
    def.weapons.push_back(death);

    Roster roster;
    const rm::UnitTypeIndex type = roster.addType(targetDef());
    const UnitId dying = roster.add(type, 0.0f, 0.0f, 0, 100.0f);
    const UnitId enemy = roster.add(type, 0.0f, 40.0f, 1, 100.0f);

    CHECK(rm::sim::explodeOnDeath(def, rm::test::at(0, 0, 0), 0, roster.store, armies, dying)
          == rm::sim::Mag{});
    CHECK(rm::test::asFloat(roster.health(enemy).current) == Approx(100.0f));
}

TEST_CASE("a unit with no death weapon detonates harmlessly") {
    const std::vector<Army> armies = rm::sim::freeForAll(2);
    UnitDef def;
    def.weapons.push_back(directFire(10.0f, 300.0f));  // a gun, not a death blast

    Roster roster;
    const UnitId bystander = roster.add(roster.addType(targetDef()), 0.0f, 0.0f, 1, 100.0f);

    CHECK(rm::test::asFloat(rm::sim::explodeOnDeath(def, rm::test::at(0, 0, 0), 0, roster.store,
                                                   armies))
          == Approx(0.0f));
    CHECK(rm::test::asFloat(roster.health(bystander).current) == Approx(100.0f));
}

TEST_CASE("a bearing is measured the way a unit's yaw is") {
    // atan2(dx, dz), NOT atan2(dz, dx): yaw is measured from +Z toward +X because that is
    // what the vertex shader does with it. Swapping the arguments compiles, runs, and points
    // every turret ninety degrees off.
    // Asserted in binary radians, which is what a bearing IS now: a quarter turn is 16,384,
    // exactly, with no tolerance needed — the whole reason for the type.
    CHECK(rm::sim::bearingTo(rm::test::at(0, 0, 0), rm::test::at(0, 0, 100)) == 0);  // +Z
    CHECK(rm::sim::bearingTo(rm::test::at(0, 0, 0), rm::test::at(100, 0, 0))
          == rm::sim::kBradQuarterTurn);                                            // +X
}

TEST_CASE("a heading error takes the shorter way round") {
    // A unit one degree the wrong side of north must read as one degree off, not 359 — or it
    // turns the long way and looks broken.
    // A tenth of a radian is 1043 brad (65,536 / 2*pi / 10).
    constexpr rm::Brad tenth = 1043;
    CHECK(rm::sim::headingError(0, tenth) == tenth);
    CHECK(rm::sim::headingError(tenth, 0) == tenth);  // symmetric
    CHECK(rm::sim::headingError(static_cast<rm::Brad>(-521), 522) == tenth);

    // A tenth of a turn SHORT of a full turn reads as a tenth of a turn, exactly — no
    // tolerance, because the wrap is unsigned overflow rather than a modulo by 2*pi. This
    // case needed `.margin(1e-4)` in float for precisely that reason.
    CHECK(rm::sim::headingError(0, static_cast<rm::Brad>(65536 - tenth)) == tenth);
    CHECK(rm::sim::headingError(0, rm::sim::kBradHalfTurn) == rm::sim::kBradHalfTurn);
}

TEST_CASE("a turreted weapon fires whatever the hull is doing") {
    // 284 of the 399 weapons that say either way are turreted, and this engine does not
    // animate turrets — so gating them on the hull would leave two thirds of the corpus
    // unable to shoot at all.
    Weapon turret = directFire(10.0f, 300.0f);
    turret.turreted = true;
    turret.firingToleranceBrads = rm::unitdef::firingToleranceBradsFromDegrees(1.0f);

    CHECK(rm::sim::canFireAt(turret, 0, 0));
    CHECK(rm::sim::canFireAt(turret, 0, rm::sim::kBradHalfTurn));  // directly behind
}

TEST_CASE("an unturreted weapon must be pointed at what it shoots") {
    // The fix for a tank firing out of its side armour.
    Weapon fixed = directFire(10.0f, 300.0f);
    fixed.turreted = false;
    fixed.firingToleranceBrads = rm::unitdef::firingToleranceBradsFromDegrees(2.0f);  // the corpus's own mode

    CHECK(rm::sim::canFireAt(fixed, 0, 0));
    CHECK(rm::sim::canFireAt(fixed, 0, rm::sim::bradFromRadians(0.03f)));  // just under two degrees
    CHECK_FALSE(rm::sim::canFireAt(fixed, 0, rm::sim::bradFromRadians(0.5f)));
    CHECK_FALSE(rm::sim::canFireAt(fixed, 0, rm::sim::kBradQuarterTurn));
}

TEST_CASE("an idle unit turns to bring its gun to bear, at its own rate") {
    const std::vector<Army> armies = rm::sim::freeForAll(2);

    Weapon fixed = directFire(10.0f, 300.0f);
    fixed.turreted = false;

    Roster roster;
    const UnitId gunner =
        roster.add(roster.addType(gunnerDef(fixed)), 0.0f, 0.0f, 0, 100.0f);
    roster.motion(gunner).turnPerTick = roster.rate.bradPerTick(1.0f);  // one radian a second
    roster.motion(gunner).moving = false;

    // Due +X, so a bearing of pi/2.
    (void)roster.add(roster.addType(targetDef()), 100.0f, 0.0f, 1, 100.0f);

    // One tick is a tenth of a radian, so it does not snap round — a slow hull is slow to
    // aim, which is why the turn rate is read off the blueprint at all.
    CHECK(rm::sim::aimAtTargets(roster.store, roster.catalog, armies) == 1);
    // A tenth of a radian is 1043 brad, and the turn is exact rather than approximate.
    CHECK(roster.transform(gunner).heading == 1043);

    // ...and it gets there eventually.
    for (int tick = 0; tick < 100; ++tick) {
        (void)rm::sim::aimAtTargets(roster.store, roster.catalog, armies);
    }
    CHECK(rm::sim::headingError(roster.transform(gunner).heading, rm::sim::kBradQuarterTurn)
          < 200);  // within ~1 degree of due +X
}

TEST_CASE("fixed-hull aiming breaks equal-distance unit targets by lower ID") {
    const std::vector<Army> armies = rm::sim::freeForAll(2);
    Weapon first = directFire(10.0f, 300.0f);
    first.targetPriorities = {{"B"}};
    Weapon second = directFire(10.0f, 300.0f);
    second.targetPriorities = {{"A"}};
    UnitDef gunner;
    gunner.name = "test_fixed_hull";
    gunner.weapons = {first, second};
    UnitDef typeA = targetDef();
    typeA.name = "test_type_a";
    typeA.categories = {"A"};
    UnitDef typeB = targetDef();
    typeB.name = "test_type_b";
    typeB.categories = {"B"};

    Roster roster;
    const UnitId shooter =
        roster.add(roster.addType(gunner), 0.0f, 0.0f, 0, 100.0f);
    roster.motion(shooter).turnPerTick = rm::sim::kBradQuarterTurn;
    (void)roster.add(roster.addType(typeA), 100.0f, 0.0f, 1, 100.0f);
    (void)roster.add(roster.addType(typeB), -100.0f, 0.0f, 1, 100.0f);

    CHECK(rm::sim::aimAtTargets(roster.store, roster.catalog, armies) == 1);
    CHECK(roster.transform(shooter).heading == rm::sim::kBradQuarterTurn);
}

TEST_CASE("a moving unit is not turned by aiming, and a turreted one has no reason to") {
    const std::vector<Army> armies = rm::sim::freeForAll(2);

    SECTION("moving: its order decides where it points") {
        Weapon fixed = directFire(10.0f, 300.0f);
        fixed.turreted = false;

        Roster roster;
        const UnitId gunner =
            roster.add(roster.addType(gunnerDef(fixed)), 0.0f, 0.0f, 0, 100.0f);
        roster.motion(gunner).moving = true;
        (void)roster.add(roster.addType(targetDef()), 100.0f, 0.0f, 1, 100.0f);

        CHECK(rm::sim::aimAtTargets(roster.store, roster.catalog, armies) == 0);
        CHECK(roster.transform(gunner).heading == 0);
    }
    SECTION("turreted: the turret aims, not the hull") {
        Weapon turret = directFire(10.0f, 300.0f);
        turret.turreted = true;

        Roster roster;
        (void)roster.add(roster.addType(gunnerDef(turret)), 0.0f, 0.0f, 0, 100.0f);
        (void)roster.add(roster.addType(targetDef()), 100.0f, 0.0f, 1, 100.0f);

        CHECK(rm::sim::aimAtTargets(roster.store, roster.catalog, armies) == 0);
    }
}

TEST_CASE("a unit facing the wrong way holds its shot rather than spending it") {
    // The reload must NOT be consumed while turning: a unit that spent its shot waiting to
    // line up would fire far more slowly than its blueprint says.
    const std::vector<Army> armies = rm::sim::freeForAll(2);

    Weapon fixed = directFire(10.0f, 300.0f);
    fixed.turreted = false;
    fixed.firingToleranceBrads = rm::unitdef::firingToleranceBradsFromDegrees(2.0f);

    Roster roster;
    const UnitId gunner =
        roster.add(roster.addType(gunnerDef(fixed)), 0.0f, 0.0f, 0, 100.0f);
    roster.transform(gunner).heading = rm::sim::kBradHalfTurn;  // facing away

    // Due +Z, a bearing of 0.
    (void)roster.add(roster.addType(targetDef()), 0.0f, 100.0f, 1, 100.0f);

    std::vector<Projectile> shots;
    for (int tick = 0; tick < 30; ++tick) {
        CHECK(rm::sim::fireWeapons(roster.store, roster.catalog, armies, shots, rm::sim::TickRate{}) == 0);
    }
    CHECK(shots.empty());

    // Turn it round and it fires on the very next tick, its reload never having been spent.
    roster.transform(gunner).heading = 0;
    CHECK(rm::sim::fireWeapons(roster.store, roster.catalog, armies, shots, rm::sim::TickRate{}) == 1);
}

TEST_CASE("a priority row outranks distance, however far away it is") {
    // Retail's ordering is (range/arc class, priority row, score, incumbency), and the ROW is
    // the one term that is a true lexicographic key rather than a score adjustment (`C-157`).
    // A row-0 match therefore beats a row-1 match at any distance — which is what makes an
    // anti-air gun ignore the tank standing next to it.
    const std::vector<Army> armies = rm::sim::freeForAll(2);

    Roster roster;
    UnitDef bomberDef = targetDef();
    bomberDef.name = "test_air";
    bomberDef.categories = {"AIR", "MOBILE"};
    UnitDef tankDef = targetDef();
    tankDef.name = "test_land";
    tankDef.categories = {"LAND", "MOBILE"};

    const rm::UnitTypeIndex air = roster.addType(bomberDef);
    const rm::UnitTypeIndex land = roster.addType(tankDef);

    const UnitId farAir = roster.add(air, 0.0f, 250.0f, 1, 100.0f);
    (void)roster.add(land, 0.0f, 20.0f, 1, 100.0f);  // far nearer, and wanted less

    Weapon weapon = directFire(10.0f, 300.0f);
    weapon.targetPriorities = {{"AIR"}, {"LAND"}};

    const auto target = rm::sim::nearestTarget(rm::test::at(0, 0, 0), 0, weapon, roster.store,
                                               armies, nullptr, &roster.catalog);
    REQUIRE(target.has_value());
    CHECK(*target == farAir);  // the distant air unit, not the tank at arm's length
}

TEST_CASE("automatic acquisition keeps a per-weapon incumbent until a strictly better target appears") {
    const std::vector<Army> armies = rm::sim::freeForAll(2);

    Weapon weapon = directFire(10.0f, 300.0f);
    weapon.beam = true;  // make the selected target observable on this firing tick
    weapon.turreted = false;
    Roster roster;
    const UnitId gunner =
        roster.add(roster.addType(gunnerDef(weapon)), 0.0f, 0.0f, 0, 100.0f);
    const rm::UnitTypeIndex target = roster.addType(targetDef());
    // The grid sees this equal-score challenger first. Without incumbent preference it wins the
    // tie solely because of that insertion order.
    const UnitId challenger = roster.add(target, 0.0f, 100.0f, 1, 100.0f);
    const UnitId incumbent = roster.add(target, 0.0f, -100.0f, 1, 100.0f);
    roster.health(gunner).automaticTargets = {incumbent};
    roster.transform(gunner).heading = rm::sim::kBradHalfTurn;

    std::vector<Projectile> shots;
    // A fixed weapon must aim at its retained incumbent too. Re-deriving the equal challenger
    // here would turn the hull away from the target that fireWeapons keeps.
    CHECK(rm::sim::aimAtTargets(roster.store, roster.catalog, armies) == 0);
    CHECK(rm::sim::fireWeapons(roster.store, roster.catalog, armies, shots, roster.rate) == 1);
    CHECK(roster.health(incumbent).current == rm::test::mag(90.0f));
    CHECK(roster.health(challenger).current == rm::test::mag(100.0f));
    CHECK(roster.health(gunner).automaticTargets == std::vector<UnitId>{incumbent});

    // A strictly nearer candidate replaces the incumbent. Equal score did not; this one does.
    const UnitId better = roster.add(target, 0.0f, -50.0f, 1, 100.0f);
    roster.health(gunner).reloadRemaining[0] = 0;
    CHECK(rm::sim::fireWeapons(roster.store, roster.catalog, armies, shots, roster.rate) == 1);
    CHECK(roster.health(better).current == rm::test::mag(90.0f));
    CHECK(roster.health(gunner).automaticTargets == std::vector<UnitId>{better});
}

TEST_CASE("a stale explicit Attack does not fall through to automatic acquisition") {
    const std::vector<Army> armies = rm::sim::freeForAll(2);

    Weapon weapon = directFire(10.0f, 300.0f);
    weapon.beam = true;
    weapon.turreted = false;
    Roster roster;
    const UnitId gunner =
        roster.add(roster.addType(gunnerDef(weapon)), 0.0f, 0.0f, 0, 100.0f);
    const rm::UnitTypeIndex target = roster.addType(targetDef());
    const UnitId stale = roster.add(target, 0.0f, 100.0f, 1, 100.0f);
    const UnitId automatic = roster.add(target, 0.0f, 50.0f, 1, 100.0f);
    roster.health(gunner).automaticTargets = {automatic};
    roster.transform(gunner).heading = rm::sim::kBradHalfTurn;
    roster.store.kill(stale);

    rm::sim::Command attack{.kind = rm::sim::CommandKind::Attack,
                            .unit = gunner,
                            .target = stale};
    (void)roster.store.orders()[gunner.index].give(attack, false);
    roster.store.orders()[gunner.index].markCurrentActive();

    std::vector<Projectile> shots;
    CHECK(rm::sim::aimAtTargets(roster.store, roster.catalog, armies) == 0);
    CHECK(roster.transform(gunner).heading == rm::sim::kBradHalfTurn);
    CHECK(rm::sim::fireWeapons(roster.store, roster.catalog, armies, shots, roster.rate) == 0);
    CHECK(roster.health(automatic).current == rm::test::mag(100.0f));
    CHECK(roster.health(gunner).automaticTargets == std::vector<UnitId>{automatic});
}

TEST_CASE("an automatic incumbent cannot survive its target slot being recycled") {
    const std::vector<Army> armies = rm::sim::freeForAll(2);

    Weapon weapon = directFire(10.0f, 300.0f);
    weapon.beam = true;
    weapon.turreted = true;
    Roster roster;
    const UnitId gunner =
        roster.add(roster.addType(gunnerDef(weapon)), 0.0f, 0.0f, 0, 100.0f);
    const rm::UnitTypeIndex target = roster.addType(targetDef());
    const UnitId incumbent = roster.add(target, 0.0f, 100.0f, 1, 100.0f);
    roster.health(gunner).automaticTargets = {incumbent};
    roster.store.kill(incumbent);
    const UnitId replacement = roster.add(target, 0.0f, 50.0f, 1, 100.0f);
    REQUIRE(replacement.index == incumbent.index);
    REQUIRE(replacement.generation != incumbent.generation);

    std::vector<Projectile> shots;
    CHECK(rm::sim::fireWeapons(roster.store, roster.catalog, armies, shots, roster.rate) == 1);
    CHECK(roster.health(replacement).current == rm::test::mag(90.0f));
    CHECK(roster.health(gunner).automaticTargets == std::vector<UnitId>{replacement});
}

TEST_CASE("automatic acquisition clears an incumbent moved outside the playable rectangle") {
    const std::vector<Army> armies = rm::sim::freeForAll(2);
    Weapon weapon = directFire(10.0f, 300.0f);
    weapon.beam = true;
    Roster roster;
    const UnitId gunner =
        roster.add(roster.addType(gunnerDef(weapon)), 0.0f, 0.0f, 0, 100.0f);
    const rm::UnitTypeIndex target = roster.addType(targetDef());
    const UnitId incumbent = roster.add(target, 0.0f, 50.0f, 1, 100.0f);
    roster.health(gunner).automaticTargets = {incumbent};
    const rm::sim::PlayableRect playable{
        .minX = rm::test::fx(-100.0f),
        .maxX = rm::test::fx(100.0f),
        .minZ = rm::test::fx(-100.0f),
        .maxZ = rm::test::fx(100.0f),
    };

    roster.transform(incumbent).z = rm::test::fx(150.0f);
    roster.reindex();
    std::vector<Projectile> shots;
    CHECK(rm::sim::fireWeapons(roster.store, roster.catalog, armies, shots, roster.rate,
                               nullptr, nullptr, &playable)
          == 0);
    CHECK(roster.health(gunner).automaticTargets == std::vector<UnitId>{UnitId{}});
}

TEST_CASE("an explicit Attack may fire outside the playable rectangle") {
    const std::vector<Army> armies = rm::sim::freeForAll(2);
    Weapon weapon = directFire(10.0f, 300.0f);
    weapon.beam = true;
    Roster roster;
    const UnitId gunner =
        roster.add(roster.addType(gunnerDef(weapon)), 0.0f, 0.0f, 0, 100.0f);
    const UnitId target = roster.add(roster.addType(targetDef()), 0.0f, 150.0f, 1, 100.0f);
    const rm::sim::PlayableRect playable{
        .minX = rm::test::fx(-100.0f),
        .maxX = rm::test::fx(100.0f),
        .minZ = rm::test::fx(-100.0f),
        .maxZ = rm::test::fx(100.0f),
    };
    const rm::sim::Command attack{.kind = rm::sim::CommandKind::Attack,
                                   .unit = gunner,
                                   .target = target};
    (void)roster.store.orders()[gunner.index].give(attack, false);
    roster.store.orders()[gunner.index].markCurrentActive();

    std::vector<Projectile> shots;
    CHECK(rm::sim::fireWeapons(roster.store, roster.catalog, armies, shots, roster.rate,
                               nullptr, nullptr, &playable)
          == 1);
    CHECK(roster.health(target).current == rm::test::mag(90.0f));
}

TEST_CASE("a target beyond the weapon's height reach is not a target at all") {
    // Retail folds "too far" and "cannot elevate" into one class, so a weapon that cannot look
    // up treats a target above it as unreachable rather than merely distant (`C-167`).
    const std::vector<Army> armies = rm::sim::freeForAll(2);

    Roster roster;
    const rm::UnitTypeIndex type = roster.addType(targetDef());
    const UnitId high = roster.add(type, 0.0f, 30.0f, 1, 100.0f);
    roster.transform(high).y = rm::test::fx(200.0f);

    Weapon weapon = directFire(10.0f, 300.0f);
    weapon.maxHeightDifference = rm::test::fx(50.0f);

    CHECK_FALSE(rm::sim::nearestTarget(rm::test::at(0, 0, 0), 0, weapon, roster.store, armies)
                    .has_value());

    // Zero means UNLIMITED, not "must be exactly level" — no shipped weapon states 0, and
    // reading it literally would stop every weapon shooting anything on a slope.
    weapon.maxHeightDifference = rm::sim::Fx{};
    CHECK(rm::sim::nearestTarget(rm::test::at(0, 0, 0), 0, weapon, roster.store, armies)
              .has_value());
}

TEST_CASE("a flare diverts a matching hostile projectile onto its owner") {
    // C-088 (c): the Aeon decoy. ART-S007 `lua/sim/defaultantiprojectile.lua` — a Flare
    // entity attached to its owner retargets category-matching hostile shots onto the
    // owner (`other:SetNewTarget(self.Owner)`), never damaging them. UAB4201 authors
    // `Flare = {Category = 'MISSILE', Radius = 15}` in ogrids. A shot with no matching
    // category, a friendly shot, and a shot outside the radius all fly on untouched.
    const std::vector<Army> armies = rm::sim::freeForAll(2);
    // Sorted, as the category check binary-searches.
    const std::vector<std::string> missile{"MISSILE", "TACTICAL"};

    const auto flareUnit = [&](Roster& roster) {
        Weapon decoy = directFire(10.0f, 300.0f);
        decoy.flare = Weapon::Flare{.category = "MISSILE",
                                    .radiusElmos = rm::test::fx(120.0f)};
        return roster.add(roster.addType(gunnerDef(decoy)), 0.0f, 0.0f, 0, 100.0f);
    };
    const auto incoming = [&](int x, int z, int vx) {
        Projectile shot{.position = rm::test::at(x, 4, z),
                        .velocity = rm::test::at(vx, 0, 0),
                        .firedByArmy = 1,
                        .ticksRemaining = 10,
                        .categories = missile};
        return shot;
    };
    const auto flyOne = [&](Roster& roster, Projectile shot) {
        std::vector<Projectile> shots{shot};
        rm::sim::advanceProjectiles(shots, roster.store, armies,
                                    rm::sim::Terrain{flatField()}, roster.rate, nullptr,
                                    &roster.catalog);
        return shots;
    };

    SECTION("a missile crossing the radius is turned onto the flare owner") {
        Roster roster;
        (void)flareUnit(roster);
        // Flying past well inside the 120-elmo radius, moving away from the owner.
        const std::vector<Projectile> shots = flyOne(roster, incoming(80, 0, 20));
        REQUIRE(shots.size() == 1);
        // The velocity now points back at the owner: negative x, still level in z.
        CHECK(shots.front().velocity[0] < rm::sim::Fx{});
        CHECK(shots.front().velocity[2] == rm::sim::Fx{});
    }

    SECTION("a category mismatch flies on") {
        Roster roster;
        (void)flareUnit(roster);
        // An interceptor carries ANTIMISSILE, not MISSILE — the flare is not its decoy.
        Projectile shot = incoming(100, 0, 20);
        shot.categories = std::vector<std::string>{"ANTIMISSILE"};
        const std::vector<Projectile> shots = flyOne(roster, shot);
        REQUIRE(shots.size() == 1);
        CHECK(shots.front().velocity[0] > rm::sim::Fx{});
    }

    SECTION("a friendly missile flies on") {
        Roster roster;
        (void)flareUnit(roster);
        Projectile shot = incoming(100, 0, 20);
        shot.firedByArmy = 0;
        const std::vector<Projectile> shots = flyOne(roster, shot);
        REQUIRE(shots.size() == 1);
        CHECK(shots.front().velocity[0] > rm::sim::Fx{});
    }

    SECTION("a missile outside the radius flies on") {
        Roster roster;
        (void)flareUnit(roster);
        const std::vector<Projectile> shots = flyOne(roster, incoming(500, 0, 20));
        REQUIRE(shots.size() == 1);
        CHECK(shots.front().velocity[0] > rm::sim::Fx{});
    }
}

TEST_CASE("a redirector turns an enemy missile back on its launcher") {
    // C-088's Cybran MissileRedirect (ART-S007 `lua/sim/defaultantiprojectile.lua`): a
    // redirector retargets a non-strategic enemy MISSILE onto its launcher
    // (`other:SetNewTarget(self.Enemy)`), rate-limited by `RedirectRateOfFire`. URL0303
    // authors `Defense.AntiMissile = {Radius = 5, RedirectRateOfFire = 1}`. Each redirect
    // costs one full rate cycle; while cooling, the unit only watches.
    const std::vector<Army> armies = rm::sim::freeForAll(2);
    // Sorted, as the category check binary-searches.
    const std::vector<std::string> missile{"MISSILE", "TACTICAL"};

    Roster roster;
    (void)roster.add(roster.addType(targetDef()), 0.0f, 0.0f, 0, 100.0f);
    const UnitId launcher = roster.add(roster.addType(targetDef()), 300.0f, 0.0f, 1, 100.0f);
    const auto flyOne = [&](std::vector<rm::sim::MissileRedirect>& redirects,
                            rm::sim::Projectile shot) {
        std::vector<Projectile> shots{shot};
        rm::sim::advanceProjectiles(shots, roster.store, armies,
                                    rm::sim::Terrain{flatField()}, roster.rate, nullptr,
                                    &roster.catalog, redirects);
        return shots;
    };
    const auto incoming = [&]() {
        Projectile shot{.position = rm::test::at(100, 4, 0),
                        .velocity = rm::test::at(-20, 0, 0),
                        .firedBy = launcher,
                        .firedByArmy = 1,
                        .ticksRemaining = 10,
                        .categories = missile};
        return shot;
    };

    SECTION("a missile in radius is re-aimed at its launcher and the rate is spent") {
        std::vector<rm::sim::MissileRedirect> redirects{
            rm::sim::MissileRedirect{.owner = roster.store.idAt(0),
                                     .radiusElmos = rm::sim::fxFromFloat(120.0f),
                                     .cooldownTicks = 10,
                                     .remaining = 0}};
        const std::vector<Projectile> shots = flyOne(redirects, incoming());
        REQUIRE(shots.size() == 1);
        // Was flying away from the launcher at x=300; now flies back toward it.
        CHECK(shots.front().velocity[0] > rm::sim::Fx{});
        CHECK(redirects.front().remaining == 10);
    }

    SECTION("a cooling redirector only watches") {
        std::vector<rm::sim::MissileRedirect> redirects{
            rm::sim::MissileRedirect{.owner = roster.store.idAt(0),
                                     .radiusElmos = rm::sim::fxFromFloat(120.0f),
                                     .cooldownTicks = 10,
                                     .remaining = 10}};
        const std::vector<Projectile> shots = flyOne(redirects, incoming());
        REQUIRE(shots.size() == 1);
        CHECK(shots.front().velocity[0] < rm::sim::Fx{});
        CHECK(redirects.front().remaining == 9);
    }

    SECTION("a strategic missile is not redirected") {
        std::vector<rm::sim::MissileRedirect> redirects{
            rm::sim::MissileRedirect{.owner = roster.store.idAt(0),
                                     .radiusElmos = rm::sim::fxFromFloat(120.0f),
                                     .cooldownTicks = 10,
                                     .remaining = 0}};
        Projectile shot = incoming();
        shot.categories = std::vector<std::string>{"MISSILE", "STRATEGIC"};
        const std::vector<Projectile> shots = flyOne(redirects, shot);
        REQUIRE(shots.size() == 1);
        CHECK(shots.front().velocity[0] < rm::sim::Fx{});
        CHECK(redirects.front().remaining == 0);
    }

    SECTION("a friendly missile is not redirected") {
        std::vector<rm::sim::MissileRedirect> redirects{
            rm::sim::MissileRedirect{.owner = roster.store.idAt(0),
                                     .radiusElmos = rm::sim::fxFromFloat(120.0f),
                                     .cooldownTicks = 10,
                                     .remaining = 0}};
        Projectile shot = incoming();
        shot.firedByArmy = 0;
        const std::vector<Projectile> shots = flyOne(redirects, shot);
        REQUIRE(shots.size() == 1);
        CHECK(shots.front().velocity[0] < rm::sim::Fx{});
        CHECK(redirects.front().remaining == 0);
    }
}
