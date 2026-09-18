// Player-perspective coverage for the FA-ECON claims (WP-19/WP-17; see
// docs/fa-exe-analysis-plan.md). Each TEST_CASE cites its claim IDs.
//
// C-051 is three retail adjacency defects reproduced deliberately, not fixed:
// the T2 power build bonus's dropped digit, the RateOfFire "bonus" that is a
// penalty, and the dead Size8..20 RateOfFire rows. C-243 is the target-side
// active-captor count: two engineers on one victim each bank two progress per
// funded beat, so the capture lands twice as fast.
#include <catch2/catch_test_macros.hpp>

#include "core/map/HeightField.hpp"
#include "core/sim/Adjacency.hpp"
#include "core/sim/Combat.hpp"
#include "core/sim/Skirmish.hpp"
#include "core/unit/Adjacency.hpp"

#include "support/FxMatchers.hpp"
#include "support/TestRoster.hpp"

#include <vector>

using rm::sim::Army;
using rm::sim::Command;
using rm::sim::CommandKind;
using rm::sim::Player;
using rm::sim::UnitId;
using rm::unitdef::UnitDef;

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

/// A 2x2-skirted structure granting `table` — the giver side of an adjacency pair.
[[nodiscard]] UnitDef giverDef(const char* name, const char* table) {
    UnitDef def;
    def.name = name;
    def.categories = {"STRUCTURE"};
    def.skirtSquaresX = 2.0f;
    def.skirtSquaresZ = 2.0f;
    def.adjacencyBuffs = table;
    return def;
}

/// A 2x2-skirted receiver of size category `size` — `STRUCTURE SIZEn`, the
/// `EntityCategory` every adjacency buff gates on.
[[nodiscard]] UnitDef receiverDef(const char* name, const char* size) {
    UnitDef def;
    def.name = name;
    def.categories = {"STRUCTURE", size};
    def.skirtSquaresX = 2.0f;
    def.skirtSquaresZ = 2.0f;
    return def;
}

/// A SIZE4 artillery piece: retail's only live RateOfFire receiver
/// (`STRUCTURE SIZE4 ARTILLERY` plus a weapon, `C-051`).
[[nodiscard]] UnitDef artilleryDef(const char* name, const char* size = "SIZE4") {
    UnitDef def = receiverDef(name, size);
    // Sorted: `hasCategory` binary-searches this list.
    std::sort(def.categories.begin(), def.categories.end());
    def.categories.insert(def.categories.begin(), "ARTILLERY");
    rm::unitdef::Weapon gun{.trackingRadius = rm::sim::Fx::fromInt(1)};
    gun.label = "howitzer";
    gun.role = rm::unitdef::WeaponRole::DirectFire;
    gun.targetPriorities = {{"LAND"}};
    gun.damage = rm::sim::magFromFloat(50.0f);
    gun.maxRange = rm::sim::fxFromFloat(300.0f);
    gun.muzzleVelocityElmosPerSecond = 100.0f;
    // Unturreted, so the hull must count as aimed: a half-turn tolerance means the
    // bore is always "on" — the reload number is what this test is about, not the
    // traverse dance.
    gun.firingToleranceBrads = 32768;
    gun.rateOfFire = 0.1f;  // one shot per ten seconds: 100 ticks at 10 Hz
    def.weapons.push_back(gun);
    return def;
}

/// Two armies, an engineer with capture capability, and funded economies —
/// the same shape test_capture's Fixture builds, with two captor slots.
struct CaptureFixture {
    rm::HeightField field = flatField();
    rm::sim::Terrain terrain{field};
    rm::sim::PassabilityGrid grid = rm::sim::buildPassability(field, 0.0f);
    rm::test::Roster roster;
    std::vector<Army> armies = rm::sim::freeForAll(2);
    std::vector<Player> players = rm::sim::onePlayerPerArmy(2, /*humanArmy=*/0);
    std::vector<rm::sim::Economy> economies{2};
    std::vector<rm::sim::Projectile> shots;
    std::vector<rm::sim::Construction> building;
    std::vector<rm::sim::CaptureWork> captures;
    std::vector<int> commandersEver{0, 0};
    rm::sim::EventQueue events;

    rm::UnitTypeIndex captorType{};
    rm::UnitTypeIndex structureType{};

