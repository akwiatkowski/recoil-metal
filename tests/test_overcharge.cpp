// The commander's OverCharge: a manual weapon, an explicit order, and an energy bill.
//
// The numbers are UEL0001's own: Damage 12000, EnergyRequired 5000, MaxRadius 22 ogrids
// (176 elmos), ManualFire = true. One click is one shot — `fireOvercharge` forgets the
// order's target on firing, which is what retires it — and a short energy bar HOLDS the
// shot rather than refusing the click.
#include <catch2/catch_test_macros.hpp>

#include "core/sim/Combat.hpp"
#include "core/sim/Command.hpp"
#include "core/sim/Skirmish.hpp"
#include "core/sim/UnitStore.hpp"

#include "support/FxMatchers.hpp"
#include "support/TestRoster.hpp"

#include <cstdint>
#include <filesystem>
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

/// The commander's manual weapon, in the blueprint's own numbers.
[[nodiscard]] rm::unitdef::Weapon overchargeCannon() {
    rm::unitdef::Weapon cannon;
    cannon.label = "OverCharge";
    cannon.role = rm::unitdef::WeaponRole::DirectFire;
    cannon.damage = rm::sim::magFromFloat(12000.0f);
    cannon.damageRadius = rm::sim::fxFromFloat(20.0f);  // 2.5 ogrids
    cannon.maxRange = rm::sim::fxFromFloat(176.0f);     // 22 ogrids
    cannon.rateOfFire = 1.0f;
    cannon.muzzleVelocityElmosPerSecond = 200.0f;       // 25 ogrids/s
    cannon.turreted = true;
    cannon.manualFire = true;
    cannon.energyRequired = rm::sim::magFromFloat(5000.0f);
    return cannon;
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

    rm::UnitTypeIndex acuType{};
    rm::UnitTypeIndex tankType{};

    Fixture() {
        rm::unitdef::UnitDef acu;
        acu.name = "test_acu";
        acu.weapons.push_back(overchargeCannon());
        acuType = roster.addType(acu);

        rm::unitdef::UnitDef tank;
        tank.name = "test_tank";
        tankType = roster.addType(tank);
    }

    [[nodiscard]] bool overcharge(UnitId who, UnitId target) {
        const rm::sim::Transform& at = roster.store.transforms()[target.index];
        const Command command{.kind = CommandKind::Overcharge,
                              .unit = who,
                              .targetX = at.x,
                              .targetZ = at.z,
                              .target = target};
        return rm::sim::applyCommand(command, roster.store, roster.catalog, players, armies,
                                     terrain, grid, roster.rate, &building);
    }

    rm::sim::TickReport tick(int times = 1) {
        const std::vector<const rm::sim::PassabilityGrid*> grids(roster.catalog.size(), &grid);
        rm::sim::Match match{.armies = armies,
                             .economies = economies,
                             .projectiles = &shots,
                             .building = &building,
                             .passability = grids,
                             .commandersEver = commandersEver,
                             .baseStorage = {.mass = rm::sim::magFromFloat(1000.0f),
                                             .energy = rm::sim::magFromFloat(10000.0f)}};
        rm::sim::TickReport report;
        for (int i = 0; i < times; ++i) {
            report = rm::sim::tickSkirmish(roster.store, roster.catalog, match, terrain,
                                           roster.rate);
        }
        return report;
    }
};

} // namespace

TEST_CASE("an overcharge fires once, drains the store, and kills what it hits") {
    Fixture f;
    const UnitId acu = f.roster.add(f.acuType, 200.0f, 200.0f, 0, 10000.0f);
    const UnitId victim = f.roster.add(f.tankType, 230.0f, 200.0f, 1, 500.0f);
    f.economies[0].stored.energy = rm::sim::magFromFloat(6000.0f);

    REQUIRE(f.overcharge(acu, victim));

    // The shot leaves this tick — in range, energy banked — and the bill is paid the same
    // tick: 6000 - 5000.
    const rm::sim::TickReport first = f.tick(1);
    CHECK(first.shotsFired == 1);
    CHECK(rm::test::asFloat(f.economies[0].stored.energy) == 1000.0f);

    // 30 elmos at 20 an elmo-tick: the third tick lands it, 12000 into 500.
    f.tick(3);
    CHECK_FALSE(f.roster.store.alive(victim));

    // And the order is spent — one click, one shot.
    CHECK(f.roster.store.orders()[acu.index].empty());
}

