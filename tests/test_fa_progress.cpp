// Player-perspective coverage for progress claims (see docs/fa-exe-analysis-plan.md):
// WP-34 ACU enhancements (C-251..C-255, C-376..C-379) and WP-35 veterancy (C-380).
//
// The unit-level mechanics are pinned in test_script_task.cpp, test_economy.cpp and
// test_veterancy.cpp. What those do not answer is the player's question: issue the
// enhancement through the same command queue a click uses, tick the real match, and
// watch the unit change — build rate, buildable tiers, health, and the slot registry.
// These cases run `advanceMatch`, the same runner the window uses.
#include "app/Match.hpp"
#include "app/SceneBuild.hpp"
#include "core/lua/LuaTable.hpp"
#include "core/sim/Enhancement.hpp"
#include "core/sim/Movement.hpp"
#include "core/sim/StateHash.hpp"
#include "core/sim/SaveState.hpp"
#include "core/sim/Terrain.hpp"
#include "core/unit/UnitDef.hpp"
#include "support/FxMatchers.hpp"
#include "support/TestRoster.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

namespace {

rm::HeightField flatField() {
    rm::HeightField field;
    field.squaresX = 128;
    field.squaresZ = 128;
    field.heightScale = 1.0f;
    field.raw.assign(field.sampleCount(), std::uint16_t{0});
    return field;
}

/// The smallest scene that runs a real match tick: two free-for-all armies, player 0
/// driving army 0 — the same shape test_unit_scenarios.cpp's Scenario builds.
struct Scenario {
    rm::HeightField field = flatField();
    rm::app::UnitScene scene;
    rm::app::PassabilitySet passability;
    rm::vfs::Vfs content;

    Scenario() : passability(field, false, 0.0f) {
        scene.armies = rm::sim::freeForAll(2);
        scene.players = rm::sim::onePlayerPerArmy(2, 0);
        scene.playerArmy = 0;
        scene.economies.resize(2);
        scene.commandersEver.assign(2, 0);
    }

    rm::UnitTypeIndex registerType(const rm::unitdef::UnitDef& def) {
        scene.definitions.push_back(def);
        const auto type = scene.catalog.add(&scene.definitions.back(), rm::app::gAppTickRate);
        scene.setTypeTraits(type, rm::data::moveDefFor(def), def.meshToElmos);
        const std::string path = "/units/" + def.name + "/" + def.name + "_unit.bp";
        scene.setPathForType(type, path);
        scene.typeForBlueprint.emplace(path, type);
        return type;
    }

    rm::sim::UnitId spawn(const rm::unitdef::UnitDef& def, float x, float z, int army = 0) {
        const auto type = registerType(def);
        return scene.store.spawn({
            .type = type,
            .transform = {.x = rm::sim::fxFromFloat(x), .z = rm::sim::fxFromFloat(z)},
            .motion = rm::app::motionFor(def, army),
            .health = rm::sim::initialHealth(def.health),
        });
    }

