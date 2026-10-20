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
#include "core/sim/Combat.hpp"
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

TEST_CASE("C-252: a paused enhancement holds its progress, bills nothing, and resumes",
          "[fa-progress][enhancement]") {
    // `EnhanceTask.lua`'s `Enhancing.TaskTick` returns `Wait` while `IsPaused()`:
    // the order stands, the bar freezes, the bank is untouched, and unpausing
    // resumes the same row — the unpaused half of C-252's stationary+unpaused
    // gate, through the same ToggleProduction sink a click uses.
    Scenario job;
    const auto acu = job.spawn(commanderDef(), 300, 300);
    auto runner = job.runner();
    int tick = 0;

    REQUIRE(issueEnhancement(job, acu, static_cast<rm::TickIndex>(tick), "AdvancedEngineering"));
    (void)rm::app::advanceMatch(runner, tick++, 0);
    REQUIRE(job.scene.enhancementWork.size() == 1);

    // One funded beat moves the bar off full time.
    job.scene.economies[0].stored = {rm::test::mag(100.0f), rm::test::mag(1000.0f)};
    (void)rm::app::advanceMatch(runner, tick++, 0);
    (void)rm::app::advanceMatch(runner, tick++, 0);
    CHECK(rm::test::asFloat(job.scene.enhancementWork[0].buildTimeRemaining) < 8.0f);

    // The player's hold: ToggleProduction on the enhancing commander — a builder,
    // so the pause cell is live for it.
    const auto togglePause = [&] {
        return rm::app::submitCommand(job.scene, rm::sim::CommandIssue{
            .tick = static_cast<rm::TickIndex>(tick),
            .phase = rm::sim::CommandPhase::PreTick,
            .source = static_cast<rm::CommandSource>(0),
            .player = 0,
            .kind = rm::sim::CommandKind::ToggleProduction,
            .units = {acu},
        }).has_value();
    };
    REQUIRE(togglePause());
    (void)rm::app::advanceMatch(runner, tick++, 0);
    REQUIRE(job.scene.store.productionPaused(acu));

    // Held, not cancelled: the order stands and the row survives.
    CHECK_FALSE(job.scene.store.orders()[acu.index].empty());
    REQUIRE(job.scene.enhancementWork.size() == 1);
    const float held = rm::test::asFloat(job.scene.enhancementWork[0].buildTimeRemaining);

    // Funded beats while held advance nothing, bill nothing, and fire no
    // milestones — each frame is checked because the queue opens a new one
    // every tick.
    job.scene.economies[0].stored = {rm::test::mag(100.0f), rm::test::mag(1000.0f)};
    for (int i = 0; i < 4; ++i) {
        (void)rm::app::advanceMatch(runner, tick++, 0);
        REQUIRE(job.scene.enhancementWork.size() == 1);
        CHECK(rm::test::asFloat(job.scene.enhancementWork[0].buildTimeRemaining)
              == Catch::Approx(held));
        CHECK(job.scene.events.count(rm::sim::EventKind::EnhancementProgress) == 0);
    }
    CHECK(rm::test::asFloat(job.scene.economies[0].stored.mass) >= 100.0f);
    CHECK(rm::test::asFloat(job.scene.economies[0].stored.energy) >= 1000.0f);

    // Releasing the hold resumes the same row to a normal install.
    REQUIRE(togglePause());
    (void)rm::app::advanceMatch(runner, tick++, 0);
    CHECK_FALSE(job.scene.store.productionPaused(acu));
    job.scene.economies[0].stored = {rm::test::mag(100.0f), rm::test::mag(1000.0f)};
    for (int i = 0; i < 14 && !job.scene.enhancementWork.empty(); ++i) {
        (void)rm::app::advanceMatch(runner, tick++, 0);
    }
    REQUIRE(job.scene.enhancementWork.empty());
    CHECK(job.scene.store.enhancements()[acu.index].at("LCH") == "AdvancedEngineering");
}