    CaptureFixture() {
        rm::unitdef::UnitDef captor;
        captor.name = "test_captor";
        captor.buildRate = 10.0f;  // 1 build unit per tick at 10 Hz
        captor.categories = {"CAPTURE"};
        captorType = roster.addType(captor);

        // A T1 economy structure: 100 work units, 200 energy — 50 funded beats
        // for a lone rate-10 captor.
        rm::unitdef::UnitDef structure;
        structure.name = "test_extractor";
        structure.categories = {"LAND"};
        structure.buildCostMass = rm::sim::magFromFloat(100.0f);
        structure.buildCostEnergy = rm::sim::magFromFloat(200.0f);
        structure.buildTime = rm::sim::magFromFloat(100.0f);
        structureType = roster.addType(structure);

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

TEST_CASE("C-051: the T2 power build bonus keeps retail's dropped digit", "[fa-econ]") {
    // `T2PowerEnergyBuildBonusSize20` is `Add = -0.0125` where every sibling is
    // `-0.125` — a dropped digit, so a SIZE20 receiver gets a tenth of the
    // intended build-energy discount. Reproduced deliberately: "fixing" it
    // would change balance away from retail.
    const auto& t2 = rm::unitdef::adjacencyGrants(rm::unitdef::AdjacencyClass::T2PowerGenerator);
    CHECK(t2.energyBuild[0] == -0.125f);
    CHECK(t2.energyBuild[1] == -0.125f);
    CHECK(t2.energyBuild[2] == -0.125f);
    CHECK(t2.energyBuild[3] == -0.125f);
    CHECK(t2.energyBuild[4] == -0.0125f);  // the defect, verbatim
    // The maintenance row at the same size is correct — the defect is only in
    // the build bonus, which is what makes it a transcription slip and not a rule.
    CHECK(t2.energyMaintenance[4] == -0.125f);
    // Hydrocarbon lists the T2 table's buff names verbatim, so it inherits the
    // defect rather than dodging it.
    const auto& hydro = rm::unitdef::adjacencyGrants(rm::unitdef::AdjacencyClass::Hydrocarbon);
    CHECK(hydro.energyBuild[4] == -0.0125f);

    // Player-visible: a SIZE20 factory beside a T2 generator pays 98.75% build
    // energy where a SIZE4 one pays 87.5%.
    rm::test::Roster roster;
    const auto gen = roster.addType(giverDef("t2gen", "T2PowerGeneratorAdjacencyBuffs"));
    const auto big = roster.addType(receiverDef("big_factory", "SIZE20"));
    const auto small = roster.addType(receiverDef("small_factory", "SIZE4"));
    const UnitId genUnit = roster.add(gen, 0.0f, 0.0f, 0, 1000.0f);
    const UnitId bigUnit = roster.add(big, 16.0f, 0.0f, 0, 1000.0f);
    const UnitId smallUnit = roster.add(small, 0.0f, 16.0f, 0, 1000.0f);
    (void)genUnit;

    std::vector<rm::sim::AdjacencyEffects> effects;
    rm::sim::adjacencyEffects(roster.store, roster.catalog, effects);
    CHECK(effects[bigUnit.index].energyBuild == rm::sim::kFxOne - rm::sim::fxFromFloat(0.0125f));
    CHECK(effects[smallUnit.index].energyBuild == rm::sim::kFxOne - rm::sim::fxFromFloat(0.125f));
}

TEST_CASE("C-051: RateOfFire adjacency is a penalty and only SIZE4 artillery gets it",
          "[fa-econ]") {
    // The "bonus" slows the gun: `Buff.lua` lands the new rate at
    // `val x bpRateOfFire` with `Add` negative, so the reload stretches by
    // `1/val`. And only the Size4 row is live — the Size8..20 buffs are defined
    // but referenced by no producer list (defect (c)).
    const auto& t1 = rm::unitdef::adjacencyGrants(rm::unitdef::AdjacencyClass::T1PowerGenerator);
    CHECK(t1.rateOfFire[0] == -0.025f);
    CHECK(t1.rateOfFire[1] == 0.0f);  // dead code, reproduced as no grant
    CHECK(t1.rateOfFire[4] == 0.0f);
    const auto& t3 = rm::unitdef::adjacencyGrants(rm::unitdef::AdjacencyClass::T3PowerGenerator);
    CHECK(t3.rateOfFire[0] == -0.075f);

    rm::test::Roster roster;
    const auto gen = roster.addType(giverDef("t1gen", "T1PowerGeneratorAdjacencyBuffs"));
    const auto arty4 = roster.addType(artilleryDef("arty4"));
    const auto arty8 = roster.addType(artilleryDef("arty8", "SIZE8"));
    const auto radar = roster.addType(receiverDef("radar", "SIZE4"));  // no ARTILLERY, no gun
    const UnitId genUnit = roster.add(gen, 0.0f, 0.0f, 0, 1000.0f);
    const UnitId small = roster.add(arty4, 16.0f, 0.0f, 0, 1000.0f);
    const UnitId big = roster.add(arty8, 0.0f, 16.0f, 0, 1000.0f);
    const UnitId blind = roster.add(radar, 16.0f, 16.0f, 0, 1000.0f);
    (void)genUnit;

    std::vector<rm::sim::AdjacencyEffects> effects;
    rm::sim::adjacencyEffects(roster.store, roster.catalog, effects);
    // The SIZE4 artillery's multiplier drops below one — the penalty.
    CHECK(effects[small.index].rateOfFire.raw() == (rm::sim::kFxOne - rm::sim::fxFromFloat(0.025f)).raw());
    // The SIZE8 artillery gets nothing: its buff row is retail's dead code.
    CHECK(effects[big.index].rateOfFire == rm::sim::kFxOne);
    // A SIZE4 structure that is not artillery never sees the grant at all.
    CHECK(effects[blind.index].rateOfFire == rm::sim::kFxOne);
}

TEST_CASE("C-051: an adjacency-buffed artillery reloads slower, not faster",
          "[fa-econ]") {
    // The defect end to end: a T1 generator beside a SIZE4 artillery stretches
    // its 100-tick reload to 102 — the weapon fires SLOWER, which is the part a
    // player would call a bug and retail ships anyway.
    rm::test::Roster roster;
    const auto gen = roster.addType(giverDef("t1gen", "T1PowerGeneratorAdjacencyBuffs"));
    const auto arty = roster.addType(artilleryDef("arty"));
    const UnitId genUnit = roster.add(gen, 0.0f, 0.0f, 0, 1000.0f);
    const UnitId gun = roster.add(arty, 16.0f, 0.0f, 0, 1000.0f);
    UnitDef enemy;
    enemy.name = "target";
    enemy.categories = {"LAND"};
    const auto enemyType = roster.addType(enemy);
    (void)roster.add(enemyType, 60.0f, 0.0f, 1, 1000.0f);
    (void)genUnit;

    std::vector<Army> armies = rm::sim::freeForAll(2);
    std::vector<rm::sim::Projectile> shots;
    std::vector<rm::sim::AdjacencyEffects> adjacency;
    rm::sim::adjacencyEffects(roster.store, roster.catalog, adjacency);

    const rm::sim::TickRate rate{};
    const rm::TickCount authored =
        roster.catalog.weaponRates(roster.store.typeAt(gun.index), 0).reloadTicks;
    REQUIRE(authored == 100);
    const std::size_t fired = rm::sim::fireWeapons(
        roster.store, roster.catalog, armies, shots, rate, nullptr, nullptr, nullptr, 0,
        {}, nullptr, {}, adjacency);
    REQUIRE(fired == 1);
    // 100 / 0.975 = 102.56 → 102 ticks: two ticks slower than authored.
    CHECK(roster.health(gun).reloadRemaining[0] == 102);

    // The same gun with no generator beside it reloads at the authored rate.
    rm::test::Roster lone;
    const auto loneArty = lone.addType(artilleryDef("lone_arty"));
    const auto loneEnemy = lone.addType(enemy);
    const UnitId loneGun = lone.add(loneArty, 20.0f, 0.0f, 0, 1000.0f);
    (void)lone.add(loneEnemy, 60.0f, 0.0f, 1, 1000.0f);
    std::vector<rm::sim::Projectile> loneShots;
    std::vector<rm::sim::AdjacencyEffects> none;
    rm::sim::adjacencyEffects(lone.store, lone.catalog, none);
    REQUIRE(rm::sim::fireWeapons(lone.store, lone.catalog, armies, loneShots, rate,
                                 nullptr, nullptr, nullptr, 0, {}, nullptr, {}, none)
            == 1);
    CHECK(lone.health(loneGun).reloadRemaining[0] == 100);
}

TEST_CASE("C-243: two captors on one target each bank the active-captor count",
          "[fa-econ]") {
    // Retail's `Unit+0x690`: the target counts its ACTIVE capture tasks, and
    // each funded beat adds that count to every task's progress. Two engineers
    // on one extractor therefore finish in half the lone-captor time — 25
    // funded beats, not 50.
    CaptureFixture f;
    const UnitId first = f.roster.add(f.captorType, 200.0f, 200.0f, 0, 100.0f);
    const UnitId second = f.roster.add(f.captorType, 200.0f, 206.0f, 0, 100.0f);
    const UnitId target = f.roster.add(f.structureType, 206.0f, 203.0f, 1, 100.0f);
    REQUIRE(f.capture(first, target));
    REQUIRE(f.capture(second, target));

    f.tick(10);
    REQUIRE(f.captures.size() == 2);
    // Ten funded beats at +2 each: both tasks stand at 20 of 50, where a lone
    // captor would stand at 10.
    CHECK(f.captures[0].progress == 20);
    CHECK(f.captures[1].progress == 20);
    CHECK(f.roster.store.alive(target));

    f.tick(15);
    // Beat 25 completes the work: the target transferred to army 0 and both
    // tasks retired — the second captor's progress dies with the victim, which
    // is retail's own outcome when the count outruns the budget.
    CHECK_FALSE(f.roster.store.alive(target));
    CHECK(f.captures.empty());
    const UnitId replacement = f.roster.store.idAt(target.index);
    REQUIRE(f.roster.store.alive(replacement));
    CHECK(f.roster.store.motion()[replacement.index].armyIndex == 0);
}

TEST_CASE("C-243: a lone captor still banks one per funded beat", "[fa-econ]") {
    // The count degenerates to the old behaviour with one captor — the claim's
    // own note that a lone active captor adds one.
    CaptureFixture f;
    const UnitId captor = f.roster.add(f.captorType, 200.0f, 200.0f, 0, 100.0f);
    const UnitId target = f.roster.add(f.structureType, 206.0f, 200.0f, 1, 100.0f);
    REQUIRE(f.capture(captor, target));
    f.tick(10);
    REQUIRE(f.captures.size() == 1);
    CHECK(f.captures[0].progress == 10);
}
