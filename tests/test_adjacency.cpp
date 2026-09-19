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
#include "core/unit/UnitBlueprint.hpp"

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
    def.categories = {"STRUCTURE", "SIZE4"};
    return def;
}

struct Fixture {
    rm::HeightField field = flatField();
    // Free placement, deliberately: these cases probe the half-ogrid slack and skirt offsets
    // at hand-chosen coordinates. Grid mode's zero tolerance is tested on skirtsShareEdge.
    rm::sim::Terrain terrain{field, false, 0.0f, nullptr, {}, rm::sim::PlacementMode::Free};
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
    // Grid placement: zero tolerance. Exact contact counts, the half-ogrid slack does not.
    CHECK(rm::sim::skirtsShareEdge(Fx::fromInt(100), Fx::fromInt(100), eight, eight,
                                   Fx::fromInt(116), Fx::fromInt(100), eight, eight, Fx{}));
    CHECK_FALSE(rm::sim::skirtsShareEdge(Fx::fromInt(100), Fx::fromInt(100), eight, eight,
                                         Fx::fromInt(119), Fx::fromInt(100), eight, eight, Fx{}));
}

TEST_CASE("an FA skirt rectangle starts at footprint plus its offset") {
    // Retail's rectangle is `position - Footprint/2 + SkirtOffset`, extending by
    // SkirtSize. This 2x2 footprint with a 4x2 skirt therefore has its skirt centre one
    // half an ogrid along X and one ogrid along Z after its authored negative offsets.
    const auto parsed = rm::unitbp::load(R"(
        UnitBlueprint {
            Footprint = { SizeX = 2, SizeZ = 2 },
            Physics = {
                MotionType = 'RULEUMT_None',
                SkirtOffsetX = -0.5,
                SkirtOffsetZ = -1,
                SkirtSizeX = 4,
                SkirtSizeZ = 6,
            },
            SizeX = 2,
            SizeZ = 2,
        }
    )", "SHIFTED_unit.bp");
    REQUIRE(parsed.has_value());

    Fixture f;
    rm::unitdef::UnitDef storage = *parsed;
    storage.categories = {"STRUCTURE", "SIZE4"};
    storage.adjacencyBuffs = "T1MassStorageAdjacencyBuffs";
    const rm::UnitTypeIndex storageType = f.roster.addType(storage);
    CHECK(storage.skirtCentreOffsetSquaresX == Approx(0.5f));
    CHECK(storage.skirtCentreOffsetSquaresZ == Approx(1.0f));
    const rm::sim::UnitCatalog::AdjacencyInfo& geometry =
        f.roster.catalog.adjacency(storageType);
    CHECK(geometry.skirtCentreOffsetXElmos == Fx::fromInt(4));
    CHECK(geometry.skirtCentreOffsetZElmos == Fx::fromInt(8));

    rm::unitdef::UnitDef mex = smallStructure("test_mex");
    mex.producesMassPerSecond = 2.0f;
    const rm::UnitTypeIndex mexType = f.roster.addType(mex);

    (void)f.roster.add(storageType, 200.0f, 200.0f, 0, 500.0f);
    // Shifted storage east edge = 220; mex west edge = 224. The four-elmo gap is the
    // deliberate free-placement tolerance. A wrongly centred storage ends at 216.
    (void)f.roster.add(mexType, 232.0f, 208.0f, 0, 500.0f);
    f.tick();

    CHECK(rm::test::asFloat(f.economies[0].incomePerTick.mass)
          == Approx(0.225f).margin(0.0001));
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

TEST_CASE("a generator discounts its neighbour's upkeep") {
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

TEST_CASE("adjacency modifiers remain unclamped") {
    Fixture f;

    rm::unitdef::UnitDef receiver = smallStructure("sized_upkeep_receiver");
    receiver.upkeepEnergyPerSecond = 2.0f;
    const rm::UnitTypeIndex receiverType = f.roster.addType(receiver);

    rm::unitdef::UnitDef pgen = smallStructure("test_t3_pgen");
    pgen.adjacencyBuffs = "T3PowerGeneratorAdjacencyBuffs";
    const rm::UnitTypeIndex pgenType = f.roster.addType(pgen);

    const UnitId receiverId = f.roster.add(receiverType, 200.0f, 200.0f, 0, 500.0f);
    for (int giver = 0; giver < 6; ++giver) {
        (void)f.roster.add(pgenType, 200.0f, 200.0f, 0, 500.0f);
    }

    std::vector<rm::sim::AdjacencyEffects> effects;
    rm::sim::adjacencyEffects(f.roster.store, f.roster.catalog, effects);
    CHECK(effects[receiverId.index].energyUpkeep < Fx{});

    f.tick();
    CHECK(rm::test::asFloat(f.economies[0].upkeepPerTick.energy) < 0.0f);
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

TEST_CASE("a skirted STRUCTURE without an authored size receives no adjacency") {
    Fixture f;

    rm::unitdef::UnitDef mex = smallStructure("unclassified_mex");
    mex.categories = {"STRUCTURE"};
    mex.producesMassPerSecond = 2.0f;
    const rm::UnitTypeIndex mexType = f.roster.addType(mex);

    rm::unitdef::UnitDef storage = smallStructure("test_mass_storage");
    storage.adjacencyBuffs = "T1MassStorageAdjacencyBuffs";
    const rm::UnitTypeIndex storageType = f.roster.addType(storage);

    (void)f.roster.add(mexType, 200.0f, 200.0f, 0, 500.0f);
    (void)f.roster.add(storageType, 216.0f, 200.0f, 0, 500.0f);
    f.tick();

    CHECK(rm::test::asFloat(f.economies[0].incomePerTick.mass) == Approx(0.2f).margin(0.0001));
}

TEST_CASE("a STRUCTURE with an authored size receives its adjacency row") {
    Fixture f;

    rm::unitdef::UnitDef mex = smallStructure("sized_mex");
    mex.producesMassPerSecond = 2.0f;
    const rm::UnitTypeIndex mexType = f.roster.addType(mex);

    rm::unitdef::UnitDef storage = smallStructure("test_mass_storage");
    storage.adjacencyBuffs = "T1MassStorageAdjacencyBuffs";
    const rm::UnitTypeIndex storageType = f.roster.addType(storage);

    (void)f.roster.add(mexType, 200.0f, 200.0f, 0, 500.0f);
    (void)f.roster.add(storageType, 216.0f, 200.0f, 0, 500.0f);
    f.tick();

    CHECK(rm::test::asFloat(f.economies[0].incomePerTick.mass) == Approx(0.225f).margin(0.0001));
}

TEST_CASE("an authored size selects its row even when its skirt disagrees") {
    Fixture f;

    rm::unitdef::UnitDef mex = smallStructure("mismatched_mex");
    mex.categories = {"STRUCTURE", "SIZE16"};
    mex.producesMassPerSecond = 2.0f;
    const rm::UnitTypeIndex mexType = f.roster.addType(mex);

    rm::unitdef::UnitDef storage = smallStructure("test_mass_storage");
    storage.adjacencyBuffs = "T1MassStorageAdjacencyBuffs";
    const rm::UnitTypeIndex storageType = f.roster.addType(storage);

    (void)f.roster.add(mexType, 200.0f, 200.0f, 0, 500.0f);
    (void)f.roster.add(storageType, 216.0f, 200.0f, 0, 500.0f);
    f.tick();

    // A SIZE16 receiver takes the fourth mass-storage row (+3.125%), not the SIZE4 row
    // suggested by its 2x2 skirt.
    CHECK(rm::test::asFloat(f.economies[0].incomePerTick.mass)
           == Approx(0.20625f).margin(0.0001));
}

TEST_CASE("a storage requires exactly STRUCTURE and one valid authored size") {
    const auto incomeBesideStorage = [](std::vector<std::string> categories) {
        Fixture f;

        rm::unitdef::UnitDef receiver = smallStructure("test_receiver");
        receiver.categories = std::move(categories);
        receiver.producesMassPerSecond = 2.0f;
        const rm::UnitTypeIndex receiverType = f.roster.addType(receiver);

        rm::unitdef::UnitDef storage = smallStructure("test_mass_storage");
        storage.adjacencyBuffs = "T1MassStorageAdjacencyBuffs";
        const rm::UnitTypeIndex storageType = f.roster.addType(storage);

        (void)f.roster.add(receiverType, 200.0f, 200.0f, 0, 500.0f);
        (void)f.roster.add(storageType, 216.0f, 200.0f, 0, 500.0f);
        f.tick();
        return rm::test::asFloat(f.economies[0].incomePerTick.mass);
    };

    const auto baseIncome = Approx(0.2f).margin(0.0001); // 2 mass/s at 10 Hz.
    CHECK(incomeBesideStorage({"SIZE4"}) == baseIncome);
    CHECK(incomeBesideStorage({"STRUCTURE", "SIZE4", "SIZE8"}) == baseIncome);
    CHECK(incomeBesideStorage({"STRUCTURE", "SIZE4", "SIZE24"}) == baseIncome);
}

TEST_CASE("a ghost's adjacency preview reports the grant in both directions") {
    // The card and the connection lines read this one answer: a storage ghost beside an
    // extractor says what the extractor GAINS; an extractor ghost beside a storage says
    // what the new building GETS. The standing world supplies the truth either way — the
    // ghost is a participant that does not exist yet.
    Fixture f;

    rm::unitdef::UnitDef mex = smallStructure("test_mex");
    mex.producesMassPerSecond = 2.0f;
    const rm::UnitTypeIndex mexType = f.roster.addType(mex);
    rm::unitdef::UnitDef storage = smallStructure("test_mass_storage");
    storage.adjacencyBuffs = "T1MassStorageAdjacencyBuffs";
    const rm::UnitTypeIndex storageType = f.roster.addType(storage);

    const UnitId standingMex = f.roster.add(mexType, 200.0f, 200.0f, 0, 500.0f);
    const UnitId standingStorage = f.roster.add(storageType, 216.0f, 200.0f, 0, 500.0f);

    // A storage ghost beside the standing extractor: grants it +12.5%, receives nothing.
    const auto grant = rm::sim::adjacencyPreview(
        f.roster.store, f.roster.catalog, 0, f.roster.catalog.adjacency(storageType),
        rm::sim::fxFromFloat(200.0f), rm::sim::fxFromFloat(216.0f));
    REQUIRE(grant.links.size() == 1);
    CHECK(grant.links.front().slot == standingMex.index);
    CHECK(rm::sim::fxToFloat(grant.links.front().fromGhost.massProduction)
          == Approx(0.125f));
    CHECK_FALSE(grant.links.front().toGhost.any());
    CHECK(grant.received.massProduction == rm::sim::kFxOne);

    // An extractor ghost beside the standing storage: receives +12.5%, grants nothing.
    const auto receive = rm::sim::adjacencyPreview(
        f.roster.store, f.roster.catalog, 0, f.roster.catalog.adjacency(mexType),
        rm::sim::fxFromFloat(232.0f), rm::sim::fxFromFloat(200.0f));
    REQUIRE(receive.links.size() == 1);
    CHECK(receive.links.front().slot == standingStorage.index);
    CHECK(rm::sim::fxToFloat(receive.links.front().toGhost.massProduction)
          == Approx(0.125f));
    CHECK(rm::sim::fxToFloat(receive.received.massProduction) == Approx(1.125f));
}

TEST_CASE("the ghost preview only links a pair a bonus actually crosses") {
    // The beneficial filter: two skirted structures can touch and still be nobody's
    // business — neither authors a grant. A line drawn for them would say "adjacent"
    // where nothing is paid, which is noise the player's eye has to discount.
    Fixture f;

    rm::unitdef::UnitDef plain = smallStructure("test_plain");
    const rm::UnitTypeIndex plainType = f.roster.addType(plain);
    rm::unitdef::UnitDef storage = smallStructure("test_mass_storage");
    storage.adjacencyBuffs = "T1MassStorageAdjacencyBuffs";
    const rm::UnitTypeIndex storageType = f.roster.addType(storage);

    const UnitId friendly = f.roster.add(storageType, 232.0f, 216.0f, 0, 500.0f);
    // An ENEMY's storage touching the site too: no bonus crosses the front line.
    (void)f.roster.add(storageType, 200.0f, 216.0f, 1, 500.0f);
    // And a structure that gives nothing — the ghost's receiver row is real, but the
    // building beside it authors no grant in either direction.
    (void)f.roster.add(plainType, 216.0f, 200.0f, 0, 500.0f);

    // A buffless ghost at (216,216) touches all three. Only the friendly storage
    // answers: the plain building grants nothing, the enemy's is another army.
    const auto preview = rm::sim::adjacencyPreview(
        f.roster.store, f.roster.catalog, 0, f.roster.catalog.adjacency(plainType),
        rm::sim::fxFromFloat(216.0f), rm::sim::fxFromFloat(216.0f));
    REQUIRE(preview.links.size() == 1);
    CHECK(preview.links.front().slot == friendly.index);
    CHECK(rm::sim::fxToFloat(preview.links.front().toGhost.massProduction)
          == Approx(0.125f));
    CHECK(rm::sim::fxToFloat(preview.received.massProduction) == Approx(1.125f));

    // And a ghost with no skirt — a mobile unit being placed does not ask the question.
    rm::unitdef::UnitDef tank;
    tank.name = "test_tank";
    const rm::UnitTypeIndex tankType = f.roster.addType(tank);
    CHECK(rm::sim::adjacencyPreview(f.roster.store, f.roster.catalog, 0,
                                    f.roster.catalog.adjacency(tankType),
                                    rm::sim::fxFromFloat(200.0f), rm::sim::fxFromFloat(208.0f))
              .links.empty());
}

TEST_CASE("the ghost preview asks the same skirt question the tick does") {
    // Grid placement gives the tick zero slack; the preview must hold the same line or
    // it promises bonuses the placement will not pay.
    Fixture f;

    rm::unitdef::UnitDef storage = smallStructure("test_mass_storage");
    storage.adjacencyBuffs = "T1MassStorageAdjacencyBuffs";
    const rm::UnitTypeIndex storageType = f.roster.addType(storage);
    rm::unitdef::UnitDef mex = smallStructure("test_mex");
    const rm::UnitTypeIndex mexType = f.roster.addType(mex);

    const UnitId standing = f.roster.add(storageType, 200.0f, 200.0f, 0, 500.0f);

    // Two elmos of daylight between the skirts: adjacency under free placement's slack,
    // not under grid placement's exact contact.
    const auto slack = rm::sim::adjacencyPreview(
        f.roster.store, f.roster.catalog, 0, f.roster.catalog.adjacency(mexType),
        rm::sim::fxFromFloat(218.0f), rm::sim::fxFromFloat(200.0f),
        rm::sim::kAdjacencyGapElmos);
    REQUIRE(slack.links.size() == 1);
    CHECK(slack.links.front().slot == standing.index);
    CHECK(rm::sim::adjacencyPreview(f.roster.store, f.roster.catalog, 0,
                                    f.roster.catalog.adjacency(mexType),
                                    rm::sim::fxFromFloat(218.0f), rm::sim::fxFromFloat(200.0f),
                                    Fx{})
              .links.empty());
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

TEST_CASE("C-052: no adjacency while either party is under construction", "[fa-econ]") {
    // `OnAdjacentTo` returns early when `self:IsBeingBuilt()` or
    // `adjacentUnit:IsBeingBuilt()` (`defaultunits.lua:357-359`) — a factory
    // rising to its next tier neither grants nor receives until it stands.
    // Here "being built" is a live unit named by an unfinished upgrade row:
    // a scaffold is a `Construction` record, not a unit, so the only entity
    // the sim can point `IsBeingBuilt` at is an `upgradeOf` target.
    Fixture f;

    rm::unitdef::UnitDef factory = smallStructure("test_factory");
    factory.upkeepEnergyPerSecond = 2.0f;
    const rm::UnitTypeIndex factoryType = f.roster.addType(factory);
    rm::unitdef::UnitDef pgen = smallStructure("test_pgen");
    pgen.producesEnergyPerSecond = 20.0f;
    pgen.adjacencyBuffs = "T1PowerGeneratorAdjacencyBuffs";
    const rm::UnitTypeIndex pgenType = f.roster.addType(pgen);

    const UnitId rising = f.roster.add(factoryType, 200.0f, 200.0f, 0, 500.0f);
    const UnitId gen = f.roster.add(pgenType, 216.0f, 200.0f, 0, 500.0f);

    // Control: both standing, the discount flows — 2 e/s × (1 − 0.0625).
    std::vector<rm::sim::AdjacencyEffects> effects;
    rm::sim::adjacencyEffects(f.roster.store, f.roster.catalog, effects);
    CHECK(effects[rising.index].energyUpkeep
          == rm::sim::kFxOne - rm::sim::fxFromFloat(0.0625f));

    // The factory starts upgrading: an unfinished row names it `upgradeOf`.
    std::vector<rm::sim::Construction> building{
        rm::sim::Construction{.armyIndex = 0,
                              .buildTimeRemaining = rm::sim::magFromFloat(50.0f),
                              .totalBuildTime = rm::sim::magFromFloat(100.0f),
                              .upgradeOf = rising,
                              .builder = rising}};
    rm::sim::adjacencyEffects(f.roster.store, f.roster.catalog, effects,
                              rm::sim::kAdjacencyGapElmos, building);
    CHECK(effects[rising.index].energyUpkeep == rm::sim::kFxOne);
    CHECK(effects[gen.index].energyUpkeep == rm::sim::kFxOne);
    CHECK(effects[rising.index].energyBuild == rm::sim::kFxOne);
    CHECK(effects[gen.index].energyBuild == rm::sim::kFxOne);

    // A FINISHED row is no longer "being built": the bonus returns.
    building.front().buildTimeRemaining = rm::sim::Mag{};
    rm::sim::adjacencyEffects(f.roster.store, f.roster.catalog, effects,
                              rm::sim::kAdjacencyGapElmos, building);
    CHECK(effects[rising.index].energyUpkeep
          == rm::sim::kFxOne - rm::sim::fxFromFloat(0.0625f));
}

TEST_CASE("C-052: capture clears the unit's adjacency links", "[fa-econ]") {
    // `TransferUnitsOwnership` destroys and recreates the entity, so the
    // replacement's buff list starts empty (`Unit.lua:555-620`); the army
    // change alone already gates the old link out, because grants only flow
    // inside one army. The capture transfer in `applyCaptureWork` is the same
    // kill-and-respawn — verify the derived scan agrees.
    Fixture f;

    rm::unitdef::UnitDef mex = smallStructure("test_mex");
    mex.producesMassPerSecond = 2.0f;
    const rm::UnitTypeIndex mexType = f.roster.addType(mex);
    rm::unitdef::UnitDef storage = smallStructure("test_mass_storage");
    storage.adjacencyBuffs = "T1MassStorageAdjacencyBuffs";
    const rm::UnitTypeIndex storageType = f.roster.addType(storage);

    const UnitId victim = f.roster.add(mexType, 200.0f, 200.0f, 1, 500.0f);
    (void)f.roster.add(storageType, 216.0f, 200.0f, 1, 500.0f);

    std::vector<rm::sim::AdjacencyEffects> effects;
    rm::sim::adjacencyEffects(f.roster.store, f.roster.catalog, effects);
    CHECK(effects[victim.index].massProduction
          == rm::sim::kFxOne + rm::sim::fxFromFloat(0.125f));

    // The capture transfer's own shape: the old handle dies and a same-type,
    // same-spot replacement stands under the captor's army.
    const rm::sim::Transform at = f.roster.store.transforms()[victim.index];
    const rm::sim::Health health = f.roster.store.health()[victim.index];
    rm::sim::MoveState motion = f.roster.store.motion()[victim.index];
    motion.armyIndex = 0;
    f.roster.store.kill(victim);
    const UnitId replacement = f.roster.store.spawn(rm::sim::UnitStore::Spawn{
        .type = mexType, .transform = at, .motion = motion, .health = health});

    rm::sim::adjacencyEffects(f.roster.store, f.roster.catalog, effects);
    CHECK(effects[replacement.index].massProduction == rm::sim::kFxOne);
}
