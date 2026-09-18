// Player-perspective coverage for the FA-TRANSPORT claims (WP-25; see
// docs/fa-exe-analysis-plan.md and build/re-fa/coverage/FA-TRANSPORT-MISSILES.md).
// Each TEST_CASE cites its claim IDs.
//
// The picks are what a player watches happen: cargo riding its carrier's
// transform every tick (C-196), a shot-down transport taking its passengers
// with it (C-197), and a ferry holding at the beacon for a straggler (C-199).
//
// Deliberately absent: the 99% cargo-survival roll (C-197 — the kill cascade
// is unconditional here, a recorded divergence), carrier storage pools
// (C-225, not implemented), and the carrier/mobile-factory attach-store
// pattern (C-264, not implemented).
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "core/map/HeightField.hpp"
#include "core/sim/Command.hpp"
#include "core/sim/Skirmish.hpp"
#include "core/sim/Transport.hpp"
#include "core/unit/UnitDef.hpp"

#include "support/FxMatchers.hpp"
#include "support/TestRoster.hpp"

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

[[nodiscard]] rm::HeightField flatField() {
    rm::HeightField field;
    field.squaresX = 100;
    field.squaresZ = 100;
    field.baseHeight = 0.0f;
    field.heightScale = 1.0f;
    field.raw.assign(field.sampleCount(), std::uint16_t{0});
    return field;
}

/// A landed UEA0107-shaped carrier: six class-1 slots, a class-2 unit costs
/// two, a class-3 unit four, class 4 does not fit at all.
[[nodiscard]] UnitDef transportDef() {
    UnitDef def;
    def.name = "test_transport";
    def.categories = {"AIR", "MOBILE", "TRANSPORTATION"};  // sorted: hasCategory is a binary search
    def.motion = rm::unitdef::MotionType::Air;
    def.canFly = true;
    def.speedElmosPerSecond = 10.0f;
    def.commandCaps = {"RULEUCC_Guard", "RULEUCC_Transport"};
    def.commandCapsDeclared = true;
    def.transport.transportClass = 6;
    def.transport.class2AttachSize = 2;
    def.transport.class3AttachSize = 4;
    def.transport.airClass = true;
    return def;
}

[[nodiscard]] UnitDef cargoDef(int cls = 1) {
    UnitDef def;
    def.name = "test_cargo";
    def.categories = {"LAND", "MOBILE"};
    def.speedElmosPerSecond = 8.0f;
    def.commandCaps = {"RULEUCC_CallTransport"};
    def.commandCapsDeclared = true;
    def.transport.transportClass = cls;
    return def;
}

/// An anti-everything gun for the shoot-down case.
[[nodiscard]] UnitDef shooterDef() {
    UnitDef def;
    def.name = "test_shooter";
    def.categories = {"LAND", "MOBILE"};
    Weapon gun;
    gun.label = "test gun";
    gun.role = rm::unitdef::WeaponRole::DirectFire;
    gun.targetPriorities = {{"MOBILE"}};
    gun.turreted = true;
    gun.damage = rm::sim::Mag::fromInt(500);
    gun.maxRange = Fx::fromInt(200);
    gun.rateOfFire = 1.0f;
    gun.muzzleVelocityElmosPerSecond = 200.0f;
    gun.targetLayers = rm::unitdef::TargetLayerMask::Both;
    def.weapons.push_back(gun);
    return def;
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

/// The flight block a `canFly` unit needs to leave the deck — UEA0101's
/// control numbers, the same set `flyer()` in test_movement uses.
void giveFlight(rm::sim::MoveState& motion) {
    motion.canFly = true;
    motion.speedPerTick = Fx::fromInt(16);
    motion.airMaxSpeedElmosPerSec = Fx::fromInt(160);
    motion.airKMove = Fx::fromInt(1);
    motion.airKMoveDamping = Fx::fromInt(1);
    motion.airKLift = Fx::fromInt(3);
    motion.airKLiftDamping = Fx::fromRatio(5, 2);
    motion.airLiftFactor = Fx::fromInt(7);
    motion.airElevation = Fx::fromInt(80);
    motion.idleLandThreshold = std::numeric_limits<std::uint32_t>::max();  // no auto-land mid-test
    motion.fuelDrainPerTick = Fx::fromRatio(1, 5000);
    motion.fuelRatio = Fx::fromInt(1);
}

/// Spawn a transport already sitting on the deck.
[[nodiscard]] UnitId landedTransport(rm::test::Roster& roster, rm::UnitTypeIndex type,
                                     float x, float z, int army = 0) {
    const UnitId id = roster.add(type, x, z, army, 500.0f);
    giveFlight(roster.motion(id));
    roster.motion(id).airborne = false;
    roster.motion(id).airState = rm::sim::MoveState::AirState::Bottom;
    roster.transform(id).y = Fx{};
    return id;
}

[[nodiscard]] CommandIssue moveIssue(const UnitId unit, float x, float z,
                                     const std::uint32_t serial) {
    return CommandIssue{.source = 0,
                        .id = rm::commandId(0, serial),
                        .player = 0,
                        .kind = CommandKind::Move,
                        .units = {unit},
                        .targetX = rm::sim::fxFromFloat(x),
                        .targetZ = rm::sim::fxFromFloat(z)};
}

} // namespace

