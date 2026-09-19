// Player-perspective coverage for the FA weapon claims (WP-36; see
// docs/fa-exe-analysis-plan.md and build/re-fa/findings/). Each TEST_CASE cites
// its claim IDs.
//
// The picks are what a player watches happen: the Galactic Colossus's tractor
// claw vacuuming a tank off the ground and crushing it against the muzzle
// (C-381) — the one bespoke experimental mechanic that is neither a projectile
// nor a beam in the usual sense.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "core/data/MoveDef.hpp"
#include "core/map/HeightField.hpp"
#include "core/sim/Command.hpp"
#include "core/sim/Economy.hpp"
#include "core/sim/Pathfinding.hpp"
#include "core/sim/Skirmish.hpp"
#include "core/unit/UnitDef.hpp"

#include "support/FxMatchers.hpp"
#include "support/TestRoster.hpp"

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

/// A UAL0401-shaped Galactic Colossus: two tractor claws, keyed the way the
/// catalog keys every script-only rule — blueprint id plus the weapon `Label`
/// the script binds (`C-265`'s precedent, `C-381`). The shipped claw states
/// `Damage = 0.01`, `RateOfFire = 1`, `MaxRadius = 40`, `Turreted = true`.
[[nodiscard]] UnitDef colossusDef() {
    UnitDef def;
    def.name = "UAL0401";
    def.categories = {"AEON", "EXPERIMENTAL", "LAND", "MOBILE"};
    def.speedElmosPerSecond = 2.5f;
    for (const char* label : {"RightArmTractor", "LeftArmTractor"}) {
        Weapon claw;
        claw.label = label;
        claw.role = rm::unitdef::WeaponRole::Other;  // 'Experimental' maps here
        claw.targetPriorities = {{"ALLUNITS"}};
        claw.turreted = true;
        claw.damage = rm::sim::magFromFloat(0.01f);
        claw.maxRange = Fx::fromInt(40);
        claw.minRange = Fx::fromInt(2);
        claw.rateOfFire = 1.0f;
        claw.targetLayers = rm::unitdef::TargetLayerMask::Surface;
        def.weapons.push_back(claw);
    }
    return def;
}

/// The claw's lawful prey: a mobile land unit — no STRUCTURE, COMMAND,
/// EXPERIMENTAL, NAVAL or SUBCOMMANDER tag (`aeonweapons.lua:53-60`).
[[nodiscard]] UnitDef tankDef() {
    UnitDef def;
    def.name = "test_tank";
    def.categories = {"LAND", "MOBILE"};
    def.speedElmosPerSecond = 4.0f;
    return def;
}

/// A building the claw may aim at but must never grab: the script refuses
/// STRUCTURE inside `PlayFxBeamStart`, AFTER the weapon has already fired.
[[nodiscard]] UnitDef structureDef() {
    UnitDef def;
    def.name = "test_structure";
    def.categories = {"STRUCTURE"};
    return def;
}

[[nodiscard]] Match loneMatch(std::vector<Army>& armies,
                              std::vector<rm::sim::Economy>& economies,
                              std::vector<int>& commandersEver,
                              std::vector<rm::sim::Projectile>& projectiles) {
    Match match{.armies = armies,
                .economies = economies,
                .commandersEver = commandersEver};
    match.projectiles = &projectiles;
    return match;
}

} // namespace

TEST_CASE("C-381: the tractor claw vacuums a tank to the muzzle and crushes it",
          "[fa-weapons]") {
    // `aeonweapons.lua` `ADFTractorClaw.TractorThread`: the target is
    // `SetDoNotTarget(true)`, `AttachBoneTo(-1, unit, muzzle)`'d, then a
    // slider at speed 15 retracts the bone to (0,0,0) — and on arrival the
    // target is `Kill`ed outright, credited to the colossus. The player sees
    // the tank leave the ground, glide to the arm, and die.
    const rm::HeightField field = flatField();
    const rm::sim::Terrain terrain{field};

    rm::test::Roster roster;
    const rm::UnitTypeIndex colossusType = roster.addType(colossusDef());
    const rm::UnitTypeIndex tankType = roster.addType(tankDef());
    const UnitId colossus = roster.add(colossusType, 100.0f, 100.0f, 0, 9000.0f);
    const UnitId tank = roster.add(tankType, 120.0f, 100.0f, 1, 500.0f);

    std::vector<Army> armies = rm::sim::freeForAll(2);
    std::vector<rm::sim::Economy> economies(2);
    std::vector<int> commandersEver(2, 0);
    std::vector<rm::sim::Projectile> projectiles;
    Match match = loneMatch(armies, economies, commandersEver, projectiles);

    // Phase one: the grab. The tank ends up attached to the colossus and
    // marked do-not-target, exactly as `TractorThread` leaves it.
    bool grabbed = false;
    for (int i = 0; i < 100 && !grabbed; ++i) {
        (void)rm::sim::tickSkirmish(roster.store, roster.catalog, match, terrain);
        grabbed = roster.motion(tank).attached;
    }
    REQUIRE(grabbed);
    CHECK(roster.store.parentOf(tank) == colossus);
    CHECK(roster.store.doNotTarget(tank));

    // Phase two: the pull. The tank rides toward the colossus — the slider's
    // retraction — and dies on arrival, which is also what frees the arm.
    bool crushed = false;
    for (int i = 0; i < 200 && !crushed; ++i) {
        (void)rm::sim::tickSkirmish(roster.store, roster.catalog, match, terrain);
        crushed = !roster.store.alive(tank);
    }
    CHECK(crushed);
    // `TractorWatchThread`'s half: the corpse is off the bone, not left
    // hanging under the arm.
    CHECK(!roster.store.parentOf(tank).has_value());
}

