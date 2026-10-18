// Taking a hostile unit intact: the capture order, funded progress, and the transfer.
//
// The numbers are the retail progress contract (`docs/capture-implementation-spec.md`,
// `core/sim/Capture.hpp`): seconds = target buildTime / captor rate per second / 2,
// workTicks = seconds * 10 Hz, energy per beat = target build energy / workTicks.
// A rate-10 engineer taking a 100-work 200-energy structure works 50 ticks at 4
// energy a beat, and the arithmetic below is exact in fixed point.
#include <catch2/catch_test_macros.hpp>

#include "core/sim/Capture.hpp"
#include "core/sim/Command.hpp"
#include "core/sim/SaveState.hpp"
#include "core/sim/StateHash.hpp"
#include "core/sim/Skirmish.hpp"
#include "core/sim/UnitStore.hpp"

#include "support/FxMatchers.hpp"
#include "support/TestRoster.hpp"

#include <filesystem>
#include <optional>
#include <vector>

using rm::sim::Command;
using rm::sim::CommandKind;
using rm::sim::Player;
using rm::sim::UnitId;

namespace {

[[nodiscard]] rm::HeightField flatField() {
    rm::HeightField field;
    field.squaresX = 128;
    field.squaresZ = 128;
    field.baseHeight = 0.0f;
    field.heightScale = 1.0f;
    field.raw.assign(field.sampleCount(), std::uint16_t{0});
    return field;
}

/// Two armies, an engineer with capture capability, and funded economies.
struct Fixture {
    rm::HeightField field = flatField();
    rm::sim::Terrain terrain{field};
    rm::sim::PassabilityGrid grid = rm::sim::buildPassability(field, 0.0f);
    rm::test::Roster roster;
    std::vector<rm::sim::Army> armies = rm::sim::freeForAll(2);
    std::vector<Player> players = rm::sim::onePlayerPerArmy(2, /*humanArmy=*/0);
    std::vector<rm::sim::Economy> economies{2};
    std::vector<rm::sim::Projectile> shots;
    std::vector<rm::sim::Construction> building;
    std::vector<rm::sim::CaptureWork> captures;
    std::vector<int> commandersEver{0, 0};
    rm::sim::EventQueue events;

    rm::UnitTypeIndex captorType{};
    rm::UnitTypeIndex engineerType{};
    rm::UnitTypeIndex structureType{};
    rm::UnitTypeIndex commanderType{};

    Fixture() {
        rm::unitdef::UnitDef captor;
        captor.name = "test_captor";
        captor.buildRate = 10.0f; // 1 build unit per tick at 10 Hz
        captor.categories = {"CAPTURE"};
        captorType = roster.addType(captor);

        rm::unitdef::UnitDef engineer = captor;
        engineer.name = "test_engineer";
        engineer.categories = {};
        engineerType = roster.addType(engineer);

        // A T1 economy structure: 100 work units, 200 energy, the acceptance pair.
        rm::unitdef::UnitDef structure;
        structure.name = "test_extractor";
        structure.categories = {"LAND"};
        structure.buildCostMass = rm::sim::magFromFloat(100.0f);
        structure.buildCostEnergy = rm::sim::magFromFloat(200.0f);
        structure.buildTime = rm::sim::magFromFloat(100.0f);
        structureType = roster.addType(structure);

        rm::unitdef::UnitDef commander = structure;
        commander.name = "test_commander";
        commander.categories = {"COMMAND"};
        commanderType = roster.addType(commander);

        for (auto& economy : economies) {
            economy.storage = {.mass = rm::sim::magFromFloat(1000.0f),
                               .energy = rm::sim::magFromFloat(1000.0f)};
            economy.stored = {.mass = rm::sim::magFromFloat(1000.0f),
                              .energy = rm::sim::magFromFloat(1000.0f)};
        }
    }

    [[nodiscard]] bool capture(UnitId who, UnitId target) {
        const rm::sim::Transform& at = roster.store.transforms()[target.index];
        return rm::sim::applyCommand(Command{.kind = CommandKind::Capture,
                                             .unit = who,
                                             .targetX = at.x,
                                             .targetZ = at.z,
                                             .target = target},
                                     roster.store, roster.catalog, players, armies, terrain,
                                     grid, roster.rate, &building);
    }

    void tick(int times = 1) {
        const std::vector<const rm::sim::PassabilityGrid*> grids(roster.catalog.size(), &grid);
        rm::sim::Match match{.armies = armies,
                             .economies = economies,
                             .projectiles = &shots,
                             .building = &building,
                             .captures = &captures,
                             .events = &events,
                             .passability = grids,
                             .commandersEver = commandersEver,
                             .baseStorage = {.mass = rm::sim::magFromFloat(1000.0f),
                                             .energy = rm::sim::magFromFloat(1000.0f)}};
        for (int i = 0; i < times; ++i) {
            (void)rm::sim::tickSkirmish(roster.store, roster.catalog, match, terrain,
                                        roster.rate);
        }
    }
};

} // namespace