TEST_CASE("C-196: cargo rides its carrier's transform every tick", "[fa-transport]") {
    // Retail recomputes the attached transform into the pending slot every
    // tick (0x00680ab0). The player-visible half of that claim: a unit slung
    // under a transport tracks the hull exactly — same offset, every tick,
    // however the carrier moves.
    const rm::HeightField field = flatField();
    const rm::sim::Terrain terrain{field};
    const rm::sim::PassabilityGrid grid =
        rm::sim::buildPassability(field, 0.0f, 60.0f, 0.0f);

    rm::test::Roster roster;
    const rm::UnitTypeIndex transportType = roster.addType(transportDef());
    const rm::UnitTypeIndex cargoType = roster.addType(cargoDef());
    const UnitId transport = landedTransport(roster, transportType, 100.0f, 100.0f);
    const UnitId cargo = roster.add(cargoType, 103.0f, 100.0f, 0, 500.0f);
    REQUIRE(roster.store.attach(transport, cargo));

    std::vector<Army> armies = rm::sim::freeForAll(1);
    std::vector<rm::sim::Economy> economies(1);
    std::vector<int> commandersEver(1, 0);
    std::vector<const rm::sim::PassabilityGrid*> grids(roster.catalog.size(), &grid);
    Match match = loneMatch(armies, economies, commandersEver, grids);

    // The offset captured at attach time is what the per-tick recompute must
    // reproduce against the carrier's CURRENT transform.
    const Fx offsetX = roster.transform(cargo).x - roster.transform(transport).x;
    const Fx offsetZ = roster.transform(cargo).z - roster.transform(transport).z;

    rm::sim::orderTo(roster.motion(transport), terrain, rm::sim::fxFromFloat(400.0f),
                     rm::sim::fxFromFloat(100.0f));
    for (int i = 0; i < 300 && roster.motion(transport).moving; ++i) {
        (void)rm::sim::tickSkirmish(roster.store, roster.catalog, match, terrain);
        INFO("tick " << i);
        CHECK(roster.transform(cargo).x - roster.transform(transport).x == offsetX);
        CHECK(roster.transform(cargo).z - roster.transform(transport).z == offsetZ);
    }
    // The carrier actually went somewhere — the checks above were not
    // comparing two parked transforms.
    CHECK(rm::sim::fxToFloat(roster.transform(transport).x) > 200.0f);
}

TEST_CASE("C-196: attached cargo stays in the collision grid and can be hit", "[fa-transport]") {
    // Retail never removes an attached child from the collision world: the
    // per-tick transform recompute keeps its box under the hull, and a swept
    // projectile that meets the box strikes the CARGO, not the carrier. The
    // player sees a tank slung under a transport take fire on the rack — it
    // is not a free ablative shield for the hull.
    const rm::HeightField field = flatField();
    const rm::sim::Terrain terrain{field};

    rm::test::Roster roster;
    const rm::UnitTypeIndex transportType = roster.addType(transportDef());
    const rm::UnitTypeIndex cargoType = roster.addType(cargoDef());
    const rm::UnitTypeIndex shooterType = roster.addType(shooterDef());
    const UnitId transport = landedTransport(roster, transportType, 50.0f, 50.0f);
    // Cargo racks BETWEEN the shooter and the carrier: the swept shot meets
    // its collision box first, so the hit lands on the passenger. The 5k
    // hull outlasts several 500-point hits, so the damage lands while the
    // unit is still attached rather than in the same tick as its death.
    const UnitId cargo = roster.add(cargoType, 55.0f, 50.0f, 0, 5000.0f);
    REQUIRE(roster.store.attach(transport, cargo));
    (void)roster.add(shooterType, 120.0f, 50.0f, 1, 500.0f);

    std::vector<Army> armies = rm::sim::freeForAll(2);
    std::vector<rm::sim::Economy> economies(2);
    std::vector<rm::sim::Projectile> projectiles;
    std::vector<int> commandersEver(2, 0);
    Match match = loneMatch(armies, economies, commandersEver);
    match.projectiles = &projectiles;

    const rm::sim::Mag before = roster.health(cargo).current;
    for (int i = 0; i < 400 && roster.health(cargo).current == before; ++i) {
        (void)rm::sim::tickSkirmish(roster.store, roster.catalog, match, terrain);
    }

    // The cargo took fire while still racked: still attached, health down,
    // and the carrier behind it untouched by the shot the cargo absorbed.
    CHECK(roster.motion(cargo).attached);
    CHECK(roster.health(cargo).current < before);
    CHECK(roster.health(transport).current == rm::sim::Mag::fromInt(500));
}

