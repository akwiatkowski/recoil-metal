// Player-perspective coverage for the FA-NAVY claims (WP-23 surface naval
// movement, WP-24 submarines; see docs/fa-exe-analysis-plan.md and
// build/re-fa/coverage/FA-AIR-NAVY.md). Each TEST_CASE cites its claim IDs.
//
// The picks here are the parts of the claim set a player can observe: which
// layer a sub spawns on (C-324), that a sub keeps sailing while it changes
// depth (C-320), that an attack order never surfaces a boat (C-323's
// AutoSurfaceMode defaults OFF), and that a submerged deck gun stays silent
// while the torpedo still answers (C-202/C-323 gating).
//
// Also covered: the firer-side water gates comparing Y against the unit's own
// `Physics.Elevation` datum (C-321), the Seabed-only target-side gates
// (C-322), `Air.FlyInWater` as a hard fire reject (C-327), and AutoSurfaceMode
// surfacing a sub that holds an attack task (C-203).
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "app/SceneBuild.hpp"  // motionFor — the one derivation both spawn paths use
#include "core/map/HeightField.hpp"
#include "core/sim/Combat.hpp"
#include "core/sim/Command.hpp"
#include "core/sim/Movement.hpp"
#include "core/sim/Pathfinding.hpp"
#include "core/sim/Skirmish.hpp"
#include "core/unit/UnitDef.hpp"

#include "support/FxMatchers.hpp"
#include "support/TestRoster.hpp"

#include <algorithm>
#include <vector>

using Catch::Approx;
using rm::sim::Army;
using rm::sim::CommandIssue;
using rm::sim::CommandKind;
using rm::sim::Fx;
using rm::sim::Match;
using rm::sim::Player;
using rm::sim::UnitId;
using rm::unitdef::UnitDef;
using rm::unitdef::Weapon;

namespace {

/// A flat seabed: a test about water layers is not also a test about terrain.
[[nodiscard]] rm::HeightField flatField() {
    rm::HeightField field;
    field.squaresX = 100;
    field.squaresZ = 100;
    field.baseHeight = 0.0f;
    field.heightScale = 1.0f;
    field.raw.assign(field.sampleCount(), std::uint16_t{0});
    return field;
}

/// Water 80 elmos deep over the whole field — deep enough that the authored
/// -12 elmo keel never meets the 2-elmo seabed clearance (C-200).
[[nodiscard]] rm::sim::Terrain waterTerrain(const rm::HeightField& field) {
    return rm::sim::Terrain{field, /*hasWater=*/true, /*waterLevelElmos=*/80.0f};
}

/// A Tigershark-shaped boat: a SurfacingSub with an authored keel 12 elmos
/// under the waterline (UES0203's -1.5 ogrids), the retail 8-elmo/s dive rate,
/// and the Dive command cap the toggle checks at intake (C-323).
[[nodiscard]] UnitDef subDef(bool experimental = false) {
    UnitDef def;
    def.name = experimental ? "test_experimental_sub" : "test_sub";
    // Categories must stay sorted: hasCategory is a binary search.
    def.categories = experimental ? std::vector<std::string>{"EXPERIMENTAL", "MOBILE", "SUBMERSIBLE"}
                                  : std::vector<std::string>{"MOBILE", "SUBMERSIBLE"};
    def.motion = rm::unitdef::MotionType::SurfacingSub;
    def.speedElmosPerSecond = 6.0f;
    def.elevationElmos = -12.0f;
    def.diveSurfaceSpeedElmosPerSecond = 8.0f;
    def.commandCaps = {"RULEUCC_Dive"};
    def.commandCapsDeclared = true;
    def.health = rm::sim::Mag::fromInt(1000);
    return def;
}

/// A surface ship, for the sub to shoot at.
[[nodiscard]] UnitDef shipDef() {
    UnitDef def;
    def.name = "test_ship";
    def.categories = {"MOBILE", "NAVAL"};
    def.motion = rm::unitdef::MotionType::Water;
    def.speedElmosPerSecond = 4.0f;
    def.health = rm::sim::Mag::fromInt(1000);
    return def;
}

/// Spawn through `motionFor`, the same derivation the scene builder uses —
/// `Roster::add` fills a generic MoveState and would never set the sub flags.
[[nodiscard]] UnitId spawnDef(rm::test::Roster& roster, rm::UnitTypeIndex type,
                              const UnitDef& def, float x, float z, int army) {
    const UnitId id = roster.store.spawn(rm::sim::UnitStore::Spawn{
        .type = type,
        .transform = {.x = rm::sim::fxFromFloat(x), .z = rm::sim::fxFromFloat(z)},
        .motion = rm::app::motionFor(def, army),
        .health = [&] {
            rm::sim::Health health = rm::sim::initialHealth(def.health);
            health.reloadRemaining = std::vector<int>(def.weapons.size(), 0);
            return health;
        }(),
    });
    roster.reindex();
    return id;
}

[[nodiscard]] Match loneMatch(std::vector<Army>& armies,
                              std::vector<rm::sim::Economy>& economies,
                              std::vector<int>& commandersEver,
                              std::span<const rm::sim::PassabilityGrid* const> grids = {}) {
    return Match{.armies = armies,
                 .economies = economies,
                 .passability = grids,
                 .commandersEver = commandersEver};
}

} // namespace

