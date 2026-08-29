// Standing beside things pays: the skirt geometry, the buff tables, and the income they
// change. Numbers are the game's own (`AdjacencyBuffs.lua` via `core/unit/Adjacency.hpp`): a
// mass storage adds +12.5% to a SIZE4 extractor, an energy storage **+12.5%** to a SIZE4
// generator, a T1 generator -6.25% to a SIZE4 neighbour's upkeep.
//
// That energy figure used to read +25% here and in the table, which was every value in the
// row doubled — a full ring paid +100% where retail pays +50%. An audit against the shipped
// buff file caught it (`C-072`). The giveaway is the invariant the whole table is built on:
// `Add x n` must be constant across the size rows, and the doubled row broke it.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "core/sim/Adjacency.hpp"
#include "core/sim/Skirmish.hpp"
#include "core/unit/Adjacency.hpp"

#include "support/FxMatchers.hpp"
#include "support/TestRoster.hpp"

#include <cstdint>
#include <vector>

using Catch::Approx;
using rm::sim::Fx;
using rm::sim::UnitId;
using rm::unitdef::AdjacencyClass;

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

/// A 2x2-skirt SIZE4 structure — the shape every T1 economy building shares.
[[nodiscard]] rm::unitdef::UnitDef smallStructure(std::string name) {
    rm::unitdef::UnitDef def;
    def.name = std::move(name);
    def.motion = rm::unitdef::MotionType::None;
    def.skirtSquaresX = 2.0f;
    def.skirtSquaresZ = 2.0f;
    def.categories = {"SIZE4"};
    return def;
}

struct Fixture {
    rm::HeightField field = flatField();
    rm::sim::Terrain terrain{field};
    rm::test::Roster roster;
    std::vector<rm::sim::Army> armies = rm::sim::freeForAll(2);
    std::vector<rm::sim::Economy> economies{2};
    std::vector<rm::sim::Projectile> shots;
    std::vector<int> commandersEver{0, 0};

    void tick() {
        rm::sim::Match match{.armies = armies,
                             .economies = economies,
                             .projectiles = &shots,
                             .commandersEver = commandersEver};
        (void)rm::sim::tickSkirmish(roster.store, roster.catalog, match, terrain,
                                    roster.rate);
    }
};

} // namespace

TEST_CASE("skirts share an edge, not a corner, and free placement gets its slack") {
    const Fx eight = Fx::fromInt(8);  // a 2x2-ogrid skirt's half is 8 elmos

    // Side by side, skirts exactly abutting: 16 elmos apart.
    CHECK(rm::sim::skirtsShareEdge(Fx::fromInt(100), Fx::fromInt(100), eight, eight,
                                   Fx::fromInt(116), Fx::fromInt(100), eight, eight));
    // Within the half-ogrid slack free placement needs.
    CHECK(rm::sim::skirtsShareEdge(Fx::fromInt(100), Fx::fromInt(100), eight, eight,
                                   Fx::fromInt(119), Fx::fromInt(100), eight, eight));
    // A lane a tank drives through is not adjacency.
    CHECK_FALSE(rm::sim::skirtsShareEdge(Fx::fromInt(100), Fx::fromInt(100), eight, eight,
                                         Fx::fromInt(126), Fx::fromInt(100), eight, eight));
    // Corner-to-corner: both axes at the meeting point — no shared edge, no bonus.
    CHECK_FALSE(rm::sim::skirtsShareEdge(Fx::fromInt(100), Fx::fromInt(100), eight, eight,
                                         Fx::fromInt(116), Fx::fromInt(116), eight, eight));
    // Overlapping counts: standing ON the apron is no less adjacent than beside it.
    CHECK(rm::sim::skirtsShareEdge(Fx::fromInt(100), Fx::fromInt(100), eight, eight,
                                   Fx::fromInt(108), Fx::fromInt(100), eight, eight));
}

TEST_CASE("the buff tables resolve by name and say what the file says") {
    CHECK(rm::unitdef::adjacencyClassFromName("T1MassStorageAdjacencyBuffs")
          == AdjacencyClass::T1MassStorage);
    CHECK(rm::unitdef::adjacencyClassFromName("T1MassStorage")
          == AdjacencyClass::T1MassStorage);
    CHECK(rm::unitdef::adjacencyClassFromName("SomeModsOwnTable") == AdjacencyClass::None);

    // Hydrocarbon is T2PowerGenerator by the file's own aliasing.
    CHECK(&rm::unitdef::adjacencyGrants(AdjacencyClass::Hydrocarbon)
          == &rm::unitdef::adjacencyGrants(AdjacencyClass::T2PowerGenerator));

    const rm::unitdef::AdjacencyGrants& storage =
        rm::unitdef::adjacencyGrants(AdjacencyClass::T1MassStorage);
    CHECK(storage.massProduction[0] == Approx(0.125f));   // SIZE4 receiver
    CHECK(storage.massProduction[3] == Approx(0.03125f)); // SIZE16 receiver
    const rm::unitdef::AdjacencyGrants& pgen =
        rm::unitdef::adjacencyGrants(AdjacencyClass::T1PowerGenerator);
    CHECK(pgen.energyMaintenance[0] == Approx(-0.0625f));
}