    rm::app::MatchRunner runner() {
        auto result = rm::app::makeMatchRunner(scene, field, passability, content, {}, {});
        result.match.baseStorage = rm::app::kStartingStorage;
        result.scripts.clear();
        return result;
    }
};

/// A commander-shaped unit with the UEF engineering ladder: AdvancedEngineering in
/// slot LCH, T3Engineering chained on it, and the paid pseudo-enhancement that
/// removes the base one. Numbers are small so the funded drain finishes in a few
/// ticks; the shape is the retail ACU's (C-255: slots LCH/RCH/Back, linear prereq
/// chains, `XxxRemove` entries at cost 1/1).
[[nodiscard]] rm::unitdef::UnitDef commanderDef() {
    rm::unitdef::UnitDef def;
    def.name = "test_acu";
    def.categories = {"COMMAND", "MOBILE"};
    def.health = rm::test::mag(100.0f);
    def.buildRate = 10.0f;
    def.speedElmosPerSecond = 20.0f;
    def.turnRateRadiansPerSecond = 3.0f;
    def.collisionRadiusElmos = 2.0f;
    def.buildableCategory = {rm::unitdef::parseCategoryTerm("TECH1")};
    const auto t2 = rm::lua::parseTable(
        "{ NewBuildRate=30, NewHealth=20, NewRegenRate=2,"
        "  BuildableCategoryAdds='BUILTBYTIER2COMMANDER' }");
    const auto t3 = rm::lua::parseTable(
        "{ NewBuildRate=90, BuildableCategoryAdds='BUILTBYTIER3COMMANDER' }");
    const auto none = rm::lua::parseTable("{}");
    REQUIRE(t2);
    REQUIRE(t3);
    REQUIRE(none);
    def.enhancements.push_back({.name = "AdvancedEngineering", .slot = "LCH",
        .buildCostMass = rm::test::mag(8.0f), .buildCostEnergy = rm::test::mag(80.0f),
        .buildTime = rm::test::fx(8.0f), .parameters = *t2});
    def.enhancements.push_back({.name = "T3Engineering", .slot = "LCH",
        .prerequisite = "AdvancedEngineering",
        .buildCostMass = rm::test::mag(8.0f), .buildCostEnergy = rm::test::mag(80.0f),
        .buildTime = rm::test::fx(8.0f), .parameters = *t3});
    // The paid removal pseudo-enhancement (C-378): ~free, Prerequisite is the base
    // enhancement, and it removes itself too so the slot ends empty.
    def.enhancements.push_back({.name = "AdvancedEngineeringRemove", .slot = "LCH",
        .prerequisite = "AdvancedEngineering",
        .buildCostMass = rm::test::mag(1.0f), .buildCostEnergy = rm::test::mag(1.0f),
        .buildTime = rm::test::fx(1.0f),
        .removes = {"AdvancedEngineering", "AdvancedEngineeringRemove"},
        .parameters = *none});
    return def;
}

/// A buildable product for the tier checks.
[[nodiscard]] rm::unitdef::UnitDef productDef(std::string name, std::string category) {
    rm::unitdef::UnitDef def;
    def.name = std::move(name);
    def.categories = {std::move(category)};
    def.health = rm::test::mag(50.0f);
    return def;
}

/// Issues the two-command `UNITCOMMAND_Script` enhancement order (C-251): one
/// `CommandKind::Script` carrying `EnhanceTask` and the enhancement id, through the
/// same intake a UI click uses.
[[nodiscard]] bool issueEnhancement(Scenario& job, rm::sim::UnitId unit,
                                    rm::TickIndex tick, const std::string& name,
                                    bool queued = false) {
    return rm::app::submitCommand(job.scene, rm::sim::CommandIssue{
        .tick = tick,
        .source = static_cast<rm::CommandSource>(0),
        .player = 0,
        .kind = rm::sim::CommandKind::Script,
        .queued = queued,
        .units = {unit},
        .scriptTask = "EnhanceTask",
        .scriptData = {name.begin(), name.end()},
    }).has_value();
}

} // namespace