TEST_CASE("C-324: an experimental surfacing sub spawns on the surface, an ordinary one submerged",
          "[fa-navy]") {
    // Retail derives the spawn layer from caps and the EXPERIMENTAL category
    // (ART-E001 0x631800): every shipped SurfacingSub spawns Sub except the
    // seven EXPERIMENTAL ones, which spawn Water. The player sees this as "my
    // sub starts under water; the experimental starts afloat".
    const rm::HeightField field = flatField();
    const rm::sim::Terrain terrain = waterTerrain(field);

    rm::test::Roster roster;
    const UnitDef ordinary = subDef();
    const UnitDef experimental = subDef(/*experimental=*/true);
    const rm::UnitTypeIndex ordinaryType = roster.addType(ordinary);
    const rm::UnitTypeIndex experimentalType = roster.addType(experimental);
    const UnitId boat = spawnDef(roster, ordinaryType, ordinary, 300.0f, 300.0f, 0);
    const UnitId experimentalBoat =
        spawnDef(roster, experimentalType, experimental, 400.0f, 300.0f, 0);

    CHECK(roster.motion(boat).submersible);
    CHECK(roster.motion(boat).submerged);
    CHECK(roster.motion(boat).diveTargetSubmerged);
    CHECK(roster.motion(boat).submarineOffset == Fx::fromInt(-12));

    CHECK(roster.motion(experimentalBoat).submersible);
    CHECK_FALSE(roster.motion(experimentalBoat).submerged);
    CHECK_FALSE(roster.motion(experimentalBoat).diveTargetSubmerged);
    CHECK(roster.motion(experimentalBoat).submarineOffset == Fx{});

    // One tick of the water mover settles each hull on its own layer: the
    // ordinary boat rides 12 elmos under the 80-elmo waterline, the
    // experimental floats on it.
    rm::sim::tick(roster.store.transforms(), roster.store.motion(), terrain);
    CHECK(rm::sim::fxToFloat(roster.transform(boat).y) == Approx(68.0f));
    CHECK(rm::sim::fxToFloat(roster.transform(experimentalBoat).y) == Approx(80.0f));
}

TEST_CASE("C-320: a sub keeps sailing while it changes depth", "[fa-navy]") {
    // CalcMoveWater is the shared mover plus the dive stepper — there is no
    // separate submarine locomotion, so a boat mid-dive still answers a move
    // order at full speed. The player sees the dive as a vertical ease, not a
    // pause in the journey.
    const rm::HeightField field = flatField();
    const rm::sim::Terrain terrain = waterTerrain(field);

    rm::test::Roster roster;
    const UnitDef sub = subDef();
    const rm::UnitTypeIndex type = roster.addType(sub);
    const UnitId boat = spawnDef(roster, type, sub, 300.0f, 300.0f, 0);
    auto& motion = roster.motion(boat);
    REQUIRE(motion.submerged);
    // Ordered along +Z, the direction the spawn transform already faces, so
    // the check is about the shared mover's stride rather than the turn.
    rm::sim::orderTo(motion, terrain, rm::sim::fxFromFloat(300.0f),
                     rm::sim::fxFromFloat(500.0f));
    const Fx startZ = roster.transform(boat).z;
    for (int i = 0; i < 100; ++i) {
        rm::sim::tick(roster.store.transforms(), roster.store.motion(), terrain);
    }

    // 6 elmos/s at 10 ticks/s is 0.6 elmos a tick: a hundred ticks of shared
    // mover carries the boat ~60 elmos while the stepper holds it under.
    CHECK(rm::sim::fxToFloat(roster.transform(boat).z - startZ)
          == Approx(60.0f).margin(2.0f));
    CHECK(motion.submerged);
    CHECK(rm::sim::fxToFloat(roster.transform(boat).y) == Approx(68.0f));
}