TEST_CASE("C-197: a shot-down transport takes its passengers with it", "[fa-transport]") {
    // Retail's TransportDetachAllUnits kills the cargo before Lua ever hears
    // OnKilled — the player sees the whole lift die in the same explosion.
    // The claim's 99% survival roll is NOT modelled here (the cascade is
    // unconditional); what this pins is the observable contract that cargo
    // never outlives its carrier.
    const rm::HeightField field = flatField();
    const rm::sim::Terrain terrain{field};

    rm::test::Roster roster;
    const rm::UnitTypeIndex transportType = roster.addType(transportDef());
    const rm::UnitTypeIndex cargoType = roster.addType(cargoDef());
    const rm::UnitTypeIndex shooterType = roster.addType(shooterDef());
    const UnitId transport = landedTransport(roster, transportType, 50.0f, 50.0f);
    // Cargo racks BEHIND the carrier relative to the firing line: attached
    // units keep a collision box (they can be hit, C-196), so cargo spawned
    // on the shooter's side would body-block the hull this test needs dead.
    const UnitId first = roster.add(cargoType, 45.0f, 50.0f, 0, 100000.0f);
    const UnitId second = roster.add(cargoType, 40.0f, 50.0f, 0, 100000.0f);
    REQUIRE(roster.store.attach(transport, first));
    REQUIRE(roster.store.attach(transport, second));
    // The cargo's 100k hull is the point: nothing the shooter does can kill
    // it directly, so its death can only be the carrier's cascade.
    (void)roster.add(shooterType, 120.0f, 50.0f, 1, 500.0f);

    std::vector<Army> armies = rm::sim::freeForAll(2);
    std::vector<rm::sim::Economy> economies(2);
    std::vector<rm::sim::Projectile> projectiles;
    std::vector<int> commandersEver(2, 0);
    Match match = loneMatch(armies, economies, commandersEver);
    match.projectiles = &projectiles;

    for (int i = 0; i < 400 && roster.store.alive(transport); ++i) {
        (void)rm::sim::tickSkirmish(roster.store, roster.catalog, match, terrain);
    }

    REQUIRE_FALSE(roster.store.alive(transport));
    CHECK_FALSE(roster.store.alive(first));
    CHECK_FALSE(roster.store.alive(second));
    CHECK(roster.store.childrenOf(transport).empty());
}

TEST_CASE("C-199: a ferry holds at the beacon for a straggler", "[fa-transport]") {
    // The waiting queue is a formation in retail — units ordered to the
    // beacon are waited on, so a squad strung out along the route is not left
    // behind. The player sees the ferry sit at the pickup until the last
    // inbound unit is aboard, then fly the drop with everyone.
    const rm::HeightField field = flatField();
    const rm::sim::Terrain terrain{field};
    const rm::sim::PassabilityGrid grid =
        rm::sim::buildPassability(field, 0.0f, 60.0f, 0.0f);

    rm::test::Roster roster;
    const rm::UnitTypeIndex transportType = roster.addType(transportDef());
    const rm::UnitTypeIndex cargoType = roster.addType(cargoDef());
    // Landed at the beacon; the ferry order names the drop point.
    const UnitId transport = landedTransport(roster, transportType, 30.0f, 30.0f);
    const UnitId near = roster.add(cargoType, 42.0f, 30.0f, 0, 500.0f);
    const UnitId far = roster.add(cargoType, 30.0f, 90.0f, 0, 500.0f);

    const std::vector<Player> players{Player{.index = 0, .army = 0}};
    std::vector<Army> armies = rm::sim::freeForAll(1);
    std::vector<rm::sim::Economy> economies(1);
    std::vector<int> commandersEver(1, 0);
    std::vector<const rm::sim::PassabilityGrid*> grids(roster.catalog.size(), &grid);
    const auto gridFor = [&grid](UnitId) { return &grid; };

    // The ferry first, so the beacon exists where the transport stands.
    REQUIRE(rm::sim::applyCommand(
                CommandIssue{.source = 0,
                             .id = rm::commandId(0, 1),
                             .player = 0,
                             .kind = CommandKind::Ferry,
                             .units = {transport},
                             .targetX = rm::sim::fxFromFloat(80.0f),
                             .targetZ = rm::sim::fxFromFloat(80.0f)},
                roster.store, roster.catalog, players, armies, terrain, gridFor,
                roster.rate)
                .accepted.size()
            == 1);
    // Both cargo units are sent to the beacon — the near one is already in
    // the pickup zone, the far one has a walk ahead of it.
    REQUIRE(rm::sim::applyCommand(moveIssue(near, 30.0f, 30.0f, 2), roster.store,
                                  roster.catalog, players, armies, terrain, gridFor,
                                  roster.rate)
                .accepted.size()
            == 1);
    REQUIRE(rm::sim::applyCommand(moveIssue(far, 30.0f, 30.0f, 3), roster.store,
                                  roster.catalog, players, armies, terrain, gridFor,
                                  roster.rate)
                .accepted.size()
            == 1);

    Match match = loneMatch(armies, economies, commandersEver, grids);
    bool nearBoarded = false;
    bool farBoarded = false;
    bool delivered = false;
    for (int i = 0; i < 6000 && !delivered; ++i) {
        (void)rm::sim::tickSkirmish(roster.store, roster.catalog, match, terrain);
        nearBoarded = nearBoarded || roster.motion(near).attached;
        farBoarded = farBoarded || roster.motion(far).attached;
        if (nearBoarded && !farBoarded) {
            // One passenger aboard and one still inbound: the ferry must be
            // holding at the beacon, not halfway to the drop.
            const Fx dx = roster.transform(transport).x - rm::sim::fxFromFloat(30.0f);
            const Fx dz = roster.transform(transport).z - rm::sim::fxFromFloat(30.0f);
            INFO("tick " << i);
            CHECK(dx * dx + dz * dz
                  <= rm::sim::kFerryPickupRadius * rm::sim::kFerryPickupRadius * 4);
        }
        delivered = farBoarded && !roster.motion(far).attached;
    }

    REQUIRE(nearBoarded);
    REQUIRE(farBoarded);
    REQUIRE(delivered);
    // Both passengers came down at the drop, not just the early boarder.
    CHECK(rm::sim::fxToFloat(roster.transform(near).x) == Approx(80.0f).margin(10.0f));
    CHECK(rm::sim::fxToFloat(roster.transform(near).z) == Approx(80.0f).margin(10.0f));
    CHECK(rm::sim::fxToFloat(roster.transform(far).x) == Approx(80.0f).margin(10.0f));
    CHECK(rm::sim::fxToFloat(roster.transform(far).z) == Approx(80.0f).margin(10.0f));
}

