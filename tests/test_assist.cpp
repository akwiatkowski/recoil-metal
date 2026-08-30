// Lending a build arm: the Assist order, the combined rate, and the bill that grows with
// it. A rate-10 founder helped by a rate-10 assister advances a build at 2 units a tick at
// 10 Hz — and drains twice as fast, which is the decision assist puts in the player's
// hands: time bought with bank.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "core/sim/Assist.hpp"
#include "core/sim/Command.hpp"
#include "core/sim/Skirmish.hpp"
#include "core/sim/UnitStore.hpp"

#include "support/FxMatchers.hpp"
#include "support/TestRoster.hpp"

#include <cstdint>
#include <filesystem>
#include <vector>

using Catch::Approx;
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
    std::vector<int> commandersEver{0, 0};

    rm::UnitTypeIndex engineerType{};
    rm::UnitTypeIndex factoryType{};
    rm::UnitTypeIndex tankType{};
    rm::UnitTypeIndex hutType{};

    Fixture() {
        rm::unitdef::UnitDef engineer;
        engineer.name = "test_engineer";
        engineer.buildRate = 10.0f;  // 1 build unit per tick at 10 Hz
        engineer.buildableCategory = {{"TESTHUT"}};
        engineerType = roster.addType(engineer);

        rm::unitdef::UnitDef factory = engineer;
        factory.name = "test_factory";
        factory.categories = {"FACTORY"};
        factoryType = roster.addType(factory);

        rm::unitdef::UnitDef tank;
        tank.name = "test_tank";
        tankType = roster.addType(tank);

        rm::unitdef::UnitDef hut;
        hut.name = "test_hut";
        hut.categories = {"TESTHUT"};
        hut.buildCostMass = rm::sim::magFromFloat(100.0f);
        hut.buildCostEnergy = rm::sim::magFromFloat(100.0f);
        hut.buildTime = rm::sim::magFromFloat(100.0f);  // 100 ticks alone, 50 helped
        hutType = roster.addType(hut);
    }

    [[nodiscard]] bool apply(const Command& command) {
        return rm::sim::applyCommand(command, roster.store, roster.catalog, players, armies,
                                     terrain, grid, roster.rate, &building);
    }

    [[nodiscard]] bool build(UnitId who, float x, float z, bool queued = false) {
        return apply(Command{.kind = CommandKind::Build,
                             .queued = queued,
                             .unit = who,
                             .targetX = rm::sim::fxFromFloat(x),
                             .targetZ = rm::sim::fxFromFloat(z),
                             .buildType = hutType});
    }

    [[nodiscard]] bool move(UnitId who, float x, float z) {
        return apply(Command{.kind = CommandKind::Move,
                             .unit = who,
                             .targetX = rm::sim::fxFromFloat(x),
                             .targetZ = rm::sim::fxFromFloat(z)});
    }

    [[nodiscard]] bool assist(UnitId who, UnitId target, bool queued = false) {
        const rm::sim::Transform& at = roster.store.transforms()[target.index];
        return apply(Command{.kind = CommandKind::Assist,
                             .unit = who,
                             .targetX = at.x,
                             .targetZ = at.z,
                             .target = target,
                             .queued = queued});
    }

    void tick(int times = 1) {
        const std::vector<const rm::sim::PassabilityGrid*> grids(roster.catalog.size(), &grid);
        rm::sim::Match match{.armies = armies,
                             .economies = economies,
                             .projectiles = &shots,
                             .building = &building,
                             .passability = grids,
                             .commandersEver = commandersEver,
                             .baseStorage = {.mass = rm::sim::magFromFloat(10000.0f),
                                             .energy = rm::sim::magFromFloat(10000.0f)}};
        for (int i = 0; i < times; ++i) {
            (void)rm::sim::tickSkirmish(roster.store, roster.catalog, match, terrain,
                                        roster.rate);
        }
    }
};

} // namespace

TEST_CASE("an assisted build advances at the combined rate, and drains for it") {
    Fixture f;
    const UnitId founder = f.roster.add(f.engineerType, 200.0f, 200.0f, 0, 100.0f);
    const UnitId helper = f.roster.add(f.engineerType, 210.0f, 200.0f, 0, 100.0f);
    f.economies[0].stored = {.mass = rm::sim::magFromFloat(1000.0f),
                             .energy = rm::sim::magFromFloat(1000.0f)};

    REQUIRE(f.build(founder, 205.0f, 200.0f));
    REQUIRE(f.assist(helper, founder));

    // Ten ticks helped: 2 build units a tick, 20 of 100 done — where alone it would be 10.
    f.tick(10);
    REQUIRE(f.building.size() == 1);
    CHECK(rm::test::asFloat(f.building[0].buildTimeRemaining) == Approx(80.0f).margin(0.01));
    CHECK(rm::test::asFloat(f.building[0].assistPerTick) == Approx(1.0f).margin(0.001));
    // Q18 floors the per-tick cost share, leaving roughly 0.04 after ten drains; what matters
    // here is that the bank paid for twenty build units, not the founder's ten.
    CHECK(rm::test::asFloat(f.economies[0].stored.mass) == Approx(980.0f).margin(0.05));
    CHECK(rm::test::asFloat(f.economies[0].stored.energy) == Approx(980.0f).margin(0.05));

    // And the whole hut lands in ~50 ticks instead of 100.
    f.tick(41);
    CHECK(f.building[0].finished());
}

