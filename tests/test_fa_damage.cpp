// Player-perspective coverage for the FA-DAMAGE claims (WP-30/31/32; see
// docs/fa-exe-analysis-plan.md and build/re-fa/coverage/FA-DAMAGE.md, plus
// C-262's ring-detonation slice from FA-TRANSPORT-MISSILES.md). Each
// TEST_CASE cites its claim IDs.
//
// The picks are what a player watches: a shield bubble soaking fire until it
// collapses and only then the hull bleeding (C-143/C-144/C-145), and an ACU
// death blast that is annihilating up close and merely dangerous at the rim
// (C-262's two-ring detonation on the death-weapon path).
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "core/map/HeightField.hpp"
#include "core/sim/Combat.hpp"
#include "core/sim/Skirmish.hpp"
#include "core/unit/UnitBlueprint.hpp"
#include "core/unit/UnitDef.hpp"

#include "support/FxMatchers.hpp"
#include "support/TestRoster.hpp"

#include <cstdlib>
#include <filesystem>
#include <vector>

using Catch::Approx;
using rm::sim::Army;
using rm::sim::Fx;
using rm::sim::Match;
using rm::sim::UnitId;
using rm::unitdef::UnitDef;
using rm::unitdef::Weapon;

namespace {

[[nodiscard]] rm::HeightField flatField() {
    rm::HeightField field;
    field.squaresX = 100;
    field.squaresZ = 100;
    field.baseHeight = 0.0f;
    field.heightScale = 1.0f;
    field.raw.assign(field.sampleCount(), std::uint16_t{0});
    return field;
}

[[nodiscard]] Match loneMatch(std::vector<Army>& armies,
                              std::vector<rm::sim::Economy>& economies,
                              std::vector<int>& commandersEver) {
    return Match{.armies = armies,
                 .economies = economies,
                 .commandersEver = commandersEver};
}

/// A turreted gun, so a test about damage is not also a test about facing.
[[nodiscard]] Weapon turretedGun(float damage, float rangeElmos) {
    Weapon weapon;
    weapon.label = "test gun";
    weapon.role = rm::unitdef::WeaponRole::DirectFire;
    weapon.targetPriorities = {{"MOBILE"}};
    weapon.turreted = true;
    weapon.damage = rm::test::mag(damage);
    weapon.maxRange = rm::test::fx(rangeElmos);
    weapon.rateOfFire = 1.0f;
    weapon.muzzleVelocityElmosPerSecond = 200.0f;
    return weapon;
}

/// An ordinary bubble: 100 points over a radius-80 dome, with a recharge
/// delay far beyond the test so a collapsed bubble stays down.
[[nodiscard]] UnitDef shieldDef() {
    UnitDef def;
    def.name = "test_shield_generator";
    def.categories = {"STRUCTURE"};
    def.shield.maximum = rm::sim::Mag::fromInt(100);
    def.shield.radiusElmos = Fx::fromInt(80);
    def.shield.regenPerSecond = 10.0f;
    // Already imported durations: FA's WaitSeconds adds one 100 ms tick.
    def.shield.regenDelay = rm::sim::seconds(1.1f);
    def.shield.rechargeDelay = rm::sim::seconds(600.0f);
    return def;
}

[[nodiscard]] UnitDef mobileDef(std::string name = "test_unit") {
    UnitDef def;
    def.name = std::move(name);
    def.categories = {"MOBILE"};
    return def;
}

/// The extracted retail units tree, or empty when the corpus is not on disk.
[[nodiscard]] std::filesystem::path corpusUnitsDir() {
    const char* home = std::getenv("HOME");
    if (home == nullptr) {
        return {};
    }
    const std::filesystem::path dir =
        std::filesystem::path{home} / "projects/llm/input/faf/units";
    return std::filesystem::exists(dir) ? dir : std::filesystem::path{};
}

} // namespace

