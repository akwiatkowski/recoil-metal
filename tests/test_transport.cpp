// Transport cargo and ferries (#15633, #15725): a TRANSPORTATION carrier declares a
// capacity in class-1 attach slots (`TransportClass`) plus the slot cost of each
// bigger cargo class (`ClassNAttachSize`, or `ClassGenericUpTo` for the one-unit
// carriers). A cargo unit's own `TransportClass` is its size class; mobile units
// that omit it are class 1. Boarding is the cargo's order, the flight is the
// transport's, and a ferry is a standing beacon→drop loop that picks up whatever
// was ordered to the beacon.
//
// Cargo rides on the generic attachment machinery (`C-195`/`C-196`): an attached
// unit does not move, does not fight, and follows the carrier's transform.
#include <catch2/catch_test_macros.hpp>

#include "core/map/HeightField.hpp"
#include "core/sim/Command.hpp"
#include "core/sim/SaveState.hpp"
#include "core/sim/Skirmish.hpp"
#include "core/sim/StateHash.hpp"
#include "core/sim/Transport.hpp"
#include "core/unit/UnitBlueprint.hpp"
#include "core/unit/UnitDef.hpp"

#include "support/FxMatchers.hpp"
#include "support/TestRoster.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace {

using rm::sim::Army;
using rm::sim::CommandIssue;
using rm::sim::CommandKind;
using rm::sim::Match;
using rm::sim::Player;
using rm::sim::UnitId;
using rm::unitdef::UnitDef;

[[nodiscard]] rm::HeightField flatField() {
    rm::HeightField field;
    field.squaresX = 100;
    field.squaresZ = 100;
    field.baseHeight = 0.0f;
    field.heightScale = 1.0f;
    field.raw.assign(field.sampleCount(), std::uint16_t{0});
    return field;
}