/// A FERRYBEACON-shaped marker: UEB5102's category set, immobile, no command
/// caps — the engine spawns it, so it never needs to act on its own.
[[nodiscard]] UnitDef beaconDef() {
    UnitDef def;
    def.name = "test_beacon";
    def.categories = {"FERRYBEACON", "UNTARGETABLE"};  // sorted for hasCategory
    def.health = rm::sim::Mag::fromInt(10);
    return def;
}

TEST_CASE("C-183: a transport guarding a ferry beacon flies its route",
          "[fa-transport]") {
    // The guard ladder's ferry rung: a transport guarding a FERRYBEACON picks
    // up whatever waits at the beacon and ferries it to the beacon's own
    // command target — the same loop a `Ferry` order flies, driven by the
    // guard instead of a standing route.
    const rm::HeightField field = flatField();
    const rm::sim::Terrain terrain{field};
    const rm::sim::PassabilityGrid grid =
        rm::sim::buildPassability(field, 0.0f, 60.0f, 0.0f);

    rm::test::Roster roster;
    const rm::UnitTypeIndex transportType = roster.addType(transportDef());
    const rm::UnitTypeIndex cargoType = roster.addType(cargoDef());
    const rm::UnitTypeIndex beaconType = roster.addType(beaconDef());
    const UnitId transport = landedTransport(roster, transportType, 40.0f, 30.0f);
    const UnitId beacon = roster.add(beaconType, 30.0f, 30.0f, 0, 10.0f);
    const UnitId cargo = roster.add(cargoType, 32.0f, 30.0f, 0, 500.0f);

    const std::vector<Player> players{Player{.index = 0, .army = 0}};
    std::vector<Army> armies = rm::sim::freeForAll(1);
    std::vector<rm::sim::Economy> economies(1);
    std::vector<int> commandersEver(1, 0);
    std::vector<const rm::sim::PassabilityGrid*> grids(roster.catalog.size(), &grid);
    const auto gridFor = [&grid](UnitId) { return &grid; };

    // The beacon's own command carries the drop point — retail's beacon unit
    // holds the route's destination on its command.
    REQUIRE(rm::sim::applyCommand(moveIssue(beacon, 80.0f, 80.0f, 1), roster.store,
                                  roster.catalog, players, armies, terrain, gridFor,
                                  roster.rate)
                .accepted.size()
            == 1);
    // The transport guards the beacon; the cargo walks to the pickup ring.
    REQUIRE(rm::sim::applyCommand(
                CommandIssue{.source = 0,
                             .id = rm::commandId(0, 2),
                             .player = 0,
                             .kind = CommandKind::Guard,
                             .units = {transport},
                             .target = beacon},
                roster.store, roster.catalog, players, armies, terrain, gridFor,
                roster.rate)
                .accepted.size()
            == 1);
    REQUIRE(rm::sim::applyCommand(moveIssue(cargo, 30.0f, 30.0f, 3), roster.store,
                                  roster.catalog, players, armies, terrain, gridFor,
                                  roster.rate)
                .accepted.size()
            == 1);

    Match match = loneMatch(armies, economies, commandersEver, grids);
    bool boarded = false;
    bool delivered = false;
    for (int i = 0; i < 6000 && !delivered; ++i) {
        (void)rm::sim::tickSkirmish(roster.store, roster.catalog, match, terrain);
        boarded = boarded || roster.motion(cargo).attached;
        delivered = boarded && !roster.motion(cargo).attached;
    }

    REQUIRE(boarded);
    REQUIRE(delivered);
    CHECK(rm::sim::fxToFloat(roster.transform(cargo).x) == Approx(80.0f).margin(10.0f));
    CHECK(rm::sim::fxToFloat(roster.transform(cargo).z) == Approx(80.0f).margin(10.0f));
    // The guard order still stands — the route is a standing loop, not a
    // one-shot delivery.
    const rm::sim::QueuedCommand* head = roster.store.orders()[transport.index].active();
    REQUIRE(head != nullptr);
    CHECK(head->kind() == CommandKind::Guard);
}