TEST_CASE("capture budgeting follows the retail progress contract", "[capture]") {
    // 100 work units at 10 per second, halved: 5 seconds, 50 ticks at 10 Hz.
    CHECK(rm::sim::captureWorkTicks(rm::sim::magFromFloat(100.0f),
                                    rm::sim::magFromFloat(1.0f), 10)
          == 50);
    // Retail Lua's `or 10` keeps an explicitly authored zero (URA0001); the
    // native `max(1, seconds * 10)` makes that a one-beat capture.
    CHECK(rm::sim::captureWorkTicks(rm::sim::Mag{},
                                    rm::sim::magFromFloat(1.0f), 10) == 1);
    CHECK(rm::sim::captureDemand(rm::sim::magFromFloat(200.0f), 50).energy
          == rm::sim::magFromFloat(4.0f));
    CHECK(rm::sim::captureDemand(rm::sim::magFromFloat(200.0f), 50).mass == rm::sim::Mag{});
}

TEST_CASE("capture distinguishes absent and authored zero BuildTime", "[capture]") {
    // `C-236`: URA0001 states BuildTime=0; Lua's `or 10` applies only when absent.
    for (const bool authored : {false, true}) {
        Fixture f;
        rm::unitdef::UnitDef targetDef = *f.roster.catalog.def(f.structureType);
        targetDef.buildTime = {};
        targetDef.buildTimeSpecified = authored;
        const auto type = f.roster.addType(targetDef);
        const UnitId captor = f.roster.add(f.captorType, 200.0f, 200.0f, 0, 100.0f);
        const UnitId target = f.roster.add(type, 206.0f, 200.0f, 1, 100.0f);
        REQUIRE(f.capture(captor, target));
        f.tick();
        CHECK(f.roster.store.alive(target) == !authored);
        if (!authored) {
            f.tick(4);
            CHECK_FALSE(f.roster.store.alive(target));
        }
    }
}

TEST_CASE("an engineer captures an enemy structure and the army changes", "[capture]") {
    Fixture f;
    const UnitId captor = f.roster.add(f.captorType, 200.0f, 200.0f, 0, 100.0f);
    const UnitId target = f.roster.add(f.structureType, 206.0f, 200.0f, 1, 100.0f);
    REQUIRE(f.capture(captor, target));
    REQUIRE(f.roster.store.orders()[captor.index].active() != nullptr);
    CHECK(f.roster.store.orders()[captor.index].active()->kind() == CommandKind::Capture);

    f.tick(10);
    REQUIRE(f.captures.size() == 1);
    CHECK(f.captures[0].progress == 10);
    CHECK(f.roster.store.alive(target));

    f.tick(40);
    // Transferred, not killed: the old handle is stale and a replacement of the
    // same type stands under the captor's army with its health intact.
    CHECK_FALSE(f.roster.store.alive(target));
    const UnitId replacement = f.roster.store.idAt(target.index);
    REQUIRE(f.roster.store.alive(replacement));
    CHECK(f.roster.store.typeAt(replacement.index) == f.structureType);
    CHECK(f.roster.store.motion()[replacement.index].armyIndex == 0);
    CHECK(f.roster.health(replacement).current == rm::sim::magFromFloat(100.0f));
    // Fifty beats at 4 energy: the whole 200 build-energy cost changed hands.
    CHECK(f.economies[0].stored.energy == rm::sim::magFromFloat(800.0f));

    // The captor's order retires on its stale target next dispatch.
    f.tick(1);
    CHECK(f.roster.store.orders()[captor.index].empty());
}

TEST_CASE("capture callbacks fire in retail's order", "[capture]") {
    // `C-237` (`0x0060B942`-`0x0060B968`, `0x0060B822`-`0x0060B870`): activation
    // is target `OnStartBeingCaptured` then captor `OnStartCapture`; completion
    // is captor `OnStopCapture`, target `OnStopBeingCaptured`, target
    // `OnCaptured` — and `OnCaptured`'s Lua body performs the transfer, so the
    // replacement's `UnitCreated` trails it.
    Fixture f;
    const UnitId captor = f.roster.add(f.captorType, 200.0f, 200.0f, 0, 100.0f);
    const UnitId target = f.roster.add(f.structureType, 206.0f, 200.0f, 1, 100.0f);
    REQUIRE(f.capture(captor, target));

    const auto kinds = [&f] {
        std::vector<rm::sim::EventKind> out;
        for (const rm::sim::Event& event : f.events.all()) {
            out.push_back(event.kind);
        }
        return out;
    };
    const auto position = [](const std::vector<rm::sim::EventKind>& all,
                             rm::sim::EventKind kind) {
        const auto found = std::ranges::find(all, kind);
        return found == all.end() ? all.size()
                                  : static_cast<std::size_t>(found - all.begin());
    };

    f.tick(60);  // the whole capture: reach, fifty funded beats, transfer

    const std::vector<rm::sim::EventKind> all = kinds();
    const std::size_t startBeing = position(all, rm::sim::EventKind::StartBeingCaptured);
    const std::size_t start = position(all, rm::sim::EventKind::StartCapture);
    const std::size_t stop = position(all, rm::sim::EventKind::StopCapture);
    const std::size_t stopBeing = position(all, rm::sim::EventKind::StopBeingCaptured);
    const std::size_t captured = position(all, rm::sim::EventKind::Captured);
    const std::size_t created = position(all, rm::sim::EventKind::UnitCreated);

    REQUIRE(startBeing < all.size());
    REQUIRE(start < all.size());
    REQUIRE(stop < all.size());
    REQUIRE(stopBeing < all.size());
    REQUIRE(captured < all.size());
    CHECK(startBeing < start);        // target first, then captor
    CHECK(stop < stopBeing);          // captor first at completion
    CHECK(stopBeing < captured);      // then the target's OnCaptured
    CHECK(captured < created);        // OnCaptured's body does the transfer
    CHECK(position(all, rm::sim::EventKind::FailedCapture) == all.size());
    CHECK(position(all, rm::sim::EventKind::FailedBeingCaptured) == all.size());

    // The events name their parties: the target-side kinds carry the target,
    // the captor-side kinds the captor, each with the other as instigator.
    for (const rm::sim::Event& event : f.events.all()) {
        if (event.kind == rm::sim::EventKind::StartBeingCaptured
            || event.kind == rm::sim::EventKind::StopBeingCaptured
            || event.kind == rm::sim::EventKind::Captured) {
            CHECK(event.unit == target);
            CHECK(event.instigator == captor);
        }
        if (event.kind == rm::sim::EventKind::StartCapture
            || event.kind == rm::sim::EventKind::StopCapture) {
            CHECK(event.unit == captor);
            CHECK(event.instigator == target);
        }
    }
}