TEST_CASE("a short energy bar holds the shot instead of refusing the click") {
    Fixture f;
    const UnitId acu = f.roster.add(f.acuType, 200.0f, 200.0f, 0, 10000.0f);
    const UnitId victim = f.roster.add(f.tankType, 230.0f, 200.0f, 1, 500.0f);
    f.economies[0].stored.energy = rm::sim::magFromFloat(3000.0f);

    REQUIRE(f.overcharge(acu, victim));

    // Five ticks on 3000 energy: nothing fires, nothing drains, the order waits.
    rm::sim::TickReport report = f.tick(5);
    CHECK(report.shotsFired == 0);
    CHECK(rm::test::asFloat(f.economies[0].stored.energy) == 3000.0f);
    CHECK_FALSE(f.roster.store.orders()[acu.index].empty());

    // The store fills; the held shot leaves on the next tick.
    f.economies[0].stored.energy = rm::sim::magFromFloat(5000.0f);
    report = f.tick(1);
    CHECK(report.shotsFired == 1);
    CHECK(rm::test::asFloat(f.economies[0].stored.energy) == 0.0f);
}

TEST_CASE("an out-of-range overcharge pursues its enemy instead of becoming an assist") {
    Fixture f;
    const UnitId acu = f.roster.add(f.acuType, 200.0f, 200.0f, 0, 10000.0f);
    const UnitId victim = f.roster.add(f.tankType, 500.0f, 200.0f, 1, 500.0f);

    REQUIRE(f.overcharge(acu, victim));
    CHECK(f.roster.store.motion()[acu.index].moving);
    REQUIRE(f.roster.store.orders()[acu.index].current() != nullptr);
    CHECK(f.roster.store.orders()[acu.index].current()->kind() == CommandKind::Overcharge);
}

TEST_CASE("one order is one shot, even into something that survives it") {
    Fixture f;
    const UnitId acu = f.roster.add(f.acuType, 200.0f, 200.0f, 0, 10000.0f);
    const UnitId fortress = f.roster.add(f.tankType, 230.0f, 200.0f, 1, 50000.0f);
    f.economies[0].stored.energy = rm::sim::magFromFloat(10000.0f);

    REQUIRE(f.overcharge(acu, fortress));

    // Twenty ticks is two full reloads past the shot: the target stands, wounded once,
    // and exactly one bill was paid — the order retired with the first shot.
    f.tick(20);
    CHECK(f.roster.store.alive(fortress));
    CHECK(rm::test::asFloat(f.roster.store.health()[fortress.index].current) == 38000.0f);
    CHECK(rm::test::asFloat(f.economies[0].stored.energy) == 5000.0f);
    CHECK(f.roster.store.orders()[acu.index].empty());
}

TEST_CASE("a unit with no manual weapon cannot be asked to overcharge") {
    Fixture f;
    const UnitId tank = f.roster.add(f.tankType, 200.0f, 200.0f, 0, 500.0f);
    const UnitId victim = f.roster.add(f.tankType, 230.0f, 200.0f, 1, 500.0f);
    CHECK_FALSE(f.overcharge(tank, victim));
}

TEST_CASE("an overcharge order survives the log round trip") {
    rm::sim::CommandLog log;
    log.record(rm::sim::CommandIssue{
        .tick = 12,
        .source = 0,
        .id = rm::commandId(0, 0),
        .player = 0,
        .kind = CommandKind::Overcharge,
        .units = {UnitId{1, 1}},
        .targetX = rm::sim::fxFromFloat(230.0f),
        .targetZ = rm::sim::fxFromFloat(200.0f),
        .target = UnitId{2, 1},
    });

    const auto path = std::filesystem::temp_directory_path() / "rm_overcharge_log_test.txt";
    REQUIRE(rm::sim::writeCommandLog(log, path.string()));
    const auto reread = rm::sim::readCommandLog(path.string());
    std::filesystem::remove(path);

    REQUIRE(reread.has_value());
    REQUIRE(reread->size() == 1);
    CHECK(reread->all()[0] == log.all()[0]);
}
