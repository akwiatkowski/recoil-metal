// A silo's missile: a counted manual weapon, an explicit launch order, one round per click.
//
// The numbers are URB2108's own: the Loa tactical missile is `CountedProjectile`,
// `ManualFire = true`, `MaxProjectileStorage = 10`, `MinRadius = 15` ogrids of dead zone,
// `MaxRadius = 256` ogrids of reach. The order is `CommandKind::MissileLaunch` — a position
// or a hostile unit — and it differs from Overcharge in what gates the trigger: not energy
// but a missile sitting in the tube. An empty silo HOLDS the order rather than refusing the
// click, because the build may still be running; `fireMissiles` spends one `SiloAmmo` the
// tick the shot leaves and marks the order spent on the launcher itself, which is what
// retires it — the same trick `fireOvercharge` plays with a cleared target, in a shape that
// also works for a ground zero.
#include <catch2/catch_test_macros.hpp>

#include "core/sim/Combat.hpp"
#include "core/sim/Command.hpp"
#include "core/sim/Economy.hpp"
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

/// The Loa, in the blueprint's own numbers.
[[nodiscard]] rm::unitdef::Weapon cruiseMissile() {
    rm::unitdef::Weapon missile;
    missile.label = "CruiseMissile";
    missile.role = rm::unitdef::WeaponRole::Artillery;
    missile.damage = rm::sim::magFromFloat(6000.0f);
    missile.damageRadius = rm::sim::fxFromFloat(32.0f);  // 2 ogrids
    missile.maxRange = rm::sim::fxFromFloat(4096.0f);    // 256 ogrids
    missile.minRange = rm::sim::fxFromFloat(240.0f);     // 15 ogrids of dead zone
    missile.rateOfFire = 3.0f;
    missile.muzzleVelocityElmosPerSecond = 160.0f;       // 10 ogrids/s
    missile.manualFire = true;
    missile.countedProjectile = true;
    missile.maxProjectileStorage = 10;
    missile.projectileId = "/projectiles/CIFMissileTactical03/CIFMissileTactical03_proj.bp";
    return missile;
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
    std::vector<rm::sim::SiloAmmo> siloAmmo;
    std::vector<int> commandersEver{0, 0};

    rm::UnitTypeIndex siloType{};
    rm::UnitTypeIndex tankType{};
    rm::UnitTypeIndex mobileLauncherType{};

    Fixture() {
        rm::unitdef::UnitDef silo;
        silo.name = "test_silo";
        silo.weapons.push_back(cruiseMissile());
        siloType = roster.addType(silo);

        rm::unitdef::UnitDef tank;
        tank.name = "test_tank";
        tankType = roster.addType(tank);

        // A launcher on legs — the MML case. Its missile is short-ranged (400 elmos) so a
        // target can sit out of reach yet still on the test map: the long Loa envelope
        // would put every out-of-range click past the edge, where routing gives up.
        rm::unitdef::UnitDef mml;
        mml.name = "test_mml";
        mml.categories = {"LAND"};
        mml.speedElmosPerSecond = 30.0f;
        mml.turnRateRadiansPerSecond = 100.0f;
        rm::unitdef::Weapon shortMissile = cruiseMissile();
        shortMissile.maxRange = rm::sim::fxFromFloat(400.0f);
        shortMissile.minRange = rm::sim::Fx{};
        mml.weapons.push_back(shortMissile);
        mobileLauncherType = roster.addType(mml);
    }

    /// A launch order at a point on the map.
    [[nodiscard]] bool launchAt(UnitId who, float x, float z) {
        const Command command{.kind = CommandKind::MissileLaunch,
                              .unit = who,
                              .targetX = rm::sim::fxFromFloat(x),
                              .targetZ = rm::sim::fxFromFloat(z)};
        return rm::sim::applyCommand(command, roster.store, roster.catalog, players, armies,
                                     terrain, grid, roster.rate, &building);
    }

    /// A launch order at a hostile unit.
    [[nodiscard]] bool launchAt(UnitId who, UnitId target) {
        const rm::sim::Transform& at = roster.store.transforms()[target.index];
        const Command command{.kind = CommandKind::MissileLaunch,
                              .unit = who,
                              .targetX = at.x,
                              .targetZ = at.z,
                              .target = target};
        return rm::sim::applyCommand(command, roster.store, roster.catalog, players, armies,
                                     terrain, grid, roster.rate, &building);
    }

    void loadSilo(UnitId who, int stored, int capacity = 10) {
        siloAmmo.push_back(rm::sim::SiloAmmo{.owner = who,
                                             .weapon = 0,
                                             .slot = 0,
                                             .stored = stored,
                                             .capacity = capacity});
    }

    [[nodiscard]] int stored(UnitId who) const {
        for (const rm::sim::SiloAmmo& ammo : siloAmmo) {
            if (ammo.owner == who) return ammo.stored;
        }
        return -1;
    }

    rm::sim::TickReport tick(int times = 1) {
        const std::vector<const rm::sim::PassabilityGrid*> grids(roster.catalog.size(), &grid);
        rm::sim::Match match{.armies = armies,
                             .economies = economies,
                             .projectiles = &shots,
                             .building = &building,
                             .siloAmmo = &siloAmmo,
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

TEST_CASE("a loaded silo launches one missile at a ground position and the order completes") {
    Fixture f;
    const UnitId silo = f.roster.add(f.siloType, 200.0f, 200.0f, 0, 2000.0f);
    f.loadSilo(silo, 3);

    REQUIRE(f.launchAt(silo, 800.0f, 800.0f));

    const rm::sim::TickReport first = f.tick(1);
    CHECK(first.shotsFired == 1);
    REQUIRE(f.shots.size() == 1);
    CHECK(f.stored(silo) == 2);  // the round left the tube

    // The order is spent: one click is one missile, and the queue is empty again.
    f.tick(5);
    CHECK(f.roster.store.orders()[silo.index].empty());
}

TEST_CASE("an empty silo holds the launch order until a missile exists") {
    Fixture f;
    const UnitId silo = f.roster.add(f.siloType, 200.0f, 200.0f, 0, 2000.0f);
    f.loadSilo(silo, 0);

    REQUIRE(f.launchAt(silo, 800.0f, 800.0f));

    rm::sim::TickReport report = f.tick(5);
    CHECK(report.shotsFired == 0);
    CHECK(f.shots.empty());
    CHECK_FALSE(f.roster.store.orders()[silo.index].empty());  // still waiting on the build

    // A missile arrives: the held order fires on the next tick.
    f.siloAmmo.front().stored = 1;
    report = f.tick(1);
    CHECK(report.shotsFired == 1);
    CHECK(f.stored(silo) == 0);
}

TEST_CASE("a launch order at a hostile unit fires once at where it stands") {
    Fixture f;
    const UnitId silo = f.roster.add(f.siloType, 200.0f, 200.0f, 0, 2000.0f);
    const UnitId victim = f.roster.add(f.tankType, 800.0f, 800.0f, 1, 50000.0f);
    f.loadSilo(silo, 1);

    REQUIRE(f.launchAt(silo, victim));

    const rm::sim::TickReport first = f.tick(1);
    CHECK(first.shotsFired == 1);
    REQUIRE(f.shots.size() == 1);
    CHECK(f.stored(silo) == 0);

    f.tick(5);
    CHECK(f.roster.store.orders()[silo.index].empty());
}

TEST_CASE("a launch order at a dead target retires without firing") {
    Fixture f;
    const UnitId silo = f.roster.add(f.siloType, 200.0f, 200.0f, 0, 2000.0f);
    const UnitId victim = f.roster.add(f.tankType, 800.0f, 800.0f, 1, 10.0f);
    f.loadSilo(silo, 2);

    REQUIRE(f.launchAt(silo, victim));
    f.roster.store.kill(victim);  // dead before the first tick gets there

    f.tick(3);
    CHECK(f.stored(silo) == 2);           // nothing fired at the corpse
    CHECK(f.roster.store.orders()[silo.index].empty());  // and the order is gone
}

TEST_CASE("a unit with no counted manual weapon cannot be asked to launch") {
    Fixture f;
    const UnitId tank = f.roster.add(f.tankType, 200.0f, 200.0f, 0, 500.0f);
    CHECK_FALSE(f.launchAt(tank, 800.0f, 800.0f));
}

TEST_CASE("a target inside the dead zone cannot be fired at") {
    Fixture f;
    const UnitId silo = f.roster.add(f.siloType, 200.0f, 200.0f, 0, 2000.0f);
    f.loadSilo(silo, 1);

    // 100 elmos out — inside the 240-elmo MinRadius. The click is accepted (the silo will
    // happily hold it) but the missile cannot leave: the order holds, unfired.
    REQUIRE(f.launchAt(silo, 240.0f, 200.0f));
    const rm::sim::TickReport report = f.tick(5);
    CHECK(report.shotsFired == 0);
    CHECK(f.stored(silo) == 1);
}

TEST_CASE("a mobile launcher closes on a ground zero until it is in range") {
    Fixture f;
    const UnitId mml = f.roster.add(f.mobileLauncherType, 200.0f, 200.0f, 0, 2000.0f);
    f.loadSilo(mml, 1);

    // 1000 elmos out — past the 400-elmo reach but well inside the map. The order is
    // accepted and the launcher walks toward it rather than refusing or firing anyway.
    REQUIRE(f.launchAt(mml, 1200.0f, 200.0f));

    const rm::sim::Fx startX = f.roster.store.transforms()[mml.index].x;
    rm::sim::TickReport report = f.tick(20);
    CHECK(report.shotsFired == 0);  // still out of reach
    CHECK(f.roster.store.transforms()[mml.index].x > startX);  // but closing

    // March until the shot goes: ~600 elmos at 30 elmos/s is 3 elmos/tick, so a few
    // hundred ticks; bound it generously and assert the round left once in range.
    std::size_t fired = 0;
    for (int i = 0; i < 2000 && fired == 0; ++i) {
        fired = f.tick(1).shotsFired;
    }
    CHECK(fired == 1);
    CHECK(f.stored(mml) == 0);
    // It stopped inside the envelope: the shot went while maxRange still covered it.
    const rm::sim::Fx dx = f.roster.store.transforms()[mml.index].x
                         - rm::sim::fxFromFloat(1200.0f);
    CHECK(rm::sim::fxToFloat(dx > rm::sim::Fx{} ? dx : -dx) <= 400.0f);
}

TEST_CASE("a missile order survives the log round trip") {
    rm::sim::CommandLog log;
    log.record(rm::sim::CommandIssue{
        .tick = 12,
        .source = 0,
        .id = rm::commandId(0, 0),
        .player = 0,
        .kind = CommandKind::MissileLaunch,
        .units = {UnitId{1, 1}},
        .targetX = rm::sim::fxFromFloat(800.0f),
        .targetZ = rm::sim::fxFromFloat(800.0f),
    });

    const auto path = std::filesystem::temp_directory_path() / "rm_missile_log_test.txt";
    REQUIRE(rm::sim::writeCommandLog(log, path.string()));
    const auto reread = rm::sim::readCommandLog(path.string());
    std::filesystem::remove(path);

    REQUIRE(reread.has_value());
    REQUIRE(reread->size() == 1);
    CHECK(reread->all()[0] == log.all()[0]);
}