TEST_CASE("a cancelled capture reports failure, not stop", "[capture]") {
    // `C-237` (spec §"Deactivation reports failure"): an active task ending on
    // a LIVE target fires `OnFailedBeingCaptured`/`OnFailedCapture` — the order
    // cancelled mid-capture is the ordinary case.
    Fixture f;
    const UnitId captor = f.roster.add(f.captorType, 200.0f, 200.0f, 0, 100.0f);
    const UnitId target = f.roster.add(f.structureType, 206.0f, 200.0f, 1, 100.0f);
    REQUIRE(f.capture(captor, target));

    f.tick(5);  // in reach and working: the start pair has fired
    REQUIRE(f.captures.size() == 1);
    REQUIRE(f.captures[0].inReach);

    // Cancel: a Move order replaces the Capture head.
    REQUIRE(rm::sim::applyCommand(
        Command{.kind = CommandKind::Move, .unit = captor,
                .targetX = rm::sim::fxFromFloat(400.0f),
                .targetZ = rm::sim::fxFromFloat(400.0f)},
        f.roster.store, f.roster.catalog, f.players, f.armies, f.terrain, f.grid,
        f.roster.rate, &f.building));
    f.tick(1);

    std::size_t failedBeing = 0;
    std::size_t failed = 0;
    for (const rm::sim::Event& event : f.events.all()) {
        failedBeing += event.kind == rm::sim::EventKind::FailedBeingCaptured ? 1 : 0;
        failed += event.kind == rm::sim::EventKind::FailedCapture ? 1 : 0;
    }
    CHECK(failedBeing == 1);
    CHECK(failed == 1);
    CHECK(f.captures.empty());
}

TEST_CASE("capture intake refuses the inadmissible", "[capture]") {
    Fixture f;
    const UnitId captor = f.roster.add(f.captorType, 200.0f, 200.0f, 0, 100.0f);
    const UnitId engineer = f.roster.add(f.engineerType, 210.0f, 200.0f, 0, 100.0f);
    const UnitId own = f.roster.add(f.structureType, 220.0f, 200.0f, 0, 100.0f);
    const UnitId enemy = f.roster.add(f.structureType, 230.0f, 200.0f, 1, 100.0f);
    const UnitId commander = f.roster.add(f.commanderType, 240.0f, 200.0f, 1, 100.0f);
    const UnitId aircraft = f.roster.add(f.structureType, 250.0f, 200.0f, 1, 100.0f);
    f.roster.store.motion()[aircraft.index].airborne = true;
    CHECK_FALSE(f.capture(engineer, enemy));   // no CAPTURE category
    CHECK_FALSE(f.capture(captor, own));       // own side
    CHECK_FALSE(f.capture(captor, captor));    // itself
    CHECK_FALSE(f.capture(captor, commander)); // commanders are immune
    CHECK_FALSE(f.capture(captor, aircraft));  // unreachable layer
    CHECK(f.capture(captor, enemy));
}

TEST_CASE("a capture prices an attached child's costs onto the parent's budget", "[capture]") {
    // `C-239` (`0x0060B521`-`0x0060B6AA`, `capture-implementation-spec.md`):
    // attached live units add their individual costs and tick budgets to the
    // captor's task, and the demands are divided by the total work budget.
    // `SimUtils.lua:58-62` only ever takes top-level units, so the child
    // itself is still refused its own task.
    Fixture f;
    const UnitId captor = f.roster.add(f.captorType, 200.0f, 200.0f, 0, 100.0f);
    const UnitId carrier = f.roster.add(f.structureType, 206.0f, 200.0f, 1, 100.0f);
    const UnitId cargo = f.roster.add(f.structureType, 206.0f, 200.0f, 1, 100.0f);
    REQUIRE(f.roster.store.attach(carrier, cargo));
    CHECK(f.capture(captor, carrier));     // top-level parent: admitted with priced children
    CHECK_FALSE(f.capture(captor, cargo)); // attached child: no budget of its own
    f.tick(3);
    REQUIRE(f.captures.size() == 1);
    // Parent (100 work, 200 energy) plus one identical child: 50 + 50 ticks
    // at 400 / 100 = 4 energy a beat — the same per-beat rate, twice as long.
    CHECK(f.captures[0].workTicks == 100);
    CHECK(rm::test::asFloat(f.captures[0].demand.energy) == Catch::Approx(4.0f).margin(0.01f));
    f.tick(120);
    // The transfer carried the attachment: both replacements stand under the
    // captor's army, the child still hung on its parent.
    CHECK_FALSE(f.roster.store.alive(carrier));
    const UnitId newCarrier = f.roster.store.idAt(carrier.index);
    const UnitId newCargo = f.roster.store.idAt(cargo.index);
    REQUIRE(f.roster.store.alive(newCarrier));
    REQUIRE(f.roster.store.alive(newCargo));
    CHECK(f.roster.store.motion()[newCarrier.index].armyIndex == 0);
    CHECK(f.roster.store.motion()[newCargo.index].armyIndex == 0);
    CHECK(f.roster.store.parentOf(newCargo) == newCarrier);
}