TEST_CASE("the final assisted tick requests every builder's full offered work") {
    rm::sim::Economy economy;
    economy.storage = {.mass = rm::sim::magFromFloat(1000.0f),
                       .energy = rm::sim::magFromFloat(1000.0f)};
    economy.stored = economy.storage;

    std::vector<rm::sim::Construction> building{
        rm::sim::Construction{
            .cost = {.mass = rm::sim::magFromFloat(100.0f),
                     .energy = rm::sim::magFromFloat(100.0f)},
            .totalBuildTime = rm::sim::magFromFloat(100.0f),
            .buildTimeRemaining = rm::sim::magFromFloat(1.0f),
            .buildPerTick = rm::sim::magFromFloat(1.0f),
            .assistPerTick = rm::sim::magFromFloat(2.0f),
        },
    };

    rm::sim::tickEconomy(economy, building);

    CHECK(building[0].finished());
    // Materialize clamps progress at completion, but retail discards the applied amount and
    // never reconciles it against the full 1 founder + 2 assister request.
    CHECK(rm::test::asFloat(economy.requestedLastTick.mass) == Approx(3.0f).margin(0.01));
    CHECK(rm::test::asFloat(economy.requestedLastTick.energy) == Approx(3.0f).margin(0.01));
    CHECK(rm::test::asFloat(economy.usageLastTick.mass) == Approx(3.0f).margin(0.01));
    CHECK(rm::test::asFloat(economy.usageLastTick.energy) == Approx(3.0f).margin(0.01));
    CHECK(rm::test::asFloat(economy.stored.mass) == Approx(997.0f).margin(0.01));
    CHECK(rm::test::asFloat(economy.stored.energy) == Approx(997.0f).margin(0.01));
}

TEST_CASE("final build progress scales the full request before clamping") {
    rm::sim::Economy economy;
    economy.storage = {.mass = rm::test::mag(1000.0f),
                       .energy = rm::test::mag(1000.0f)};
    // Half of the roughly three-resource completing request.
    economy.stored = {.mass = rm::test::mag(1.5f), .energy = rm::test::mag(1.5f)};

    std::vector<rm::sim::Construction> building{
        rm::sim::Construction{
            .cost = {.mass = rm::test::mag(100.0f),
                     .energy = rm::test::mag(100.0f)},
            .totalBuildTime = rm::test::mag(100.0f),
            .buildTimeRemaining = rm::test::mag(1.0f),
            .buildPerTick = rm::test::mag(1.0f),
            .assistPerTick = rm::test::mag(2.0f),
        },
    };

    rm::sim::tickEconomy(economy, building);

    CHECK(rm::test::asFloat(economy.requestedLastTick.mass) == Approx(3.0f).margin(0.01));
    CHECK(rm::test::asFloat(economy.usageLastTick.mass) == Approx(1.5f).margin(0.01));
    CHECK(rm::test::asFloat(economy.usageLastTick.energy) == Approx(1.5f).margin(0.01));
    CHECK(rm::test::asFloat(economy.fundedFraction) == Approx(0.5f).margin(0.01));
    CHECK(building[0].finished());
    CHECK(rm::test::asFloat(economy.stored.mass) == Approx(0.0f).margin(0.01));
    CHECK(rm::test::asFloat(economy.stored.energy) == Approx(0.0f).margin(0.01));
    const rm::sim::Resources afterCompletion = economy.stored;

    rm::sim::tickEconomy(economy, building);

    CHECK(economy.requestedLastTick.mass == rm::sim::Mag{});
    CHECK(economy.requestedLastTick.energy == rm::sim::Mag{});
    CHECK(economy.usageLastTick.mass == rm::sim::Mag{});
    CHECK(economy.usageLastTick.energy == rm::sim::Mag{});
    CHECK(economy.stored.mass == afterCompletion.mass);
    CHECK(economy.stored.energy == afterCompletion.energy);
}