TEST_CASE("C-198: cargo hangs from its class's nearest free attach bone",
          "[fa-transport]") {
    // Retail files a carrier's `Attachpoint*` bones into per-class lists and
    // hangs each cargo at the nearest free bone of its class (0x005eb450,
    // 0x005ed740). The player-visible half: a tank slung aboard sits at the
    // authored hook under the hull — not a slot number — and swings with the
    // carrier's heading, because the attachment is bone-indexed.
    const rm::HeightField field = flatField();
    const rm::sim::Terrain terrain{field};

    rm::test::Roster roster;
    const rm::UnitTypeIndex transportType = roster.addType(transportDef());
    const rm::UnitTypeIndex cargoType = roster.addType(cargoDef());
    // Two class-1 hooks and one class-2, in model-space elmos — the courier's
    // `Left_Attachpoint_sml_02`/`_Med_01` pattern, shrunk.
    const std::array<rm::sim::UnitCatalog::AttachBoneSpec, 3> bones{{
        {.bone = 19, .cargoClass = 1, .rest = {4.0f, 7.0f, -1.0f}},
        {.bone = 20, .cargoClass = 1, .rest = {-4.0f, 7.0f, -1.0f}},
        {.bone = 23, .cargoClass = 2, .rest = {4.0f, 7.0f, -14.0f}},
    }};
    roster.catalog.setAttachBones(transportType, bones);

    const UnitId transport = landedTransport(roster, transportType, 100.0f, 100.0f);
    // The cargo walks up on the +x side, so the +x hook is its nearest.
    const UnitId cargo = roster.add(cargoType, 106.0f, 100.0f, 0, 500.0f);

    REQUIRE(rm::sim::attachCargo(roster.store, roster.catalog,
                                 *roster.catalog.def(transportType), transport, cargo));

    // Placed ON the bone: carrier origin plus the rest offset, heading zero.
    CHECK(roster.transform(cargo).x - roster.transform(transport).x == Fx::fromInt(4));
    CHECK(roster.transform(cargo).z - roster.transform(transport).z == Fx::fromInt(-1));
    CHECK(roster.transform(cargo).y - roster.transform(transport).y == Fx::fromInt(7));
    // And the attachment records the real bone index, not a slot number.
    CHECK(roster.store.attachmentBonesOf(cargo).parent == 19);

    // A second class-1 cargo cannot take the same hook: it lands on the -x one.
    const UnitId cargo2 = roster.add(cargoType, 94.0f, 100.0f, 0, 500.0f);
    REQUIRE(rm::sim::attachCargo(roster.store, roster.catalog,
                                 *roster.catalog.def(transportType), transport, cargo2));
    CHECK(roster.transform(cargo2).x - roster.transform(transport).x == Fx::fromInt(-4));
    CHECK(roster.store.attachmentBonesOf(cargo2).parent == 20);

    // The bone rides the carrier's swing: turn the hull a quarter-circle and
    // the cargo's world offset rotates with it (rotateByHeading, C-196).
    roster.transform(transport).heading = static_cast<rm::Brad>(16384);  // 90°
    roster.store.propagateAttachments();
    // 90° maps +x→+z: the (4,-1) rest offset becomes (-1,-4) in world axes.
    CHECK(roster.transform(cargo).x - roster.transform(transport).x == Fx::fromInt(-1));
    CHECK(roster.transform(cargo).z - roster.transform(transport).z == Fx::fromInt(-4));
}