TEST_CASE("an unfunded capture holds its progress until the economy pays", "[capture]") {
    Fixture f;
    f.economies[0].stored.energy = rm::sim::Mag{};
    const UnitId captor = f.roster.add(f.captorType, 200.0f, 200.0f, 0, 100.0f);
    const UnitId target = f.roster.add(f.structureType, 206.0f, 200.0f, 1, 100.0f);
    REQUIRE(f.capture(captor, target));
    f.tick(10);
    REQUIRE(f.captures.size() == 1);
    f.economies[0].stored.energy = rm::sim::magFromFloat(1000.0f);
    f.tick(50);
    CHECK_FALSE(f.roster.store.alive(target));
    CHECK(f.roster.store.motion()[f.roster.store.idAt(target.index).index].armyIndex == 0);
}

TEST_CASE("a capture out of reach keeps its task but advances nothing", "[capture]") {
    Fixture f;
    const UnitId captor = f.roster.add(f.captorType, 200.0f, 200.0f, 0, 100.0f);
    const UnitId target = f.roster.add(f.structureType, 600.0f, 200.0f, 1, 100.0f);
    REQUIRE(f.capture(captor, target));
    f.tick(3);
    REQUIRE(f.captures.size() == 1);
    CHECK(f.captures[0].progress == 0);
    CHECK(f.captures[0].demand.energy == rm::sim::Mag{});
    CHECK_FALSE(f.roster.store.orders()[captor.index].empty());
    CHECK(f.roster.store.motion()[captor.index].moving); // still walking there
}

TEST_CASE("capture work admits inside the 10-ogrid edge while still closing", "[capture]") {
    // `C-250`: the funded task works out to a 10-ogrid footprint-edge gap, but
    // the approach only stops at 5 — a captor between the two keeps walking
    // AND banks progress, the way the retail task's move outlives admission.
    Fixture f;
    const UnitId captor = f.roster.add(f.captorType, 200.0f, 200.0f, 0, 100.0f);
    // Test defs carry no footprint, so the edge gap is the centre gap: 70
    // elmos is inside the 80-elmo work gate but outside the 40-elmo stop.
    const UnitId target = f.roster.add(f.structureType, 270.0f, 200.0f, 1, 100.0f);
    REQUIRE(f.capture(captor, target));
    f.tick(3);
    REQUIRE(f.captures.size() == 1);
    CHECK(f.captures[0].inReach);
    CHECK(f.captures[0].progress > 0);
    CHECK(f.captures[0].demand.energy > rm::sim::Mag{});
    CHECK(f.roster.store.motion()[captor.index].moving); // still closing to 5
}

TEST_CASE("capture footprint widens the edge gap against big targets", "[capture]") {
    // `C-250`: each side's larger footprint subtracts WHOLE from the centre
    // gap. A 2-square target (16 elmos) plus a 1-square captor (8) puts a
    // 100-elmo centre gap at a 76-elmo edge — inside work range where the
    // centre distance alone would refuse.
    Fixture f;
    rm::unitdef::UnitDef big = *f.roster.catalog.def(f.structureType);
    big.name = "test_big";
    big.footprintSquaresX = 2;
    big.footprintSquaresZ = 2;
    const auto bigType = f.roster.addType(big);
    rm::unitdef::UnitDef walker = *f.roster.catalog.def(f.captorType);
    walker.name = "test_walker";
    walker.footprintSquaresX = 1;
    walker.footprintSquaresZ = 1;
    const auto walkerType = f.roster.addType(walker);
    const UnitId captor = f.roster.add(walkerType, 200.0f, 200.0f, 0, 100.0f);
    const UnitId target = f.roster.add(bigType, 300.0f, 200.0f, 1, 100.0f);
    // Centre gap 100 elmos; edge 100 − 8 − 16 = 76 ≤ 80: in reach.
    CHECK(rm::sim::captureEdgeDistance(f.roster.catalog, walkerType, bigType,
                                       rm::sim::fxFromFloat(100.0f))
          == rm::sim::fxFromFloat(76.0f));
    REQUIRE(f.capture(captor, target));
    f.tick(3);
    REQUIRE(f.captures.size() == 1);
    CHECK(f.captures[0].inReach);
    CHECK(f.captures[0].progress > 0);
}