TEST_CASE("C-323: an attack order never surfaces a sub; only the Dive toggle does",
          "[fa-navy]") {
    // AutoSurfaceMode defaults OFF and its only retail consumer is the attack
    // task — which this engine does not model — so ordering a submerged boat
    // to attack leaves it under water. Surfacing is the player's explicit
    // Dive toggle, and the toggle reads the COMMITTED layer: a second Dive
    // mid-surfacing does not send the boat back down.
    const rm::HeightField field = flatField();
    const rm::sim::Terrain terrain = waterTerrain(field);
    const rm::sim::PassabilityGrid water =
        rm::sim::buildSurfaceWaterPassability(field, 80.0f);

    rm::test::Roster roster;
    UnitDef sub = subDef();
    Weapon torpedo;
    torpedo.label = "Torpedo";
    torpedo.role = rm::unitdef::WeaponRole::DirectFire;
    torpedo.targetPriorities = {{"MOBILE"}};
    torpedo.turreted = true;
    torpedo.damage = rm::sim::Mag::fromInt(50);
    torpedo.maxRange = Fx::fromInt(200);
    torpedo.rateOfFire = 1.0f;
    torpedo.muzzleVelocityElmosPerSecond = 100.0f;
    torpedo.targetLayers = rm::unitdef::TargetLayerMask::Surface;
    sub.weapons.push_back(torpedo);
    const UnitDef ship = shipDef();
    const rm::UnitTypeIndex subType = roster.addType(sub);
    const rm::UnitTypeIndex shipType = roster.addType(ship);
    const UnitId boat = spawnDef(roster, subType, sub, 300.0f, 300.0f, 0);
    const UnitId target = spawnDef(roster, shipType, ship, 300.0f, 400.0f, 1);
    rm::sim::placeOnMotionLayer(roster.transform(target), roster.motion(target), terrain);
    roster.reindex();

    const std::vector<Player> players{Player{.index = 0, .army = 0}};
    std::vector<Army> armies = rm::sim::freeForAll(2);
    std::vector<rm::sim::Economy> economies(2);
    std::vector<int> commandersEver(2, 0);
    std::vector<const rm::sim::PassabilityGrid*> grids(roster.catalog.size(), &water);
    const auto gridFor = [&water](UnitId) { return &water; };
    Match match = loneMatch(armies, economies, commandersEver, grids);
    match.passabilitySubmerged = grids;

    auto& motion = roster.motion(boat);
    REQUIRE(motion.submerged);

    // Attack the surface ship. Whatever the order does to the queue, it must
    // not touch the dive state — the boat stays committed to Sub.
    const auto attack = rm::sim::applyCommand(
        CommandIssue{.source = 0,
                     .id = rm::commandId(0, 1),
                     .player = 0,
                     .kind = CommandKind::Attack,
                     .units = {boat},
                     .targetX = rm::sim::fxFromFloat(300.0f),
                     .targetZ = rm::sim::fxFromFloat(400.0f),
                     .target = target},
        roster.store, roster.catalog, players, armies, terrain, gridFor, roster.rate);
    CHECK(attack.accepted.size() == 1);
    for (int i = 0; i < 30; ++i) {
        (void)rm::sim::tickSkirmish(roster.store, roster.catalog, match, terrain);
    }
    CHECK(motion.submerged);
    CHECK(motion.diveTargetSubmerged);

    // The Dive order is the toggle (C-323: target = (cur==Sub) ? Water : Sub).
    const auto dive = rm::sim::applyCommand(
        CommandIssue{.source = 0,
                     .id = rm::commandId(0, 2),
                     .player = 0,
                     .kind = CommandKind::Dive,
                     .units = {boat}},
        roster.store, roster.catalog, players, armies, terrain, gridFor, roster.rate);
    CHECK(dive.accepted.size() == 1);
    CHECK(motion.diveTargetSubmerged == false);
    CHECK(motion.submerged);  // still committed to Sub until the ease finishes

    // A second Dive while the boat is still committed to Sub picks Water
    // again — the toggle reads the committed layer, not the transition.
    const auto diveAgain = rm::sim::applyCommand(
        CommandIssue{.source = 0,
                     .id = rm::commandId(0, 3),
                     .player = 0,
                     .kind = CommandKind::Dive,
                     .units = {boat}},
        roster.store, roster.catalog, players, armies, terrain, gridFor, roster.rate);
    CHECK(diveAgain.accepted.size() == 1);
    CHECK(motion.diveTargetSubmerged == false);

    // The ease commits at the endpoint (C-200): the boat surfaces and stays.
    for (int i = 0; i < 600 && motion.submerged; ++i) {
        (void)rm::sim::tickSkirmish(roster.store, roster.catalog, match, terrain);
    }
    REQUIRE_FALSE(motion.submerged);
    CHECK(rm::sim::fxToFloat(roster.transform(boat).y) == Approx(80.0f));
}

