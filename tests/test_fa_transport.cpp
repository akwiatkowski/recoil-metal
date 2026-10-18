// Player-perspective coverage for the FA-TRANSPORT claims (WP-25; see
// docs/fa-exe-analysis-plan.md and build/re-fa/coverage/FA-TRANSPORT-MISSILES.md).
// Each TEST_CASE cites its claim IDs.
//
// The picks are what a player watches happen: cargo riding its carrier's
// transform every tick (C-196), a shot-down transport taking its passengers
// with it (C-197), and a ferry holding at the beacon for a straggler (C-199).
//
// Deliberately absent: the 99% cargo-survival roll (C-197 — the kill cascade
// is unconditional here, a recorded divergence). The carrier/mobile-factory
// attach-store pattern (C-264) is covered below: the pad-anchor case is
// sim-level, the store-vs-roll-off seam is app-level.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "app/Match.hpp"

#include "core/data/MoveDef.hpp"
#include "core/map/HeightField.hpp"
#include "core/sim/Command.hpp"
#include "core/sim/Economy.hpp"
#include "core/sim/Pathfinding.hpp"
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

/// A URS0303-shaped carrier: `CARRIER` + `NAVALCARRIER`, `StorageSlots = 2`
/// (the shipped 50 is shrunk so a test can fill the pool).
[[nodiscard]] UnitDef carrierDef() {
    UnitDef def;
    def.name = "test_carrier";
    def.categories = {"CARRIER", "MOBILE", "NAVALCARRIER"};
    def.motion = rm::unitdef::MotionType::Water;
    def.speedElmosPerSecond = 4.0f;
    def.commandCaps = {"RULEUCC_Transport"};
    def.commandCapsDeclared = true;
    def.transport.storageSlots = 2;
    return def;
}