TEST_CASE("a capture outside the 10-ogrid edge advances nothing", "[capture]") {
    // The same footprint arithmetic, refused: a 200-elmo centre gap is a
    // 176-elmo edge — far past the 80-elmo work gate, and too far for the
    // captor to close inside three ticks.
    Fixture f;
    rm::unitdef::UnitDef big = *f.roster.catalog.def(f.structureType);
    big.name = "test_big";
    big.footprintSquaresX = 2;
    big.footprintSquaresZ = 2;
    const auto bigType = f.roster.addType(big);
    rm::unitdef::UnitDef walker = *f.roster.catalog.def(f.captorType);
    walker.name = "test_walker";
    walker.footprintSquaresX = 1;
    walker.footprintSquaresZ = 1;
    const auto walkerType = f.roster.addType(walker);
    const UnitId captor = f.roster.add(walkerType, 200.0f, 200.0f, 0, 100.0f);
    const UnitId target = f.roster.add(bigType, 400.0f, 200.0f, 1, 100.0f);
    REQUIRE(f.capture(captor, target));
    f.tick(3);
    REQUIRE(f.captures.size() == 1);
    CHECK_FALSE(f.captures[0].inReach);
    CHECK(f.captures[0].progress == 0);
    CHECK(f.captures[0].demand.energy == rm::sim::Mag{});
}

TEST_CASE("own guns spare a capture target", "[capture]") {
    Fixture f;
    rm::unitdef::UnitDef shooter = *f.roster.catalog.def(f.captorType);
    rm::unitdef::Weapon gun;
    gun.label = "spite gun";
    gun.role = rm::unitdef::WeaponRole::DirectFire;
    gun.targetPriorities = {{"LAND"}};
    gun.damage = rm::sim::magFromFloat(10.0f);
    gun.maxRange = rm::sim::fxFromFloat(300.0f);
    gun.rateOfFire = 1.0f;
    shooter.weapons.push_back(gun);
    const auto shooterType = f.roster.addType(shooter);
    (void)f.roster.add(shooterType, 200.0f, 200.0f, 0, 100.0f);
    const UnitId captor = f.roster.add(f.captorType, 212.0f, 200.0f, 0, 100.0f);
    const UnitId victim = f.roster.add(f.structureType, 206.0f, 200.0f, 1, 100.0f);
    const auto acquired = [&] {
        return rm::sim::nearestTarget(rm::test::at(200, 0, 200), 0, rm::UnitIndex{0}, gun,
                                      f.roster.store, f.armies, nullptr, &f.roster.catalog,
                                      std::nullopt, std::nullopt, nullptr,
                                      rm::sim::collectCaptureClaims(f.roster.store));
    };
    CHECK(acquired() == victim);
    REQUIRE(f.capture(captor, victim));
    CHECK_FALSE(acquired().has_value()); // C-157: taking, not shooting
}

TEST_CASE("a capture survives the log round trip", "[capture]") {
    rm::sim::CommandLog log;
    log.record(rm::sim::CommandIssue{
        .tick = 9,
        .source = 1,
        .id = rm::commandId(1, 0),
        .player = 1,
        .kind = CommandKind::Capture,
        .units = {UnitId{3, 2}},
        .targetX = rm::sim::fxFromFloat(210.0f),
        .targetZ = rm::sim::fxFromFloat(200.0f),
        .target = UnitId{5, 1},
    });
    const auto path = std::filesystem::temp_directory_path() / "rm_capture_log_test.txt";
    REQUIRE(rm::sim::writeCommandLog(log, path.string()));
    const auto reread = rm::sim::readCommandLog(path.string());
    std::filesystem::remove(path);
    REQUIRE(reread.has_value());
    REQUIRE(reread->size() == 1);
    CHECK(reread->all()[0] == log.all()[0]);
    CHECK(reread->all()[0].kind == CommandKind::Capture);
}

TEST_CASE("cancelling a capture drops its progress with no transfer", "[capture]") {
    Fixture f;
    const UnitId captor = f.roster.add(f.captorType, 200.0f, 200.0f, 0, 100.0f);
    const UnitId target = f.roster.add(f.structureType, 206.0f, 200.0f, 1, 100.0f);
    REQUIRE(f.capture(captor, target));
    f.tick(10);
    REQUIRE(f.captures.size() == 1);
    REQUIRE(f.captures[0].progress == 10);
    REQUIRE(rm::sim::applyCommand(Command{.kind = CommandKind::Stop, .unit = captor},
                                  f.roster.store, f.roster.catalog, f.players, f.armies,
                                  f.terrain, f.grid, f.roster.rate, &f.building));
    f.tick(50);
    // Nothing transferred and nothing lingers: the target stands under its own
    // army, the task is gone, and the captor has no order.
    CHECK(f.roster.store.alive(target));
    CHECK(f.roster.store.motion()[target.index].armyIndex == 1);
    CHECK(f.captures.empty());
    CHECK(f.roster.store.orders()[captor.index].empty());
}

TEST_CASE("a capture runs deterministically across identical matches", "[capture]") {
    // Twice-built, twice-ticked, one hash: funding, progress and the transfer
    // itself must replay exactly, which is what the state hash exists to catch.
    const auto run = [] {
        Fixture f;
        const UnitId captor = f.roster.add(f.captorType, 200.0f, 200.0f, 0, 100.0f);
        const UnitId target = f.roster.add(f.structureType, 206.0f, 200.0f, 1, 100.0f);
        if (!f.capture(captor, target)) {
            return std::optional<rm::StateHash>{};
        }
        f.tick(60);
        const std::vector<const rm::sim::PassabilityGrid*> grids(f.roster.catalog.size(),
                                                                 &f.grid);
        rm::sim::Match match{.armies = f.armies,
                             .economies = f.economies,
                             .projectiles = &f.shots,
                             .building = &f.building,
                             .captures = &f.captures,
                             .events = &f.events,
                             .passability = grids,
                             .commandersEver = f.commandersEver,
                             .baseStorage = {.mass = rm::sim::magFromFloat(1000.0f),
                                             .energy = rm::sim::magFromFloat(1000.0f)}};
        return std::optional<rm::StateHash>{hashMatch(f.roster.store, match)};
    };
    const auto first = run();
    const auto second = run();
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    CHECK(*first == *second);
}