TEST_CASE("C-202/C-323: a submerged deck gun stays silent while the torpedo answers",
          "[fa-navy]") {
    // The deck gun's FireTargetLayerCapsTable has no Sub row, so a submerged
    // boat cannot fire it — while the torpedo, allowed on both source layers,
    // fires from either. The player sees the gun wake up exactly when the boat
    // finishes surfacing, not before.
    const rm::HeightField field = flatField();
    const rm::sim::Terrain terrain = waterTerrain(field);

    rm::test::Roster roster;
    UnitDef sub = subDef();
    Weapon deckGun;
    deckGun.label = "DeckGun";
    deckGun.role = rm::unitdef::WeaponRole::DirectFire;
    deckGun.targetPriorities = {{"MOBILE"}};
    deckGun.turreted = true;
    deckGun.damage = rm::sim::Mag::fromInt(50);
    deckGun.maxRange = Fx::fromInt(200);
    deckGun.rateOfFire = 1.0f;
    deckGun.muzzleVelocityElmosPerSecond = 100.0f;
    // Water row: surface targets. Sub row: absent — nothing fires while under.
    deckGun.submarineSourceCaps = std::array<rm::unitdef::Weapon::LayerCaps, 2>{
        rm::unitdef::Weapon::LayerCaps{.targets = rm::unitdef::TargetLayerMask::Surface,
                                       .submerged = false},
        rm::unitdef::Weapon::LayerCaps{.targets = rm::unitdef::TargetLayerMask::None,
                                       .submerged = false}};
    Weapon torpedo;
    torpedo.label = "Torpedo";
    torpedo.role = rm::unitdef::WeaponRole::DirectFire;
    torpedo.targetPriorities = {{"MOBILE"}};
    torpedo.turreted = true;
    torpedo.damage = rm::sim::Mag::fromInt(50);
    torpedo.maxRange = Fx::fromInt(200);
    torpedo.rateOfFire = 1.0f;
    torpedo.muzzleVelocityElmosPerSecond = 100.0f;
    torpedo.submarineSourceCaps = std::array<rm::unitdef::Weapon::LayerCaps, 2>{
        rm::unitdef::Weapon::LayerCaps{.targets = rm::unitdef::TargetLayerMask::Surface,
                                       .submerged = false},
        rm::unitdef::Weapon::LayerCaps{.targets = rm::unitdef::TargetLayerMask::Surface,
                                       .submerged = false}};
    sub.weapons = {deckGun, torpedo};
    const UnitDef ship = shipDef();
    const rm::UnitTypeIndex subType = roster.addType(sub);
    const rm::UnitTypeIndex shipType = roster.addType(ship);
    const UnitId boat = spawnDef(roster, subType, sub, 300.0f, 300.0f, 0);
    const UnitId target = spawnDef(roster, shipType, ship, 300.0f, 400.0f, 1);
    rm::sim::placeOnMotionLayer(roster.transform(target), roster.motion(target), terrain);
    roster.reindex();

    const std::vector<Army> armies = rm::sim::freeForAll(2);
    auto& motion = roster.motion(boat);
    REQUIRE(motion.submerged);

    const auto fires = [&](std::string_view label) {
        roster.health(boat).reloadRemaining.assign(sub.weapons.size(), 0);
        rm::sim::EventQueue events;
        std::vector<rm::sim::Projectile> shots;
        (void)rm::sim::fireWeapons(roster.store, roster.catalog, armies, shots,
                                   roster.rate, &events);
        return std::ranges::any_of(events.all(), [&](const rm::sim::Event& event) {
            return event.visualId == std::string{sub.name} + ":" + std::string{label};
        });
    };

    // Submerged: the torpedo answers, the deck gun does not.
    CHECK(fires("Torpedo"));
    CHECK_FALSE(fires("DeckGun"));

    // Surfaced: both answer.
    motion.diveTargetSubmerged = false;
    for (int i = 0; i < 600 && motion.submerged; ++i) {
        rm::sim::tick(roster.store.transforms(), roster.store.motion(), terrain);
    }
    REQUIRE_FALSE(motion.submerged);
    CHECK(fires("Torpedo"));
    CHECK(fires("DeckGun"));
}