TEST_CASE("C-376/C-379/C-253: an ACU enhancement installs through the command queue "
          "and changes what the unit can do", "[fa-progress][enhancement]") {
    Scenario job;
    const auto acu = job.spawn(commanderDef(), 300, 300);
    const auto t1 = job.registerType(productDef("t1_mex", "TECH1"));
    const auto t2 = job.registerType(productDef("t2_factory", "BUILTBYTIER2COMMANDER"));
    const auto t3 = job.registerType(productDef("t3_factory", "BUILTBYTIER3COMMANDER"));
    auto runner = job.runner();
    int tick = 0;

    // Before the upgrade the commander builds its authored T1 list and nothing more:
    // the COMMAND gate keeps the advanced categories out until a script removes the
    // restriction (retail ACU OnCreate, C-255).
    const auto& defs = job.scene.definitions;
    CHECK(rm::sim::canBuild(job.scene.store, job.scene.catalog, acu.index, defs[t1]));
    CHECK_FALSE(rm::sim::canBuild(job.scene.store, job.scene.catalog, acu.index, defs[t2]));
    CHECK_FALSE(rm::sim::canBuild(job.scene.store, job.scene.catalog, acu.index, defs[t3]));

    // The prereq chain is enforced in the task, not the UI (C-253): T3Engineering
    // cannot be the first thing in the slot, so the order is accepted at intake and
    // dies on its first dispatch without creating work.
    REQUIRE(issueEnhancement(job, acu, static_cast<rm::TickIndex>(tick), "T3Engineering"));
    (void)rm::app::advanceMatch(runner, tick++, 0);
    CHECK(job.scene.enhancementWork.empty());
    CHECK(job.scene.store.orders()[acu.index].empty());
    CHECK(job.scene.store.enhancements()[acu.index].empty());

    // The real order: EnhanceTask for AdvancedEngineering. The first tick creates the
    // work row — the progress bar the player watches (C-376).
    REQUIRE(issueEnhancement(job, acu, static_cast<rm::TickIndex>(tick), "AdvancedEngineering"));
    (void)rm::app::advanceMatch(runner, tick++, 0);
    REQUIRE(job.scene.enhancementWork.size() == 1);
    CHECK(rm::test::asFloat(job.scene.enhancementWork[0].buildTimeRemaining)
          == Catch::Approx(8.0f));

    // Funded, the work drains and installs; the registry records the slot (C-379's
    // `SimUnitEnhancements[id][slot]`).
    job.scene.economies[0].stored = {rm::test::mag(100.0f), rm::test::mag(1000.0f)};
    for (int i = 0; i < 12 && !job.scene.enhancementWork.empty(); ++i) {
        (void)rm::app::advanceMatch(runner, tick++, 0);
    }
    REQUIRE(job.scene.enhancementWork.empty());
    CHECK(job.scene.store.enhancements()[acu.index].at("LCH") == "AdvancedEngineering");

    // What the player got for the resources: a faster build rate (NewBuildRate 30
    // replaces the authored 10), +20 maximum health, and the T2 build tier.
    CHECK(rm::test::asFloat(
              rm::sim::effectiveBuildPerTick(job.scene.store, job.scene.catalog, acu.index))
          == Catch::Approx(3.0f));  // 30/s at the app's 10 Hz
    CHECK(rm::test::near(job.scene.store.health()[acu.index].maximum) == 120.0f);
    CHECK(rm::sim::canBuild(job.scene.store, job.scene.catalog, acu.index, defs[t2]));
    CHECK_FALSE(rm::sim::canBuild(job.scene.store, job.scene.catalog, acu.index, defs[t3]));

    // The follow-on in the same slot: T3Engineering's prerequisite is the installed
    // T2 suite, and installing it REPLACES the occupant — one slot, one name — and
    // walks the prereq chain so both tiers stay buildable (C-379's per-script
    REQUIRE(issueEnhancement(job, acu, static_cast<rm::TickIndex>(tick), "T3Engineering"));
    job.scene.economies[0].stored = {rm::test::mag(100.0f), rm::test::mag(1000.0f)};
    // The first tick creates the work row; the rest drain it to install.
    (void)rm::app::advanceMatch(runner, tick++, 0);
    for (int i = 0; i < 14 && !job.scene.enhancementWork.empty(); ++i) {
        (void)rm::app::advanceMatch(runner, tick++, 0);
    }
    REQUIRE(job.scene.enhancementWork.empty());
    CHECK(job.scene.store.enhancements()[acu.index].at("LCH") == "T3Engineering");
    CHECK(job.scene.store.enhancements()[acu.index].size() == 1);
    CHECK(rm::test::asFloat(
              rm::sim::effectiveBuildPerTick(job.scene.store, job.scene.catalog, acu.index))
          == Catch::Approx(9.0f));  // 90/s at 10 Hz — the T2 buff is gone, not stacked
    CHECK(rm::sim::canBuild(job.scene.store, job.scene.catalog, acu.index, defs[t2]));
    CHECK(rm::sim::canBuild(job.scene.store, job.scene.catalog, acu.index, defs[t3]));
}