/// A landed UEA0107-shaped carrier: six class-1 slots, a class-2 unit costs two,
/// a class-3 unit four, class 4 does not fit at all.
[[nodiscard]] UnitDef transportDef() {
    UnitDef def;
    def.name = "test_transport";
    def.categories = {"AIR", "MOBILE", "TRANSPORTATION"};  // sorted: hasCategory is a binary search
    def.motion = rm::unitdef::MotionType::Air;
    def.canFly = true;
    def.speedElmosPerSecond = 10.0f;
    def.commandCaps = {"RULEUCC_Transport"};
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

/// A shooter that out-ranges everything, for the not-targetable case.
[[nodiscard]] UnitDef shooterDef() {
    UnitDef def = cargoDef();
    def.name = "test_shooter";
    rm::unitdef::Weapon gun;
    gun.maxRange = rm::sim::Fx::fromInt(60);
    gun.damage = rm::sim::Mag::fromInt(10);
    gun.rateOfFire = 1.0f;
    gun.targetLayers = rm::unitdef::TargetLayerMask::Surface;
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

/// The flight block a `canFly` unit needs to leave the deck: `roster.add` only
/// sets `airborne` from the motion class, so the lift law's constants — the
/// ones `motionFor` fills from the blueprint — are spelled out here with
/// UEA0101's control numbers, the same set `flyer()` in test_movement uses.
void giveFlight(rm::sim::MoveState& motion) {
    motion.canFly = true;
    motion.speedPerTick = rm::sim::Fx::fromInt(16);
    motion.airMaxSpeedElmosPerSec = rm::sim::Fx::fromInt(160);
    motion.airKMove = rm::sim::Fx::fromInt(1);
    motion.airKMoveDamping = rm::sim::Fx::fromInt(1);
    motion.airKLift = rm::sim::Fx::fromInt(3);
    motion.airKLiftDamping = rm::sim::Fx::fromRatio(5, 2);
    motion.airLiftFactor = rm::sim::Fx::fromInt(7);
    motion.airElevation = rm::sim::Fx::fromInt(80);
    motion.idleLandThreshold = std::numeric_limits<std::uint32_t>::max();  // no auto-land mid-test
    motion.fuelDrainPerTick = rm::sim::Fx::fromRatio(1, 5000);
    motion.fuelRatio = rm::sim::Fx::fromInt(1);
}

/// Spawn a transport already sitting on the deck.
[[nodiscard]] UnitId landedTransport(rm::test::Roster& roster, rm::UnitTypeIndex type,
                                     float x, float z, int army = 0) {
    const UnitId id = roster.add(type, x, z, army, 500.0f);
    giveFlight(roster.motion(id));
    roster.motion(id).airborne = false;
    roster.motion(id).airState = rm::sim::MoveState::AirState::Bottom;
    roster.transform(id).y = rm::sim::Fx{};
    return id;
}

[[nodiscard]] CommandIssue loadIssue(const UnitId cargo, const UnitId transport,
                                     const std::uint32_t serial) {
    return CommandIssue{.source = 0,
                        .id = rm::commandId(0, serial),
                        .player = 0,
                        .kind = CommandKind::LoadTransport,
                        .units = {cargo},
                        .target = transport};
}

[[nodiscard]] CommandIssue dropIssue(const UnitId transport, float x, float z,
                                     const std::uint32_t serial,
                                     CommandKind kind = CommandKind::UnloadTransport) {
    return CommandIssue{.source = 0,
                        .id = rm::commandId(0, serial),
                        .player = 0,
                        .kind = kind,
                        .units = {transport},
                        .targetX = rm::sim::fxFromFloat(x),
                        .targetZ = rm::sim::fxFromFloat(z)};
}

} // namespace

// --- The data model --------------------------------------------------------

TEST_CASE("the Transport block parses on a real carrier blueprint", "[transport]") {
    const char* home = std::getenv("HOME");
    if (home == nullptr) {
        SKIP("no HOME to locate the FA corpus");
    }
    const std::filesystem::path path = std::filesystem::path{home}
                                       / "projects/llm/input/faf/units/UEA0107/UEA0107_unit.bp";
    if (!std::filesystem::exists(path)) {
        SKIP("the FA corpus is not extracted");
    }
    const auto def = rm::unitbp::loadFile(path);
    REQUIRE(def.has_value());
    CHECK(def->isTransport());
    CHECK(def->transport.airClass);
    CHECK(def->transportCapacity() == 10);
    CHECK(def->transportAttachCost(1) == 1);
    CHECK(def->transportAttachCost(2) == 2);
    CHECK(def->transportAttachCost(3) == 4);
    CHECK(def->transportAttachCost(4) == 0);  // never declared: experimentals do not fit
}

TEST_CASE("cargo class comes from TransportClass and defaults to one", "[transport]") {
    UnitDef titan = cargoDef(3);
    CHECK(titan.transportCargoClass() == 3);
    UnitDef bot = cargoDef(0);  // no class stated, like UEL0103's absent block
    CHECK(bot.transportCargoClass() == 1);

    UnitDef carrier = transportDef();
    CHECK_FALSE(carrier.transportable());       // a carrier is not itself cargo
    CHECK(bot.transportable());

    UnitDef building;
    building.name = "test_building";
    building.categories = {"STRUCTURE"};
    building.commandCaps = {"RULEUCC_CallTransport"};
    building.commandCapsDeclared = true;
    CHECK_FALSE(building.transportable());      // immobile

    UnitDef uncarriable = cargoDef();
    uncarriable.commandCaps.clear();
    CHECK_FALSE(uncarriable.transportable());   // no CallTransport cap

    // The one-unit carriers say `ClassGenericUpTo` instead of per-class sizes:
    // a class-N unit costs N slots and nothing above the cap rides.
    UnitDef stinger = transportDef();
    stinger.transport.class2AttachSize = 0;
    stinger.transport.class3AttachSize = 0;
    stinger.transport.classGenericUpTo = 2;
    stinger.transport.transportClass = 2;
    CHECK(stinger.transportAttachCost(1) == 1);
    CHECK(stinger.transportAttachCost(2) == 2);
    CHECK(stinger.transportAttachCost(3) == 0);
}

// --- Loading ---------------------------------------------------------------

TEST_CASE("a cargo unit walks to a landed transport and boards", "[transport]") {
    const rm::HeightField field = flatField();
    const rm::sim::Terrain terrain{field};
    const rm::sim::PassabilityGrid grid =
        rm::sim::buildPassability(field, 0.0f, 60.0f, 0.0f);

    rm::test::Roster roster;
    const rm::UnitTypeIndex transportType = roster.addType(transportDef());
    const rm::UnitTypeIndex cargoType = roster.addType(cargoDef());
    const UnitId transport = landedTransport(roster, transportType, 50.0f, 50.0f);
    const UnitId cargo = roster.add(cargoType, 58.0f, 50.0f, 0, 500.0f);

    const std::vector<Player> players{Player{.index = 0, .army = 0}};
    std::vector<Army> armies = rm::sim::freeForAll(1);
    std::vector<rm::sim::Economy> economies(1);
    std::vector<int> commandersEver(1, 0);
    std::vector<const rm::sim::PassabilityGrid*> grids(
        roster.catalog.size(), &grid);

    const auto set = rm::sim::applyCommand(
        loadIssue(cargo, transport, 1), roster.store, roster.catalog, players, armies,
        terrain, [&grid](UnitId) { return &grid; }, roster.rate);
    REQUIRE(set.accepted.size() == 1);

    Match match = loneMatch(armies, economies, commandersEver, grids);
    for (int i = 0; i < 400 && !roster.motion(cargo).attached; ++i) {
        (void)rm::sim::tickSkirmish(roster.store, roster.catalog, match, terrain);
    }

    REQUIRE(roster.motion(cargo).attached);
    CHECK(roster.store.parentOf(cargo) == transport);
    CHECK(roster.store.childrenOf(transport).size() == 1);
    CHECK(roster.store.orders()[cargo.index].empty());  // the order retires on boarding
}

TEST_CASE("an idle transport flies to cargo that wants aboard", "[transport]") {
    const rm::HeightField field = flatField();
    const rm::sim::Terrain terrain{field};
    const rm::sim::PassabilityGrid grid =
        rm::sim::buildPassability(field, 0.0f, 60.0f, 0.0f);

    rm::test::Roster roster;
    const rm::UnitTypeIndex transportType = roster.addType(transportDef());
    const rm::UnitTypeIndex cargoType = roster.addType(cargoDef());
    // Airborne and idle, far away — the pickup must come to the unit.
    const UnitId transport = roster.add(transportType, 10.0f, 10.0f, 0, 500.0f);
    giveFlight(roster.motion(transport));
    roster.motion(transport).airState = rm::sim::MoveState::AirState::Top;
    const UnitId cargo = roster.add(cargoType, 70.0f, 70.0f, 0, 500.0f);

    const std::vector<Player> players{Player{.index = 0, .army = 0}};
    std::vector<Army> armies = rm::sim::freeForAll(1);
    std::vector<rm::sim::Economy> economies(1);
    std::vector<int> commandersEver(1, 0);
    std::vector<const rm::sim::PassabilityGrid*> grids(
        roster.catalog.size(), &grid);

    REQUIRE(rm::sim::applyCommand(
                loadIssue(cargo, transport, 1), roster.store, roster.catalog, players,
                armies, terrain, [&grid](UnitId) { return &grid; }, roster.rate)
                .accepted.size()
            == 1);

    Match match = loneMatch(armies, economies, commandersEver, grids);
    for (int i = 0; i < 3000 && !roster.motion(cargo).attached; ++i) {
        (void)rm::sim::tickSkirmish(roster.store, roster.catalog, match, terrain);
    }
    CHECK(roster.motion(cargo).attached);
    CHECK(roster.store.parentOf(cargo) == transport);
}

TEST_CASE("capacity caps the lift and the overflow waits", "[transport]") {
    const rm::HeightField field = flatField();
    const rm::sim::Terrain terrain{field};
    const rm::sim::PassabilityGrid grid =
        rm::sim::buildPassability(field, 0.0f, 60.0f, 0.0f);

    rm::test::Roster roster;
    UnitDef small = transportDef();
    small.transport.transportClass = 2;  // two class-1 slots
    const rm::UnitTypeIndex transportType = roster.addType(small);
    const rm::UnitTypeIndex cargoType = roster.addType(cargoDef());
    const rm::UnitTypeIndex heavyType = roster.addType(cargoDef(3));
    const UnitId transport = landedTransport(roster, transportType, 50.0f, 50.0f);
    const UnitId first = roster.add(cargoType, 56.0f, 50.0f, 0, 500.0f);
    const UnitId second = roster.add(cargoType, 58.0f, 50.0f, 0, 500.0f);
    const UnitId third = roster.add(cargoType, 60.0f, 50.0f, 0, 500.0f);
    const UnitId heavy = roster.add(heavyType, 62.0f, 50.0f, 0, 500.0f);

    const std::vector<Player> players{Player{.index = 0, .army = 0}};
    std::vector<Army> armies = rm::sim::freeForAll(1);
    std::vector<rm::sim::Economy> economies(1);
    std::vector<int> commandersEver(1, 0);
    std::vector<const rm::sim::PassabilityGrid*> grids(
        roster.catalog.size(), &grid);
    const auto gridFor = [&grid](UnitId) { return &grid; };

    std::uint32_t serial = 1;
    for (const UnitId cargo : {first, second, third}) {
        REQUIRE(rm::sim::applyCommand(loadIssue(cargo, transport, serial++),
                                      roster.store, roster.catalog, players, armies,
                                      terrain, gridFor, roster.rate)
                    .accepted.size()
                == 1);
    }
    // A class-3 unit costs four slots the carrier never had: refused at the
    // door, not parked waiting for capacity that cannot exist.
    CHECK(rm::sim::applyCommand(loadIssue(heavy, transport, serial++),
                                roster.store, roster.catalog, players, armies,
                                terrain, gridFor, roster.rate)
              .accepted.empty());

    Match match = loneMatch(armies, economies, commandersEver, grids);
    for (int i = 0; i < 600; ++i) {
        (void)rm::sim::tickSkirmish(roster.store, roster.catalog, match, terrain);
    }

    CHECK(roster.motion(first).attached);
    CHECK(roster.motion(second).attached);
    CHECK_FALSE(roster.motion(third).attached);   // two slots full
    // A class-3 unit costs four slots the carrier never had: refused outright,
    // not parked waiting for capacity that cannot exist.
    CHECK(roster.store.orders()[heavy.index].empty());
}

// --- Unloading ---------------------------------------------------------------

TEST_CASE("unload sets the cargo down at the destination", "[transport]") {
    const rm::HeightField field = flatField();
    const rm::sim::Terrain terrain{field};
    const rm::sim::PassabilityGrid grid =
        rm::sim::buildPassability(field, 0.0f, 60.0f, 0.0f);

    rm::test::Roster roster;
    const rm::UnitTypeIndex transportType = roster.addType(transportDef());
    const rm::UnitTypeIndex cargoType = roster.addType(cargoDef());
    const UnitId transport = roster.add(transportType, 20.0f, 20.0f, 0, 500.0f);
    giveFlight(roster.motion(transport));
    roster.motion(transport).airState = rm::sim::MoveState::AirState::Top;
    const UnitId cargo = roster.add(cargoType, 21.0f, 20.0f, 0, 500.0f);
    REQUIRE(roster.store.attach(transport, cargo));

    const std::vector<Player> players{Player{.index = 0, .army = 0}};
    std::vector<Army> armies = rm::sim::freeForAll(1);
    std::vector<rm::sim::Economy> economies(1);
    std::vector<int> commandersEver(1, 0);
    std::vector<const rm::sim::PassabilityGrid*> grids(
        roster.catalog.size(), &grid);

    const auto set = rm::sim::applyCommand(
        dropIssue(transport, 80.0f, 80.0f, 1), roster.store, roster.catalog, players,
        armies, terrain, [&grid](UnitId) { return &grid; }, roster.rate);
    REQUIRE(set.accepted.size() == 1);

    Match match = loneMatch(armies, economies, commandersEver, grids);
    for (int i = 0; i < 3000 && roster.motion(cargo).attached; ++i) {
        (void)rm::sim::tickSkirmish(roster.store, roster.catalog, match, terrain);
    }

    REQUIRE_FALSE(roster.motion(cargo).attached);
    CHECK(roster.store.childrenOf(transport).empty());
    const rm::sim::Transform& at = roster.transform(cargo);
    CHECK(rm::sim::fxToFloat(at.y) == Catch::Approx(0.0f).margin(0.5f));
    CHECK(rm::sim::fxToFloat(at.x) == Catch::Approx(80.0f).margin(8.0f));
    CHECK(rm::sim::fxToFloat(at.z) == Catch::Approx(80.0f).margin(8.0f));
}

// --- Ferry -------------------------------------------------------------------

TEST_CASE("a ferry loops beacon to drop and back with whatever was sent to it",
          "[transport]") {
    const rm::HeightField field = flatField();
    const rm::sim::Terrain terrain{field};
    const rm::sim::PassabilityGrid grid =
        rm::sim::buildPassability(field, 0.0f, 60.0f, 0.0f);

    rm::test::Roster roster;
    const rm::UnitTypeIndex transportType = roster.addType(transportDef());
    const rm::UnitTypeIndex cargoType = roster.addType(cargoDef());
    // Landed at the beacon; the ferry order names the drop point.
    const UnitId transport = landedTransport(roster, transportType, 30.0f, 30.0f);
    const UnitId cargo = roster.add(cargoType, 42.0f, 30.0f, 0, 500.0f);

    const std::vector<Player> players{Player{.index = 0, .army = 0}};
    std::vector<Army> armies = rm::sim::freeForAll(1);
    std::vector<rm::sim::Economy> economies(1);
    std::vector<int> commandersEver(1, 0);
    std::vector<const rm::sim::PassabilityGrid*> grids(
        roster.catalog.size(), &grid);
    const auto gridFor = [&grid](UnitId) { return &grid; };

    // The ferry first, so the beacon exists where the transport stands — then the
    // cargo is ordered to that beacon the way a player sends a squad to a ferry.
    REQUIRE(rm::sim::applyCommand(
                dropIssue(transport, 80.0f, 80.0f, 1, CommandKind::Ferry),
                roster.store, roster.catalog, players, armies, terrain, gridFor,
                roster.rate)
                .accepted.size()
            == 1);
    REQUIRE(rm::sim::applyCommand(
                CommandIssue{.source = 0,
                             .id = rm::commandId(0, 2),
                             .player = 0,
                             .kind = CommandKind::Move,
                             .units = {cargo},
                             .targetX = rm::sim::fxFromFloat(30.0f),
                             .targetZ = rm::sim::fxFromFloat(30.0f)},
                roster.store, roster.catalog, players, armies, terrain, gridFor,
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
    const rm::sim::Transform& at = roster.transform(cargo);
    CHECK(rm::sim::fxToFloat(at.x) == Catch::Approx(80.0f).margin(10.0f));
    CHECK(rm::sim::fxToFloat(at.z) == Catch::Approx(80.0f).margin(10.0f));
}

// --- Cargo protection ----------------------------------------------------------

TEST_CASE("attached cargo is not a target and does not shoot", "[transport]") {
    const rm::HeightField field = flatField();
    const rm::sim::Terrain terrain{field};

    rm::test::Roster roster;
    const rm::UnitTypeIndex transportType = roster.addType(transportDef());
    const rm::UnitTypeIndex cargoType = roster.addType(cargoDef());
    const rm::UnitTypeIndex shooterType = roster.addType(shooterDef());
    const UnitId transport = landedTransport(roster, transportType, 50.0f, 50.0f);
    const UnitId cargo = roster.add(cargoType, 51.0f, 50.0f, 0, 500.0f);
    REQUIRE(roster.store.attach(transport, cargo));
    const UnitId shooter = roster.add(shooterType, 70.0f, 50.0f, 1, 500.0f);
    (void)shooter;

    std::vector<Army> armies = rm::sim::freeForAll(2);
    std::vector<rm::sim::Economy> economies(2);
    std::vector<int> commandersEver(2, 0);
    std::vector<rm::sim::Projectile> projectiles;
    Match match = loneMatch(armies, economies, commandersEver);
    match.projectiles = &projectiles;

    const rm::sim::Mag before = roster.health(cargo).current;
    for (int i = 0; i < 200; ++i) {
        (void)rm::sim::tickSkirmish(roster.store, roster.catalog, match, terrain);
    }
    // The transport in the line of fire may take damage; its cargo must not.
    CHECK(roster.health(cargo).current == before);
}

// --- Persistence -----------------------------------------------------------------

TEST_CASE("a mid-ferry transport round-trips through save state", "[transport]") {
    const rm::HeightField field = flatField();
    const rm::sim::Terrain terrain{field};
    const rm::sim::PassabilityGrid grid =
        rm::sim::buildPassability(field, 0.0f, 60.0f, 0.0f);

    rm::test::Roster roster;
    const rm::UnitTypeIndex transportType = roster.addType(transportDef());
    const rm::UnitTypeIndex cargoType = roster.addType(cargoDef());
    const UnitId transport = landedTransport(roster, transportType, 30.0f, 30.0f);
    const UnitId cargo = roster.add(cargoType, 40.0f, 30.0f, 0, 500.0f);

    const std::vector<Player> players{Player{.index = 0, .army = 0}};
    std::vector<Army> armies = rm::sim::freeForAll(1);
    std::vector<rm::sim::Economy> economies(1);
    std::vector<int> commandersEver(1, 0);
    std::vector<const rm::sim::PassabilityGrid*> grids(
        roster.catalog.size(), &grid);
    const auto gridFor = [&grid](UnitId) { return &grid; };

    REQUIRE(rm::sim::applyCommand(
                dropIssue(transport, 80.0f, 80.0f, 1, CommandKind::Ferry),
                roster.store, roster.catalog, players, armies, terrain, gridFor,
                roster.rate)
                .accepted.size()
            == 1);
    REQUIRE(rm::sim::applyCommand(
                CommandIssue{.source = 0,
                             .id = rm::commandId(0, 2),
                             .player = 0,
                             .kind = CommandKind::Move,
                             .units = {cargo},
                             .targetX = rm::sim::fxFromFloat(30.0f),
                             .targetZ = rm::sim::fxFromFloat(30.0f)},
                roster.store, roster.catalog, players, armies, terrain, gridFor,
                roster.rate)
                .accepted.size()
            == 1);

    Match match = loneMatch(armies, economies, commandersEver, grids);
    for (int i = 0; i < 400 && !roster.motion(cargo).attached; ++i) {
        (void)rm::sim::tickSkirmish(roster.store, roster.catalog, match, terrain);
    }
    REQUIRE(roster.motion(cargo).attached);

    const std::vector<std::byte> bytes =
        rm::sim::SaveState::encode({.units = roster.store.snapshot()});
    const auto decoded = rm::sim::SaveState::decode(bytes);
    REQUIRE(decoded.has_value());
    // The attachment and the ferry's own execution state both survive the write.
    const std::size_t cargoSlot = static_cast<std::size_t>(cargo.index);
    CHECK(decoded->units.parents[cargoSlot].has_value());
    const auto& queue = decoded->units.orders[transport.index];
    REQUIRE(!queue.entries.empty());
    CHECK(queue.entries.front().execution.transportPhase
          != rm::sim::TransportPhase::None);
}

// --- Auto-embark (#15800) ------------------------------------------------------
//
// A move the grid cannot serve is not refused when an idle transport can lift the
// unit: the order is rewritten into a boarding run — the cargo walks a
// LoadTransport to the carrier, the carrier flies an UnloadTransport to the
// click, then ferries itself back to where it waited.

/// A flat field with a cliff down the middle: the two halves have no land route
/// between them, while the air above the wall is open. The cliff is a saw-tooth
/// ridge a cell wide — every square of cell 6 holds a steep face, so the cell
/// is wholly unwalkable. A plateau would not do: its flat top is walkable, and
/// a thinner wall only costs speed under P10.4 either way.
[[nodiscard]] rm::HeightField walledField() {
    rm::HeightField field = flatField();
    for (int z = 0; z < field.verticesZ(); ++z) {
        for (int x = 49; x <= 56; x += 2) {
            field.raw[static_cast<std::size_t>(z)
                          * static_cast<std::size_t>(field.verticesX())
                      + static_cast<std::size_t>(x)] = 4000;
        }
    }
    return field;
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

TEST_CASE("an unreachable move embarks the unit on an idle transport", "[transport]") {
    const rm::HeightField field = walledField();
    const rm::sim::Terrain terrain{field};
    const rm::sim::PassabilityGrid grid = rm::sim::buildPassability(field, -1000.0f);
    // The wall is real: the click has no land route.
    REQUIRE(rm::sim::findPath(grid, rm::sim::fxFromFloat(200.0f), rm::sim::fxFromFloat(400.0f),
                            rm::sim::fxFromFloat(600.0f), rm::sim::fxFromFloat(400.0f))
                .empty());

    rm::test::Roster roster;
    const rm::UnitTypeIndex transportType = roster.addType(transportDef());
    const rm::UnitTypeIndex cargoType = roster.addType(cargoDef());
    const UnitId transport = landedTransport(roster, transportType, 240.0f, 400.0f);
    const UnitId cargo = roster.add(cargoType, 200.0f, 400.0f, 0, 500.0f);

    const std::vector<Player> players{Player{.index = 0, .army = 0}};
    std::vector<Army> armies = rm::sim::freeForAll(1);
    std::vector<rm::sim::Economy> economies(1);
    std::vector<int> commandersEver(1, 0);
    std::vector<const rm::sim::PassabilityGrid*> grids(roster.catalog.size(), &grid);
    const auto gridFor = [&grid](UnitId) { return &grid; };

    const auto set = rm::sim::applyCommand(moveIssue(cargo, 600.0f, 400.0f, 1),
                                         roster.store, roster.catalog, players, armies,
                                         terrain, gridFor, roster.rate);
    REQUIRE(set.accepted.size() == 1);

    // The refusal became a boarding run: the unit owes the carrier a walk and
    // still owes the click; the carrier owes the click an unload and itself a
    // ride home.
    const auto& cargoQueue = roster.store.orders()[cargo.index].entries();
    REQUIRE(cargoQueue.size() == 2);
    CHECK(cargoQueue[0].kind() == CommandKind::LoadTransport);
    CHECK(cargoQueue[0].target() == transport);
    CHECK(cargoQueue[1].kind() == CommandKind::Move);
    const auto& carrierQueue = roster.store.orders()[transport.index].entries();
    REQUIRE(carrierQueue.size() == 2);
    CHECK(carrierQueue[0].kind() == CommandKind::UnloadTransport);
    CHECK(carrierQueue[1].kind() == CommandKind::Move);

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
    const rm::sim::Transform& at = roster.transform(cargo);
    CHECK(rm::sim::fxToFloat(at.x) == Catch::Approx(600.0f).margin(12.0f));
    CHECK(rm::sim::fxToFloat(at.z) == Catch::Approx(400.0f).margin(12.0f));

    // RTB: the carrier's appended Move flies it back to where it waited.
    for (int i = 0; i < 6000 && !roster.store.orders()[transport.index].empty(); ++i) {
        (void)rm::sim::tickSkirmish(roster.store, roster.catalog, match, terrain);
    }
    CHECK(roster.store.orders()[transport.index].empty());
    CHECK(rm::sim::fxToFloat(roster.transform(transport).x)
          == Catch::Approx(240.0f).margin(12.0f));
}

TEST_CASE("auto-embark follows the path service's refusal too", "[transport]") {
    const rm::HeightField field = walledField();
    const rm::sim::Terrain terrain{field};
    const rm::sim::PassabilityGrid grid = rm::sim::buildPassability(field, -1000.0f);

    rm::test::Roster roster;
    const rm::UnitTypeIndex transportType = roster.addType(transportDef());
    const rm::UnitTypeIndex cargoType = roster.addType(cargoDef());
    const UnitId transport = landedTransport(roster, transportType, 240.0f, 400.0f);
    const UnitId cargo = roster.add(cargoType, 200.0f, 400.0f, 0, 500.0f);
    (void)transport;

    const std::vector<Player> players{Player{.index = 0, .army = 0}};
    std::vector<Army> armies = rm::sim::freeForAll(1);
    std::vector<rm::sim::Economy> economies(1);
    std::vector<int> commandersEver(1, 0);
    std::vector<const rm::sim::PassabilityGrid*> grids(roster.catalog.size(), &grid);
    rm::sim::PathService paths;

    REQUIRE(rm::sim::applyCommand(moveIssue(cargo, 600.0f, 400.0f, 1), roster.store,
                                  roster.catalog, players, armies, terrain,
                                  [&grid](UnitId) { return &grid; }, roster.rate,
                                  nullptr, nullptr, nullptr, &paths)
                .accepted.size()
            == 1);

    // The deferred search retries before it admits defeat (C-176): the rewrite
    // lands only once the service publishes its empty route.
    Match match = loneMatch(armies, economies, commandersEver, grids);
    match.pathService = &paths;
    bool boarded = false;
    bool delivered = false;
    for (int i = 0; i < 6000 && !delivered; ++i) {
        (void)rm::sim::tickSkirmish(roster.store, roster.catalog, match, terrain);
        boarded = boarded || roster.motion(cargo).attached;
        delivered = boarded && !roster.motion(cargo).attached;
    }
    REQUIRE(boarded);
    REQUIRE(delivered);
    CHECK(rm::sim::fxToFloat(roster.transform(cargo).x)
          == Catch::Approx(600.0f).margin(12.0f));
}

TEST_CASE("a reachable move walks; no transport is borrowed", "[transport]") {
    const rm::HeightField field = flatField();
    const rm::sim::Terrain terrain{field};
    const rm::sim::PassabilityGrid grid = rm::sim::buildPassability(field, -1000.0f);

    rm::test::Roster roster;
    const rm::UnitTypeIndex transportType = roster.addType(transportDef());
    const rm::UnitTypeIndex cargoType = roster.addType(cargoDef());
    const UnitId transport = landedTransport(roster, transportType, 240.0f, 400.0f);
    const UnitId cargo = roster.add(cargoType, 200.0f, 400.0f, 0, 500.0f);

    const std::vector<Player> players{Player{.index = 0, .army = 0}};
    std::vector<Army> armies = rm::sim::freeForAll(1);
    std::vector<rm::sim::Economy> economies(1);
    std::vector<int> commandersEver(1, 0);
    std::vector<const rm::sim::PassabilityGrid*> grids(roster.catalog.size(), &grid);

    REQUIRE(rm::sim::applyCommand(moveIssue(cargo, 300.0f, 400.0f, 1), roster.store,
                                  roster.catalog, players, armies, terrain,
                                  [&grid](UnitId) { return &grid; }, roster.rate)
                .accepted.size()
            == 1);

    Match match = loneMatch(armies, economies, commandersEver, grids);
    for (int i = 0; i < 400; ++i) {
        (void)rm::sim::tickSkirmish(roster.store, roster.catalog, match, terrain);
    }

    CHECK(rm::sim::fxToFloat(roster.transform(cargo).x)
          == Catch::Approx(300.0f).margin(8.0f));
    CHECK_FALSE(roster.motion(cargo).attached);
    CHECK(roster.store.orders()[transport.index].empty());
    CHECK(rm::sim::fxToFloat(roster.transform(transport).x)
          == Catch::Approx(240.0f).margin(2.0f));
}

TEST_CASE("an unreachable move with no free carrier is still refused", "[transport]") {
    const rm::HeightField field = walledField();
    const rm::sim::Terrain terrain{field};
    const rm::sim::PassabilityGrid grid = rm::sim::buildPassability(field, -1000.0f);

    rm::test::Roster roster;
    const rm::UnitTypeIndex transportType = roster.addType(transportDef());
    const rm::UnitTypeIndex cargoType = roster.addType(cargoDef());
    const rm::UnitTypeIndex heavyType = roster.addType(cargoDef(4));
    // A carrier of the WRONG ARMY, and one already committed to a ferry loop.
    const UnitId theirs = landedTransport(roster, transportType, 240.0f, 400.0f, 1);
    const UnitId busy = landedTransport(roster, transportType, 250.0f, 400.0f);
    const UnitId cargo = roster.add(cargoType, 200.0f, 400.0f, 0, 500.0f);
    const UnitId heavy = roster.add(heavyType, 210.0f, 400.0f, 0, 500.0f);
    (void)theirs;

    const std::vector<Player> players{Player{.index = 0, .army = 0}};
    std::vector<Army> armies = rm::sim::freeForAll(2);
    std::vector<rm::sim::Economy> economies(2);
    std::vector<int> commandersEver(2, 0);
    std::vector<const rm::sim::PassabilityGrid*> grids(roster.catalog.size(), &grid);
    const auto gridFor = [&grid](UnitId) { return &grid; };

    REQUIRE(rm::sim::applyCommand(
                dropIssue(busy, 600.0f, 400.0f, 1, CommandKind::Ferry), roster.store,
                roster.catalog, players, armies, terrain, gridFor, roster.rate)
                .accepted.size()
            == 1);

    // A committed carrier is not poached, a foreign one is not borrowed, and a
    // class the hull cannot hold is not offered a ride.
    CHECK(rm::sim::applyCommand(moveIssue(cargo, 600.0f, 400.0f, 2), roster.store,
                                roster.catalog, players, armies, terrain, gridFor,
                                roster.rate)
              .accepted.empty());
    CHECK(roster.store.orders()[cargo.index].empty());
    CHECK(rm::sim::applyCommand(moveIssue(heavy, 600.0f, 400.0f, 3), roster.store,
                                roster.catalog, players, armies, terrain, gridFor,
                                roster.rate)
              .accepted.empty());
}

TEST_CASE("a transport's cargo dies with it", "[transport]") {
    // Retail scores the attached units at their remaining health when a loaded
    // transport is destroyed — the cargo dies aboard (`TransportUnitComponent`
    // dispersal, `engine-analysis/11-fa-sim-layer.md`). Nothing survives to
    // detach: a kill on the carrier cascades down the attachment tree.
    const rm::HeightField field = flatField();
    const rm::sim::Terrain terrain{field};

    rm::test::Roster roster;
    const rm::UnitTypeIndex transportType = roster.addType(transportDef());
    const rm::UnitTypeIndex cargoType = roster.addType(cargoDef());
    const UnitId transport = landedTransport(roster, transportType, 50.0f, 50.0f);
    const UnitId first = roster.add(cargoType, 51.0f, 50.0f, 0, 500.0f);
    const UnitId second = roster.add(cargoType, 52.0f, 50.0f, 0, 500.0f);
    REQUIRE(roster.store.attach(transport, first));
    REQUIRE(roster.store.attach(transport, second));

    roster.store.kill(transport);

    CHECK_FALSE(roster.store.alive(first));
    CHECK_FALSE(roster.store.alive(second));
    CHECK(roster.store.childrenOf(transport).empty());
}

// --- Retail validation ---------------------------------------------------------
//
// Everything above proves the mechanism on synthetic numbers; these prove the
// SHIPPED numbers drive it — a real C-6 Courier (UEA0107, `TransportClass = 10`,
// `Class2AttachSize = 2`, `Class3AttachSize = 4`) lifting real class-1 cargo
// (UEL0101, no `TransportClass` → the default), Pillars (UEL0202, class 2)
// and a UEF ACU (UEL0001, class 3 → four slots).
//
// `CANTRANSPORTCOMMANDER` is deliberately NOT modelled: every shipped carrier
// that can afford four slots also carries the flag, and the one that cannot
// (UEA0203's `ClassGenericUpTo = 2`) is excluded by the attach-cost table
// already — the flag would be dead enforcement on shipped content.

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

/// One shipped blueprint, read the way the content loader reads it.
[[nodiscard]] UnitDef realUnitDef(const std::filesystem::path& units,
                                  const char* id) {
    const auto loaded =
        rm::unitbp::loadFile(units / id / (std::string{id} + "_unit.bp"));
    REQUIRE(loaded.has_value());
    return *loaded;
}

/// Every shared piece of a lift scenario: one landed carrier plus the pieces
/// each section sends to board it.
struct RetailLift {
    rm::HeightField field = flatField();
    rm::sim::Terrain terrain{field};
    rm::sim::PassabilityGrid grid =
        rm::sim::buildPassability(field, 0.0f, 60.0f, 0.0f);
    rm::test::Roster roster;
    std::vector<Player> players{Player{.index = 0, .army = 0}};
    std::vector<Army> armies = rm::sim::freeForAll(1);
    std::vector<rm::sim::Economy> economies = std::vector<rm::sim::Economy>(1);
    // One army that never fielded a commander: `{1, 0}` would instead seed
    // army 0 with a past commander it does not have, and the Assassination
    // poll would declare it dead inside three seconds of tick one.
    std::vector<int> commandersEver = std::vector<int>(1, 0);
    std::vector<const rm::sim::PassabilityGrid*> grids;
    Match match;
    std::uint32_t serial = 1;

    RetailLift(const std::filesystem::path& units)
        : grids(roster.catalog.size(), nullptr), match(loneMatch(armies, economies,
                                                                commandersEver)) {
        const rm::UnitTypeIndex courier = roster.addType(realUnitDef(units, "UEA0107"));
        marineType = roster.addType(realUnitDef(units, "UEL0101"));
        pillarType = roster.addType(realUnitDef(units, "UEL0202"));
        acuType = roster.addType(realUnitDef(units, "UEL0001"));
        grids.assign(roster.catalog.size(), &grid);
        match.passability = grids;
        transport = landedTransport(roster, courier, 50.0f, 400.0f);
    }

    rm::UnitTypeIndex marineType{}, pillarType{}, acuType{};
    UnitId transport;

    [[nodiscard]] UnitId cargo(rm::UnitTypeIndex type, float x, float z) {
        return roster.add(type, x, z, 0, 500.0f);
    }

    /// Issues the board order; intake accepts it whether or not the carrier
    /// can still fit the unit — overflow waits on the pad.
    void board(UnitId unit) {
        REQUIRE(rm::sim::applyCommand(loadIssue(unit, transport, serial++),
                                    roster.store, roster.catalog, players, armies,
                                    terrain, [&](UnitId) { return &grid; },
                                    roster.rate)
                    .accepted.size()
                == 1);
    }

    /// Lets the boarders walk and climb until nobody new attaches.
    void settle(int ticks = 2000) {
        for (int i = 0; i < ticks; ++i) {
            (void)rm::sim::tickSkirmish(roster.store, roster.catalog, match, terrain);
        }
    }
};

TEST_CASE("a retail C-6 Courier fills on the shipped class costs",
          "[transport][retail]") {
    const std::filesystem::path units = corpusUnitsDir();
    if (units.empty()) {
        SKIP("the FA corpus is not extracted");
    }
    RetailLift lift{units};

    // The anchors themselves, so the capacity maths below is checked against
    // the blueprint rather than assumed: ten slots, class 2 costs two,
    // class 3 costs four.
    const UnitDef* courier = lift.roster.catalog.def(
        lift.roster.store.typeAt(lift.transport.index));
    REQUIRE(courier != nullptr);
    CHECK(courier->transportCapacity() == 10);
    CHECK(courier->transportAttachCost(1) == 1);
    CHECK(courier->transportAttachCost(2) == 2);
    CHECK(courier->transportAttachCost(3) == 4);
    CHECK(lift.roster.catalog.def(lift.marineType)->transportCargoClass() == 1);
    CHECK(lift.roster.catalog.def(lift.pillarType)->transportCargoClass() == 2);
    CHECK(lift.roster.catalog.def(lift.acuType)->transportCargoClass() == 3);

    SECTION("ten class-1 units fill it; the eleventh waits on the pad") {
        std::vector<UnitId> marines;
        for (int i = 0; i < 11; ++i) {
            // Spread on a 10-elmo grid so boarding is not also a congestion
            // test — the question is capacity, not the jam at the ramp.
            marines.push_back(lift.cargo(
                lift.marineType, 60.0f + 10.0f * static_cast<float>(i % 4),
                400.0f + 10.0f * static_cast<float>(i / 4)));
            lift.board(marines.back());
        }
        lift.settle();
        int attached = 0;
        for (const UnitId marine : marines) {
            attached += lift.roster.motion(marine).attached ? 1 : 0;
        }
        CHECK(attached == 10);
        CHECK(lift.roster.store.childrenOf(lift.transport).size() == 10);
    }

    SECTION("five Pillars fill it; the sixth waits on the pad") {
        std::vector<UnitId> pillars;
        for (int i = 0; i < 6; ++i) {
            pillars.push_back(lift.cargo(lift.pillarType, 60.0f + 10.0f * static_cast<float>(i),
                                         400.0f));
            lift.board(pillars.back());
        }
        lift.settle();
        int attached = 0;
        for (const UnitId pillar : pillars) {
            attached += lift.roster.motion(pillar).attached ? 1 : 0;
        }
        CHECK(attached == 5);
        CHECK(lift.roster.store.childrenOf(lift.transport).size() == 5);
    }

    SECTION("an ACU rides with six marines; the seventh waits") {
        const UnitId acu = lift.cargo(lift.acuType, 60.0f, 400.0f);
        lift.board(acu);
        std::vector<UnitId> marines;
        for (int i = 0; i < 7; ++i) {
            marines.push_back(lift.cargo(
                lift.marineType, 70.0f + 10.0f * static_cast<float>(i % 4),
                400.0f + 10.0f * static_cast<float>(i / 4)));
            lift.board(marines.back());
        }
        lift.settle();
        CHECK(lift.roster.motion(acu).attached);
        int attached = 0;
        for (const UnitId marine : marines) {
            attached += lift.roster.motion(marine).attached ? 1 : 0;
        }
        // 4 slots of ACU plus 6 of marine is exactly the ten the blueprint
        // states — the seventh marine found the bay full.
        CHECK(attached == 6);
        CHECK(lift.roster.store.childrenOf(lift.transport).size() == 7);
    }

    SECTION("a loaded courier flies to the drop and sets its marines down") {
        std::vector<UnitId> marines;
        for (int i = 0; i < 3; ++i) {
            marines.push_back(lift.cargo(lift.marineType, 60.0f + 10.0f * static_cast<float>(i),
                                         400.0f));
            lift.board(marines.back());
        }
        lift.settle(1200);
        for (const UnitId marine : marines) {
            REQUIRE(lift.roster.motion(marine).attached);
        }
        REQUIRE(rm::sim::applyCommand(
                    dropIssue(lift.transport, 500.0f, 400.0f, lift.serial++),
                    lift.roster.store, lift.roster.catalog, lift.players,
                    lift.armies, lift.terrain,
                    [&](UnitId) { return &lift.grid; }, lift.roster.rate)
                    .accepted.size()
                == 1);
        for (int i = 0; i < 4000; ++i) {
            (void)rm::sim::tickSkirmish(lift.roster.store, lift.roster.catalog,
                                        lift.match, lift.terrain);
            if (std::ranges::none_of(marines, [&](UnitId m) {
                    return lift.roster.motion(m).attached;
                })) {
                break;
            }
        }
        for (const UnitId marine : marines) {
            CHECK_FALSE(lift.roster.motion(marine).attached);
            const rm::sim::Transform& at = lift.roster.transform(marine);
            CHECK(rm::sim::fxToFloat(at.x) == Catch::Approx(500.0f).margin(16.0f));
        }
    }
}