TEST_CASE("C-321: the fire gates compare the firer's Y against its own Elevation datum",
          "[fa-navy]") {
    // `isAbove = firerY > Physics.Elevation` — the unit's OWN datum, never the
    // waterline. Give the boat a datum between its keel and the surface and
    // the gates flip with the dive: submerged (y 68 < 74) the AboveWater gun
    // is silent and the BelowWater gun answers; surfaced (y 80 > 74) they
    // trade places. A unit with no authored Elevation keeps retail's −10000
    // default, is always "above", and its BelowWaterFireOnly weapon can never
    // fire — the reason the corpus ships zero of them.
    const rm::HeightField field = flatField();
    const rm::sim::Terrain terrain = waterTerrain(field);

    rm::test::Roster roster;
    UnitDef sub = subDef();
    sub.waterGateElevationElmos = 74.0f;  // authored ogrids ×8, between keel and surface
    const auto gun = [](std::string_view label, bool above, bool below) {
        Weapon weapon;
        weapon.label = std::string{label};
        weapon.role = rm::unitdef::WeaponRole::DirectFire;
        weapon.targetPriorities = {{"MOBILE"}};
        weapon.turreted = true;
        weapon.damage = rm::sim::Mag::fromInt(50);
        weapon.maxRange = Fx::fromInt(200);
        weapon.rateOfFire = 1.0f;
        weapon.muzzleVelocityElmosPerSecond = 100.0f;
        weapon.aboveWaterFireOnly = above;
        weapon.belowWaterFireOnly = below;
        return weapon;
    };
    sub.weapons = {gun("AboveGun", true, false), gun("BelowGun", false, true)};

    UnitDef defaultSub = subDef();
    defaultSub.name = "test_default_sub";
    defaultSub.weapons = {gun("NeverGun", false, true)};  // default datum: never below

    const UnitDef ship = shipDef();
    const rm::UnitTypeIndex subType = roster.addType(sub);
    const rm::UnitTypeIndex defaultType = roster.addType(defaultSub);
    const rm::UnitTypeIndex shipType = roster.addType(ship);
    const UnitId boat = spawnDef(roster, subType, sub, 300.0f, 300.0f, 0);
    const UnitId defaultBoat = spawnDef(roster, defaultType, defaultSub, 500.0f, 300.0f, 0);
    const UnitId target = spawnDef(roster, shipType, ship, 300.0f, 400.0f, 1);
    rm::sim::placeOnMotionLayer(roster.transform(target), roster.motion(target), terrain);
    roster.reindex();

    const std::vector<Army> armies = rm::sim::freeForAll(2);
    auto& motion = roster.motion(boat);
    REQUIRE(motion.submerged);
    REQUIRE(roster.motion(defaultBoat).submerged);

    const auto fires = [&](UnitId shooter, const UnitDef& def, std::string_view label) {
        roster.health(shooter).reloadRemaining.assign(def.weapons.size(), 0);
        rm::sim::EventQueue events;
        std::vector<rm::sim::Projectile> shots;
        (void)rm::sim::fireWeapons(roster.store, roster.catalog, armies, shots,
                                   roster.rate, &events);
        return std::ranges::any_of(events.all(), [&](const rm::sim::Event& event) {
            return event.visualId == std::string{def.name} + ":" + std::string{label};
        });
    };

    // Submerged at y 68, below the 74-elmo datum: Below answers, Above does not.
    CHECK(fires(boat, sub, "BelowGun"));
    CHECK_FALSE(fires(boat, sub, "AboveGun"));
    // The default-datum boat is always "above": its BelowWater gun never fires.
    CHECK_FALSE(fires(defaultBoat, defaultSub, "NeverGun"));

    // Surfaced at y 80, above the datum: the guns trade places.
    motion.diveTargetSubmerged = false;
    for (int i = 0; i < 600 && motion.submerged; ++i) {
        rm::sim::tick(roster.store.transforms(), roster.store.motion(), terrain);
    }
    REQUIRE_FALSE(motion.submerged);
    CHECK(fires(boat, sub, "AboveGun"));
    CHECK_FALSE(fires(boat, sub, "BelowGun"));
}