TEST_CASE("C-199: the pickup is an assignment — a parked unit is left alone",
          "[fa-transport]") {
    // Retail's waiting set is `CAiTransportImpl+0x30`, a list the unit joins by
    // holding a `CUnitWaitForFerryTask` — "not a queue and not a spatial scan".
    // The sim's stand-in for the task is the held-open move: a unit ordered to
    // the beacon keeps its order at the head while it waits, and a unit merely
    // PARKED in the ring is not the ferry's business. The player sees a squad
    // mate idling under the pickup get left behind while the sent unit boards.
    const rm::HeightField field = flatField();
    const rm::sim::Terrain terrain{field};
    const rm::sim::PassabilityGrid grid =
        rm::sim::buildPassability(field, 0.0f, 60.0f, 0.0f);

    rm::test::Roster roster;
    const rm::UnitTypeIndex transportType = roster.addType(transportDef());
    const rm::UnitTypeIndex cargoType = roster.addType(cargoDef());
    const UnitId transport = landedTransport(roster, transportType, 30.0f, 30.0f);
    // Inside the pickup ring from the start, never ordered anywhere.
    const UnitId parked = roster.add(cargoType, 35.0f, 30.0f, 0, 500.0f);
    const UnitId sent = roster.add(cargoType, 60.0f, 30.0f, 0, 500.0f);

    const std::vector<Player> players{Player{.index = 0, .army = 0}};
    std::vector<Army> armies = rm::sim::freeForAll(1);
    std::vector<rm::sim::Economy> economies(1);
    std::vector<int> commandersEver(1, 0);
    std::vector<const rm::sim::PassabilityGrid*> grids(roster.catalog.size(), &grid);
    const auto gridFor = [&grid](UnitId) { return &grid; };

    REQUIRE(rm::sim::applyCommand(
                CommandIssue{.source = 0,
                             .id = rm::commandId(0, 1),
                             .player = 0,
                             .kind = CommandKind::Ferry,
                             .units = {transport},
                             .targetX = rm::sim::fxFromFloat(80.0f),
                             .targetZ = rm::sim::fxFromFloat(80.0f)},
                roster.store, roster.catalog, players, armies, terrain, gridFor,
                roster.rate)
                .accepted.size()
            == 1);
    REQUIRE(rm::sim::applyCommand(moveIssue(sent, 30.0f, 30.0f, 2), roster.store,
                                  roster.catalog, players, armies, terrain, gridFor,
                                  roster.rate)
                .accepted.size()
            == 1);

    Match match = loneMatch(armies, economies, commandersEver, grids);
    bool heldWhileWaiting = false;
    bool boarded = false;
    bool delivered = false;
    for (int i = 0; i < 6000 && !delivered; ++i) {
        (void)rm::sim::tickSkirmish(roster.store, roster.catalog, match, terrain);
        boarded = boarded || roster.motion(sent).attached;
        // While the sent unit is inside the ring unboarded, its move is still
        // the live order — the held-open order IS the wait state. A completed
        // move would leave the queue empty and the unit unpickable.
        if (!roster.motion(sent).attached) {
            const Fx dx = roster.transform(sent).x - rm::sim::fxFromFloat(30.0f);
            const Fx dz = roster.transform(sent).z - rm::sim::fxFromFloat(30.0f);
            if (dx * dx + dz * dz
                <= rm::sim::kFerryPickupRadius * rm::sim::kFerryPickupRadius) {
                const rm::sim::QueuedCommand* head =
                    roster.store.orders()[sent.index].active();
                heldWhileWaiting = heldWhileWaiting
                                   || (head != nullptr
                                       && head->kind() == CommandKind::Move);
            }
        }
        delivered = boarded && !roster.motion(sent).attached;
    }

    REQUIRE(heldWhileWaiting);
    REQUIRE(delivered);
    // The parked unit was never the ferry's business: still on the ground,
    // still unattached, still at the pickup — not at the drop. (It may be
    // nudged an elmo or two by ordinary collision as the sent unit walks by;
    // that is not the ferry moving it.)
    CHECK_FALSE(roster.motion(parked).attached);
    CHECK(roster.store.alive(parked));
    CHECK(rm::sim::fxToFloat(roster.transform(parked).x) == Approx(35.0f).margin(10.0f));
}