TEST_CASE("captures survive save load", "[capture][save]") {
    using namespace rm::sim;
    SaveState state;
    state.captures = {{
        .armyIndex = 0,
        .captor = 4,
        .target = {.index = 9, .generation = 2},
        .workTicks = 50,
        .progress = 10,
        .demand = {.mass = Mag{}, .energy = Mag::fromInt(4)},
        .funded = kFxOne,
    }};
    const auto saved = SaveState::encode(state);
    const auto restored = SaveState::decode(saved);
    REQUIRE(restored);
    REQUIRE(restored->captures.size() == 1);
    const auto& resumed = restored->captures[0];
    CHECK(resumed.armyIndex == 0);
    CHECK(resumed.captor == 4);
    CHECK(resumed.target == state.captures[0].target);
    CHECK(resumed.workTicks == 50);
    CHECK(resumed.progress == 10);
    CHECK(resumed.demand.energy == Mag::fromInt(4));
    CHECK(resumed.funded == kFxOne);
    CHECK(SaveState::encode(*restored) == saved);
}

TEST_CASE("a capture keeps the target's enhancements and shield toggle", "[capture]") {
    // `SimUtils.lua:68-132` snapshots the installed enhancement names and the
    // shield on/off state before `ChangeUnitArmy`, then restores both on the
    // replacement — `CreateEnhancement` per name, `EnableShield`/`DisableShield`
    // for the toggle. Our replacement-entity transfer must carry the same two
    // pieces of live state or a captured enhanced unit silently reverts to
    // stock.
    Fixture f;
    // Give the structure type an installable enhancement so the map has a
    // real entry to carry over.
    rm::unitdef::UnitDef enhanced = *f.roster.catalog.def(f.structureType);
    const auto parameters = rm::lua::parseTable("{ NewBuildRate=30, NewHealth=20 }");
    REQUIRE(parameters);
    enhanced.enhancements.push_back(
        rm::unitdef::EnhancementSpec{.name = "AdvancedEngineering",
                                     .slot = "LCH",
                                     .parameters = *parameters});
    const rm::UnitTypeIndex enhancedType = f.roster.addType(enhanced);

    const UnitId captor = f.roster.add(f.captorType, 200.0f, 200.0f, 0, 100.0f);
    const UnitId target = f.roster.add(enhancedType, 206.0f, 200.0f, 1, 100.0f);
    f.roster.store.enhancements()[target.index]["LCH"] = "AdvancedEngineering";
    REQUIRE(f.roster.store.setScriptBitDisabled(target, 0, true));  // shield off

    REQUIRE(f.capture(captor, target));
    f.tick(50);

    CHECK_FALSE(f.roster.store.alive(target));
    const UnitId replacement = f.roster.store.idAt(target.index);
    REQUIRE(f.roster.store.alive(replacement));
    CHECK(f.roster.store.enhancements()[replacement.index].at("LCH")
          == "AdvancedEngineering");
    CHECK(f.roster.store.scriptBitDisabled(replacement, 0));
}
TEST_CASE("a capture keeps the target's remaining fuel", "[capture]") {
     // Motion (with `fuelRatio`) snapshots whole onto the replacement, so a
     // half-drained target must not refuel on transfer — the replacement
     // keeps the victim's remaining fuel, like shield health and veterancy.
     Fixture f;
     const UnitId captor = f.roster.add(f.captorType, 200.0f, 200.0f, 0, 100.0f);
     const UnitId target = f.roster.add(f.structureType, 206.0f, 200.0f, 1, 100.0f);
     f.roster.store.motion()[target.index].fuelRatio = rm::sim::fxFromFloat(0.5f);
     REQUIRE(f.capture(captor, target));
     f.tick(50);
     CHECK_FALSE(f.roster.store.alive(target));
     const UnitId replacement = f.roster.store.idAt(target.index);
     REQUIRE(f.roster.store.alive(replacement));
     CHECK(f.roster.store.motion()[replacement.index].fuelRatio == rm::sim::fxFromFloat(0.5f));
 }