TEST_CASE("C-143/C-144/C-145: a bubble soaks fire until it collapses, then the hull bleeds",
          "[fa-damage]") {
    // The shield rewrite's player-facing contract: every covering bubble
    // absorbs for the targets under it, charged once per blast, and a hull
    // under a live dome takes nothing. When the bubble's strength (stored in
    // the same fields as hull health, C-144) is spent it collapses — and
    // while it recharges (C-145) the hull behind it is exposed.
    const rm::HeightField field = flatField();
    const rm::sim::Terrain terrain{field};

    rm::test::Roster roster;
    const rm::UnitTypeIndex shieldType = roster.addType(shieldDef());
    const rm::UnitTypeIndex targetType = roster.addType(mobileDef());
    UnitDef shooterDef = mobileDef("test_shooter");
    shooterDef.weapons.push_back(turretedGun(30.0f, 200.0f));
    const rm::UnitTypeIndex shooterType = roster.addType(shooterDef);

    const UnitId generator = roster.add(shieldType, 0.0f, 0.0f, 1, 1000.0f);
    const UnitId sheltered = roster.add(targetType, 20.0f, 0.0f, 1, 1000.0f);
    const UnitId shooter = roster.add(shooterType, 60.0f, 0.0f, 0, 500.0f);
    (void)shooter;

    std::vector<Army> armies = rm::sim::freeForAll(2);
    std::vector<rm::sim::Economy> economies(2);
    std::vector<rm::sim::Projectile> projectiles;
    std::vector<int> commandersEver(2, 0);
    rm::sim::EventQueue events;
    Match match = loneMatch(armies, economies, commandersEver);
    match.projectiles = &projectiles;
    match.events = &events;

    bool sawCollapse = false;
    for (int i = 0; i < 600; ++i) {
        (void)rm::sim::tickSkirmish(roster.store, roster.catalog, match, terrain);
        sawCollapse = sawCollapse || roster.health(generator).shield.current == rm::sim::Mag{};
        if (!sawCollapse) {
            // While the dome stands, the hull under it takes nothing — the
            // bubble pays instead (C-143: per-target absorption).
            INFO("tick " << i);
            CHECK(roster.health(sheltered).current == rm::sim::Mag::fromInt(1000));
        }
    }

    // 30 damage a second against a 100-point dome: it must have fallen, and
    // only then may the sheltered hull show damage.
    REQUIRE(sawCollapse);
    CHECK(roster.health(generator).shield.current == rm::sim::Mag{});
    CHECK(roster.health(sheltered).current < rm::sim::Mag::fromInt(1000));
    CHECK(events.count(rm::sim::EventKind::ShieldCollapsed) >= 1);
}

TEST_CASE("C-262: a commander's death blast is annihilating up close and dangerous at the rim",
          "[fa-damage][corpus]") {
    // The ACU death weapon carries NukeInnerRing (45000 damage inside 30
    // elmos) and NukeOuterRing (5000 inside 40): the two-ring detonation the
    // Yolona Oss analysis recovered (C-262), implemented on the death-weapon
    // path. The player sees the blast erase what stands next to the corpse
    // and only scorch what stands at the rim.
    const std::filesystem::path units = corpusUnitsDir();
    if (units.empty()) {
        SKIP("the FA corpus is not extracted");
    }
    const auto acuDef = rm::unitbp::loadFile(units / "UEL0001/UEL0001_unit.bp");
    REQUIRE(acuDef.has_value());
    const rm::unitdef::Weapon* death = rm::sim::deathWeapon(*acuDef);
    REQUIRE(death != nullptr);
    // The authored rings, checked against the blueprint rather than assumed:
    // 30 and 40 OGRIDS, which the loader converts to elmos like every other
    // authored distance (×8).
    CHECK(rm::test::asFloat(death->innerRingDamage) == Approx(45000.0f));
    CHECK(rm::test::asFloat(death->innerRingRadius) == Approx(240.0f));
    CHECK(rm::test::asFloat(death->outerRingDamage) == Approx(5000.0f));
    CHECK(rm::test::asFloat(death->outerRingRadius) == Approx(320.0f));
    const rm::HeightField field = flatField();
    const rm::sim::Terrain terrain{field};

    rm::test::Roster roster;
    const rm::UnitTypeIndex acuType = roster.addType(*acuDef);
    const rm::UnitTypeIndex targetType = roster.addType(mobileDef());
    const UnitId acu = roster.add(acuType, 400.0f, 400.0f, 0, 100.0f);
    // Inside the inner ring (240), inside only the outer ring (320), and
    // clear of both.
    const UnitId nearTarget = roster.add(targetType, 600.0f, 400.0f, 1, 100000.0f);
    const UnitId rimTarget = roster.add(targetType, 680.0f, 400.0f, 1, 100000.0f);
    const UnitId farTarget = roster.add(targetType, 800.0f, 400.0f, 1, 100000.0f);

    std::vector<Army> armies = rm::sim::freeForAll(2);
    std::vector<rm::sim::Economy> economies(2);
    std::vector<rm::sim::Projectile> projectiles;
    std::vector<int> commandersEver(2, 0);
    Match match = loneMatch(armies, economies, commandersEver);
    match.projectiles = &projectiles;

    roster.health(acu).current = rm::sim::Mag{};
    const rm::sim::TickReport report =
        rm::sim::tickSkirmish(roster.store, roster.catalog, match, terrain);

    // 45000 + 5000 inside the inner ring: half the 100k hull is gone.
    CHECK(rm::test::asFloat(rm::sim::Mag::fromInt(100000)
                            - roster.health(nearTarget).current)
          == Approx(50000.0f).margin(10.0f));
    // 5000 at the rim: wounded, not erased.
    CHECK(roster.store.alive(rimTarget));
    CHECK(rm::test::asFloat(rm::sim::Mag::fromInt(100000)
                            - roster.health(rimTarget).current)
          == Approx(5000.0f).margin(1.0f));
    // Past 320 elmos the blast does not reach at all.
    CHECK(roster.health(farTarget).current == rm::sim::Mag::fromInt(100000));
}