TEST_CASE("a helper out of reach contributes nothing until it arrives") {
    Fixture f;
    const UnitId founder = f.roster.add(f.engineerType, 200.0f, 200.0f, 0, 100.0f);
    // 400 elmos away — far outside the 40-elmo default build reach.
    const UnitId helper = f.roster.add(f.engineerType, 600.0f, 200.0f, 0, 100.0f);
    f.economies[0].stored = {.mass = rm::sim::magFromFloat(1000.0f),
                             .energy = rm::sim::magFromFloat(1000.0f)};

    REQUIRE(f.build(founder, 205.0f, 200.0f));
    REQUIRE(f.assist(helper, founder));  // accepted: it routes over

    f.tick(5);
    REQUIRE(f.building.size() == 1);
    // No contribution from across the map; the founder works alone at 1 a tick.
    CHECK(rm::test::asFloat(f.building[0].assistPerTick) == 0.0f);
    CHECK(rm::test::asFloat(f.building[0].buildTimeRemaining) == Approx(95.0f).margin(0.01));

    // The standing order is a pursuit, not a proximity-only buff: after crossing the map the
    // helper enters build reach and starts contributing without another command.
    f.tick(45);
    CHECK(f.roster.transform(helper).x < rm::sim::fxFromFloat(600.0f));
    CHECK(rm::test::asFloat(f.building[0].assistPerTick) == Approx(1.0f).margin(0.001));
}

TEST_CASE("factory assist accelerates the oldest unfinished queue entry") {
    Fixture f;
    const UnitId factory = f.roster.add(f.factoryType, 200.0f, 200.0f, 0, 100.0f);
    const UnitId helper = f.roster.add(f.engineerType, 210.0f, 200.0f, 0, 100.0f);
    f.economies[0].stored = {.mass = rm::sim::magFromFloat(1000.0f),
                             .energy = rm::sim::magFromFloat(1000.0f)};

    // The second product is queued, not constructed in parallel. Assist follows the factory's
    // one active project and transfers when the queue advances.
    REQUIRE(f.build(factory, 205.0f, 200.0f));
    REQUIRE(f.build(factory, 240.0f, 200.0f, true));
    REQUIRE(f.assist(helper, factory));
    f.tick(1);

    REQUIRE(f.building.size() == 1);
    CHECK(rm::test::asFloat(f.building[0].buildTimeRemaining) == Approx(98.0f).margin(0.01));
    CHECK(rm::test::asFloat(f.building[0].assistPerTick) == Approx(1.0f).margin(0.001));

    // Once the first product finishes, the next tick starts the second and the same standing
    // assist order rolls onto it.
    f.building[0].buildTimeRemaining = rm::sim::magFromFloat(1.0f);
    f.tick(1);
    f.tick(1);
    REQUIRE(f.building.size() == 2);
    CHECK(rm::test::asFloat(f.building[1].buildTimeRemaining) == Approx(98.0f).margin(0.01));
    CHECK(rm::test::asFloat(f.building[1].assistPerTick) == Approx(1.0f).margin(0.001));
}

TEST_CASE("multiple helpers add their build rates to the same project") {
    Fixture f;
    const UnitId founder = f.roster.add(f.engineerType, 200.0f, 200.0f, 0, 100.0f);
    const UnitId first = f.roster.add(f.engineerType, 210.0f, 200.0f, 0, 100.0f);
    const UnitId second = f.roster.add(f.engineerType, 190.0f, 200.0f, 0, 100.0f);
    f.economies[0].stored = {.mass = rm::sim::magFromFloat(1000.0f),
                             .energy = rm::sim::magFromFloat(1000.0f)};

    REQUIRE(f.build(founder, 205.0f, 200.0f));
    REQUIRE(f.assist(first, founder));
    REQUIRE(f.assist(second, founder));
    f.tick(1);

    CHECK(rm::test::asFloat(f.building[0].assistPerTick) == Approx(2.0f).margin(0.001));
    CHECK(rm::test::asFloat(f.building[0].buildTimeRemaining) == Approx(97.0f).margin(0.01));
}

TEST_CASE("a queued assist starts after the order ahead of it finishes") {
    Fixture f;
    const UnitId founder = f.roster.add(f.engineerType, 200.0f, 200.0f, 0, 100.0f);
    const UnitId helper = f.roster.add(f.engineerType, 100.0f, 200.0f, 0, 100.0f);
    f.economies[0].stored = {.mass = rm::sim::magFromFloat(1000.0f),
                             .energy = rm::sim::magFromFloat(1000.0f)};

    REQUIRE(f.build(founder, 205.0f, 200.0f));
    REQUIRE(f.move(helper, 300.0f, 200.0f));
    REQUIRE(f.assist(helper, founder, true));
    REQUIRE(f.roster.store.orders()[helper.index].current() != nullptr);
    CHECK(f.roster.store.orders()[helper.index].current()->kind == CommandKind::Move);

    f.tick(45);
    REQUIRE(f.roster.store.orders()[helper.index].current() != nullptr);
    CHECK(f.roster.store.orders()[helper.index].current()->kind == CommandKind::Assist);
    CHECK(rm::test::asFloat(f.building[0].assistPerTick) == Approx(1.0f).margin(0.001));
}