TEST_CASE("C-378/C-376: stopping an enhancement mid-work keeps the spent resources "
          "and installs nothing", "[fa-progress][enhancement]") {
    Scenario job;
    const auto acu = job.spawn(commanderDef(), 300, 300);
    auto runner = job.runner();
    int tick = 0;

    REQUIRE(issueEnhancement(job, acu, static_cast<rm::TickIndex>(tick), "AdvancedEngineering"));
    (void)rm::app::advanceMatch(runner, tick++, 0);
    REQUIRE(job.scene.enhancementWork.size() == 1);

    // One funded tick: the demand is 10 mass and 10 energy — C-253's retail bug
    // prices the mass line at the ENERGY cost (80 over 8 s at 10/s build rate),
    // not the blueprint's 8. Stored 8 mass binds at ratio 0.8, so 8 mass and 8
    // energy are gone. Progress consumes the PREVIOUS tick's grant, so the bar
    // moves on the tick after the money does.
    job.scene.economies[0].stored = {rm::test::mag(8.0f), rm::test::mag(80.0f)};
    (void)rm::app::advanceMatch(runner, tick++, 0);
    (void)rm::app::advanceMatch(runner, tick++, 0);
    CHECK(rm::test::asFloat(job.scene.enhancementWork[0].buildTimeRemaining) < 8.0f);

    // The player's cancel is a Stop: the task's OnDestroy drops the work and — per
    // C-378's "no refund" — the consumed resources stay consumed.
    REQUIRE(rm::app::issueMove(job.scene, acu, 0, static_cast<rm::TickIndex>(tick),
                               rm::sim::Fx{}, rm::sim::Fx{}, false,
                               rm::sim::CommandKind::Stop));
    (void)rm::app::advanceMatch(runner, tick++, 0);
    CHECK(job.scene.enhancementWork.empty());
    CHECK(job.scene.store.orders()[acu.index].empty());
    CHECK(job.scene.store.enhancements()[acu.index].empty());
    CHECK(rm::test::asFloat(job.scene.economies[0].stored.mass) < 8.0f);
    CHECK(rm::test::asFloat(job.scene.economies[0].stored.energy) < 80.0f);
}

TEST_CASE("C-378/C-379: the paid Remove pseudo-enhancement frees the slot for a "
          "re-install", "[fa-progress][enhancement]") {
    Scenario job;
    const auto acu = job.spawn(commanderDef(), 300, 300);
    auto runner = job.runner();
    int tick = 0;
    const auto install = [&](const std::string& name) {
        REQUIRE(issueEnhancement(job, acu, static_cast<rm::TickIndex>(tick), name));
        job.scene.economies[0].stored = {rm::test::mag(100.0f), rm::test::mag(1000.0f)};
        // The first tick creates the work row; the rest drain it to install.
        (void)rm::app::advanceMatch(runner, tick++, 0);
        for (int i = 0; i < 14 && !job.scene.enhancementWork.empty(); ++i) {
            (void)rm::app::advanceMatch(runner, tick++, 0);
        }
        REQUIRE(job.scene.enhancementWork.empty());
    };

    install("AdvancedEngineering");
    REQUIRE(job.scene.store.enhancements()[acu.index].at("LCH") == "AdvancedEngineering");
    const rm::sim::Mag enhancedMax = job.scene.store.health()[acu.index].maximum;

    // The UI's replacement protocol's first half (C-378): `<old>Remove`, a paid
    // pseudo-enhancement whose script branch undoes the effect — the +20 health goes
    // away with the suite and the slot ends empty.
    install("AdvancedEngineeringRemove");
    CHECK(job.scene.store.enhancements()[acu.index].empty());
    CHECK(job.scene.store.health()[acu.index].maximum < enhancedMax);
    CHECK(rm::test::near(job.scene.store.health()[acu.index].maximum) == 100.0f);

    // And the second half: the freed slot takes the same enhancement again — the
    // two-command replacement round-trips through the sim, which is the part of
    // C-378 the sim owns (the 0.5 s UI spacing is presentation).
    install("AdvancedEngineering");
    CHECK(job.scene.store.enhancements()[acu.index].at("LCH") == "AdvancedEngineering");
    CHECK(rm::test::near(job.scene.store.health()[acu.index].maximum) == 120.0f);
}