TEST_CASE("C-397: a strategic missile's splash bypasses the bubble it penetrated",
          "[fa-damage]") {
    // `shield.lua` `Shield.OnCollisionCheck` (`C-397`): the STRATEGIC+MISSILE
    // gate refuses the collision, and the impact path threads that refusal
    // into the blast — the point hit and its splash land on the hull beneath
    // while the bubble pays nothing. The point-hit half is pinned in
    // test_shield.cpp; this pins the splash half, including a sheltered
    // bystander under the same dome that a normal blast would spare.
    rm::test::Roster roster;
    const rm::UnitTypeIndex shieldType = roster.addType(shieldDef());
    const rm::UnitTypeIndex targetType = roster.addType(mobileDef());
    const UnitId generator = roster.add(shieldType, 0.0f, 0.0f, 1, 100.0f);
    const UnitId sheltered = roster.add(targetType, 20.0f, 0.0f, 1, 100.0f);
    const std::vector<Army> armies = rm::sim::freeForAll(2);

    rm::sim::Projectile shot;
    shot.position = rm::test::at(0, 1, -100);
    shot.velocity = rm::test::at(0, 0, 120);
    shot.damage = rm::unitdef::flatDamage(rm::test::mag(40.0f));
    shot.damageRadiusElmos = rm::test::fx(30.0f);
    shot.targetLayers = rm::unitdef::TargetLayerMask::Surface;
    shot.firedByArmy = 0;
    shot.ticksRemaining = 2;
    shot.categories = {"MISSILE", "STRATEGIC"};
    std::vector<rm::sim::Projectile> shots{shot};

    const rm::HeightField field = flatField();

    rm::sim::advanceProjectiles(shots, roster.store, armies, rm::sim::Terrain{field},
                                roster.rate, nullptr, &roster.catalog);
    REQUIRE(shots.size() == 1);
    CHECK(shots.front().pendingImpact == rm::sim::ImpactType::Unit);
    CHECK(shots.front().impactTarget == generator);

    rm::sim::advanceProjectiles(shots, roster.store, armies, rm::sim::Terrain{field},
                                roster.rate, nullptr, &roster.catalog);

    CHECK(shots.empty());
    // The dome paid nothing: no point-hit charge, no splash admission.
    CHECK(roster.health(generator).shield.current == rm::sim::Mag::fromInt(100));
    // The 40-point blast lands once on each hull under it — hull 60 each —
    // and the dome pays nothing: no splash admission for a shot whose
    // shield refused the collision.
    CHECK(rm::test::asFloat(roster.health(generator).current) == Approx(60.0f));
    CHECK(rm::test::asFloat(roster.health(sheltered).current) == Approx(60.0f));
}
TEST_CASE("a crashed aircraft's DeathImpact is a crash blast, not a death-site detonation",
          "[fa-damage]") {
    // `lua/sim/Unit.lua:955-970` (`DoDeathWeapon`) detonates only the weapon
    // labelled `DeathWeapon`; the `DeathImpact` blast lives on the air-crash
    // path instead (`lua/defaultunits.lua:1469-1486`, `OnImpact` at the corpse's
    // crash position — 56 of the corpus's 99 `WeaponCategory = 'Death'`
    // weapons). A unit carrying ONLY a `DeathImpact` must therefore leave no
    // blast at its death site: the crater happens where the wreck hits the
    // ground, not where the unit died.
    rm::test::Roster roster;
    const rm::UnitTypeIndex targetType = roster.addType(mobileDef());
    const UnitId bystander = roster.add(targetType, 0.0f, 0.0f, 1, 100.0f);

    UnitDef crashing;
    crashing.name = "test_air_crash";
    crashing.categories = {"AIR", "MOBILE"};
    rm::unitdef::Weapon impact;
    impact.label = "DeathImpact";
    impact.role = rm::unitdef::WeaponRole::Death;
    impact.damage = rm::test::mag(100.0f);
    impact.maxRange = rm::test::fx(1.0f);
    impact.damageRadius = rm::test::fx(30.0f);
    crashing.weapons.push_back(impact);

    const rm::HeightField field = flatField();
    const rm::sim::Terrain terrain{field};
    std::vector<Army> armies = rm::sim::freeForAll(2);
    std::vector<rm::sim::Economy> economies(2);
    std::vector<int> commandersEver(2, 0);
    std::vector<rm::sim::Projectile> projectiles;
    rm::sim::EventQueue events;
    Match match = loneMatch(armies, economies, commandersEver);
    match.projectiles = &projectiles;
    match.events = &events;

    const rm::UnitTypeIndex crashType = roster.addType(crashing);
    const UnitId doomed = roster.add(crashType, 0.0f, 0.0f, 0, 100.0f);
    roster.health(doomed).current = rm::sim::Mag{};
    (void)rm::sim::tickSkirmish(roster.store, roster.catalog, match, terrain);

    // No detonation at the death site: the bystander standing on it is unhurt.
    CHECK(roster.health(bystander).current == rm::sim::Mag::fromInt(100));
}
TEST_CASE("a dummy script-driven DeathWeapon leaves no death-site blast",
          "[fa-damage]") {
    // `reference/supcom-wiki/pages/weapons/Blueprint_Weapon.md:311-316`
    // (`DummyWeapon`): the field "instructs the engine not to create a C++
    // weapon object" — a dummy is purely script-driven, and the death thread
    // never fires it. Corpus shape: every factory's `DeathWeapon`
    // (`UAB1101_unit.bp`: `Damage = 250`, `DamageRadius = 2`,
    // `DummyWeapon = true`, no `FireOnDeath`) versus the commanders'
    // (`UEL0001_unit.bp`: `FireOnDeath = true`, rings, no dummy flag).
    rm::test::Roster roster;
    const rm::UnitTypeIndex targetType = roster.addType(mobileDef());
    const UnitId bystander = roster.add(targetType, 0.0f, 0.0f, 1, 100.0f);

    UnitDef factory;
    factory.name = "test_factory_dummy_death";
    factory.categories = {"STRUCTURE"};
    rm::unitdef::Weapon dummy;
    dummy.label = "DeathWeapon";
    dummy.role = rm::unitdef::WeaponRole::Death;
    dummy.dummyWeapon = true;
    dummy.damage = rm::test::mag(250.0f);
    dummy.damageRadius = rm::test::fx(16.0f);
    factory.weapons.push_back(dummy);

    const rm::HeightField field = flatField();
    const rm::sim::Terrain terrain{field};
    std::vector<Army> armies = rm::sim::freeForAll(2);
    std::vector<rm::sim::Economy> economies(2);
    std::vector<int> commandersEver(2, 0);
    std::vector<rm::sim::Projectile> projectiles;
    rm::sim::EventQueue events;
    Match match = loneMatch(armies, economies, commandersEver);
    match.projectiles = &projectiles;
    match.events = &events;

    const rm::UnitTypeIndex factoryType = roster.addType(factory);
    const UnitId doomed = roster.add(factoryType, 0.0f, 0.0f, 0, 100.0f);
    roster.health(doomed).current = rm::sim::Mag{};
    const rm::sim::TickReport report =
        rm::sim::tickSkirmish(roster.store, roster.catalog, match, terrain);

    // No death-site detonation: the bystander on the corpse is unhurt and no
    // blast is reported.
    CHECK(roster.health(bystander).current == rm::sim::Mag::fromInt(100));
    CHECK(report.deathBlasts == 0);
}