TEST_CASE("a transfer refused by the captor's unit cap leaves the old unit and reports failure", "[capture]") {
    // Spec §"Failure" (`0x0074E421`): replacement-creation failure leaves
    // the ordinary unattached old target intact, cleans capture
    // funding/activation, and reports failure explicitly. The bounded
    // mechanism is retail's creation gate (`0x0074fda0`): the captor's army
    // is at its unit cap, so the replacement cannot exist.
    Fixture f;
    const UnitId captor = f.roster.add(f.captorType, 200.0f, 200.0f, 0, 100.0f);
    const UnitId target = f.roster.add(f.structureType, 206.0f, 200.0f, 1, 100.0f);
    REQUIRE(f.capture(captor, target));
    f.tick(10);
    REQUIRE(f.captures.size() == 1);
    REQUIRE(f.captures[0].progress == 10);
    // The captor's army is full: any replacement would exceed the cap.
    f.armies[0].unitCap = rm::sim::Fx::fromInt(0);
    f.tick(40);
    // The old target stands under its own army; the task is gone and the
    // captor's failure pair fired, not the completion triple.
    CHECK(f.roster.store.alive(target));
    CHECK(f.roster.store.motion()[target.index].armyIndex == 1);
    CHECK(f.captures.empty());
    std::size_t failed = 0;
    std::size_t failedBeing = 0;
    std::size_t captured = 0;
    for (const rm::sim::Event& event : f.events.all()) {
        failed += event.kind == rm::sim::EventKind::FailedCapture ? 1 : 0;
        failedBeing += event.kind == rm::sim::EventKind::FailedBeingCaptured ? 1 : 0;
        captured += event.kind == rm::sim::EventKind::Captured ? 1 : 0;
    }
    CHECK(failed == 1);
    CHECK(failedBeing == 1);
    CHECK(captured == 0);
}