TEST_CASE("C-322: the target gates apply to Seabed-layer candidates only",
          "[fa-navy]") {
    // `AboveWaterTargetsOnly`/`BelowWaterTargetsOnly` test a SEABED candidate's
    // Y against its own `Physics.Elevation` datum — a ground unit under water.
    // A submerged sub is the Sub layer, not Seabed, so the flags are inert
    // against it; and with the −10000 default every seabed unit reads "above",
    // which is why `BelowWaterTargetsOnly` is unshipped.
    const rm::HeightField field = flatField();
    const rm::sim::Terrain terrain = waterTerrain(field);

    rm::test::Roster roster;
    UnitDef ship = shipDef();
    const auto gun = [](std::string_view label, bool above, bool below) {
        Weapon weapon;
        weapon.label = std::string{label};
        weapon.role = rm::unitdef::WeaponRole::DirectFire;
        weapon.targetPriorities = {{"MOBILE"}};
        weapon.turreted = true;
        weapon.damage = rm::sim::Mag::fromInt(50);
        weapon.maxRange = Fx::fromInt(400);
        weapon.rateOfFire = 1.0f;
        weapon.muzzleVelocityElmosPerSecond = 100.0f;
        weapon.aboveWaterTargetsOnly = above;
        weapon.belowWaterTargetsOnly = below;
        return weapon;
    };
    ship.weapons = {gun("AboveGun", true, false), gun("BelowGun", false, true)};

    // Two seabed walkers: one on the default datum (always "above"), one with
    // an authored datum above the waterline (always "below").
    UnitDef walker = shipDef();
    walker.name = "test_walker";
    walker.motion = rm::unitdef::MotionType::Amphibious;
    UnitDef deepWalker = walker;
    deepWalker.name = "test_deep_walker";
    deepWalker.waterGateElevationElmos = 90.0f;  // above the 80-elmo waterline

    const UnitDef sub = subDef();
    const rm::UnitTypeIndex shipType = roster.addType(ship);
    const rm::UnitTypeIndex walkerType = roster.addType(walker);
    const rm::UnitTypeIndex deepType = roster.addType(deepWalker);
    const rm::UnitTypeIndex subType = roster.addType(sub);
    const UnitId shooter = spawnDef(roster, shipType, ship, 300.0f, 300.0f, 0);
    const UnitId walkerId = spawnDef(roster, walkerType, walker, 320.0f, 300.0f, 1);
    const UnitId deepId = spawnDef(roster, deepType, deepWalker, 340.0f, 300.0f, 1);
    const UnitId subId = spawnDef(roster, subType, sub, 700.0f, 300.0f, 1);
    rm::sim::placeOnMotionLayer(roster.transform(shooter), roster.motion(shooter), terrain);
    rm::sim::placeOnMotionLayer(roster.transform(walkerId), roster.motion(walkerId), terrain);
    rm::sim::placeOnMotionLayer(roster.transform(deepId), roster.motion(deepId), terrain);
    roster.reindex();

    // One movement tick publishes the Seabed layer: both walkers sit at y 0
    // under the 80-elmo waterline; the sub is Sub, not Seabed.
    rm::sim::tick(roster.store.transforms(), roster.store.motion(), terrain);
    CHECK(roster.motion(walkerId).seabed);
    CHECK(roster.motion(deepId).seabed);
    CHECK_FALSE(roster.motion(subId).seabed);

    const std::vector<Army> armies = rm::sim::freeForAll(2);
    const auto firesAt = [&](std::string_view label, UnitId victim) {
        roster.health(shooter).reloadRemaining.assign(ship.weapons.size(), 0);
        rm::sim::EventQueue events;
        std::vector<rm::sim::Projectile> shots;
        (void)rm::sim::fireWeapons(roster.store, roster.catalog, armies, shots,
                                   roster.rate, &events);
        return std::ranges::any_of(events.all(), [&](const rm::sim::Event& event) {
            return event.visualId == std::string{ship.name} + ":" + std::string{label}
                   && event.instigator == victim;
        });
    };

    // The default-datum walker reads "above" its datum: AboveGun takes it,
    // BelowGun refuses it. The authored-datum walker reads "below": the
    // mirror image. And the submerged sub is the Sub layer — both flags are
    // inert against it, so AboveGun can still engage it.
    CHECK(firesAt("AboveGun", walkerId));
    CHECK(firesAt("BelowGun", deepId));
    CHECK_FALSE(firesAt("AboveGun", deepId));
    CHECK_FALSE(firesAt("BelowGun", walkerId));
    // The flags are inert against the Sub layer: put the submerged sub in
    // range as the NEAREST candidate and BOTH guns engage it — neither Above
    // nor Below applies to a Sub-layer target.
    roster.transform(subId).x = Fx::fromInt(310.0f);
    roster.reindex();
    CHECK(firesAt("AboveGun", subId));
    CHECK(firesAt("BelowGun", subId));
}