TEST_CASE("C-380: veteran regeneration stacks with an installed enhancement's regen",
          "[fa-progress][veterancy]") {
    // C-380's regen rule is `bp.Defense.RegenRate + Σ buffs`, recomputed — the
    // veteran ladder and the enhancement's NewRegenRate are both adds into the same
    // per-tick figure, so a veteran enhanced commander regenerates the sum.
    rm::test::Roster roster;
    roster.rate = rm::sim::TickRate{10};
    rm::unitdef::UnitDef def = commanderDef();
    def.regenPerSecond = 1.0f;
    def.veterancyKills = {2, 4, 6, 8, 10};
    const auto type = roster.addType(def);
    const auto unit = roster.add(type, 40.0f, 40.0f, 0, 100.0f);
    roster.store.health()[unit.index].current = rm::test::mag(50.0f);

    REQUIRE(rm::sim::installEnhancement(roster.store, roster.catalog, unit,
                                      "AdvancedEngineering"));
    for (int i = 0; i < 2; ++i) {
        (void)rm::sim::creditKill(roster.store, roster.catalog, unit, nullptr);
    }
    REQUIRE(roster.store.health()[unit.index].veterancy.level == 1);

    // 1 (blueprint) + 2 (VeterancyRegen1) + 2 (NewRegenRate) = 5 a second, 0.5 a
    // tick at 10 Hz.
    const rm::sim::Mag before = roster.store.health()[unit.index].current;
    rm::sim::tickRegeneration(roster.store, roster.catalog, roster.rate);
    CHECK(roster.store.health()[unit.index].current - before == rm::test::mag(0.5f));
}

TEST_CASE("C-380/C-028: promotion heals on top of enhancement health, recomputed "
          "from base", "[fa-progress][veterancy]") {
    // The two buff systems compose the way retail's do: the veteran maximum is
    // (blueprint + enhancement adds) × (1 + 0.1/level), not either half alone — and
    // the promotion still heals by the increase.
    rm::test::Roster roster;
    rm::unitdef::UnitDef def = commanderDef();
    def.veterancyKills = {2, 4, 6, 8, 10};
    const auto type = roster.addType(def);
    const auto unit = roster.add(type, 40.0f, 40.0f, 0, 100.0f);

    REQUIRE(rm::sim::installEnhancement(roster.store, roster.catalog, unit,
                                      "AdvancedEngineering"));
    REQUIRE(rm::test::near(roster.store.health()[unit.index].maximum) == 120.0f);

    (void)rm::sim::creditKill(roster.store, roster.catalog, unit, nullptr);
    (void)rm::sim::creditKill(roster.store, roster.catalog, unit, nullptr);
    CHECK(roster.store.health()[unit.index].veterancy.level == 1);
    // (100 + 20) × 1.1 = 132, and the +12 increase lands in current health too.
    CHECK(rm::test::near(roster.store.health()[unit.index].maximum) == 132.0f);
    CHECK(rm::test::near(roster.store.health()[unit.index].current) == 132.0f);
}