TEST_CASE("C-255: installing ShieldHeavy stands its shield up without a toggle cap",
          "[fa-progress][enhancement]") {
    // Retail's heavy-shield branch runs `CreateShield` with no `AddToggleCap`
    // (the scripts only take the cap away on Remove) — but it still stands
    // the bubble up charging like every other shield branch.
    rm::test::Roster roster;
    roster.rate = rm::sim::TickRate{10};
    rm::unitdef::UnitDef def;
    def.name = "test_commander";
    def.health = rm::test::mag(100.0f);
    const auto params = rm::lua::parseTable(
        "{ ShieldMaxHealth=800, ShieldRegenRate=10, ShieldRechargeTime=20, "
        "  ShieldRegenStartTime=5, ShieldSize=12, ShieldVerticalOffset=-1 }");
    REQUIRE(params);
    def.enhancements.push_back({.name = "ShieldHeavy", .slot = "Back",
        .buildCostMass = rm::test::mag(1.0f), .buildCostEnergy = rm::test::mag(1.0f),
        .buildTime = rm::test::fx(1.0f), .parameters = *params});
    const auto type = roster.addType(def);
    const auto unit = roster.add(type, 40.0f, 40.0f, 0, 100.0f);
    REQUIRE(rm::sim::installEnhancement(roster.store, roster.catalog, unit, "ShieldHeavy"));
    CHECK(roster.store.health()[unit.index].shield.maximum == rm::test::mag(800.0f));
    CHECK(rm::sim::shieldFor(roster.store, roster.catalog, unit.index).maximum
          == rm::test::mag(800.0f));
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

TEST_CASE("C-265: a dying Ythotha spawns an invulnerable Othuy that destroys "
          "itself when its Lifetime runs out", "[fa-progress][othuy]") {
    // `XSL0401_Script.lua:124-135`: the Ythotha's `DeathThread` `CreateUnitHPR`s
    // an XSL0402 Othuy at its own position under the same army. The Othuy's
    // script (`seraphimunits.lua:614-700`) calls `SetCanTakeDamage(false)` /
    // `SetCanBeKilled(false)` and `self:Destroy()`s when the blueprint's
    // `Lifetime` (30 s retail) expires — no wreck, no kill, no report.
    Scenario job;
    rm::unitdef::UnitDef ythotha;
    ythotha.name = "XSL0401";
    ythotha.categories = {"EXPERIMENTAL", "MOBILE"};  // sorted: hasCategory binary-searches
    ythotha.health = rm::test::mag(100.0f);
    ythotha.collisionRadiusElmos = 4.0f;  // a corpse needs a radius to be retired
    ythotha.deathSpawn = "xsl0402";  // lowercase on purpose: the id is case-folded
    rm::unitdef::UnitDef othuy;
    othuy.name = "XSL0402";
    othuy.categories = {"MOBILE"};
    othuy.health = rm::test::mag(500.0f);
    othuy.collisionRadiusElmos = 3.0f;  // a body the blast can overlap
    othuy.invulnerable = true;
    othuy.lifetimeSeconds = 2.0f;  // short, so the test watches it expire
    const rm::UnitTypeIndex othuyType = job.registerType(othuy);
    const auto giant = job.spawn(ythotha, 300.0f, 300.0f, 0);
    // A bystander beside the corpse, same army: proves the blast gate rejects
    // the Othuy specifically rather than muting damage outright.
    rm::unitdef::UnitDef tank;
    tank.name = "bystander";
    tank.categories = {"MOBILE"};
    tank.health = rm::test::mag(100.0f);
    tank.collisionRadiusElmos = 2.0f;
    const auto bystander = job.spawn(tank, 304.0f, 300.0f, 0);
    auto runner = job.runner();
    int tick = 0;

    // The Ythotha dies this tick; the Othuy stands where it fell.
    job.scene.store.health()[giant.index].current = rm::sim::Mag{};
    (void)rm::app::advanceMatch(runner, tick++, 0);
    rm::sim::UnitId spawned{};
    for (rm::UnitIndex slot = 0; slot < job.scene.store.slotCount(); ++slot) {
        if (job.scene.store.slotAlive(slot)
            && job.scene.store.typeAt(slot) == othuyType) {
            spawned = job.scene.store.idAt(slot);
        }
    }
    REQUIRE(job.scene.store.alive(spawned));
    CHECK(rm::test::asFloat(job.scene.store.transforms()[spawned.index].x)
          == Catch::Approx(300.0f));
    CHECK(rm::test::asFloat(job.scene.store.transforms()[spawned.index].z)
          == Catch::Approx(300.0f));
    CHECK(job.scene.store.motion()[spawned.index].armyIndex == 0);

    // `SetCanTakeDamage(false)`: a blast centred on the Othuy hurts the
    // bystander and leaves the Othuy untouched. The reindex first is load-
    // bearing: the Othuy spawned after the tick's last spatial rebuild, so
    // without it `damageArea`'s grid query never reaches the new unit and the
    // gate below would pass untested.
    job.scene.store.reindex(rm::sim::Fx::fromInt(128));
    const rm::sim::Mag dealt = rm::sim::damageArea(
        {rm::sim::fxFromFloat(300.0f), rm::sim::Fx{}, rm::sim::fxFromFloat(300.0f)},
        rm::sim::fxFromFloat(10.0f), rm::test::mag(50.0f), 1, job.scene.store,
        job.scene.armies, rm::sim::UnitId{}, nullptr, &job.scene.catalog);
    CHECK(rm::test::near(dealt) == 50.0f);
    CHECK(rm::test::near(job.scene.store.health()[spawned.index].current) == 500.0f);
    CHECK(rm::test::near(job.scene.store.health()[bystander.index].current) == 50.0f);

    // `Lifetime` counts down and the Othuy `Destroy()`s itself: gone, with no
    // wreck and no death report — the Ythotha's wreck is the only feature.
    const std::size_t wrecksBefore = job.scene.features.size();
    const std::size_t destroyedBefore = runner.unitsDestroyed;
    for (int i = 0; i < 30 && job.scene.store.alive(spawned); ++i) {
        (void)rm::app::advanceMatch(runner, tick++, 0);
    }
    CHECK_FALSE(job.scene.store.alive(spawned));
    CHECK(job.scene.features.size() == wrecksBefore);
    CHECK(runner.unitsDestroyed == destroyedBefore);
}

TEST_CASE("C-261: a finished crab egg hatches its Economy.BuildUnit and is "
          "gone", "[fa-progress][crabegg]") {
    // `cybranunits.lua:355-400`: the Megalith's eggs are `CConstructionEggUnit`s
    // — FACTORY+STRUCTURE units it builds like any other product — whose
    // `OnStopBeingBuilt` `CreateUnitHPR`s `bp.Economy.BuildUnit` at the egg's
    // position under the same army, then `Destroy()`s the egg (no wreck).
    Scenario job;
    rm::unitdef::UnitDef megalith;
    megalith.name = "XRL0403";
    megalith.categories = {"EXPERIMENTAL", "FACTORY", "MOBILE"};
    megalith.health = rm::test::mag(1000.0f);
    megalith.buildRate = 100.0f;
    megalith.motion = rm::unitdef::MotionType::Land;  // a None mover has no grid to build on
    megalith.speedElmosPerSecond = 20.0f;
    megalith.buildableCategory = {rm::unitdef::parseCategoryTerm("CRABEGG")};
    rm::unitdef::UnitDef egg;
    egg.name = "XRL0002";
    egg.categories = {"CRABEGG", "FACTORY", "STRUCTURE"};
    egg.health = rm::test::mag(50.0f);
    egg.collisionRadiusElmos = 4.0f;
    egg.buildCostMass = rm::test::mag(10.0f);
    egg.buildCostEnergy = rm::test::mag(10.0f);
    egg.buildTime = rm::test::mag(1.0f);
    egg.economyBuildUnit = "xrl0305";  // lowercase on purpose: case-folded at use
    rm::unitdef::UnitDef crab;
    crab.name = "XRL0305";
    crab.categories = {"MOBILE"};
    crab.health = rm::test::mag(200.0f);
    crab.collisionRadiusElmos = 3.0f;
    const auto eggType = job.registerType(egg);
    (void)job.registerType(crab);
    const auto layer = job.spawn(megalith, 300.0f, 300.0f, 0);
    auto runner = job.runner();
    int tick = 0;

    job.scene.economies[0].stored = {rm::test::mag(1000.0f), rm::test::mag(1000.0f)};
    REQUIRE(rm::app::issueBuild(job.scene, layer, 0, static_cast<rm::TickIndex>(tick),
                              eggType, rm::sim::fxFromFloat(320.0f),
                              rm::sim::fxFromFloat(300.0f)));
    rm::sim::UnitId hatched{};
    for (int i = 0; i < 60 && !job.scene.store.alive(hatched); ++i) {
        (void)rm::app::advanceMatch(runner, tick++, 0);
        for (rm::UnitIndex slot = 0; slot < job.scene.store.slotCount(); ++slot) {
            const rm::unitdef::UnitDef* def =
                job.scene.catalog.def(job.scene.store.typeAt(slot));
            if (job.scene.store.slotAlive(slot) && def != nullptr && def->name == "XRL0305") {
                hatched = job.scene.store.idAt(slot);
            }
        }
    }
    REQUIRE(job.scene.store.alive(hatched));
    // The click at x=320 snapped to the 8-elmo build grid's cell centre (the
    // egg's odd footprint): the crab stands where the egg stood, at 324.
    CHECK(rm::test::asFloat(job.scene.store.transforms()[hatched.index].x)
          == Catch::Approx(324.0f));
    CHECK(job.scene.store.motion()[hatched.index].armyIndex == 0);

    // The egg is gone — `Destroy()`, not a death: no egg unit stands, no wreck
    // was left, and the death tally never moved.
    for (rm::UnitIndex slot = 0; slot < job.scene.store.slotCount(); ++slot) {
        const rm::unitdef::UnitDef* def =
            job.scene.catalog.def(job.scene.store.typeAt(slot));
        if (job.scene.store.slotAlive(slot) && def != nullptr) {
            CHECK(def->name != "XRL0002");
        }
    }
    CHECK(job.scene.features.size() == 0);
    CHECK(runner.unitsDestroyed == 0);
}

TEST_CASE("C-436: an enhancement fires quarter-progress milestones on funded ticks only",
          "[fa-progress][enhancement]") {
    // `EnhanceTask.lua`'s `LastProgress` ratchet: `OnBuildProgress` fires at
    // 0.25/0.50/0.75, edge-triggered on the pre-tick fraction — an unfunded
    // tick advances nothing and reports nothing, and the finish itself is not
    // a fourth milestone.
    Scenario job;
    const auto acu = job.spawn(commanderDef(), 300, 300);
    auto runner = job.runner();
    int tick = 0;

    REQUIRE(issueEnhancement(job, acu, static_cast<rm::TickIndex>(tick), "AdvancedEngineering"));
    (void)rm::app::advanceMatch(runner, tick++, 0);
    REQUIRE(job.scene.enhancementWork.size() == 1);

    // Starved: no funding means no progress and no milestones.
    job.scene.economies[0].stored = {rm::sim::Mag{}, rm::sim::Mag{}};
    for (int i = 0; i < 3; ++i) (void)rm::app::advanceMatch(runner, tick++, 0);
    CHECK(job.scene.events.count(rm::sim::EventKind::EnhancementProgress) == 0);
    REQUIRE(job.scene.enhancementWork.size() == 1);

    // Funded: 8 work units at 1/tick cross the quarters on ticks 2, 4 and 6.
    // Each `advanceMatch` opens a new event frame, so the milestones are
    // collected tick by tick rather than counted at the end.
    std::size_t milestones = 0;
    std::vector<float> amounts;
    job.scene.economies[0].stored = {rm::test::mag(100.0f), rm::test::mag(1000.0f)};
    for (int i = 0; i < 12 && !job.scene.enhancementWork.empty(); ++i) {
        (void)rm::app::advanceMatch(runner, tick++, 0);
        for (const rm::sim::Event& event : job.scene.events.all()) {
            if (event.kind != rm::sim::EventKind::EnhancementProgress) continue;
            ++milestones;
            amounts.push_back(rm::test::asFloat(event.amount));
            CHECK(event.unit == acu);
            CHECK(event.army == 0);
        }
    }
    REQUIRE(job.scene.enhancementWork.empty());
    CHECK(milestones == 3);
    REQUIRE(amounts.size() == 3);
    CHECK(amounts[0] == Catch::Approx(0.25f));
    CHECK(amounts[1] == Catch::Approx(0.5f));
    CHECK(amounts[2] == Catch::Approx(0.75f));
}

TEST_CASE("C-450/OnBuildProgress: enhancement milestones carry the retail call shape",
          "[fa-progress][enhancement]") {
    // Retail's mod-compat shape is `unit:OnBuildProgress(oldProg, newProg)`
    // (`Unit.lua:1731`, `EnhanceTask.lua:66`): the pre-tick `LastProgress`
    // fraction and the post-tick fraction, not just the boundary crossed.
    // Mods override the hook, so the sim must carry both fractions (0..1) to
    // the dispatcher.
    Scenario job;
    const auto acu = job.spawn(commanderDef(), 300, 300);
    auto runner = job.runner();
    int tick = 0;

    REQUIRE(issueEnhancement(job, acu, static_cast<rm::TickIndex>(tick), "AdvancedEngineering"));
    (void)rm::app::advanceMatch(runner, tick++, 0);
    REQUIRE(job.scene.enhancementWork.size() == 1);

    // 8 work units at 1 unit/tick: quarters fall on ticks 2, 4 and 6, with
    // one funded tick (0.125) between the pre-tick edge and each boundary.
    job.scene.economies[0].stored = {rm::test::mag(100.0f), rm::test::mag(1000.0f)};
    std::vector<std::pair<float, float>> calls;
    for (int i = 0; i < 12 && !job.scene.enhancementWork.empty(); ++i) {
        (void)rm::app::advanceMatch(runner, tick++, 0);
        for (const rm::sim::Event& event : job.scene.events.all()) {
            if (event.kind != rm::sim::EventKind::EnhancementProgress) continue;
            calls.emplace_back(rm::test::asFloat(event.progressBefore),
                               rm::test::asFloat(event.progressAfter));
            CHECK(event.unit == acu);
        }
    }
    REQUIRE(job.scene.enhancementWork.empty());
    REQUIRE(calls.size() == 3);
    CHECK(calls[0].first == Catch::Approx(0.125f));
    CHECK(calls[0].second == Catch::Approx(0.25f));
    CHECK(calls[1].first == Catch::Approx(0.375f));
    CHECK(calls[1].second == Catch::Approx(0.5f));
    CHECK(calls[2].first == Catch::Approx(0.625f));
    CHECK(calls[2].second == Catch::Approx(0.75f));
}

TEST_CASE("C-255: an installed enhancement's NewSonarRadius overrides sonar",
          "[fa-progress][enhancement]") {
    // `NewSonarRadius` is a declared retail enhancement field
    // (`UnitBlueprint.lua:869`) in the `SetIntelRadius` family alongside
    // vision/omni/jammer — the sim must apply it the same way.
    rm::test::Roster roster;
    roster.rate = rm::sim::TickRate{10};
    rm::unitdef::UnitDef def;
    def.name = "test_commander";
    def.health = rm::test::mag(100.0f);
    def.sonarRadiusElmos = 100.0f;
    const auto params = rm::lua::parseTable("{ NewSonarRadius=40 }");
    REQUIRE(params);
    def.enhancements.push_back({.name = "NaniteTorpedoTube", .slot = "Back",
        .buildCostMass = rm::test::mag(1.0f), .buildCostEnergy = rm::test::mag(1.0f),
        .buildTime = rm::test::fx(1.0f), .parameters = *params});
    const auto type = roster.addType(def);
    const auto unit = roster.add(type, 40.0f, 40.0f, 0, 100.0f);
    REQUIRE(rm::sim::installEnhancement(roster.store, roster.catalog, unit, "NaniteTorpedoTube"));
    CHECK(rm::sim::intelRadiiFor(roster.store, roster.catalog, unit.index).sonar
          == rm::sim::Fx::fromInt(320));
}

TEST_CASE("C-255: an installed enhancement's NewDamageRadius widens the blast",
          "[fa-progress][enhancement]") {
    // `NewDamageRadius` is a declared retail enhancement field
    // (`UnitBlueprint.lua:862`) authored on the shipped commanders
    // (XSL0001 `BlastAttack` 2, UEL0301 `HighExplosiveOrdnance` 2.5) for
    // `UnitWeapon:ChangeDamageRadius` (`UnitWeapon.lua:74`) — the sim must
    // apply it the way it applies `NewMaxRadius`.
    rm::test::Roster roster;
    roster.rate = rm::sim::TickRate{10};
    rm::unitdef::UnitDef def;
    def.name = "test_commander";
    def.health = rm::test::mag(100.0f);
    rm::unitdef::Weapon chrono;
    chrono.label = "ChronotronCannon";
    chrono.role = rm::unitdef::WeaponRole::DirectFire;
    chrono.maxRange = rm::sim::Fx::fromInt(30);
    chrono.rateOfFire = 1.0f;
    chrono.damage = rm::test::mag(10.0f);
    chrono.damageRadius = rm::sim::Fx{};
    def.weapons = {chrono};
    const auto params = rm::lua::parseTable("{ NewDamageRadius=2 }");
    REQUIRE(params);
    def.enhancements.push_back({.name = "BlastAttack", .slot = "LCH",
        .buildCostMass = rm::test::mag(1.0f), .buildCostEnergy = rm::test::mag(1.0f),
        .buildTime = rm::test::fx(1.0f), .parameters = *params});
    def.enhancements.push_back({.name = "BlastAttackRemove", .slot = "LCH",
        .prerequisite = "BlastAttack",
        .buildCostMass = rm::test::mag(1.0f), .buildCostEnergy = rm::test::mag(1.0f),
        .buildTime = rm::test::fx(1.0f),
        .removes = {"BlastAttack", "BlastAttackRemove"},
        .parameters = *rm::lua::parseTable("{}")});
    const auto type = roster.addType(def);
    const auto unit = roster.add(type, 40.0f, 40.0f, 0, 100.0f);
    // Before install: the authored point hit.
    CHECK(rm::sim::weaponDamageRadiusFor(roster.store, roster.catalog, unit.index, chrono)
          == rm::sim::Fx{});
    REQUIRE(rm::sim::installEnhancement(roster.store, roster.catalog, unit, "BlastAttack"));
    // `NewDamageRadius` is authored in ogrids — 2 ogrids = 16 elmos.
    CHECK(rm::sim::weaponDamageRadiusFor(roster.store, roster.catalog, unit.index, chrono)
          == rm::sim::Fx::fromInt(16));
    // The Remove carries no radius, so the blast falls back to the base.
    REQUIRE(rm::sim::installEnhancement(roster.store, roster.catalog, unit, "BlastAttackRemove"));
    CHECK(rm::sim::weaponDamageRadiusFor(roster.store, roster.catalog, unit.index, chrono)
          == rm::sim::Fx{});
}
TEST_CASE("C-255: NewDamageRadius adds to a nonzero authored blast",
          "[fa-progress][enhancement]") {
    // Retail `UEL0301_script.lua:202-204`: `HighExplosiveOrdnance` widens its
    // cannon with `wep:AddDamageRadiusMod(bp.NewDamageRadius)`, and retail
    // `weapon.lua:425-427` adds the mod to the authored radius
    // (`DamageRadius + DamageRadiusMod`) — additive on top of a nonzero base,
    // not an absolute write.
    rm::test::Roster roster;
    roster.rate = rm::sim::TickRate{10};
    rm::unitdef::UnitDef def;
    def.name = "test_heo_gunner";
    def.health = rm::test::mag(100.0f);
    rm::unitdef::Weapon cannon;
    cannon.label = "RightHeavyPlasmaCannon";
    cannon.role = rm::unitdef::WeaponRole::DirectFire;
    cannon.maxRange = rm::sim::Fx::fromInt(30);
    cannon.rateOfFire = 1.0f;
    cannon.damage = rm::test::mag(10.0f);
    cannon.damageRadius = rm::sim::Fx::fromInt(80);
    def.weapons = {cannon};
    const auto params = rm::lua::parseTable("{ NewDamageRadius=2.5 }");
    REQUIRE(params);
    def.enhancements.push_back({.name = "HighExplosiveOrdnance", .slot = "RCH",
        .buildCostMass = rm::test::mag(1.0f), .buildCostEnergy = rm::test::mag(1.0f),
        .buildTime = rm::test::fx(1.0f), .parameters = *params});
    const auto type = roster.addType(def);
    const auto unit = roster.add(type, 40.0f, 40.0f, 0, 100.0f);
    CHECK(rm::sim::weaponDamageRadiusFor(roster.store, roster.catalog, unit.index, cannon)
          == rm::sim::Fx::fromInt(80));
    REQUIRE(rm::sim::installEnhancement(roster.store, roster.catalog, unit, "HighExplosiveOrdnance"));
    // 2.5 ogrids = 20 elmos on top of the 80-elmo authored blast.
    CHECK(rm::sim::weaponDamageRadiusFor(roster.store, roster.catalog, unit.index, cannon)
          == rm::sim::Fx::fromInt(100));
}
TEST_CASE("C-255: HighExplosiveOrdnanceRemove leaves the authored blast",
          "[fa-progress][enhancement]") {
    // Retail `UEL0301_script.lua:205-207`: the Remove branch runs the SAME
    // `wep:AddDamageRadiusMod(bp.NewDamageRadius)` call with its OWN table,
    // which authors `NewDamageRadius = -4` (`UEL0301_unit.bp`) — but retail's
    // own registry (`Unit.lua:2032-2059`) then erases BOTH names: the Remove's
    // `RemoveEnhancements` lists `HighExplosiveOrdnance` AND itself, and the
    // slot write lands the Remove name first. The -4 never survives as
    // installed state; the net observable is the authored blast. The sim's
    // slot map already models exactly this, so this pins install-widens and
    // Remove-restores against the real -4 row.
    rm::test::Roster roster;
    roster.rate = rm::sim::TickRate{10};
    rm::unitdef::UnitDef def;
    def.name = "test_heo_gunner";
    def.health = rm::test::mag(100.0f);
    rm::unitdef::Weapon cannon;
    cannon.label = "RightHeavyPlasmaCannon";
    cannon.role = rm::unitdef::WeaponRole::DirectFire;
    cannon.maxRange = rm::sim::Fx::fromInt(30);
    cannon.rateOfFire = 1.0f;
    cannon.damage = rm::test::mag(10.0f);
    cannon.damageRadius = rm::sim::Fx::fromInt(80);
    def.weapons = {cannon};
    const auto installParams = rm::lua::parseTable("{ NewDamageRadius=2.5 }");
    const auto removeParams = rm::lua::parseTable("{ NewDamageRadius=-4 }");
    REQUIRE(installParams);
    REQUIRE(removeParams);
    def.enhancements.push_back({.name = "HighExplosiveOrdnance", .slot = "RCH",
        .buildCostMass = rm::test::mag(1.0f), .buildCostEnergy = rm::test::mag(1.0f),
        .buildTime = rm::test::fx(1.0f), .parameters = *installParams});
    def.enhancements.push_back({.name = "HighExplosiveOrdnanceRemove", .slot = "RCH",
        .prerequisite = "HighExplosiveOrdnance",
        .buildCostMass = rm::test::mag(1.0f), .buildCostEnergy = rm::test::mag(1.0f),
        .buildTime = rm::test::fx(1.0f),
        .removes = {"HighExplosiveOrdnance", "HighExplosiveOrdnanceRemove"},
        .parameters = *removeParams});
    const auto type = roster.addType(def);
    const auto unit = roster.add(type, 40.0f, 40.0f, 0, 100.0f);
    REQUIRE(rm::sim::installEnhancement(roster.store, roster.catalog, unit, "HighExplosiveOrdnance"));
    // 2.5 ogrids = 20 elmos on top of the 80-elmo authored blast.
    CHECK(rm::sim::weaponDamageRadiusFor(roster.store, roster.catalog, unit.index, cannon)
          == rm::sim::Fx::fromInt(100));
    REQUIRE(rm::sim::installEnhancement(roster.store, roster.catalog, unit, "HighExplosiveOrdnanceRemove"));
    // The Remove erases itself with its target: the authored blast returns.
    CHECK(rm::sim::weaponDamageRadiusFor(roster.store, roster.catalog, unit.index, cannon)
          == rm::sim::Fx::fromInt(80));
}

TEST_CASE("C-378: a dead owner's in-flight enhancement work retires with it",
          "[fa-progress][enhancement]") {
    // The install/fund/work/cancel edge the lifecycle owns: a unit killed
    // mid-work leaves no funded work row behind — the C-254 registry rule
    // extended to the work vector, so a recycled slot inherits neither the
    // upgrades nor their half-paid bill.
    rm::test::Roster roster;
    const auto type = roster.addType(commanderDef());
    const auto unit = roster.add(type, 40.0f, 40.0f, 0, 100.0f);
    std::vector<rm::sim::EnhancementWork> work{
        {.owner = unit, .name = "AdvancedEngineering",
         .totalBuildTime = rm::test::mag(8.0f),
         .buildTimeRemaining = rm::test::mag(4.0f)}};
    roster.store.kill(unit);
    (void)rm::sim::retireDeadEnhancementWork(roster.store, work);
    CHECK(work.empty());
}
TEST_CASE("C-255: NaniteTorpedoTube and MicrowaveLaserGenerator arm their guns",
          "[fa-progress][enhancement]") {
    // Retail `URL0001_script.lua:459-467`: `NaniteTorpedoTube` runs
    // `SetWeaponEnabledByLabel('Torpedo', true)` beside its sonar enable,
    // and `MicrowaveLaserGenerator` enables the `MLG` label — the sim's
    // script-effect table must gate both labels the same way.
    rm::test::Roster roster;
    rm::unitdef::UnitDef def;
    def.name = "test_commander";
    def.health = rm::test::mag(100.0f);
    rm::unitdef::Weapon torpedo;
    torpedo.label = "Torpedo";
    torpedo.role = rm::unitdef::WeaponRole::DirectFire;
    torpedo.maxRange = rm::sim::Fx::fromInt(50);
    torpedo.rateOfFire = 1.0f;
    torpedo.damage = rm::test::mag(10.0f);
    rm::unitdef::Weapon mlg = torpedo;
    mlg.label = "MLG";
    def.weapons = {torpedo, mlg};
    def.enhancements.push_back({.name = "NaniteTorpedoTube", .slot = "Back",
        .buildCostMass = rm::test::mag(1.0f), .buildCostEnergy = rm::test::mag(1.0f),
        .buildTime = rm::test::fx(1.0f), .parameters = *rm::lua::parseTable("{}")});
    def.enhancements.push_back({.name = "MicrowaveLaserGenerator", .slot = "RCH",
        .buildCostMass = rm::test::mag(1.0f), .buildCostEnergy = rm::test::mag(1.0f),
        .buildTime = rm::test::fx(1.0f), .parameters = *rm::lua::parseTable("{}")});
    def.enhancements.push_back({.name = "NaniteTorpedoTubeRemove", .slot = "Back",
        .prerequisite = "NaniteTorpedoTube",
        .buildCostMass = rm::test::mag(1.0f), .buildCostEnergy = rm::test::mag(1.0f),
        .buildTime = rm::test::fx(1.0f), .removes = {"NaniteTorpedoTube"},
        .parameters = *rm::lua::parseTable("{}")});
    const auto type = roster.addType(def);
    const auto unit = roster.add(type, 40.0f, 40.0f, 0, 100.0f);
    CHECK_FALSE(rm::sim::weaponEnabledForUnit(roster.store, roster.catalog, unit.index, def.weapons[0]));
    CHECK_FALSE(rm::sim::weaponEnabledForUnit(roster.store, roster.catalog, unit.index, def.weapons[1]));
    REQUIRE(rm::sim::installEnhancement(roster.store, roster.catalog, unit, "NaniteTorpedoTube"));
    CHECK(rm::sim::weaponEnabledForUnit(roster.store, roster.catalog, unit.index, def.weapons[0]));
    CHECK_FALSE(rm::sim::weaponEnabledForUnit(roster.store, roster.catalog, unit.index, def.weapons[1]));
    REQUIRE(rm::sim::installEnhancement(roster.store, roster.catalog, unit, "MicrowaveLaserGenerator"));
    CHECK(rm::sim::weaponEnabledForUnit(roster.store, roster.catalog, unit.index, def.weapons[1]));
    REQUIRE(rm::sim::installEnhancement(roster.store, roster.catalog, unit, "NaniteTorpedoTubeRemove"));
    CHECK_FALSE(rm::sim::weaponEnabledForUnit(roster.store, roster.catalog, unit.index, def.weapons[0]));
}
TEST_CASE("C-255: StabilitySuppressant adds its damage radius mod on top of the blast",
          "[fa-progress][enhancement]") {
    // Retail `UAL0301_script.lua:173-178`: `StabilitySuppressant` runs
    // `wep:AddDamageRadiusMod(bp.NewDamageRadiusMod or 0)` on the
    // `RightReactonCannon` label, and retail `weapon.lua:287` adds the mod to
    // the blueprint radius (`DamageRadius + DamageRadiusMod`) — additive,
    // unlike `ChangeDamageRadius` which is an absolute write (UAL0301 bp
    // `NewDamageRadiusMod = 4`, Remove `-4`, base `DamageRadius = 10`).
    rm::test::Roster roster;
    roster.rate = rm::sim::TickRate{10};
    rm::unitdef::UnitDef def;
    def.name = "test_scu";
    def.health = rm::test::mag(100.0f);
    rm::unitdef::Weapon cannon;
    cannon.label = "RightReactonCannon";
    cannon.role = rm::unitdef::WeaponRole::DirectFire;
    cannon.maxRange = rm::sim::Fx::fromInt(30);
    cannon.rateOfFire = 1.0f;
    cannon.damage = rm::test::mag(10.0f);
    cannon.damageRadius = rm::sim::Fx::fromInt(10);
    def.weapons = {cannon};
    const auto params = rm::lua::parseTable("{ NewDamageRadiusMod=4 }");
    REQUIRE(params);
    def.enhancements.push_back({.name = "StabilitySuppressant", .slot = "RCH",
        .buildCostMass = rm::test::mag(1.0f), .buildCostEnergy = rm::test::mag(1.0f),
        .buildTime = rm::test::fx(1.0f), .parameters = *params});
    def.enhancements.push_back({.name = "StabilitySuppressantRemove", .slot = "RCH",
        .prerequisite = "StabilitySuppressant",
        .buildCostMass = rm::test::mag(1.0f), .buildCostEnergy = rm::test::mag(1.0f),
        .buildTime = rm::test::fx(1.0f),
        .removes = {"StabilitySuppressant", "StabilitySuppressantRemove"},
        .parameters = *rm::lua::parseTable("{ NewDamageRadiusMod=-4 }")});
    const auto type = roster.addType(def);
    const auto unit = roster.add(type, 40.0f, 40.0f, 0, 100.0f);
    CHECK(rm::sim::weaponDamageRadiusFor(roster.store, roster.catalog, unit.index, cannon)
          == rm::sim::Fx::fromInt(10));
    REQUIRE(rm::sim::installEnhancement(roster.store, roster.catalog, unit, "StabilitySuppressant"));
    // `NewDamageRadiusMod` is ogrids-authored like the weapon `DamageRadius` it
    // adds to — mod 4 is 32 elmos on top of the 10-elmo base.
    CHECK(rm::sim::weaponDamageRadiusFor(roster.store, roster.catalog, unit.index, cannon)
          == rm::sim::Fx::fromInt(42));
    REQUIRE(rm::sim::installEnhancement(roster.store, roster.catalog, unit, "StabilitySuppressantRemove"));
    CHECK(rm::sim::weaponDamageRadiusFor(roster.store, roster.catalog, unit.index, cannon)
          == rm::sim::Fx::fromInt(10));
}
TEST_CASE("C-255: a fired shot carries its enhancement's widened blast",
          "[fa-progress][enhancement]") {
    // The reader half (`weaponDamageRadiusFor`) is pinned above; this is the
    // firing half retail `UnitWeapon:ChangeDamageRadius` implies — a shot
    // launched by an enhanced unit must carry the widened radius to impact,
    // not the authored one (`XSL0001_script.lua:547-552` `BlastAttack` on
    // `ChronotronCannon`, `NewDamageRadius` 2 ogrids = 16 elmos).
    rm::test::Roster roster;
    roster.rate = rm::sim::TickRate{10};
    rm::unitdef::UnitDef gunner;
    gunner.name = "test_enhanced_gunner";
    gunner.health = rm::test::mag(100.0f);
    gunner.categories = {"LAND"};
    rm::unitdef::Weapon chrono;
    chrono.label = "ChronotronCannon";
    chrono.role = rm::unitdef::WeaponRole::DirectFire;
    chrono.targetPriorities = {{"LAND"}};
    chrono.maxRange = rm::sim::Fx::fromInt(100);
    chrono.rateOfFire = 1.0f;
    chrono.damage = rm::test::mag(10.0f);
    chrono.damageRadius = rm::sim::Fx{};
    chrono.muzzleVelocityElmosPerSecond = 100.0f;
    gunner.weapons = {chrono};
    const auto blastParams = rm::lua::parseTable("{ NewDamageRadius=2 }");
    REQUIRE(blastParams);
    gunner.enhancements.push_back({.name = "BlastAttack", .slot = "LCH",
        .buildCostMass = rm::test::mag(1.0f), .buildCostEnergy = rm::test::mag(1.0f),
        .buildTime = rm::test::fx(1.0f), .parameters = *blastParams});
    rm::unitdef::UnitDef target;
    target.name = "test_blast_target";
    target.health = rm::test::mag(100.0f);
    target.categories = {"LAND"};
    const std::vector<rm::sim::Army> armies = rm::sim::freeForAll(2);
    const auto shooter = roster.add(roster.addType(gunner), 0.0f, 0.0f, 0, 100.0f);
    const auto victim = roster.add(roster.addType(target), 0.0f, 40.0f, 1, 100.0f);
    (void)victim;
    REQUIRE(rm::sim::installEnhancement(roster.store, roster.catalog, shooter, "BlastAttack"));
    std::vector<rm::sim::Projectile> shots;
    REQUIRE(rm::sim::fireWeapons(roster.store, roster.catalog, armies, shots, roster.rate) == 1);
    REQUIRE(shots.size() == 1);
    CHECK(shots.front().damageRadiusElmos == rm::sim::Fx::fromInt(16));
}
TEST_CASE("C-255: an installed RegenAura lends its RegenPerSecond to nearby allies",
          "[fa-progress][enhancement]") {
    // Retail `XSL0001_script.lua` `RegenBuffThread`: while installed, the aura
    // re-applies `SeraphimACURegenAura` every 5 s to friendly mobiles in the
    // authored `Radius` — `RegenPercent` is `Mult * maxHealth` clamped to
    // `RegenFloor`/`RegenCeiling` (XSL0001 bp: `RegenAura` radius 15 ogrids,
    // rate 1/180, floor 0, ceiling 75).
    rm::test::Roster roster;
    roster.rate = rm::sim::TickRate{10};
    rm::unitdef::UnitDef sera;
    sera.name = "test_sera_acu";
    sera.health = rm::test::mag(1000.0f);
    sera.categories = {"COMMAND", "MOBILE"};
    sera.speedElmosPerSecond = 20.0f;
    const auto auraParams = rm::lua::parseTable(
        "{ Radius=15, RegenPerSecond=0.0055555555, RegenFloor=0, RegenCeiling=75 }");
    REQUIRE(auraParams);
    sera.enhancements.push_back({.name = "RegenAura", .slot = "RCH",
        .buildCostMass = rm::test::mag(1.0f), .buildCostEnergy = rm::test::mag(1.0f),
        .buildTime = rm::test::fx(1.0f), .parameters = *auraParams});
    rm::unitdef::UnitDef ally;
    ally.name = "test_ally_tank";
    ally.health = rm::test::mag(100.0f);
    ally.categories = {"MOBILE"};
    ally.speedElmosPerSecond = 20.0f;
    const auto seraType = roster.addType(sera);
    const auto allyType = roster.addType(ally);
    const auto seraUnit = roster.add(seraType, 0.0f, 0.0f, 0, 1000.0f);
    const auto allyUnit = roster.add(allyType, 10.0f, 0.0f, 0, 100.0f);
    roster.store.health()[allyUnit.index].current = rm::test::mag(50.0f);
    REQUIRE(rm::sim::installEnhancement(roster.store, roster.catalog, seraUnit, "RegenAura"));
    // 100 max health * 1/180 per second = 5/9 per second, 1/18 a tick at 10 Hz.
    const rm::sim::Mag before = roster.store.health()[allyUnit.index].current;
    rm::sim::tickRegeneration(roster.store, roster.catalog, roster.rate);
    CHECK(rm::test::near(
        roster.store.health()[allyUnit.index].current - before) == 1.0f / 18.0f);
}
