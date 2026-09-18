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