TEST_CASE("C-376: an enhancement waits for the unit to stand still before work "
          "begins", "[fa-progress][enhancement]") {
    // EnhanceTask.lua's `Stopping` phase: `IsMobile() and IsMoving()` gates
    // `OnWorkBegin`, with `Navigator:AbortMove()` doing the stopping. A queued
    // script reaches dispatch without the intake teardown, so the commander is
    // still under way when the task first ticks — the work row must NOT appear
    // until it stands still.
    Scenario job;
    const auto acu = job.spawn(commanderDef(), 300, 300);
    auto runner = job.runner();
    int tick = 0;

    // Orderless motion — the shape a congestion sidestep or a direct `orderTo`
    // leaves: `moving` set with no order at the queue head.
    const rm::sim::Terrain terrain{job.field};
    rm::sim::orderTo(job.scene.store.motion()[acu.index], terrain,
                     rm::sim::fxFromFloat(400.0f), rm::sim::fxFromFloat(300.0f));
    REQUIRE(job.scene.store.motion()[acu.index].moving);

    REQUIRE(issueEnhancement(job, acu, static_cast<rm::TickIndex>(tick),
                             "AdvancedEngineering", /*queued=*/true));
    (void)rm::app::advanceMatch(runner, tick++, 0);
    // The dispatch beat is spent stopping: no progress bar yet, and the abort
    // has already taken the destination away.
    CHECK(job.scene.enhancementWork.empty());
    CHECK_FALSE(job.scene.store.motion()[acu.index].moving);

    // Standing still, the next beat runs OnWorkBegin and the bar appears.
    (void)rm::app::advanceMatch(runner, tick++, 0);
    REQUIRE(job.scene.enhancementWork.size() == 1);
    CHECK(rm::test::asFloat(job.scene.enhancementWork[0].buildTimeRemaining)
          == Catch::Approx(8.0f));

    // And the install completes normally once funded.
    job.scene.economies[0].stored = {rm::test::mag(100.0f), rm::test::mag(1000.0f)};
    for (int i = 0; i < 14 && !job.scene.enhancementWork.empty(); ++i) {
        (void)rm::app::advanceMatch(runner, tick++, 0);
    }
    REQUIRE(job.scene.enhancementWork.empty());
    CHECK(job.scene.store.enhancements()[acu.index].at("LCH") == "AdvancedEngineering");
}

TEST_CASE("C-254: a dead unit's enhancement registry entry leaves with it",
          "[fa-progress][enhancement]") {
    // `SimUnitEnhancements[id]` is live unit state: `Unit::OnKilled` drops the
    // table entry, so a recycled slot never inherits a tombstone's upgrades.
    rm::test::Roster roster;
    const auto type = roster.addType(commanderDef());
    const auto unit = roster.add(type, 40.0f, 40.0f, 0, 100.0f);

    REQUIRE(rm::sim::installEnhancement(roster.store, roster.catalog, unit,
                                      "AdvancedEngineering"));
    REQUIRE_FALSE(roster.store.enhancements()[unit.index].empty());

    roster.store.kill(unit);
    CHECK(roster.store.enhancements()[unit.index].empty());

    // The slot recycles clean: the replacement commander starts with no
    // enhancements rather than the corpse's LCH suite.
    const auto replacement = roster.add(type, 60.0f, 60.0f, 0, 100.0f);
    CHECK(roster.store.enhancements()[replacement.index].empty());
}