TEST_CASE("C-327: an Air-layer unit below the waterline cannot fire unless FlyInWater",
          "[fa-navy]") {
    // `Air.FlyInWater=false` + Air layer + below the waterline is a hard
    // `CanFire` reject — "aircraft cannot fire while submerged". The test is
    // the water plane, not the `Physics.Elevation` datum the other gates
    // read: put a plane under it and its gun stays silent; author
    // `FlyInWater` and the same plane answers.
    const rm::HeightField field = flatField();
    const rm::sim::Terrain terrain = waterTerrain(field);

    rm::test::Roster roster;
    const auto planeDef = [](bool flyInWater) {
        UnitDef def;
        def.name = flyInWater ? "test_seaplane" : "test_plane";
        def.categories = {"AIR", "MOBILE"};
        def.motion = rm::unitdef::MotionType::Air;
        def.speedElmosPerSecond = 10.0f;
        def.airWinged = true;
        def.waterGateElevationElmos = 100.0f;  // cruise datum above the waterline
        def.airFlyInWater = flyInWater;
        def.health = rm::sim::Mag::fromInt(500);
        Weapon gun;
        gun.label = "Gun";
        gun.role = rm::unitdef::WeaponRole::DirectFire;
        gun.targetPriorities = {{"MOBILE"}};
        gun.turreted = true;
        gun.damage = rm::sim::Mag::fromInt(50);
        gun.maxRange = Fx::fromInt(200);
        gun.rateOfFire = 1.0f;
        gun.muzzleVelocityElmosPerSecond = 100.0f;
        def.weapons = {gun};
        return def;
    };
    const UnitDef plane = planeDef(false);
    const UnitDef seaplane = planeDef(true);
    const UnitDef ship = shipDef();
    const rm::UnitTypeIndex planeType = roster.addType(plane);
    const rm::UnitTypeIndex seaplaneType = roster.addType(seaplane);
    const rm::UnitTypeIndex shipType = roster.addType(ship);
    const UnitId dry = spawnDef(roster, planeType, plane, 300.0f, 300.0f, 0);
    const UnitId wet = spawnDef(roster, seaplaneType, seaplane, 320.0f, 300.0f, 0);
    const UnitId target = spawnDef(roster, shipType, ship, 300.0f, 400.0f, 1);
    rm::sim::placeOnMotionLayer(roster.transform(target), roster.motion(target), terrain);
    // Both planes sit at y 50 — below the 80-elmo waterline, the submerged
    // case `unit+0x120 == Air` + `y < waterLevel` describes. The movement
    // tick owns `belowWater`; this fixture writes `transform.y` directly, so
    // it sets the flag the tick would compute.
    roster.transform(dry).y = Fx::fromInt(50);
    roster.transform(wet).y = Fx::fromInt(50);
    roster.motion(dry).belowWater = true;
    roster.motion(wet).belowWater = true;
    roster.reindex();

    const std::vector<Army> armies = rm::sim::freeForAll(2);
    const auto fires = [&](UnitId shooter, const UnitDef& def) {
        roster.health(shooter).reloadRemaining.assign(def.weapons.size(), 0);
        rm::sim::EventQueue events;
        std::vector<rm::sim::Projectile> shots;
        (void)rm::sim::fireWeapons(roster.store, roster.catalog, armies, shots,
                                   roster.rate, &events);
        return std::ranges::any_of(events.all(), [&](const rm::sim::Event& event) {
            return event.visualId == std::string{def.name} + ":Gun";
        });
    };

    CHECK_FALSE(fires(dry, plane));   // FlyInWater=false: the reject holds
    CHECK(fires(wet, seaplane));      // FlyInWater=true: the same plane fires
}