TEST_CASE("C-199: a unit reordered off the beacon is not waited on",
          "[fa-transport]") {
    // The waiting unit holds a weak pointer to its beacon and retries — a unit
    // sent elsewhere drops off the transport's list. The player sees the ferry
    // leave on time instead of holding the route for a squad that was
    // reassigned mid-walk.
    const rm::HeightField field = flatField();
    const rm::sim::Terrain terrain{field};
    const rm::sim::PassabilityGrid grid =
        rm::sim::buildPassability(field, 0.0f, 60.0f, 0.0f);

    rm::test::Roster roster;
    const rm::UnitTypeIndex transportType = roster.addType(transportDef());
    const rm::UnitTypeIndex cargoType = roster.addType(cargoDef());
    const UnitId transport = landedTransport(roster, transportType, 30.0f, 30.0f);
    const UnitId near = roster.add(cargoType, 35.0f, 30.0f, 0, 500.0f);
    const UnitId far = roster.add(cargoType, 30.0f, 90.0f, 0, 500.0f);

    const std::vector<Player> players{Player{.index = 0, .army = 0}};
    std::vector<Army> armies = rm::sim::freeForAll(1);
    std::vector<rm::sim::Economy> economies(1);
    std::vector<int> commandersEver(1, 0);
    std::vector<const rm::sim::PassabilityGrid*> grids(roster.catalog.size(), &grid);
    const auto gridFor = [&grid](UnitId) { return &grid; };

    REQUIRE(rm::sim::applyCommand(
                CommandIssue{.source = 0,
                             .id = rm::commandId(0, 1),
                             .player = 0,
                             .kind = CommandKind::Ferry,
                             .units = {transport},
                             .targetX = rm::sim::fxFromFloat(80.0f),
                             .targetZ = rm::sim::fxFromFloat(80.0f)},
                roster.store, roster.catalog, players, armies, terrain, gridFor,
                roster.rate)
                .accepted.size()
            == 1);
    REQUIRE(rm::sim::applyCommand(moveIssue(near, 30.0f, 30.0f, 2), roster.store,
                                  roster.catalog, players, armies, terrain, gridFor,
                                  roster.rate)
                .accepted.size()
            == 1);
    REQUIRE(rm::sim::applyCommand(moveIssue(far, 30.0f, 30.0f, 3), roster.store,
                                  roster.catalog, players, armies, terrain, gridFor,
                                  roster.rate)
                .accepted.size()
            == 1);

    Match match = loneMatch(armies, economies, commandersEver, grids);
    // Let the near unit board and the far one commit to its walk, then reissue
    // the far unit somewhere else — the ferry must not wait on it any more.
    for (int i = 0; i < 200 && !roster.motion(near).attached; ++i) {
        (void)rm::sim::tickSkirmish(roster.store, roster.catalog, match, terrain);
    }
    REQUIRE(roster.motion(near).attached);
    REQUIRE(rm::sim::applyCommand(moveIssue(far, 90.0f, 90.0f, 4), roster.store,
                                  roster.catalog, players, armies, terrain, gridFor,
                                  roster.rate)
                .accepted.size()
            == 1);

    bool delivered = false;
    for (int i = 0; i < 6000 && !delivered; ++i) {
        (void)rm::sim::tickSkirmish(roster.store, roster.catalog, match, terrain);
        delivered = !roster.motion(near).attached;
    }

    REQUIRE(delivered);
    CHECK_FALSE(roster.motion(far).attached);
    CHECK(rm::sim::fxToFloat(roster.transform(near).x) == Approx(80.0f).margin(10.0f));
    // The reordered unit walked its new order, not the ferry's.
    CHECK(rm::sim::fxToFloat(roster.transform(far).x) == Approx(90.0f).margin(10.0f));
    CHECK(rm::sim::fxToFloat(roster.transform(far).z) == Approx(90.0f).margin(10.0f));
}

TEST_CASE("C-199: a LoadTransport inbound to the ferry holds the route",
          "[fa-transport]") {
    // A unit boarding the ferry carrier directly is as much "still coming" as
    // one walking to the beacon — retail's assigned list does not care which
    // order brought the unit. The player sees the ferry hold for the tank
    // that was told to get aboard, then carry both.
    const rm::HeightField field = flatField();
    const rm::sim::Terrain terrain{field};
    const rm::sim::PassabilityGrid grid =
        rm::sim::buildPassability(field, 0.0f, 60.0f, 0.0f);

    rm::test::Roster roster;
    const rm::UnitTypeIndex transportType = roster.addType(transportDef());
    const rm::UnitTypeIndex cargoType = roster.addType(cargoDef());
    const UnitId transport = landedTransport(roster, transportType, 30.0f, 30.0f);
    const UnitId near = roster.add(cargoType, 35.0f, 30.0f, 0, 500.0f);
    const UnitId loader = roster.add(cargoType, 30.0f, 90.0f, 0, 500.0f);

    const std::vector<Player> players{Player{.index = 0, .army = 0}};
    std::vector<Army> armies = rm::sim::freeForAll(1);
    std::vector<rm::sim::Economy> economies(1);
    std::vector<int> commandersEver(1, 0);
    std::vector<const rm::sim::PassabilityGrid*> grids(roster.catalog.size(), &grid);
    const auto gridFor = [&grid](UnitId) { return &grid; };

    REQUIRE(rm::sim::applyCommand(
                CommandIssue{.source = 0,
                             .id = rm::commandId(0, 1),
                             .player = 0,
                             .kind = CommandKind::Ferry,
                             .units = {transport},
                             .targetX = rm::sim::fxFromFloat(80.0f),
                             .targetZ = rm::sim::fxFromFloat(80.0f)},
                roster.store, roster.catalog, players, armies, terrain, gridFor,
                roster.rate)
                .accepted.size()
            == 1);
    REQUIRE(rm::sim::applyCommand(moveIssue(near, 30.0f, 30.0f, 2), roster.store,
                                  roster.catalog, players, armies, terrain, gridFor,
                                  roster.rate)
                .accepted.size()
            == 1);
    // The far unit is told to board the carrier itself, not to walk to a point.
    REQUIRE(rm::sim::applyCommand(
                CommandIssue{.source = 0,
                             .id = rm::commandId(0, 3),
                             .player = 0,
                             .kind = CommandKind::LoadTransport,
                             .units = {loader},
                             .target = transport},
                roster.store, roster.catalog, players, armies, terrain, gridFor,
                roster.rate)
                .accepted.size()
            == 1);

    Match match = loneMatch(armies, economies, commandersEver, grids);
    bool loaderBoarded = false;
    bool delivered = false;
    for (int i = 0; i < 6000 && !delivered; ++i) {
        (void)rm::sim::tickSkirmish(roster.store, roster.catalog, match, terrain);
        loaderBoarded = loaderBoarded || roster.motion(loader).attached;
        if (roster.motion(near).attached && !loaderBoarded) {
            // One aboard, the loader still inbound: the ferry holds at the
            // beacon rather than leaving it behind.
            const Fx dx = roster.transform(transport).x - rm::sim::fxFromFloat(30.0f);
            const Fx dz = roster.transform(transport).z - rm::sim::fxFromFloat(30.0f);
            INFO("tick " << i);
            CHECK(dx * dx + dz * dz
                  <= rm::sim::kFerryPickupRadius * rm::sim::kFerryPickupRadius * 4);
        }
        delivered = loaderBoarded && !roster.motion(loader).attached;
    }

    REQUIRE(loaderBoarded);
    REQUIRE(delivered);
    CHECK(rm::sim::fxToFloat(roster.transform(loader).x) == Approx(80.0f).margin(10.0f));
    CHECK(rm::sim::fxToFloat(roster.transform(loader).z) == Approx(80.0f).margin(10.0f));
}

