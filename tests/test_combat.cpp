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

[[nodiscard]] Weapon directFire(float damage, float rangeElmos, float radiusElmos = 0.0f) {
    Weapon weapon;
    weapon.label = "test gun";
    weapon.role = WeaponRole::DirectFire;
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
