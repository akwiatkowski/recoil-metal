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
    CHECK(rm::sim::captureDemand(rm::sim::magFromFloat(200.0f), 50).energy
          == rm::sim::magFromFloat(4.0f));
    CHECK(rm::sim::captureDemand(rm::sim::magFromFloat(200.0f), 50).mass == rm::sim::Mag{});
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
        return rm::sim::nearestTarget(rm::test::at(200, 0, 200), 0, gun,
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