TEST_CASE("C-203: AutoSurfaceMode surfaces a submerged sub that holds an attack task",
          "[fa-navy]") {
    // The toggle's one retail consumer is the attack task: with the mode ON
    // the task picks the Water layer and the boat surfaces to engage — the
    // same `diveTargetSubmerged` flip the manual Dive order writes, so the
    // deck gun wakes exactly when the hull commits to the surface.
    const rm::HeightField field = flatField();
    const rm::sim::Terrain terrain = waterTerrain(field);
    const rm::sim::PassabilityGrid water =
        rm::sim::buildSurfaceWaterPassability(field, 80.0f);

    rm::test::Roster roster;
    UnitDef sub = subDef();
    sub.autoSurfaceToAttack = true;
    Weapon deckGun;
    deckGun.label = "DeckGun";
    deckGun.role = rm::unitdef::WeaponRole::DirectFire;
    deckGun.targetPriorities = {{"MOBILE"}};
    deckGun.turreted = true;
    deckGun.damage = rm::sim::Mag::fromInt(50);
    deckGun.maxRange = Fx::fromInt(200);
    deckGun.rateOfFire = 1.0f;
    deckGun.muzzleVelocityElmosPerSecond = 100.0f;
    // Water row only: the gun answers from the surface, never from under.
    deckGun.submarineSourceCaps = std::array<rm::unitdef::Weapon::LayerCaps, 2>{
        rm::unitdef::Weapon::LayerCaps{.targets = rm::unitdef::TargetLayerMask::Surface,
                                       .submerged = false},
        rm::unitdef::Weapon::LayerCaps{.targets = rm::unitdef::TargetLayerMask::None,
                                       .submerged = false}};
    sub.weapons = {deckGun};
    const UnitDef ship = shipDef();
    const rm::UnitTypeIndex subType = roster.addType(sub);
    const rm::UnitTypeIndex shipType = roster.addType(ship);
    const UnitId boat = spawnDef(roster, subType, sub, 300.0f, 300.0f, 0);
    const UnitId target = spawnDef(roster, shipType, ship, 300.0f, 400.0f, 1);
    rm::sim::placeOnMotionLayer(roster.transform(target), roster.motion(target), terrain);
    roster.reindex();

    const std::vector<Player> players{Player{.index = 0, .army = 0}};
    std::vector<Army> armies = rm::sim::freeForAll(2);
    std::vector<rm::sim::Economy> economies(2);
    std::vector<int> commandersEver(2, 0);
    std::vector<const rm::sim::PassabilityGrid*> grids(roster.catalog.size(), &water);
    const auto gridFor = [&water](UnitId) { return &water; };
    Match match = loneMatch(armies, economies, commandersEver, grids);
    match.passabilitySubmerged = grids;
    rm::sim::EventQueue events;
    match.events = &events;
    std::vector<rm::sim::Projectile> projectiles;
    match.projectiles = &projectiles;  // fireWeapons runs only when the match owns a volley list

    auto& motion = roster.motion(boat);
    REQUIRE(motion.submerged);
    REQUIRE(motion.autoSurface);  // motionFor copied the authored flag

    const auto attack = rm::sim::applyCommand(
        CommandIssue{.source = 0,
                     .id = rm::commandId(0, 1),
                     .player = 0,
                     .kind = CommandKind::Attack,
                     .units = {boat},
                     .targetX = rm::sim::fxFromFloat(300.0f),
                     .targetZ = rm::sim::fxFromFloat(400.0f),
                     .target = target},
        roster.store, roster.catalog, players, armies, terrain, gridFor, roster.rate);
    REQUIRE(attack.accepted.size() == 1);

    // The attack task picks the Water layer the first tick it runs — the boat
    // commits to surfacing without a Dive order ever being issued.
    (void)rm::sim::tickSkirmish(roster.store, roster.catalog, match, terrain);
    CHECK_FALSE(motion.diveTargetSubmerged);

    // The ease commits at the endpoint (C-200): the boat surfaces and the
    // deck gun — silent while submerged — engages the ship it was sent after.
    for (int i = 0; i < 600 && motion.submerged; ++i) {
        (void)rm::sim::tickSkirmish(roster.store, roster.catalog, match, terrain);
    }
    CHECK_FALSE(motion.submerged);
    CHECK(rm::sim::fxToFloat(roster.transform(boat).y) == Approx(80.0f));
    CHECK(std::ranges::any_of(events.all(), [&](const rm::sim::Event& event) {
        return event.kind == rm::sim::EventKind::WeaponFired
               && event.visualId == std::string{sub.name} + ":DeckGun"
               && event.instigator == target;
    }));
}