TEST_CASE("C-381: the claw refuses structures and shoots them instead",
          "[fa-weapons]") {
    // The category gate sits inside `PlayFxBeamStart`, not in the aiming
    // code: a building inside the arc is still a valid target, the beam
    // fires, and only the grab is skipped — so the structure takes the
    // claw's ordinary (0.01) damage and stays on its foundation.
    const rm::HeightField field = flatField();
    const rm::sim::Terrain terrain{field};

    rm::test::Roster roster;
    const rm::UnitTypeIndex colossusType = roster.addType(colossusDef());
    const rm::UnitTypeIndex structureType = roster.addType(structureDef());
    (void)roster.add(colossusType, 100.0f, 100.0f, 0, 9000.0f);
    const UnitId structure = roster.add(structureType, 120.0f, 100.0f, 1, 500.0f);

    std::vector<Army> armies = rm::sim::freeForAll(2);
    std::vector<rm::sim::Economy> economies(2);
    std::vector<int> commandersEver(2, 0);
    std::vector<rm::sim::Projectile> projectiles;
    Match match = loneMatch(armies, economies, commandersEver, projectiles);

    for (int i = 0; i < 100; ++i) {
        (void)rm::sim::tickSkirmish(roster.store, roster.catalog, match, terrain);
    }
    CHECK(roster.store.alive(structure));
    CHECK(!roster.motion(structure).attached);
    CHECK(!roster.store.doNotTarget(structure));
    // The fallback shot landed: 100 ticks of a 1/s claw is far more than the
    // 500 hp the structure started with if every shot dealt real damage —
    // but 0.01 a shot is not, so it stands with a dented hull.
    CHECK(roster.health(structure).current < rm::sim::Mag::fromInt(500));
    CHECK(roster.health(structure).current > rm::sim::Mag::fromInt(499));
}

TEST_CASE("C-381: a claw victim walks free when the colossus dies mid-pull",
          "[fa-weapons]") {
    // `TractorWatchThread` detaches whatever the muzzle holds when its watch
    // ends — and the colossus dying ends every watch. Retail's cargo cascade
    // (`C-197`) does NOT apply: the victim is not transport cargo, it is a
    // unit the script attached, and the script's own teardown drops it alive.
    const rm::HeightField field = flatField();
    const rm::sim::Terrain terrain{field};

    rm::test::Roster roster;
    const rm::UnitTypeIndex colossusType = roster.addType(colossusDef());
    const rm::UnitTypeIndex tankType = roster.addType(tankDef());
    const UnitId colossus = roster.add(colossusType, 100.0f, 100.0f, 0, 9000.0f);
    const UnitId tank = roster.add(tankType, 140.0f, 100.0f, 1, 500.0f);

    std::vector<Army> armies = rm::sim::freeForAll(2);
    std::vector<rm::sim::Economy> economies(2);
    std::vector<int> commandersEver(2, 0);
    std::vector<rm::sim::Projectile> projectiles;
    Match match = loneMatch(armies, economies, commandersEver, projectiles);

    bool grabbed = false;
    for (int i = 0; i < 100 && !grabbed; ++i) {
        (void)rm::sim::tickSkirmish(roster.store, roster.catalog, match, terrain);
        grabbed = roster.motion(tank).attached;
    }
    REQUIRE(grabbed);

    // The colossus dies with the tank still on the arm — the pull is 40
    // elmos at 15/s, so a kill now lands mid-retraction.
    roster.store.kill(colossus);
    CHECK(roster.store.alive(tank));
    CHECK(!roster.motion(tank).attached);
    CHECK(!roster.store.doNotTarget(tank));
}