TEST_CASE("a storage beside an extractor pays, stacks, and dies with the storage") {
    // A 2-mass/s extractor with one storage touching earns 2.25/s; with two, 2.5/s —
    // additive, the original's `Stacks = 'ALWAYS'`. And the bonus is DERIVED state: the
    // tick a storage dies, the income drops back, no bookkeeping to forget.
    Fixture f;

    rm::unitdef::UnitDef mex = smallStructure("test_mex");
    mex.producesMassPerSecond = 2.0f;
    mex.adjacencyBuffs = "T1MassExtractorAdjacencyBuffs";
    const rm::UnitTypeIndex mexType = f.roster.addType(mex);

    rm::unitdef::UnitDef storage = smallStructure("test_mass_storage");
    storage.adjacencyBuffs = "T1MassStorageAdjacencyBuffs";
    const rm::UnitTypeIndex storageType = f.roster.addType(storage);

    (void)f.roster.add(mexType, 200.0f, 200.0f, 0, 500.0f);
    f.tick();
    // Alone: 2/s at 10 Hz is 0.2 a tick.
    CHECK(rm::test::asFloat(f.economies[0].incomePerTick.mass) == Approx(0.2f).margin(0.0001));

    const UnitId east = f.roster.add(storageType, 216.0f, 200.0f, 0, 500.0f);
    f.tick();
    CHECK(rm::test::asFloat(f.economies[0].incomePerTick.mass) == Approx(0.225f).margin(0.0001));

    (void)f.roster.add(storageType, 184.0f, 200.0f, 0, 500.0f);
    f.tick();
    CHECK(rm::test::asFloat(f.economies[0].incomePerTick.mass) == Approx(0.25f).margin(0.0001));

    f.roster.health(east).current = rm::sim::Mag{};
    f.tick();  // the death is retired this tick...
    f.tick();  // ...and the income recomputed without it
    CHECK(rm::test::asFloat(f.economies[0].incomePerTick.mass) == Approx(0.225f).margin(0.0001));
}

TEST_CASE("an enemy's storage pays nobody, and a corner neighbour pays nothing") {
    Fixture f;

    rm::unitdef::UnitDef mex = smallStructure("test_mex");
    mex.producesMassPerSecond = 2.0f;
    const rm::UnitTypeIndex mexType = f.roster.addType(mex);
    rm::unitdef::UnitDef storage = smallStructure("test_mass_storage");
    storage.adjacencyBuffs = "T1MassStorageAdjacencyBuffs";
    const rm::UnitTypeIndex storageType = f.roster.addType(storage);

    (void)f.roster.add(mexType, 200.0f, 200.0f, 0, 500.0f);
    (void)f.roster.add(storageType, 216.0f, 200.0f, 1, 500.0f);   // theirs, touching
    (void)f.roster.add(storageType, 216.0f, 216.0f, 0, 500.0f);   // ours, corner only
    f.tick();
    CHECK(rm::test::asFloat(f.economies[0].incomePerTick.mass) == Approx(0.2f).margin(0.0001));
}

TEST_CASE("a generator discounts its neighbour's upkeep, and never below free") {
    Fixture f;

    rm::unitdef::UnitDef mex = smallStructure("test_mex");
    mex.producesMassPerSecond = 2.0f;
    mex.upkeepEnergyPerSecond = 2.0f;
    const rm::UnitTypeIndex mexType = f.roster.addType(mex);

    rm::unitdef::UnitDef pgen = smallStructure("test_pgen");
    pgen.producesEnergyPerSecond = 20.0f;
    pgen.adjacencyBuffs = "T1PowerGeneratorAdjacencyBuffs";
    const rm::UnitTypeIndex pgenType = f.roster.addType(pgen);

    (void)f.roster.add(mexType, 200.0f, 200.0f, 0, 500.0f);
    (void)f.roster.add(pgenType, 216.0f, 200.0f, 0, 500.0f);
    f.tick();

    // 2 e/s upkeep × (1 - 0.0625) = 1.875/s → 0.1875 a tick.
    CHECK(rm::test::asFloat(f.economies[0].upkeepPerTick.energy) == Approx(0.1875f).margin(0.0001));
}

TEST_CASE("an energy storage beside the generator raises what it makes") {
    Fixture f;

    rm::unitdef::UnitDef pgen = smallStructure("test_pgen");
    pgen.producesEnergyPerSecond = 20.0f;
    const rm::UnitTypeIndex pgenType = f.roster.addType(pgen);
    rm::unitdef::UnitDef battery = smallStructure("test_energy_storage");
    battery.adjacencyBuffs = "T1EnergyStorageAdjacencyBuffs";
    const rm::UnitTypeIndex batteryType = f.roster.addType(battery);

    (void)f.roster.add(pgenType, 200.0f, 200.0f, 0, 500.0f);
    (void)f.roster.add(batteryType, 216.0f, 200.0f, 0, 500.0f);
    f.tick();

    // 20 e/s × 1.125 = 22.5/s → 2.25 a tick. One neighbour of five possible on a SIZE4
    // receiver; a full ring would be ×1.5, which is the retail oracle.
    CHECK(rm::test::asFloat(f.economies[0].incomePerTick.energy) == Approx(2.25f).margin(0.0001));
}

TEST_CASE("a tank parked between the buildings changes nothing") {
    // Mobile units carry no skirt, so they neither give nor receive — the pair scan
    // never sees them, which is also what keeps it quadratic in STRUCTURES.
    Fixture f;

    rm::unitdef::UnitDef mex = smallStructure("test_mex");
    mex.producesMassPerSecond = 2.0f;
    const rm::UnitTypeIndex mexType = f.roster.addType(mex);
    rm::unitdef::UnitDef tank;
    tank.name = "test_tank";
    const rm::UnitTypeIndex tankType = f.roster.addType(tank);

    (void)f.roster.add(mexType, 200.0f, 200.0f, 0, 500.0f);
    (void)f.roster.add(tankType, 210.0f, 200.0f, 0, 500.0f);
    f.tick();
    CHECK(rm::test::asFloat(f.economies[0].incomePerTick.mass) == Approx(0.2f).margin(0.0001));
}