/// An aircraft the carrier's pad can produce — the only cargo a storage
/// pool accepts (`C-225`).
[[nodiscard]] UnitDef airCargoDef() {
    UnitDef def;
    def.name = "test_air_cargo";
    def.categories = {"AIR", "MOBILE"};
    def.motion = rm::unitdef::MotionType::Air;
    def.canFly = true;
    def.speedElmosPerSecond = 12.0f;
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

TEST_CASE("C-197: a shot-down transport rolls each passenger's fate from the sim RNG",
          "[fa-transport]") {
    // `Moho::Unit::Kill` runs `TransportDetachAllUnits(destroySome = true)` before
    // `OnKilled` (`0x006aee5f`): every external cargo child draws from the sim
    // MT19937 (`Sim+0x904`) and dies iff `r < 0.99` (`0x005edeb0`, constant
    // `0x00ea2c5c = 0.99f`). The survivors detach where the carrier fell — a 1%
    // chance per child, which is why this test asserts the deterministic outcome
    // of the seeded stream rather than "everyone dies".
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
    // The seeded stream's first draw kills `first` (r < 0.99); the second draw
    // lands in the top 1% and `second` walks away from the wreck — the
    // deterministic proof the roll is per-child and from the sim's own RNG.
    CHECK_FALSE(roster.store.alive(first));
    CHECK(roster.store.alive(second));
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

TEST_CASE("C-199: an arrived waiter holds its ring spot for the ferry", "[fa-transport]") {
    // The ferry-wait hold only covers a move that is still open: a unit that
    // reached the beacon BEFORE the ferry existed retired its move normally,
    // leaving an empty queue with a leftover leg destination at the beacon.
    // It is still the ferry's business — standing in the ring for the lift,
    // not parked there — so the carrier must board it rather than hold in
    // Loading beside arrived cargo forever.
    const rm::HeightField field = flatField();
    const rm::sim::Terrain terrain{field};
    const rm::sim::PassabilityGrid grid =
        rm::sim::buildPassability(field, 0.0f, 60.0f, 0.0f);

    rm::test::Roster roster;
    const rm::UnitTypeIndex transportType = roster.addType(transportDef());
    const rm::UnitTypeIndex cargoType = roster.addType(cargoDef());
    const UnitId transport = landedTransport(roster, transportType, 30.0f, 30.0f);
    const UnitId waiter = roster.add(cargoType, 60.0f, 30.0f, 0, 500.0f);

    const std::vector<Player> players{Player{.index = 0, .army = 0}};
    std::vector<Army> armies = rm::sim::freeForAll(1);
    std::vector<rm::sim::Economy> economies(1);
    std::vector<int> commandersEver(1, 0);
    std::vector<const rm::sim::PassabilityGrid*> grids(roster.catalog.size(), &grid);
    const auto gridFor = [&grid](UnitId) { return &grid; };

    // No ferry yet: the walk to the pickup completes like any arrival.
    REQUIRE(rm::sim::applyCommand(moveIssue(waiter, 30.0f, 30.0f, 1), roster.store,
                                  roster.catalog, players, armies, terrain, gridFor,
                                  roster.rate)
                .accepted.size()
            == 1);

    Match match = loneMatch(armies, economies, commandersEver, grids);
    bool arrived = false;
    for (int i = 0; i < 6000 && !arrived; ++i) {
        (void)rm::sim::tickSkirmish(roster.store, roster.catalog, match, terrain);
        arrived = roster.store.orders()[waiter.index].active() == nullptr
                  && !roster.motion(waiter).moving;
    }
    REQUIRE(arrived);
    // The retired move left its destination at the beacon — the arrived
    // waiter's mark, as opposed to a unit merely parked in the ring.
    {
        const Fx lx = roster.motion(waiter).destinationX - rm::sim::fxFromFloat(30.0f);
        const Fx lz = roster.motion(waiter).destinationZ - rm::sim::fxFromFloat(30.0f);
        REQUIRE(lx * lx + lz * lz
                <= rm::sim::kFerryPickupRadius * rm::sim::kFerryPickupRadius);
    }

    // Only now does the ferry exist, anchored where the transport stands.
    REQUIRE(rm::sim::applyCommand(
                CommandIssue{.source = 0,
                             .id = rm::commandId(0, 2),
                             .player = 0,
                             .kind = CommandKind::Ferry,
                             .units = {transport},
                             .targetX = rm::sim::fxFromFloat(80.0f),
                             .targetZ = rm::sim::fxFromFloat(80.0f)},
                roster.store, roster.catalog, players, armies, terrain, gridFor,
                roster.rate)
                .accepted.size()
            == 1);

    bool boarded = false;
    bool delivered = false;
    for (int i = 0; i < 6000 && !delivered; ++i) {
        (void)rm::sim::tickSkirmish(roster.store, roster.catalog, match, terrain);
        boarded = boarded || roster.motion(waiter).attached;
        delivered = boarded && !roster.motion(waiter).attached;
    }

    REQUIRE(boarded);
    REQUIRE(delivered);
    CHECK(rm::sim::fxToFloat(roster.transform(waiter).x) == Approx(80.0f).margin(10.0f));
    CHECK(rm::sim::fxToFloat(roster.transform(waiter).z) == Approx(80.0f).margin(10.0f));
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
    auto routeIssue = moveIssue(beacon, 80.0f, 80.0f, 1);
    SECTION("move-routed beacon") {}
    SECTION("attack-move-routed beacon") { routeIssue.kind = CommandKind::AttackMove; }
    REQUIRE(rm::sim::applyCommand(routeIssue, roster.store,
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
TEST_CASE("C-183: a bingo-fuel guard ferry docks at a staging pad instead of flying dry",
          "[fa-transport]") {
    // The ladder order made retail-obvious (`C-183`: refuel/staging outranks
    // the ferry rung): a thirsty transport guarding a FERRYBEACON diverts to a
    // pad in reach and docks, instead of flying the beacon route on an empty
    // tank. Refuelling to full and resuming the loop is the staging rung's own
    // contract, pinned in `[guard][airstaging]`; this rung pins the diversion.
    const rm::HeightField field = flatField();
    const rm::sim::Terrain terrain{field};
    const rm::sim::PassabilityGrid grid =
        rm::sim::buildPassability(field, 0.0f, 60.0f, 0.0f);

    rm::test::Roster roster;
    const rm::UnitTypeIndex transportType = roster.addType(transportDef());
    const rm::UnitTypeIndex beaconType = roster.addType(beaconDef());
    UnitDef padDef;
    padDef.name = "test_pad";
    padDef.categories = {"AIRSTAGINGPLATFORM", "STRUCTURE"};  // sorted for hasCategory
    padDef.collisionRadiusElmos = 24.0f;
    padDef.refuelingMultiplier = 10.0f;  // UEB5202 states 50; 10 refuels a test tank in ticks
    padDef.stagingScanRadiusElmos = rm::sim::fxFromFloat(400.0f);  // pad-owned reach, `C-225`
    padDef.transport.dockingSlots = 1;
    const rm::UnitTypeIndex padType = roster.addType(padDef);

    const UnitId transport = landedTransport(roster, transportType, 40.0f, 30.0f);
    // Bingo tank with a fast drain, so the staging rung outranks the ferry
    // rung on the first beat: the fuel the ferry legs burn only sharpens it.
    roster.motion(transport).fuelRatio = Fx::fromRatio(3, 100);
    roster.motion(transport).fuelDrainPerTick = Fx::fromRatio(1, 100);
    const UnitId beacon = roster.add(beaconType, 30.0f, 30.0f, 0, 10.0f);
    const UnitId pad = roster.add(padType, 55.0f, 55.0f, 0, 500.0f);
    const std::vector<Player> players{Player{.index = 0, .army = 0}};
    std::vector<Army> armies = rm::sim::freeForAll(1);
    std::vector<rm::sim::Economy> economies(1);
    std::vector<int> commandersEver(1, 0);
    std::vector<const rm::sim::PassabilityGrid*> grids(roster.catalog.size(), &grid);
    const auto gridFor = [&grid](UnitId) { return &grid; };

    REQUIRE(rm::sim::applyCommand(moveIssue(beacon, 80.0f, 80.0f, 1), roster.store,
                                  roster.catalog, players, armies, terrain, gridFor,
                                  roster.rate)
                .accepted.size()
            == 1);
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

    Match match = loneMatch(armies, economies, commandersEver, grids);
    bool docked = false;
    bool refueled = false;
    bool relaunched = false;
    bool resumed = false;
    for (int i = 0; i < 12000 && !resumed; ++i) {
        (void)rm::sim::tickSkirmish(roster.store, roster.catalog, match, terrain);
        docked = docked || roster.store.parentOf(transport) == pad;
        refueled = refueled || (docked && roster.motion(transport).fuelRatio > Fx::fromRatio(3, 100));
        relaunched = relaunched
                     || (docked && !roster.store.parentOf(transport).has_value()
                         && roster.motion(transport).fuelRatio >= Fx::fromInt(1));
        if (relaunched && !resumed) {
            // One beat after leaving the deck the ferry drive owns the tick
            // again — the guard-ferry anchor is set and the route continues.
            const rm::sim::QueuedCommand* live =
                roster.store.orders()[transport.index].active();
            resumed = live != nullptr && live->transportAnchor().has_value();
        }
    }

    CHECK(docked);  // the staging rung owned a leg of the ferry route
    CHECK(refueled);  // the pad's multiplier filled the tank, not the bare ground rate
    CHECK(relaunched);  // full tank: off the deck with the guard order still heading it
    CHECK(resumed);  // and the ferry drive picked the route back up afterwards
    const rm::sim::QueuedCommand* head = roster.store.orders()[transport.index].active();
    REQUIRE(head != nullptr);
    CHECK(head->kind() == CommandKind::Guard);
}

TEST_CASE("C-183/C-348: a bingo-fuel guard ferry diverts to a staging pad mid-route",
          "[fa-transport][guard][airstaging]") {
    // The C-183 ladder order applied to the C-348 carrier-side loop: refuel/staging
    // outranks ferry even once the guard-ferry anchor is set and the route is
    // underway. A transport flying its beacon route (anchor set, en route to the
    // drop) that drops to bingo fuel diverts to an allied AIRSTAGINGPLATFORM in
    // reach instead of pressing on dry. The guard order still heads the queue.
    // Filed against `advanceGuardFerry` (`Transport.cpp`): today it drives the
    // route unconditionally, so this fails — the carrier keeps the drop leg.
    const rm::HeightField field = flatField();
    const rm::sim::Terrain terrain{field};
    const rm::sim::PassabilityGrid grid =
        rm::sim::buildPassability(field, 0.0f, 60.0f, 0.0f);

    rm::test::Roster roster;
    const rm::UnitTypeIndex transportType = roster.addType(transportDef());
    const rm::UnitTypeIndex beaconType = roster.addType(beaconDef());
    UnitDef padDef;
    padDef.name = "test_pad";
    padDef.categories = {"AIRSTAGINGPLATFORM", "STRUCTURE"};  // sorted for hasCategory
    padDef.collisionRadiusElmos = 24.0f;
    padDef.refuelingMultiplier = 10.0f;
    padDef.stagingScanRadiusElmos = rm::sim::fxFromFloat(400.0f);  // pad-owned reach, `C-225`
    padDef.transport.dockingSlots = 1;
    const rm::UnitTypeIndex padType = roster.addType(padDef);

    // Mid-route: high over the beacon leg. Starts FULL — the first tick settles
    // the guard-ferry anchor and commits the drop leg — then the test drains
    // the tank to bingo to prove the carrier pass yields the route.
    const UnitId transport = roster.add(transportType, 45.0f, 45.0f, 0, 500.0f);
    giveFlight(roster.motion(transport));
    roster.motion(transport).airborne = true;
    roster.motion(transport).fuelDrainPerTick = Fx::fromRatio(1, 5000);
    const UnitId beacon = roster.add(beaconType, 30.0f, 30.0f, 0, 10.0f);
    // Pad ahead of the route, inside its own scan radius of the carrier.
    (void)roster.add(padType, 60.0f, 60.0f, 0, 500.0f);
    const std::vector<Player> players{Player{.index = 0, .army = 0}};
    std::vector<Army> armies = rm::sim::freeForAll(1);
    std::vector<rm::sim::Economy> economies(1);
    std::vector<int> commandersEver(1, 0);
    std::vector<const rm::sim::PassabilityGrid*> grids(roster.catalog.size(), &grid);
    const auto gridFor = [&grid](UnitId) { return &grid; };
    REQUIRE(rm::sim::applyCommand(moveIssue(beacon, 400.0f, 400.0f, 1), roster.store,
                                  roster.catalog, players, armies, terrain, gridFor,
                                  roster.rate)
                .accepted.size()
            == 1);
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

    Match match = loneMatch(armies, economies, commandersEver, grids);
    // Settle the anchor first on a full tank: one tick starts the guard-ferry
    // loop (anchor set, drop leg committed). Then drain to bingo and require
    // the carrier pass to yield the route to the staging rung.
    (void)rm::sim::tickSkirmish(roster.store, roster.catalog, match, terrain);
    REQUIRE(roster.store.orders()[transport.index].active() != nullptr);
    REQUIRE(roster.store.orders()[transport.index].active()->transportAnchor().has_value());
    roster.motion(transport).fuelRatio = Fx::fromRatio(3, 100);
    bool diverted = false;
    for (int i = 0; i < 120 && !diverted; ++i) {
        (void)rm::sim::tickSkirmish(roster.store, roster.catalog, match, terrain);
        // Diverting means routing toward the pad, not holding the drop leg: the
        // destination swings off the beacon-drop line toward the pad.
        const rm::sim::MoveState& motion = roster.motion(transport);
        if (motion.moving) {
            const Fx dx = motion.destinationX - rm::sim::fxFromFloat(60.0f);
            const Fx dz = motion.destinationZ - rm::sim::fxFromFloat(60.0f);
            diverted = dx * dx + dz * dz
                <= Fx::fromInt(30) * Fx::fromInt(30);
        }
    }

    CHECK(diverted);
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
TEST_CASE("C-199: waiting units stage onto the carrier-anchored ring",
          "[fa-transport]") {
    // `TransportGenerateWaitingFormationForUnits` (`0x005eca10`) builds a
    // formation anchored at the transport using the carrier's
    // `GuardFormationName` — retail's Guard ring (`lua/formations.lua:
    // `GuardFormation`, `sizeMult = 3` elmos, first five slots per ring).
    // `ferryStagingSpot` is the sim's ring: waiter `i` stages at radius
    // `3 + 3 × (i / 5)` elmos around the carrier, at bearing `i / ring-size`
    // of a full turn, in slot order. Two waiters take opposite sides of the
    // ring; a unit with no beacon order is not a waiter and keeps the hull.
    const rm::HeightField field = flatField();
    const rm::sim::Terrain terrain{field};
    const rm::sim::PassabilityGrid grid =
        rm::sim::buildPassability(field, 0.0f, 60.0f, 0.0f);

    rm::test::Roster roster;
    const rm::UnitTypeIndex transportType = roster.addType(transportDef());
    const rm::UnitTypeIndex cargoType = roster.addType(cargoDef());
    const UnitId transport = landedTransport(roster, transportType, 30.0f, 30.0f);
    const UnitId first = roster.add(cargoType, 36.0f, 30.0f, 0, 500.0f);
    const UnitId second = roster.add(cargoType, 30.0f, 42.0f, 0, 500.0f);
    const UnitId parked = roster.add(cargoType, 33.0f, 30.0f, 0, 500.0f);

    const std::vector<Player> players{Player{.index = 0, .army = 0}};
    std::vector<Army> armies = rm::sim::freeForAll(1);
    std::vector<rm::sim::Economy> economies(1);
    std::vector<int> commandersEver(1, 0);
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
    REQUIRE(rm::sim::applyCommand(moveIssue(first, 30.0f, 30.0f, 2), roster.store,
                                  roster.catalog, players, armies, terrain, gridFor,
                                  roster.rate)
                .accepted.size()
            == 1);
    REQUIRE(rm::sim::applyCommand(moveIssue(second, 30.0f, 30.0f, 3), roster.store,
                                  roster.catalog, players, armies, terrain, gridFor,
                                  roster.rate)
                .accepted.size()
            == 1);

    const std::array<rm::sim::Fx, 2> beacon{rm::sim::fxFromFloat(30.0f),
                                           rm::sim::fxFromFloat(30.0f)};
    const std::array<rm::sim::Fx, 2> firstSpot = rm::sim::ferryStagingSpot(
        roster.store, roster.catalog, transport, first.index, beacon);
    const std::array<rm::sim::Fx, 2> secondSpot = rm::sim::ferryStagingSpot(
        roster.store, roster.catalog, transport, second.index, beacon);
    // First waiter takes bearing zero: (30, 33) — 3 elmos off the beacon.
    CHECK(rm::sim::fxToFloat(firstSpot[0]) == Approx(30.0f).margin(0.01f));
    CHECK(rm::sim::fxToFloat(firstSpot[1]) == Approx(33.0f).margin(0.01f));
    // Second waiter takes the opposite side: (30, 27) — distinct, same ring.
    CHECK(rm::sim::fxToFloat(secondSpot[0]) == Approx(30.0f).margin(0.01f));
    CHECK(rm::sim::fxToFloat(secondSpot[1]) == Approx(27.0f).margin(0.01f));
    // The parked unit holds no beacon order: not a waiter, keeps the hull.
    const std::array<rm::sim::Fx, 2> parkedSpot = rm::sim::ferryStagingSpot(
        roster.store, roster.catalog, transport, parked.index, beacon);
    CHECK(parkedSpot[0] == roster.transform(transport).x);
    CHECK(parkedSpot[1] == roster.transform(transport).z);
}

TEST_CASE("C-199: the staging ring anchors at the carrier, not the beacon",
          "[fa-transport]") {
    // `TransportGenerateWaitingFormationForUnits` (`0x005eca10`, C-199)
    // builds the waiting formation anchored at the transport — the ring
    // follows the carrier, not the beacon point. Carrier at (40, 30),
    // beacon at (30, 30): first waiter stages 3 elmos off the HULL.
    const rm::HeightField field = flatField();
    const rm::sim::Terrain terrain{field};
    rm::test::Roster roster;
    const rm::UnitTypeIndex transportType = roster.addType(transportDef());
    const rm::UnitTypeIndex cargoType = roster.addType(cargoDef());
    const UnitId transport = landedTransport(roster, transportType, 40.0f, 30.0f);
    const UnitId waiter = roster.add(cargoType, 36.0f, 30.0f, 0, 500.0f);
    const std::vector<Player> players{Player{.index = 0, .army = 0}};
    std::vector<Army> armies = rm::sim::freeForAll(1);
    std::vector<rm::sim::Economy> economies(1);
    const auto grid = rm::sim::buildPassability(field, 0.0f, 60.0f, 0.0f);
    const auto gridFor = [&grid](UnitId) { return &grid; };
    REQUIRE(rm::sim::applyCommand(moveIssue(waiter, 30.0f, 30.0f, 1), roster.store,
                                  roster.catalog, players, armies, terrain, gridFor,
                                  roster.rate)
                .accepted.size()
            == 1);
    const std::array<rm::sim::Fx, 2> beacon{rm::sim::fxFromFloat(30.0f),
                                           rm::sim::fxFromFloat(30.0f)};
    const std::array<rm::sim::Fx, 2> spot = rm::sim::ferryStagingSpot(
        roster.store, roster.catalog, transport, waiter.index, beacon);
    CHECK(rm::sim::fxToFloat(spot[0]) == Approx(40.0f).margin(0.01f));
    CHECK(rm::sim::fxToFloat(spot[1]) == Approx(33.0f).margin(0.01f));
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

TEST_CASE("C-225: a carrier stores aircraft in its pool and launches them airborne",
          "[fa-transport]") {
    // Retail's carrier storage is a plain integer pool — `(stored + reserved)
    // < StorageSlots`, no class matching, no bone machinery — fed by
    // `AddUnitToStorage` off the build pad and emptied by a detach that reads
    // as a launch, not a landing.
    const rm::HeightField field = flatField();
    const rm::sim::Terrain terrain{field};

    rm::test::Roster roster;
    const rm::UnitTypeIndex carrierType = roster.addType(carrierDef());
    const rm::UnitTypeIndex airType = roster.addType(airCargoDef());
    const rm::UnitTypeIndex landType = roster.addType(cargoDef());
    const UnitDef& carrier = *roster.catalog.def(carrierType);
    const UnitDef& aircraft = *roster.catalog.def(airType);
    const UnitDef& tank = *roster.catalog.def(landType);

    const UnitId carrierId = roster.add(carrierType, 100.0f, 100.0f, 0, 500.0f);

    // The pool takes aircraft only — a tank is not carrier cargo.
    CHECK(rm::sim::canEverCarry(carrier, aircraft));
    CHECK_FALSE(rm::sim::canEverCarry(carrier, tank));

    // Stored aircraft ride at the carrier's origin — no bone, no deck offset.
    const UnitId first = roster.add(airType, 120.0f, 120.0f, 0, 500.0f);
    const UnitId second = roster.add(airType, 130.0f, 130.0f, 0, 500.0f);
    REQUIRE(rm::sim::attachCargo(roster.store, roster.catalog, carrier,
                               carrierId, first));
    CHECK(roster.motion(first).attached);
    CHECK(roster.transform(first).x == roster.transform(carrierId).x);
    CHECK(roster.transform(first).z == roster.transform(carrierId).z);

    // Two of two slots filled: the pool is full.
    REQUIRE(rm::sim::attachCargo(roster.store, roster.catalog, carrier,
                               carrierId, second));
    const UnitId third = roster.add(airType, 140.0f, 140.0f, 0, 500.0f);
    CHECK_FALSE(rm::sim::hasRoomFor(roster.store, roster.catalog, carrierId,
                                    aircraft));

    // The launch is a detach, not a landing: both aircraft leave the hold
    // already airborne at the carrier's position.
    rm::sim::detachCargo(roster.store, roster.catalog, terrain, carrierId);
    CHECK_FALSE(roster.motion(first).attached);
    CHECK_FALSE(roster.motion(second).attached);
    CHECK(roster.motion(first).airborne);
    CHECK(roster.motion(second).airborne);
    CHECK(roster.transform(first).x == roster.transform(carrierId).x);
    CHECK(roster.transform(first).y == roster.transform(carrierId).y);
    (void)third;
}
TEST_CASE("C-225: a staging pad parks each aircraft on its own TransportClass bone",
          "[fa-transport]") {
    // Retail keys pad docking on `bp.Transport.TransportClass` (`CUnitRefuel::TaskTick`
    // 0x00627f60 via IAiTransport +0x54/+0x5c): a class-2 aircraft parks on a
    // class-2 hook even when a class-1 hook is nearer, and vice versa. The
    // `_Med`/`_Lrg`/`_Spr` lists are the pads' machinery, not the transports'.
    rm::test::Roster roster;
    UnitDef padDef;
    padDef.name = "test_pad";
    padDef.categories = {"AIRSTAGINGPLATFORM", "STRUCTURE"};  // sorted for hasCategory
    padDef.transport.dockingSlots = 4;
    const rm::UnitTypeIndex padType = roster.addType(padDef);
    // One class-1 hook near the origin, one class-2 hook twenty elmos out.
    const std::array<rm::sim::UnitCatalog::AttachBoneSpec, 2> bones{{
        {.bone = 19, .cargoClass = 1, .rest = {2.0f, 6.0f, 0.0f}},
        {.bone = 30, .cargoClass = 2, .rest = {20.0f, 6.0f, 0.0f}},
    }};
    roster.catalog.setAttachBones(padType, bones);
    const rm::UnitTypeIndex heavyType = roster.addType(cargoDef(2));
    const rm::UnitTypeIndex lightType = roster.addType(cargoDef(1));

    // A class-2 aircraft parked beside the class-1 hook still takes the
    // class-2 hook across the deck.
    const UnitId pad = roster.add(padType, 100.0f, 100.0f, 0, 500.0f);
    const UnitId heavy = roster.add(heavyType, 103.0f, 100.0f, 0, 500.0f);
    REQUIRE(rm::sim::dockOnStagingBone(roster.store, roster.catalog, pad, heavy));
    CHECK(roster.store.attachmentBonesOf(heavy).parent == 30);

    // And a class-1 aircraft parked beside the class-2 hook takes the
    // class-1 hook across the deck.
    const UnitId pad2 = roster.add(padType, 300.0f, 300.0f, 0, 500.0f);
    const UnitId light = roster.add(lightType, 319.0f, 300.0f, 0, 500.0f);
    REQUIRE(rm::sim::dockOnStagingBone(roster.store, roster.catalog, pad2, light));
    CHECK(roster.store.attachmentBonesOf(light).parent == 19);
}

TEST_CASE("C-264: a displaced mobile factory keeps its build attached to the pad",
          "[fa-transport]") {
    // Retail's carrier/mobile-factory scripts (`UES0401`, `UAA0310`,
    // `UAS0401`, `UEL0401` — the C-264 claim) run the same state machine:
    // `BuildingState` does `unitBuilding:AttachBoneTo(-2, self, BuildAttachBone)`,
    // so the product-in-progress rides the builder wherever it goes. The sim's
    // product does not exist until completion, but the CONSTRUCTION row is its
    // stand-in — and it must anchor to the builder, not to the map spot the
    // order happened to name. A factory shoved mid-build (collision push —
    // orders can't move it, `releaseInterruptedConstruction` erases pad rows
    // on any new order) used to orphan the row: every site lookup compares
    // `work.position` against the builder's CURRENT transform, the miss read
    // as "cancelled", and the build order retired with no product.
    const rm::HeightField field = flatField();
    const rm::sim::Terrain terrain{field};
    const rm::sim::PassabilityGrid grid =
        rm::sim::buildPassability(field, 0.0f, 60.0f, 0.0f);

    rm::test::Roster roster;
    // A UEL0401-shaped mobile factory: FACTORY + MOBILE, no storage pool —
    // its product always takes the `IssueMoveOffFactory` half of the claim.
    UnitDef mobileFactory;
    mobileFactory.name = "test_mobile_factory";
    mobileFactory.categories = {"FACTORY", "MOBILE"};
    mobileFactory.motion = rm::unitdef::MotionType::Land;
    mobileFactory.speedElmosPerSecond = 4.0f;
    mobileFactory.buildRate = 10.0f;
    mobileFactory.buildableCategory = {{"PRODUCT"}};
    const rm::UnitTypeIndex factoryType = roster.addType(mobileFactory);

    UnitDef product;
    product.name = "test_product";
    product.categories = {"PRODUCT"};
    product.motion = rm::unitdef::MotionType::Land;
    product.speedElmosPerSecond = 8.0f;
    product.buildTime = rm::sim::magFromFloat(100.0f);
    const rm::UnitTypeIndex productType = roster.addType(product);

    const UnitId factory = roster.add(factoryType, 40.0f, 40.0f, 0, 500.0f);

    const std::vector<Player> players{Player{.index = 0, .army = 0}};
    std::vector<Army> armies = rm::sim::freeForAll(1);
    std::vector<rm::sim::Construction> building;
    const std::vector<const rm::sim::PassabilityGrid*> grids{&grid, &grid};

    REQUIRE(rm::sim::applyCommand(
        CommandIssue{.source = 0,
                     .id = rm::commandId(0, 1),
                     .player = 0,
                     .kind = CommandKind::Build,
                     .units = {factory},
                     .buildType = productType},
        roster.store, roster.catalog, players, armies, terrain,
        [&grid](UnitId) { return &grid; }, roster.rate, &building)
                .accepted.size() == 1);

    rm::sim::Economy economy;
    economy.storage = {rm::sim::Mag::fromInt(100000), rm::sim::Mag::fromInt(100000)};
    economy.stored = {rm::sim::Mag::fromInt(50000), rm::sim::Mag::fromInt(50000)};
    const auto beat = [&] {
        (void)rm::sim::advanceOrders(roster.store, roster.catalog, terrain, grids,
                                     roster.rate, &building);
        rm::sim::tickEconomy(economy, building);
    };

    beat();
    REQUIRE(building.size() == 1);
    const rm::sim::Mag started = building.front().buildTimeRemaining;
    REQUIRE(started < building.front().totalBuildTime);

    // The shove: the hull is displaced mid-build, the way collision resolution
    // moves a unit without touching its queue.
    roster.transform(factory).x = rm::sim::fxFromFloat(60.0f);
    roster.transform(factory).z = rm::sim::fxFromFloat(60.0f);

    beat();
    // The pad is the builder: the row followed it, the order is still the
    // head, and the work kept advancing instead of freezing orphaned.
    REQUIRE(building.size() == 1);
    CHECK(building.front().position[0] == roster.transform(factory).x);
    CHECK(building.front().position[2] == roster.transform(factory).z);
    CHECK(building.front().buildTimeRemaining < started);
    CHECK(roster.store.orders()[factory.index].active() != nullptr);
}

// --- App-level: the build→attach→store/roll-off seam (C-264) -----------------
//
// The sim never spawns units — a finished construction becomes a model out of
// the VFS in `app/Match.cpp`, which is also where `AddUnitToStorage` vs
// `IssueMoveOffFactory` is decided. These cases drive `advanceMatch` like
// test_match.cpp does, with a pre-registered product type standing in for the
// blueprint the VFS would hand back.

namespace {

/// A UES0401-shaped carrier-factory: FACTORY + CARRIER + NAVALCARRIER with a
/// two-slot pool, building only aircraft.
[[nodiscard]] UnitDef carrierFactoryDef() {
    UnitDef def;
    def.name = "test_carrier_factory";
    def.categories = {"CARRIER", "FACTORY", "MOBILE", "NAVALCARRIER"};
    def.motion = rm::unitdef::MotionType::Water;
    def.speedElmosPerSecond = 4.0f;
    def.buildRate = 60.0f;
    def.buildableCategory = {{"AIR_PRODUCT"}};
    def.commandCaps = {"RULEUCC_Transport"};
    def.commandCapsDeclared = true;
    def.transport.storageSlots = 2;
    return def;
}

[[nodiscard]] UnitDef airProductDef() {
    UnitDef def;
    def.name = "test_air_product";
    def.categories = {"AIR_PRODUCT"};
    def.motion = rm::unitdef::MotionType::Air;
    def.canFly = true;
    def.speedElmosPerSecond = 12.0f;
    def.buildTime = rm::sim::magFromFloat(1.0f);
    def.health = rm::sim::Mag::fromInt(100);  // live test aircraft, not a zero-HP corpse
    return def;
}

struct CarrierScene {
    rm::app::UnitScene scene;
    rm::app::PassabilitySet passability;
    rm::vfs::Vfs content;
    /// Emplaced only after the scene is populated — the runner captures
    /// pointers into it (economies, store), so it cannot be built first.
    std::optional<rm::app::MatchRunner> runner;
    rm::sim::UnitId carrier;
    rm::UnitTypeIndex productType = 0;

    /// A one-army match with a carrier-factory standing at (200, 200) and an
    /// air product it can build. `productType` is registered with a blueprint
    /// path so `spawnUnit` resolves it without touching the VFS.
    explicit CarrierScene(const rm::HeightField& field)
        : passability(field, false, 0.0f) {
        scene.armies = rm::sim::freeForAll(1);
        scene.players = rm::sim::onePlayerPerArmy(1, 0);
        scene.economies.assign(1, rm::sim::Economy{});
        scene.economies[0].stored = {rm::sim::Mag::fromInt(50000),
                                    rm::sim::Mag::fromInt(50000)};
        scene.commandersEver.assign(1, 0);

        UnitDef carrierDef = carrierFactoryDef();
        scene.definitions.push_back(carrierDef);
        const rm::UnitTypeIndex carrierType =
            scene.catalog.add(&scene.definitions.back(), rm::app::gAppTickRate);
        scene.setTypeTraits(carrierType, rm::data::moveDefFor(carrierDef), 1.0f);

        UnitDef product = airProductDef();
        scene.definitions.push_back(product);
        productType =
            scene.catalog.add(&scene.definitions.back(), rm::app::gAppTickRate);
        scene.setTypeTraits(productType, rm::data::moveDefFor(product), 1.0f);
        constexpr std::string_view kProductPath{"/test_air_product"};
        scene.setPathForType(productType, kProductPath);
        scene.typeForBlueprint.emplace(kProductPath, productType);

        carrier = scene.store.spawn(rm::sim::UnitStore::Spawn{
            .type = carrierType,
            .transform = {.x = rm::sim::fxFromFloat(200.0f),
                          .z = rm::sim::fxFromFloat(200.0f)},
            .motion = rm::app::motionFor(carrierDef, 0),
            .health = rm::sim::initialHealth(rm::sim::Mag::fromInt(500)),
        });

        runner.emplace(rm::app::makeMatchRunner(scene, field, passability,
                                                content, {}, {}));
        runner->scripts.clear();
    }
};
/// Build `count` products on the carrier, ticking until each finishes.
/// Returns the spawned unit ids in completion order.
[[nodiscard]] std::vector<UnitId> buildProducts(CarrierScene& f, int count) {
    std::vector<UnitId> spawned;
    for (int i = 0; i < count; ++i) {
        REQUIRE(rm::app::issueBuild(f.scene, f.carrier, 0, 0, f.productType,
                                    rm::sim::fxFromFloat(200.0f),
                                    rm::sim::fxFromFloat(200.0f)));
        const std::size_t before = f.scene.store.slotCount();
        for (int tick = 0; tick < 40 && f.scene.store.slotCount() == before; ++tick) {
            (void)rm::app::advanceMatch(*f.runner, tick, 0.0f);
        }
        REQUIRE(f.scene.store.slotCount() == before + 1);
        spawned.push_back(f.scene.store.idAt(static_cast<rm::UnitIndex>(before)));
    }
    return spawned;
}

} // namespace

TEST_CASE("C-264: a carrier's product is stored attached, not rolled off",
          "[fa-transport]") {
    // The claim's `FinishedBuildingState`: `DetachFrom` then
    // `AddUnitToStorage` when `TransportHasAvailableStorage` — the product
    // ends the build INSIDE the carrier's pool, attached, with no roll-off
    // move issued. `UnloadTransport` is the deploy: the stored aircraft
    // launches airborne at the carrier's position.
    const rm::HeightField field = flatField();
    CarrierScene f{field};

    const std::vector<UnitId> built = buildProducts(f, 1);
    const UnitId product = built.front();

    // Stored, not rolled off: attached to the carrier at its origin, and the
    // product's queue holds no roll-off move.
    CHECK(f.scene.store.parentOf(product).has_value());
    CHECK(f.scene.store.motion()[product.index].attached);
    CHECK(f.scene.store.transforms()[product.index].x
          == f.scene.store.transforms()[f.carrier.index].x);
    CHECK(f.scene.store.orders()[product.index].current() == nullptr);

    // The deploy: UnloadTransport launches the hold airborne where the
    // carrier stands (C-225's launch semantics, the claim's release half).
    REQUIRE(rm::app::submitCommand(f.scene, CommandIssue{
        .source = 0,
        .player = 0,
        .kind = CommandKind::UnloadTransport,
        .units = {f.carrier},
        .count = 1,
    }).has_value());
    for (int tick = 0; tick < 40 && f.scene.store.motion()[product.index].attached;
         ++tick) {
        (void)rm::app::advanceMatch(*f.runner, tick, 0.0f);
    }
    CHECK_FALSE(f.scene.store.motion()[product.index].attached);
    CHECK(f.scene.store.motion()[product.index].airborne);
}

TEST_CASE("C-264: a full carrier pool sends the product off the pad instead",
          "[fa-transport]") {
    // The claim's other branch: `TransportHasAvailableStorage` false →
    // `IssueMoveOffFactory`. With both pool slots taken, the third product
    // materialises DETACHED and gets the ordinary roll-off move.
    const rm::HeightField field = flatField();
    CarrierScene f{field};

    const std::vector<UnitId> built = buildProducts(f, 3);
    CHECK(f.scene.store.motion()[built[0].index].attached);
    CHECK(f.scene.store.motion()[built[1].index].attached);

    const UnitId third = built[2];
    CHECK_FALSE(f.scene.store.motion()[third.index].attached);
    const rm::sim::QueuedCommand* rollOff =
        f.scene.store.orders()[third.index].current();
    REQUIRE(rollOff != nullptr);
    CHECK(rollOff->kind() == CommandKind::Move);
}

TEST_CASE("C-458: unload lands in slot order, single cargo at the keel",
          "[fa-transport]") {
    // `CUnitUnloadUnits::TaskTick` (`0x0062ce60`) walks its task slots
    // strictly ascending with per-slot landing — landing-spot order is
    // slot/command order — and takes a single-unit fast path when the hold
    // carries one (`0x60 == 0x64`). The player sees a full hold fan out in
    // boarding order while a lone passenger steps off at the keel, not out
    // on the ring.
    const rm::HeightField field = flatField();
    const rm::sim::Terrain terrain{field};

    rm::test::Roster roster;
    const rm::UnitTypeIndex transportType = roster.addType(transportDef());
    const rm::UnitTypeIndex cargoType = roster.addType(cargoDef());
    const UnitId transport = landedTransport(roster, transportType, 50.0f, 50.0f);
    const UnitId first = roster.add(cargoType, 51.0f, 50.0f, 0, 500.0f);
    const UnitId second = roster.add(cargoType, 49.0f, 50.0f, 0, 500.0f);
    const UnitId third = roster.add(cargoType, 50.0f, 51.0f, 0, 500.0f);
    REQUIRE(rm::sim::attachCargo(roster.store, roster.catalog,
                                  *roster.catalog.def(transportType), transport, first));
    REQUIRE(rm::sim::attachCargo(roster.store, roster.catalog,
                                  *roster.catalog.def(transportType), transport, second));
    REQUIRE(rm::sim::attachCargo(roster.store, roster.catalog,
                                  *roster.catalog.def(transportType), transport, third));

    rm::sim::detachCargo(roster.store, roster.catalog, terrain, transport);
    REQUIRE_FALSE(roster.motion(first).attached);
    // Spread is deck radius (4) + 2 elmos: slot order fans out (+6,0),
    // (0,+6), (-6,+6) off the keel.
    CHECK(rm::sim::fxToFloat(roster.transform(first).x) == Approx(56.0f).margin(0.01f));
    CHECK(rm::sim::fxToFloat(roster.transform(first).z) == Approx(50.0f).margin(0.01f));
    CHECK(rm::sim::fxToFloat(roster.transform(second).x) == Approx(50.0f).margin(0.01f));
    CHECK(rm::sim::fxToFloat(roster.transform(second).z) == Approx(56.0f).margin(0.01f));
    CHECK(rm::sim::fxToFloat(roster.transform(third).x) == Approx(44.0f).margin(0.01f));
    CHECK(rm::sim::fxToFloat(roster.transform(third).z) == Approx(56.0f).margin(0.01f));

    // The single-unit fast path: a lone passenger lands at the keel.
    rm::test::Roster solo;
    const rm::UnitTypeIndex soloTransportType = solo.addType(transportDef());
    const rm::UnitTypeIndex soloCargoType = solo.addType(cargoDef());
    const UnitId soloTransport = landedTransport(solo, soloTransportType, 50.0f, 50.0f);
    const UnitId lone = solo.add(soloCargoType, 51.0f, 50.0f, 0, 500.0f);
    REQUIRE(rm::sim::attachCargo(solo.store, solo.catalog,
                                 *solo.catalog.def(soloTransportType), soloTransport, lone));
    rm::sim::detachCargo(solo.store, solo.catalog, terrain, soloTransport);
    REQUIRE_FALSE(solo.motion(lone).attached);
    CHECK(solo.transform(lone).x == solo.transform(soloTransport).x);
    CHECK(solo.transform(lone).z == solo.transform(soloTransport).z);
}

TEST_CASE("C-413: replacing the guard queue head selects a new ferry beacon",
          "[fa-transport]") {
    // C-413 (0x006d4160) selects the beacon through the queue head, not
    // through a persistent Unit field. Replacing Guard must discard the old
    // route's pickup/phase and let passengers board at the new beacon.
    const rm::HeightField field = flatField();
    const rm::sim::Terrain terrain{field};
    const rm::sim::PassabilityGrid grid =
        rm::sim::buildPassability(field, 0.0f, 60.0f, 0.0f);

    rm::test::Roster roster;
    const rm::UnitTypeIndex transportType = roster.addType(transportDef());
    const rm::UnitTypeIndex cargoType = roster.addType(cargoDef());
    const rm::UnitTypeIndex beaconType = roster.addType(beaconDef());
    const UnitId transport = landedTransport(roster, transportType, 30.0f, 30.0f);
    const UnitId beacon = roster.add(beaconType, 30.0f, 30.0f, 0, 10.0f);

    const std::vector<Player> players{Player{.index = 0, .army = 0}};
    std::vector<Army> armies = rm::sim::freeForAll(1);
    std::vector<rm::sim::Economy> economies(1);
    std::vector<int> commandersEver(1, 0);
    std::vector<const rm::sim::PassabilityGrid*> grids(roster.catalog.size(), &grid);
    const auto gridFor = [&grid](UnitId) { return &grid; };

    // The beacon carries no route command, so the guard holds at the pickup.
    REQUIRE(rm::sim::applyCommand(
                CommandIssue{.source = 0,
                             .id = rm::commandId(0, 1),
                             .player = 0,
                             .kind = CommandKind::Guard,
                             .units = {transport},
                             .target = beacon},
                roster.store, roster.catalog, players, armies, terrain, gridFor,
                roster.rate)
                .accepted.size()
            == 1);

    Match match = loneMatch(armies, economies, commandersEver, grids);
    for (int i = 0; i < 120; ++i) {
        (void)rm::sim::tickSkirmish(roster.store, roster.catalog, match, terrain);
    }
    // Holding at the pickup: still near where the route started.
    {
        const Fx dx = roster.transform(transport).x - rm::sim::fxFromFloat(30.0f);
        const Fx dz = roster.transform(transport).z - rm::sim::fxFromFloat(30.0f);
        REQUIRE(dx * dx + dz * dz
                <= rm::sim::kFerryPickupRadius * rm::sim::kFerryPickupRadius * 4);
    }

    const UnitId replacement = roster.add(beaconType, 150.0f, 150.0f, 0, 10.0f);
    REQUIRE(rm::sim::applyCommand(
                CommandIssue{.source = 0, .id = rm::commandId(0, 2), .player = 0,
                             .kind = CommandKind::Guard, .units = {transport},
                             .target = replacement},
                roster.store, roster.catalog, players, armies, terrain, gridFor,
                roster.rate).accepted.size() == 1);
    const UnitId cargo = roster.add(cargoType, 150.0f, 130.0f, 0, 500.0f);
    REQUIRE(rm::sim::applyCommand(moveIssue(cargo, 150.0f, 150.0f, 3), roster.store,
                                  roster.catalog, players, armies, terrain, gridFor,
                                  roster.rate)
                .accepted.size()
            == 1);

    // The beacon still carries no route command (drop == pickup), so a boarded carrier
    // holds its cargo: boarding is the whole assertion.
    bool boarded = false;
    for (int i = 0; i < 6000 && !boarded; ++i) {
        (void)rm::sim::tickSkirmish(roster.store, roster.catalog, match, terrain);
        boarded = roster.motion(cargo).attached;
    }
    CHECK(boarded);
}
TEST_CASE("C-458: an empty UnloadTransport holds for an inbound loader", "[fa-transport]") {
    // Retail's `CUnitUnloadUnits::TaskTick` (`0x0062ce60`) opens in state 0 by
    // repositioning the air transport before any detach — an `UnloadTransport`
    // with an empty hold and a live `LoadTransport` inbound is the pickup
    // half of the auto-embark pair, not a no-op to retire. The carrier flies
    // to meet the cargo and holds instead of dropping an order that still has
    // work to do.
    const rm::HeightField field = flatField();
    const rm::sim::Terrain terrain{field};
    const rm::sim::PassabilityGrid grid =
        rm::sim::buildPassability(field, 0.0f, 60.0f, 0.0f);

    rm::test::Roster roster;
    const rm::UnitTypeIndex transportType = roster.addType(transportDef());
    const rm::UnitTypeIndex cargoType = roster.addType(cargoDef());
    // Airborne at cruise: the pickup hold must keep the deck away from the
    // drop rather than flying the leg. Cargo west of the carrier, drop east —
    // opposite bearings, so closing on one side is leaving the other.
    const UnitId transport = roster.add(transportType, 100.0f, 100.0f, 0, 500.0f);
    giveFlight(roster.motion(transport));
    roster.motion(transport).airState = rm::sim::MoveState::AirState::Top;
    const UnitId cargo = roster.add(cargoType, 20.0f, 100.0f, 0, 500.0f);

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
                             .kind = CommandKind::LoadTransport,
                             .units = {cargo},
                             .target = transport},
                roster.store, roster.catalog, players, armies, terrain, gridFor,
                roster.rate)
                .accepted.size()
            == 1);
    REQUIRE(rm::sim::applyCommand(
                CommandIssue{.source = 0,
                             .id = rm::commandId(0, 2),
                             .player = 0,
                             .kind = CommandKind::UnloadTransport,
                             .units = {transport},
                             .targetX = rm::sim::fxFromFloat(400.0f),
                             .targetZ = rm::sim::fxFromFloat(100.0f)},
                roster.store, roster.catalog, players, armies, terrain, gridFor,
                roster.rate)
                .accepted.size()
            == 1);

    Match match = loneMatch(armies, economies, commandersEver, grids);
    for (int i = 0; i < 30; ++i) {
        (void)rm::sim::tickSkirmish(roster.store, roster.catalog, match, terrain);
    }
    // The pickup is still pending: the unload stays queued at the head.
    REQUIRE_FALSE(roster.store.orders()[transport.index].empty());
    CHECK(roster.store.orders()[transport.index].current()->kind()
          == CommandKind::UnloadTransport);
    // State-0 repositioning: the empty carrier closes on its loader instead
    // of sailing to the drop — the tick-to-tick displacement points at the
    // cargo, not the click.
    const Fx beforeX = roster.transform(transport).x;
    const Fx beforeZ = roster.transform(transport).z;
    for (int i = 0; i < 200 && !roster.motion(cargo).attached; ++i) {
        (void)rm::sim::tickSkirmish(roster.store, roster.catalog, match, terrain);
    }
    const Fx sailedX = roster.transform(transport).x - beforeX;
    const Fx sailedZ = roster.transform(transport).z - beforeZ;
    const Fx toCargoX = roster.transform(cargo).x - beforeX;
    const Fx toCargoZ = roster.transform(cargo).z - beforeZ;
    CHECK(sailedX * toCargoX + sailedZ * toCargoZ > Fx{});
}