TEST_CASE("an assist is a standing order: it waits through an idle queue and ends with its target") {
    Fixture f;
    const UnitId founder = f.roster.add(f.engineerType, 200.0f, 200.0f, 0, 100.0f);
    const UnitId helper = f.roster.add(f.engineerType, 210.0f, 200.0f, 0, 100.0f);
    f.economies[0].stored = {.mass = rm::sim::magFromFloat(1000.0f),
                             .energy = rm::sim::magFromFloat(1000.0f)};

    // Assist BEFORE any build exists: the order holds, nothing breaks, nothing finishes.
    REQUIRE(f.assist(helper, founder));
    f.tick(5);
    CHECK_FALSE(f.roster.store.orders()[helper.index].empty());

    // The founder starts working; the standing helper joins the same tick it can.
    REQUIRE(f.build(founder, 205.0f, 200.0f));
    f.tick(1);
    REQUIRE(f.building.size() == 1);
    CHECK(rm::test::asFloat(f.building[0].assistPerTick) == Approx(1.0f).margin(0.001));

    // The founder dies: help has nothing to attach to and the order retires.
    f.roster.health(founder).current = rm::sim::Mag{};
    f.tick(1);
    CHECK(rm::test::asFloat(f.building[0].assistPerTick) == 0.0f);
    CHECK_FALSE(f.roster.store.orders()[helper.index].empty());
    f.tick(1);
    CHECK(f.roster.store.orders()[helper.index].empty());
}

TEST_CASE("assist requires two friendly builders that are not the same unit") {
    Fixture f;
    const UnitId engineer = f.roster.add(f.engineerType, 200.0f, 200.0f, 0, 100.0f);
    const UnitId factory = f.roster.add(f.factoryType, 205.0f, 200.0f, 0, 100.0f);
    const UnitId tank = f.roster.add(f.tankType, 210.0f, 200.0f, 0, 100.0f);
    const UnitId theirs = f.roster.add(f.engineerType, 220.0f, 200.0f, 1, 100.0f);

    CHECK_FALSE(f.assist(factory, engineer));
    CHECK_FALSE(f.assist(tank, engineer));
    CHECK_FALSE(f.assist(engineer, tank));
    CHECK_FALSE(f.assist(engineer, engineer));
    CHECK_FALSE(f.assist(engineer, theirs));
}

TEST_CASE("a queued assist rejects a non-builder target before entering authoritative state") {
    Fixture f;
    const UnitId engineer = f.roster.add(f.engineerType, 200.0f, 200.0f, 0, 100.0f);
    const UnitId tank = f.roster.add(f.tankType, 210.0f, 200.0f, 0, 100.0f);

    CHECK_FALSE(f.assist(engineer, tank, true));
    CHECK(f.roster.store.orders()[engineer.index].empty());
}

TEST_CASE("an assist order survives the log round trip") {
    rm::sim::CommandLog log;
    log.record(Command{.tick = 3,
                       .player = 0,
                       .kind = CommandKind::Assist,
                       .unit = UnitId{1, 1},
                       .targetX = rm::sim::fxFromFloat(200.0f),
                       .targetZ = rm::sim::fxFromFloat(200.0f),
                       .target = UnitId{0, 1}});

    const auto path = std::filesystem::temp_directory_path() / "rm_assist_log_test.txt";
    REQUIRE(rm::sim::writeCommandLog(log, path.string()));
    const auto reread = rm::sim::readCommandLog(path.string());
    std::filesystem::remove(path);

    REQUIRE(reread.has_value());
    REQUIRE(reread->size() == 1);
    CHECK(reread->all()[0] == log.all()[0]);
}

TEST_CASE("shift-clicking the same assist target removes the queued order") {
    rm::sim::CommandQueue queue;
    const Command move{.kind = CommandKind::Move};
    const Command assist{.kind = CommandKind::Assist, .target = UnitId{3, 1}};

    CHECK(queue.give(move, false) == rm::sim::CommandQueue::Result::Replaced);
    CHECK(queue.give(assist, true) == rm::sim::CommandQueue::Result::Appended);
    CHECK(queue.give(assist, true) == rm::sim::CommandQueue::Result::Cancelled);
    REQUIRE(queue.current() != nullptr);
    CHECK(queue.current()->kind == CommandKind::Move);
}