TEST_CASE("C-198: a class-2 cargo consumes two class-1 points",
          "[fa-transport]") {
    // `TransportHasSpaceFor` prices every class against the class-1 list:
    // `Class2AttachSize` is literally how many class-1 points a class-2 unit
    // consumes, and the bone it hangs from is a class-1 `Attachpoint` — the
    // `_Med`/`_Lrg` lists are the staging pads' machinery (C-225). With four
    // class-1 bones and a declared capacity of six, two class-2 units fill the
    // rack: the arithmetic says a third fits (4 + 2 <= 6), the bones say full.
    const rm::HeightField field = flatField();
    const rm::sim::Terrain terrain{field};

    rm::test::Roster roster;
    const rm::UnitTypeIndex transportType = roster.addType(transportDef());
    const rm::UnitTypeIndex heavyType = roster.addType(cargoDef(2));
    const std::array<rm::sim::UnitCatalog::AttachBoneSpec, 5> bones{{
        {.bone = 19, .cargoClass = 1, .rest = {4.0f, 7.0f, -1.0f}},
        {.bone = 20, .cargoClass = 1, .rest = {-4.0f, 7.0f, -1.0f}},
        {.bone = 21, .cargoClass = 1, .rest = {4.0f, 7.0f, 3.0f}},
        {.bone = 22, .cargoClass = 1, .rest = {-4.0f, 7.0f, 3.0f}},
        {.bone = 30, .cargoClass = 2, .rest = {0.0f, 7.0f, -14.0f}},
    }};
    roster.catalog.setAttachBones(transportType, bones);

    const UnitId transport = landedTransport(roster, transportType, 100.0f, 100.0f);
    const rm::unitdef::UnitDef& carrierDef = *roster.catalog.def(transportType);
    const rm::unitdef::UnitDef& heavyDef = *roster.catalog.def(heavyType);

    const UnitId first = roster.add(heavyType, 106.0f, 100.0f, 0, 500.0f);
    REQUIRE(rm::sim::attachCargo(roster.store, roster.catalog, carrierDef,
                               transport, first));
    // The class-2 unit hangs from a class-1 bone — never the class-2 list.
    CHECK(roster.store.attachmentBonesOf(first).parent == 19);

    const UnitId second = roster.add(heavyType, 94.0f, 100.0f, 0, 500.0f);
    REQUIRE(rm::sim::attachCargo(roster.store, roster.catalog, carrierDef,
                               transport, second));
    // The first unit's two reserved points are the prefix {19, 20}, so the
    // second hangs from the next free class-1 point — 21, reserving 22.
    CHECK(roster.store.attachmentBonesOf(second).parent == 21);

    // Four class-1 points consumed of four: the rack is full even though the
    // declared capacity arithmetic (4 + 2 <= 6) would admit one more.
    const UnitId third = roster.add(heavyType, 100.0f, 106.0f, 0, 500.0f);
    CHECK_FALSE(rm::sim::hasRoomFor(roster.store, roster.catalog, transport, heavyDef));
    CHECK_FALSE(rm::sim::attachCargo(roster.store, roster.catalog, carrierDef,
                                   transport, third));
    CHECK_FALSE(roster.motion(third).attached);
}