TEST_CASE("C-263: a deficit-covering producer tops up the shortfall, clamped "
          "to its MaxMass/MaxEnergy", "[fa-progress][paragon]") {
    // `XAB1401_script.lua`'s `ResourceOn`: every 0.5 s the Paragon writes
    // `SetProductionPerSecond*` to `base + max(0, requested − income)` — the
    // script's own 20/1000 floor plus whatever the army is short — clamped to
    // the blueprint's `Economy.MaxMass`/`MaxEnergy`. The override REPLACES the
    // static rate, and each unit subtracts only its own contribution before
    // measuring, so two Paragons split the deficit rather than doubling it.
    rm::test::Roster roster;
    rm::unitdef::UnitDef paragon;
    paragon.name = "xab1401";
    paragon.categories = {"STRUCTURE"};
    paragon.health = rm::test::mag(1000.0f);
    paragon.economyMaxMassPerSecond = 10000.0f;
    paragon.economyMaxEnergyPerSecond = 1000000.0f;
    const auto paragonType = roster.addType(paragon);
    rm::unitdef::UnitDef mex;
    mex.name = "mex";
    mex.categories = {"STRUCTURE"};
    mex.health = rm::test::mag(100.0f);
    mex.producesMassPerSecond = 2.0f;
    const auto mexType = roster.addType(mex);

    const rm::HeightField field = flatField();
    const rm::sim::Terrain terrain{field};
    const rm::sim::TickRate rate{10};
    std::vector<rm::sim::Army> armies = rm::sim::freeForAll(2);
    std::vector<rm::sim::Economy> economies(2);
    std::vector<rm::sim::Resources> overrides;
    const std::vector<int> commandersEver{0, 0};
    rm::sim::Match match{.armies = armies,
                         .economies = economies,
                         .productionOverrides = &overrides,
                         .commandersEver = commandersEver,
                         .victoryMode = rm::sim::VictoryMode::Sandbox};

    // One Paragon plus a 2/s extractor. Demand 10 mass/tick: the deficit is
    // 10 − 0.2 = 9.8/tick, so the override lands at 2 + 9.8 = 11.8 — the
    // script's 20/s floor plus the shortfall, all at 10 Hz.
    const auto para = roster.add(paragonType, 40.0f, 40.0f, 0, 1000.0f);
    (void)roster.add(mexType, 60.0f, 40.0f, 0, 100.0f);
    economies[0].requestedLastTick = {.mass = rm::test::mag(10.0f),
                                    .energy = rm::test::mag(0.0f)};
    (void)rm::sim::tickSkirmish(roster.store, roster.catalog, match, terrain, rate, 0);
    REQUIRE(overrides.size() > para.index);
    CHECK(rm::test::asFloat(overrides[para.index].mass) == Catch::Approx(11.8f));
    CHECK(rm::test::asFloat(overrides[para.index].energy) == Catch::Approx(100.0f));
    // The override replaced the static rate: income is mex 0.2 + paragon 11.8
    // — demand plus the floor, exactly retail's steady state.
    CHECK(rm::test::asFloat(economies[0].incomePerTick.mass) == Catch::Approx(12.0f));

    // Demand beyond the cap clamps to MaxMass — 10000/s = 1000/tick.
    economies[0].requestedLastTick = {.mass = rm::test::mag(100000.0f),
                                    .energy = rm::test::mag(0.0f)};
    (void)rm::sim::tickSkirmish(roster.store, roster.catalog, match, terrain, rate, 5);
    CHECK(rm::test::asFloat(overrides[para.index].mass) == Catch::Approx(1000.0f));

    // A second Paragon keeps the same total: each subtracts only its own
    // contribution, so the pair covers the deficit once — combined override
    // stays at need + floor = 11.8, the same fixed point retail's two threads
    // reach (retail splits it symmetrically; ours converges asymmetrically —
    // the observable income is identical either way).
    const auto para2 = roster.add(paragonType, 80.0f, 40.0f, 0, 1000.0f);
    economies[0].requestedLastTick = {.mass = rm::test::mag(10.0f),
                                    .energy = rm::test::mag(0.0f)};
    for (rm::TickIndex tick = 6; tick < 16; ++tick) {
        (void)rm::sim::tickSkirmish(roster.store, roster.catalog, match, terrain, rate, tick);
    }
    const float total = rm::test::asFloat(overrides[para.index].mass)
                      + rm::test::asFloat(overrides[para2.index].mass);
    CHECK(total == Catch::Approx(11.8f).margin(0.05f));

    // The override rides the save: a mid-match snapshot keeps the recomputed
    // rate rather than restarting at the floor.
    rm::sim::SaveState state;
    state.tick = 16;
    state.productionOverrides = overrides;
    const std::vector<std::byte> bytes = rm::sim::SaveState::encode(state);
    const std::optional<rm::sim::SaveState> decoded = rm::sim::SaveState::decode(bytes);
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->productionOverrides.has_value());
    CHECK(rm::test::asFloat(decoded->productionOverrides->at(para.index).mass)
          == Catch::Approx(rm::test::asFloat(overrides[para.index].mass)));
}