TEST_CASE("a transfer rebuilds transport attachments and carries manipulator state", "[capture]") {
    // `C-513` (`0x0074e1fc`-`0x0074e3d8`): the native transfer rebuilds
    // transport attachments on the replacement — per stored child into
    // storage, per attach entry with its slot — and fires Lua
    // `OnTransportAttach` for each re-attached unit (`Unit.lua:3434`).
    // `C-240`/`C-404`: the two shared manipulator handles alias onto the
    // replacement (`0x006B2620`, `0x006414F0`), so its manipulator list and
    // bone-visibility mask ride the transfer, not just its health record.
    Fixture f;
    const UnitId captor = f.roster.add(f.captorType, 200.0f, 200.0f, 0, 100.0f);
    const UnitId carrier = f.roster.add(f.structureType, 206.0f, 200.0f, 1, 100.0f);
    const UnitId cargo = f.roster.add(f.structureType, 206.0f, 200.0f, 1, 100.0f);
    REQUIRE(f.roster.store.attach(carrier, cargo, {.parent = 3, .self = 5}));
    rm::sim::Manipulator slide;
    slide.kind = rm::sim::ManipulatorKind::StorageSlide;
    slide.slideBone = 7;
    REQUIRE(f.roster.store.addManipulator(carrier, slide));
    REQUIRE(f.roster.store.setBoneHidden(carrier, 9, true));
    REQUIRE(f.capture(captor, carrier));
    f.tick(120);
    CHECK_FALSE(f.roster.store.alive(carrier));
    const UnitId newCarrier = f.roster.store.idAt(carrier.index);
    const UnitId newCargo = f.roster.store.idAt(cargo.index);
    REQUIRE(f.roster.store.alive(newCarrier));
    REQUIRE(f.roster.store.alive(newCargo));
    // The attachment record is rebuilt with its bones, not as a bare link.
    CHECK(f.roster.store.parentOf(newCargo) == newCarrier);
    CHECK(f.roster.store.attachmentBonesOf(newCargo).parent == 3);
    CHECK(f.roster.store.attachmentBonesOf(newCargo).self == 5);
    // One `OnTransportAttach` per re-attached unit, naming child and carrier.
    std::size_t attached = 0;
    for (const rm::sim::Event& event : f.events.all()) {
        if (event.kind == rm::sim::EventKind::TransportAttach) {
            ++attached;
            CHECK(event.unit == newCargo);
            CHECK(event.instigator == newCarrier);
        }
    }
    CHECK(attached == 1);
    // The manipulator list and the bone mask aliased onto the replacement.
    REQUIRE(f.roster.store.manipulators()[newCarrier.index].size() == 1);
    CHECK(f.roster.store.manipulators()[newCarrier.index][0].kind
          == rm::sim::ManipulatorKind::StorageSlide);
    CHECK(f.roster.store.manipulators()[newCarrier.index][0].slideBone == 7);
    CHECK(f.roster.store.boneHiddenAt(newCarrier.index, 9));
}
TEST_CASE("a captured unit keeps its custom name on the replacement", "[capture]") {
    // `C-240` (`0x0074E1B1`-`0x0074E1CE`): the native transfer copies the
    // custom name (`Unit+0x2CC`) onto the replacement through the `+0x6C`/`+0x68`
    // getter/setter pair — a renamed unit keeps its name when captured.
    Fixture f;
    const UnitId captor = f.roster.add(f.captorType, 200.0f, 200.0f, 0, 100.0f);
    const UnitId target = f.roster.add(f.structureType, 206.0f, 200.0f, 1, 100.0f);
    REQUIRE(f.roster.store.setCustomName(target, "Bob"));
    REQUIRE(f.capture(captor, target));
    f.tick(120);
    CHECK_FALSE(f.roster.store.alive(target));
    const UnitId replacement = f.roster.store.idAt(target.index);
    REQUIRE(f.roster.store.alive(replacement));
    // The name is saved state too: a mid-transfer save keeps it across the
    // round trip (SaveState v62).
    const rm::sim::SaveState mid{.tick = 7, .units = f.roster.store.snapshot()};
    const auto bytes = rm::sim::SaveState::encode(mid);
    const auto back = rm::sim::SaveState::decode(bytes);
    REQUIRE(back);
    rm::sim::UnitStore reloaded(back->units);
    CHECK(reloaded.customName(reloaded.idAt(replacement.index)) == "Bob");
}
TEST_CASE("a captured silo keeps its queued missile builds on the replacement", "[capture]") {
    // `C-240`/`SimUtils.lua:68-132`: the transfer's Lua restores ammo onto the
    // replacement — the match-side records that keyed on the old handle follow
    // it. Stored rounds already remap (`SiloAmmo.owner`); the per-unit build
    // queue (`SiloBuild`, `CAiSiloBuildImpl+0x20`) must follow too, or a
    // captured silo with a missile in flight silently drops the queued build.
    Fixture f;
    const UnitId captor = f.roster.add(f.captorType, 200.0f, 200.0f, 0, 100.0f);
    const UnitId target = f.roster.add(f.structureType, 206.0f, 200.0f, 1, 100.0f);
    std::vector<rm::sim::SiloAmmo> siloAmmo{
        rm::sim::SiloAmmo{.owner = target,
                           .weapon = 0,
                           .slot = 1,
                           .capacity = 5,
                           .totalTicks = 100000,
                           .costPerTick = {.mass = rm::sim::Mag::fromInt(1)}}};
    std::vector<rm::sim::SiloBuild> siloQueue{
        rm::sim::SiloBuild{.owner = target, .slot = 1}};
    REQUIRE(f.capture(captor, target));
    const std::vector<const rm::sim::PassabilityGrid*> grids(f.roster.catalog.size(),
                                                             &f.grid);
    for (int tick = 0; tick < 120 && f.roster.store.alive(target); ++tick) {
        rm::sim::Match match{.armies = f.armies,
                             .economies = f.economies,
                             .projectiles = &f.shots,
                             .building = &f.building,
                             .captures = &f.captures,
                             .siloAmmo = &siloAmmo,
                             .siloQueue = &siloQueue,
                             .events = &f.events,
                             .passability = grids,
                             .commandersEver = f.commandersEver,
                             .baseStorage = {.mass = rm::sim::magFromFloat(1000.0f),
                                             .energy = rm::sim::magFromFloat(1000.0f)}};
        (void)rm::sim::tickSkirmish(f.roster.store, f.roster.catalog, match, f.terrain,
                                    f.roster.rate);
    }
    REQUIRE_FALSE(f.roster.store.alive(target));
    const UnitId replacement = f.roster.store.idAt(target.index);
    REQUIRE(f.roster.store.alive(replacement));
    REQUIRE(siloQueue.size() == 1);
    CHECK(siloQueue.front().owner == replacement);
}
TEST_CASE("a captured transport kills its non-capturable cargo", "[capture]") {
    // `C-385` (`Unit.lua:555-619`): a captured TRANSPORTATION unit kills each
    // non-capturable cargo item before the transfer — the uncapturable rider
    // dies, the capturable one rides the replacement.
    Fixture f;
    const UnitId captor = f.roster.add(f.captorType, 200.0f, 200.0f, 0, 100.0f);
    rm::unitdef::UnitDef carrierDef = *f.roster.catalog.def(f.structureType);
    carrierDef.name = "test_transport";
    carrierDef.categories = {"TRANSPORTATION"};
    const auto carrierType = f.roster.addType(carrierDef);
    const UnitId carrier = f.roster.add(carrierType, 206.0f, 200.0f, 1, 100.0f);
    const UnitId doomed = f.roster.add(f.structureType, 206.0f, 200.0f, 1, 100.0f);
    const UnitId kept = f.roster.add(f.structureType, 206.0f, 200.0f, 1, 100.0f);
    REQUIRE(f.roster.store.attach(carrier, doomed));
    REQUIRE(f.roster.store.attach(carrier, kept));
    REQUIRE(f.roster.store.setCapturable(doomed, false));
    REQUIRE(f.capture(captor, carrier));
    f.tick(300);
    REQUIRE_FALSE(f.roster.store.alive(carrier));
    // The doomed rider is dead — no live replacement stands in its slot —
    // while the capturable rider transferred onto the replacement's army.
    CHECK_FALSE(f.roster.store.alive(f.roster.store.idAt(doomed.index)));
    const UnitId newKept = f.roster.store.idAt(kept.index);
    REQUIRE(f.roster.store.alive(newKept));
    CHECK(f.roster.store.motion()[newKept.index].armyIndex == 0);
}

TEST_CASE("a unit marked uncapturable refuses capture", "[capture]") {
    // `C-250`'s per-tick preamble reads the `vt[0x40]` object `+0x69` byte
    // written by `Unit::SetCapturable` (`0x006D00B0`) — retail's transport
    // beacons (`defaultunits.lua:1165`) and stored cargo (`Unit.lua:3469`)
    // clear it. Intake and the working task must both honor the flag.
    Fixture f;
    const UnitId captor = f.roster.add(f.captorType, 200.0f, 200.0f, 0, 100.0f);
    const UnitId target = f.roster.add(f.structureType, 206.0f, 200.0f, 1, 100.0f);
    REQUIRE(f.roster.store.setCapturable(target, false));
    CHECK_FALSE(f.roster.store.capturable(target));
    CHECK_FALSE(f.capture(captor, target));
    // Re-enabling restores admission.
    REQUIRE(f.roster.store.setCapturable(target, true));
    CHECK(f.capture(captor, target));
}
