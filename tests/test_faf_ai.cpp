// The FAF AI sandbox: does the vendored corpus load, and what does it reach for? (ADR-039)
//
// THIS IS THE INSTRUMENTATION PASS, not a correctness test, and the distinction is the whole
// point of ADR-039's premise: Moho is closed, a binding written against a signature is a guess,
// and the adapter's first goal is an AI that RUNS plus a report of what it is running badly.
// So what is asserted here is that the surface is complete and the corpus parses — the two
// things that must hold before "which parts are not connected" is even a question.
//
// SKIPPED WHEN vendor/ai/faf IS ABSENT. The corpus is fetched by `make ai` and gitignored
// (ADR-039), so a fresh checkout has no copy. A test that failed there would be reporting on
// the checkout rather than on the code.
#include <catch2/catch_test_macros.hpp>

#include "app/FafAi.hpp"
#include "app/FafOpponent.hpp"
#include "core/unit/UnitBlueprint.hpp"

#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

using rm::ai::Binding;
using rm::ai::FafAi;
using rm::ai::Fidelity;

namespace {

[[nodiscard]] std::filesystem::path corpusRoot() {
    // The tests run from the build directory; the corpus sits beside the source tree.
    for (const char* candidate : {"vendor/ai/faf", "../vendor/ai/faf", "../../vendor/ai/faf"}) {
        if (std::filesystem::is_directory(candidate)) {
            return std::filesystem::absolute(candidate);
        }
    }
    return {};
}

/// The files the AI subset is built on, in the order a brain would reach them: utilities first,
/// then the objects, then the behaviours. Chosen as a spread across the corpus rather than a
/// sample of one directory, so "it loads" is a claim about the corpus and not about one file.
constexpr const char* kProbe[] = {
    "/lua/AI/aiutilities.lua",
    "/lua/AI/aiattackutilities.lua",
    "/lua/AI/AIBehaviors.lua",
    "/lua/AI/aibuildstructures.lua",
    "/lua/AI/AttackManager.lua",
    "/lua/AI/Grid.lua",
    "/lua/AI/GridBrain.lua",
    "/lua/AI/GridReclaim.lua",
    "/lua/AI/Transportutilities.lua",
    "/lua/AI/sorianutilities.lua",
    "/lua/aibrain.lua",
    "/lua/platoon.lua",
    "/lua/sim/BuilderManager.lua",
    "/lua/sim/EngineerManager.lua",
    "/lua/sim/FactoryBuilderManager.lua",
    "/lua/aibrains/base-ai.lua",
};

} // namespace

TEST_CASE("FAF enemy and blueprint queries read the observed match and retail content",
          "[faf][ai][brain-query]") {
    const auto root = corpusRoot();
    const char* home = std::getenv("HOME");
    const auto contentRoot = home ? std::filesystem::path{home} / "projects/llm/input/faf"
                                  : std::filesystem::path{};
    if (root.empty() || !std::filesystem::exists(contentRoot / "units/UEL0001/UEL0001_unit.bp")) {
        SKIP("requires the vendored FAF corpus and extracted retail unit blueprints");
    }
    rm::vfs::Vfs content;
    content.mountDirectory(contentRoot);
    auto scene = std::make_unique<rm::app::UnitScene>();
    scene->armies = {{.index = 0, .alliance = 0}, {.index = 1, .alliance = 0},
                     {.index = 2, .alliance = 2}, {.index = 3, .alliance = 3}};
    scene->economies.resize(scene->armies.size());
    auto commander = rm::unitbp::loadFile(contentRoot / "units/UEL0001/UEL0001_unit.bp");
    REQUIRE(commander);
    scene->definitions.push_back(*commander);
    const auto type = scene->catalog.add(&scene->definitions.back(), rm::sim::TickRate{});
    for (int army = 1; army < 4; ++army) {
        (void)scene->store.spawn({
            .type = type,
            .transform = {.x = rm::sim::fxFromFloat(static_cast<float>(army * 50)),
                          .z = rm::sim::fxFromFloat(100)},
            .motion = {.armyIndex = army},
            .health = {.current = rm::sim::magFromFloat(100),
                       .maximum = rm::sim::magFromFloat(100)},
        });
    }
    auto generator = rm::unitbp::loadFile(contentRoot / "units/UEB1101/UEB1101_unit.bp");
    REQUIRE(generator);
    scene->definitions.push_back(*generator);
    const auto generatorType = scene->catalog.add(&scene->definitions.back(), rm::sim::TickRate{});
    (void)scene->store.spawn({
        .type = generatorType,
        .transform = {.x = rm::sim::fxFromFloat(10), .z = rm::sim::fxFromFloat(100)},
        .motion = {.armyIndex = 0},
        .health = {.current = rm::sim::magFromFloat(100), .maximum = rm::sim::magFromFloat(100)},
    });
    rm::HeightField field{.squaresX = 64, .squaresZ = 64};
    field.raw.resize(field.sampleCount());
    const std::array<rm::mapinfo::StartPosition, 4> starts{{
        {.x = 0, .z = 100}, {.x = 50, .z = 100},
        {.x = 100, .z = 100}, {.x = 150, .z = 100},
    }};
    const char* expectedPath = "Land";
    SECTION("dry terrain connects the bases") {}
    SECTION("water requires the amphibious movement grid") {
        scene->hasWater = true;
        scene->waterLevelElmos = 10;
        expectedPath = "Amphibious";
    }
    SECTION("neither movement grid admits the endpoints") {
        scene->hasWater = true;
        // Exercise the existing amphibious MoveDef's explicit depth ceiling.
        scene->waterLevelElmos = 2 * rm::data::moveDefFor(
            rm::unitdef::MotionType::Amphibious).maxWaterDepthElmos;
        expectedPath = "Air";
    }
    const rm::ai::World world{.scene = *scene, .content = content, .field = field,
                              .starts = starts, .markers = {}};
    FafAi ai(root);
    REQUIRE(installFafDriver(ai));
    importAiEntryPoints(ai);
    rm::ai::FafOpponent opponent(ai, 0);
    opponent.observe(world, {});
    opponent.advance(0);
    bool ok = ai.eval(R"(
        local brain = __rm_faf.brains[0]
        assert(brain.Nickname == brain.Name, 'adapter identity is available to FAF diagnostics')
        local misc = import('/lua/editor/MiscBuildConditions.lua')
        assert(misc.IsIsland(brain, false), 'no Island marker is a valid negative result')
        assert(brain.islandMarker == nil)
        assert(__rm_faf.missing.islandMarker == nil, 'optional nil is not an unbound method')
        assert(not misc.ReclaimAvailableInGrid(brain, 'MAIN', false),
            'unsupported reclaim grid fails closed without a formatting exception')
        local enemy = brain:GetCurrentEnemy()
        previousEnemy = enemy
        assert(enemy:GetArmyIndex() == 3, 'nearest hostile, not the closer ally')
        assert(not enemy:IsDefeated())
        local x, z = enemy:GetArmyStartPos()
        assert(x == 100 and z == 100)
        local bp = brain:GetUnitBlueprint('ueb1101')
        assert(bp.Physics.SkirtSizeX == 2)
        assert(bp.Economy.BuildCostMass == 75 and bp.Economy.BuildTime == 125)
        assert(bp.BlueprintId == 'ueb1101' and bp.CategoriesHash.BUILTBYTIER1ENGINEER)
        assert(bp == brain:GetUnitBlueprint('UEB1101'), 'case-insensitive cached identity')
        assert(GetUnitBlueprintByName('UEB1101') == bp, 'global blueprint queries use real content')
        local home = brain:PBMGetLocationCoords('MAIN')
        assert(home[1] == 0 and home[3] == 100)
        assert(brain:PBMGetLocationCoords('missing') == nil)
        assert(brain:GetLocationPosition('MAIN')[3] == 100)
        assert(IsAlly(1, 2) and not IsAlly(1, 3))
        assert(IsEnemy(1, 3) and not IsEnemy(1, 2))
        -- The generator at x=10 excludes sites through x=250 (30 ogrids = 240 elmos).
        local positions = {{Name='ally', Position={50,0,100}}, {Name='free',Position={251,0,100}}}
        local available = import('/lua/ai/aiutilities.lua').AIFilterAlliedBases(brain, positions)
        assert(#available == 1 and available[1].Name == 'free')
        local unit = setmetatable({ bp = 'UEB1101' }, __rm_faf.unitMeta)
        assert(unit:GetBlueprint() == bp, 'unit and brain queries share the source table')
        local found, err = pcall(brain.GetUnitBlueprint, brain, 'missing-blueprint')
        assert(not found and string.find(err, 'missing%-blueprint'), 'unknown content fails closed')
        assert(brain:GetUnitBlueprint('UEL0001').General.Icon == 'amph')
        -- The real condition invokes both brain:GetUnitBlueprint and unit:GetBlueprint.
        assert(import('/lua/editor/UnitCountBuildConditions.lua').AdjacencyCheck(
            brain, 'MAIN', categories.ENERGYPRODUCTION, 100, 'ueb0101'))
    )");
    INFO(ai.lastError());
    REQUIRE(ok);
    ok = ai.eval(std::string{"assert(import('/lua/editor/MiscBuildConditions.lua').PathToEnemy("}
        + "__rm_faf.brains[0], 'MAIN', '" + expectedPath + "'))");
    INFO(ai.lastError());
    REQUIRE(ok);
    scene->armies[2].defeated = true;
    opponent.advance(30);
    ok = ai.eval(R"(
        assert(__rm_faf.brains[0]:GetCurrentEnemy():GetArmyIndex() == 4)
        assert(previousEnemy:IsDefeated(), 'held brain views refresh after defeat')
    )");
    INFO(ai.lastError());
    REQUIRE(ok);
    scene->armies[3].defeated = true;
    opponent.advance(60);
    ok = ai.eval("assert(__rm_faf.brains[0]:GetCurrentEnemy() == nil)");
    INFO(ai.lastError());
    REQUIRE(ok);
}

TEST_CASE("the FAF sandbox binds every name before any AI runs", "[faf][ai]") {
    const std::filesystem::path root = corpusRoot();
    if (root.empty()) {
        SUCCEED("vendor/ai/faf absent — run `make ai`");
        return;
    }
    const FafAi ai(root);
    REQUIRE(ai.ready());

    // ADR-039's load-time assertion. Every name the generator found is installed BEFORE a line
    // of AI Lua runs, so a call the adapter has not implemented is a counted no-op rather than
    // a nil-call forty minutes into a match. The number tracks FafApi.inc; if the generator
    // finds more names, this rises with it rather than being a magic constant to maintain.
    CHECK(ai.boundCount() > 200);
    CHECK(ai.report().size() == ai.boundCount());
}

TEST_CASE("the vendored corpus parses and imports", "[faf][ai]") {
    const std::filesystem::path root = corpusRoot();
    if (root.empty()) {
        SUCCEED("vendor/ai/faf absent — run `make ai`");
        return;
    }
    FafAi ai(root, /*verbose=*/true);
    REQUIRE(ai.ready());

    std::size_t loaded = 0;
    for (const char* path : kProbe) {
        if (ai.import(path)) {
            ++loaded;
        }
    }

    // Not asserted as all-sixteen, because that was the first thing this test got wrong:
    // `import` returns a table either way, so counting tables counted failures as successes.
    // What is reported is the outcome trail, which distinguishes "not fetched" from "fetched
    // and broken" — and only the second kind is ours.
    std::size_t executed = 0;
    std::size_t missing = 0;
    std::size_t failed = 0;
    for (const rm::ai::ModuleLoad& module : ai.modules()) {
        switch (module.outcome) {
        case rm::ai::LoadOutcome::Executed: ++executed; break;
        case rm::ai::LoadOutcome::Missing:  ++missing;  break;
        case rm::ai::LoadOutcome::Failed:   ++failed;   break;
        }
    }
    std::printf("\n  FAF corpus: %zu of %zu probes ran; %zu modules executed, %zu missing, "
                "%zu failed\n", loaded, std::size(kProbe), executed, missing, failed);
    for (const rm::ai::ModuleLoad& module : ai.modules()) {
        if (module.outcome == rm::ai::LoadOutcome::Failed) {
            std::printf("    FAILED %-44s %s\n", module.path.c_str(), module.error.c_str());
        }
    }

    // The floor: something has to have run, or the sandbox is not hosting anything and every
    // other number here is measuring an empty room.
    CHECK(executed > 0);
}

TEST_CASE("the corpus reaches for the engine, and the report ranks what it wanted",
          "[faf][ai]") {
    const std::filesystem::path root = corpusRoot();
    if (root.empty()) {
        SUCCEED("vendor/ai/faf absent — run `make ai`");
        return;
    }
    FafAi ai(root);
    REQUIRE(ai.ready());
    for (const char* path : kProbe) {
        (void)ai.import(path);
    }

    const std::vector<Binding> report = ai.report();
    REQUIRE_FALSE(report.empty());

    // `import` is the most-called name in the corpus at 592 sites, and loading anything at all
    // proves it is wired: a stubbed import would leave every file's first line returning nil.
    const auto imported = std::find_if(report.begin(), report.end(),
                                       [](const Binding& b) { return b.name == "import"; });
    REQUIRE(imported != report.end());
    CHECK(imported->fidelity == Fidelity::Known);
    CHECK(imported->calls > 0);

    // The report is the work queue, so its order is load-bearing: most-called first.
    for (std::size_t i = 1; i < report.size(); ++i) {
        CHECK(report[i - 1].calls >= report[i].calls);
    }

    // Not an assertion — the point of the exercise. What the corpus reached for while loading
    // is what an implementer should look at next, and printing it is how that reaches a human.
    std::printf("\n  FAF sandbox: %zu names bound, top of the work queue after import:\n",
                ai.boundCount());
    std::size_t shown = 0;
    for (const Binding& binding : report) {
        if (binding.calls == 0 || shown >= 12) {
            break;
        }
        std::printf("    %-28s %5zu calls  %4d sites  %s\n", binding.name.c_str(),
                    binding.calls, binding.sites,
                    std::string(rm::ai::fidelityName(binding.fidelity)).c_str());
        ++shown;
    }
}

TEST_CASE("the thread model runs: fork, wait, resume on the right tick, die alone",
          "[faf][ai][threads]") {
    const std::filesystem::path root = corpusRoot();
    if (root.empty()) {
        SKIP("no vendored corpus; run `make ai`");
    }
    FafAi ai(root);
    REQUIRE(ai.ready());

    // A worker that ticks a counter, waits a second, and repeats; a mayfly that errors.
    REQUIRE(ai.eval(R"(
        beats = 0
        worker = ForkThread(function()
            while true do
                beats = beats + 1
                WaitSeconds(1)
            end
        end)
        ForkThread(function() error("mayfly") end)
    )"));

    // Nothing has run yet: a fork schedules, the pump executes.
    CHECK(ai.threadsAlive() == 2);

    // Tick 0: both resume. The worker beats once and sleeps ten ticks; the mayfly dies
    // alone, recorded, without taking the worker with it.
    (void)ai.pump(0);
    REQUIRE(ai.eval("assert(beats == 1)"));
    CHECK(ai.threadsAlive() == 1);
    REQUIRE(ai.threadErrors().size() == 1);

    // Ticks 1..9: asleep. Tick 10: one second has passed at the adapter's 10 Hz.
    for (long long tick = 1; tick < 10; ++tick) {
        CHECK(ai.pump(tick) == 0);
    }
    CHECK(ai.pump(10) == 1);
    REQUIRE(ai.eval("assert(beats == 2)"));

    // KillThread by handle: the worker never beats again.
    REQUIRE(ai.eval("KillThread(worker)"));
    CHECK(ai.pump(20) == 0);
    REQUIRE(ai.eval("assert(beats == 2)"));
    CHECK(ai.threadsAlive() == 0);
}

TEST_CASE("forking from inside a resume does not corrupt the scheduler",
          "[faf][ai][threads]") {
    // The regression this pins: pump holds a Thread& across lua_resume, and a fork inside
    // that resume grows the container. A vector reallocated there and every later write
    // through the reference landed in freed storage; a deque promises the elements never
    // move, so the reference survives.
    const std::filesystem::path root = corpusRoot();
    if (root.empty()) {
        SKIP("no vendored corpus; run `make ai`");
    }
    FafAi ai(root);
    REQUIRE(ai.ready());

    // Enough forks to walk the container through several growths while earlier references
    // are still live inside pump. A forked thread is due immediately, so all 2000 children
    // run in this same pass — after the spawner has finished forking them.
    REQUIRE(ai.eval(R"(
        done = 0
        ForkThread(function()
            for i = 1, 2000 do
                ForkThread(function() done = done + 1 end)
            end
        end)
    )"));
    (void)ai.pump(0);
    REQUIRE(ai.eval("assert(done == 2000)"));
    CHECK(ai.threadErrors().empty());
    CHECK(ai.threadsAlive() == 0);  // every child ran to completion and died cleanly
}

TEST_CASE("a thread that kills itself mid-body is reclaimed without leaking its refs",
          "[faf][ai][threads]") {
    const std::filesystem::path root = corpusRoot();
    if (root.empty()) {
        SKIP("no vendored corpus; run `make ai`");
    }
    FafAi ai(root);
    REQUIRE(ai.ready());

    // KillThread(self) marks the thread dead WHILE pump holds it resumed. The body after
    // the kill still returns (Lua runs on), but nothing scheduled may ever run again — and
    // the sweep at the end of the pump must release both registry refs, which an inline
    // unref at resume time cannot do for a thread that died from inside.
    REQUIRE(ai.eval(R"(
        reached = 0
        ForkThread(function()
            KillThread(CurrentThread())
            reached = reached + 1   -- Lua keeps running to the end of the body...
        end)
    )"));
    CHECK(ai.pump(0) == 1);
    CHECK(ai.threadsAlive() == 0);
    // ...and it did, once: the kill stops SCHEDULING, not the current slice.
    REQUIRE(ai.eval("assert(reached == 1)"));

    // The dead thread stays dead: no further pump revives it.
    CHECK(ai.pump(1) == 0);
    REQUIRE(ai.eval("assert(reached == 1)"));
}

TEST_CASE("bare table iteration works, as LuaPlus meant it", "[faf][ai]") {
    const std::filesystem::path root = corpusRoot();
    if (root.empty()) {
        SKIP("no vendored corpus; run `make ai`");
    }
    FafAi ai(root);
    REQUIRE(ai.ready());

    // The corpus's habit, 1,134 times over: a bare table after `in`. The eval path runs the
    // raw chunk, so the rewrite is exercised through a module in the corpus's own dialect —
    // class.lua importing at construction already proved it, but prove it small and directly:
    // the wrapped iterator must also pass a REAL iterator triple through untouched.
    REQUIRE(ai.eval(R"(
        local sum = 0
        for _, v in __rm_iter({ 3, 4, 5 }) do sum = sum + v end
        assert(sum == 12)
        local keys = 0
        for k in __rm_iter(pairs({ a = 1, b = 2 })) do keys = keys + 1 end
        assert(keys == 2)
    )"));
}

TEST_CASE("the call profiler counts the corpus's own functions, by definition site",
          "[faf][ai]") {
    const std::filesystem::path root = corpusRoot();
    if (root.empty()) {
        SKIP("no vendored corpus; run `make ai`");
    }
    FafAi ai(root);
    REQUIRE(ai.ready());

    // Off by default: the sanity run opts in, a match does not pay for it.
    ai.setProfiling(true);

    // A CORPUS function — the profiler counts only functions defined under `/lua/`, so an
    // eval-defined helper or the adapter's own shims never appear; that filter is what keeps
    // the report answering "which of the AI's code ran" rather than "how busy were we".
    REQUIRE(ai.import("/lua/factions.lua"));

    const auto callsTo = [&ai](std::string_view name) {
        std::size_t total = 0;
        for (const auto& [site, calls] : ai.callProfile()) {
            if (site.find("/lua/factions.lua") != std::string::npos
                && site.find(name) != std::string::npos) {
                total = calls;
            }
        }
        return total;
    };

    // Three direct calls and one from a pumped thread — the two paths corpus code actually
    // runs through. The profile keys on the DEFINITION site, so all four land on one row,
    // on top of whatever the module's own load already counted.
    const std::size_t before = callsTo("GetFactions");
    REQUIRE(ai.eval(R"(
        local m = import('/lua/factions.lua')
        m.GetFactions(); m.GetFactions(); m.GetFactions()
        ForkThread(function() m.GetFactions() end)
    )"));
    (void)ai.pump(0);
    CHECK(callsTo("GetFactions") == before + 4);

    // The adapter's own shims stay out of the profile.
    for (const auto& [site, calls] : ai.callProfile()) {
        REQUIRE(site.find("__rm_iter") == std::string::npos);
    }

    // The profile is sorted most-called first, which is what makes the report's top-N a
    // ranking rather than a sample.
    const auto profile = ai.callProfile();
    for (std::size_t i = 1; i < profile.size(); ++i) {
        REQUIRE(profile[i - 1].second >= profile[i].second);
    }
}

TEST_CASE("categories are an algebra now, evaluated against a unit's own tags",
          "[faf][ai]") {
    const std::filesystem::path root = corpusRoot();
    if (root.empty()) {
        SKIP("no vendored corpus; run `make ai`");
    }
    FafAi ai(root);
    REQUIRE(ai.ready());

    // The exact shape the corpus builds at data-load time: intersection, difference, and a
    // unit-id atom (in Moho every unit id is a category). A tank passes the hunter filter,
    // an engineer with the same mobility does not, and ParseEntityCategory's space means AND.
    const bool ok = ai.eval(R"(
        local hunter = categories.MOBILE * categories.LAND - categories.ENGINEER
        local tank = { MOBILE = true, LAND = true, TECH1 = true, UEL0201 = true }
        assert(__rm_catMatch(hunter, tank))
        assert(__rm_catMatch(categories.ALLUNITS,tank),'ALLUNITS is universal, not an authored blueprint tag')
        assert(__rm_catMatch(categories.ALLUNITS-categories.ENGINEER,tank))
        assert(not __rm_catMatch(categories.ALLUNITS,nil),'a missing unit is not a category member')
        assert(not __rm_catMatch(hunter, { MOBILE = true, LAND = true, ENGINEER = true }))
        assert(__rm_catMatch(categories.uel0201, tank), 'unit id atom')
        assert(EntityCategoryContains(ParseEntityCategory('MOBILE LAND'), { __cats = tank }))
        assert(not EntityCategoryContains(ParseEntityCategory('MOBILE NAVAL'), { __cats = tank }))
        local down = EntityCategoryFilterDown(categories.MOBILE,
            { { __cats = tank }, { __cats = { STRUCTURE = true } } })
        assert(table.getn(down) == 1)
        assert(EntityCategoryCount(categories.STRUCTURE + categories.MOBILE,
            { { __cats = tank }, { __cats = { STRUCTURE = true } } }) == 2)
    )");
    INFO(ai.lastError());
    REQUIRE(ok);
}

TEST_CASE("the FAF driver boots a brain and the corpus's own builders decide", "[faf][ai]") {
    const std::filesystem::path root = corpusRoot();
    if (root.empty()) {
        SKIP("no vendored corpus; run `make ai`");
    }
    FafAi ai(root);
    REQUIRE(ai.ready());
    // Driver BEFORE the corpus — the condition files capture engine functions out of
    // moho.aibrain_methods at import (see the driver's economy note).
    REQUIRE(installFafDriver(ai));
    importAiEntryPoints(ai);

    // A brain, a map with mass to take, and one idle commander: the corpus should decide
    // to BUILD something. What exactly is the data's business — the assertion is that the
    // machinery turns FAF's own builder specs into a concrete decision.
    const bool ok = ai.eval(R"(
        local markers = {}
        for i = 1, 6 do
            markers[i] = { name = 'Mass ' .. i, type = 'Mass', x = 60 + i * 10, y = 0, z = 100 }
        end
        local wanted = __rm_faf_boot(0, { faction = 1, startX = 100, startZ = 100,
                                          sizeX = 512, sizeZ = 512, armies = 2,
                                          base = 'NormalMain', markers = markers })
        assert(type(wanted) == 'table', 'boot returns the train candidates to teach')
        local builders = #__rm_faf.brains[0].builders
        assert(builders > 100, 'expected NormalMain builder list, got ' .. tostring(builders))
        __rm_faf_type('UEL0001', { 'COMMAND', 'ENGINEER', 'MOBILE', 'LAND' })
        -- Handles are packed UnitIds: generation in the high 32 bits, index in the low.
        assert(__rm_faf_handle(3, 2) == 2 * 4294967296 + 3)
        local commander = { bp = 'UEL0001', h = __rm_faf_handle(1, 1), x = 100, z = 100,
                            idle = true, healthPercent = 0.75,
                            __cats = __rm_faf.cats.UEL0001 }
        setmetatable(commander, __rm_faf.unitMeta)
        assert(commander:GetHealthPercent() == 0.75,
               'unit proxy does not expose its hull health')
        -- The bare sandbox has no live store to ask; the match binds __rm_faf_beenDestroyed.
        assert(commander:BeenDestroyed() == false)
        local snap = { units = { commander }, occupied = {}, underway = {},
                       mass = 400, energy = 1500, massStorage = 650, energyStorage = 4000,
                       massIncome = 0.2, energyIncome = 10,
                       massRequested = 0, energyRequested = 0,
                       massUsage = 0, energyUsage = 0,
                       structuresUnderway = 0, mobileUnderway = 0 }
        local decisions = __rm_faf_decide(0, snap)
        assert(#decisions >= 1, 'the corpus decided nothing')
        assert(decisions[1].kind == 'build',
               'expected a build, got ' .. tostring(decisions[1].kind))
        assert(type(decisions[1].bp) == 'string' and #decisions[1].bp > 0)

        -- The engineer manager stand-in takes the corpus's (group, category) arity: with the
        -- group name bound as the category every engineer cap read zero and the base built
        -- a hundred engineers.
        local brain = __rm_faf.brains[0]
        local manager = brain.BuilderManagers.MAIN.EngineerManager
        assert(manager:GetNumCategoryUnits('Engineers', categories.COMMAND) == 1,
               'GetNumCategoryUnits must read the category, not the group name')
        assert(manager:GetNumCategoryUnits('Engineers', categories.TECH1 * categories.ENGINEER) == 0)

        -- Unit counts are memoised per pass by category TEXT: a rebuilt expression hits the
        -- memo, and the next pass's snapshot invalidates it.
        assert(brain:GetCurrentUnits(categories.COMMAND * categories.MOBILE) == 1)
        assert(brain:GetCurrentUnits(categories.COMMAND * categories.MOBILE) == 1)
        assert(brain:GetCurrentUnits(categories.MOBILE - categories.COMMAND) == 0)
        local second = setmetatable({ bp = 'UEL0001', h = __rm_faf_handle(2, 1), x = 120, z = 100, idle = true,
                                      healthPercent = 1, __cats = __rm_faf.cats.UEL0001 },
                                    __rm_faf.unitMeta)
        local snap2 = {}
        for k, v in pairs(snap) do snap2[k] = v end
        snap2.units = { commander, second }
        __rm_faf_decide(0, snap2)
        assert(brain:GetCurrentUnits(categories.COMMAND * categories.MOBILE) == 2,
               'a new snapshot must invalidate the count memo')

        -- The reclaim grid the corpus's ReclaimAvailableInGrid reads: Grid.lua cell mapping
        -- over the native per-cell wreck totals, richest cell within N rings, 10 mass floor.
        snap2.reclaim = { cellCount = 16, cellSize = 32,
                          cells = { [4] = { [4] = { mass = 120, energy = 40, count = 2 } } } }
        local gx, gz = brain.GridReclaim:ToGridSpace(100, 100)
        assert(gx == 4 and gz == 4, 'ToGridSpace floors world/CellSize and is 1-based')
        assert(select(1, brain.GridReclaim:ToGridSpace(-5, 0)) == 1)
        assert(select(1, brain.GridReclaim:ToGridSpace(100000, 0)) == 16, 'clamped to the grid')
        assert(brain.GridReclaim:MaximumInRadius(4, 4, 0).TotalMass == 120)
        assert(brain.GridReclaim:MaximumInRadius(1, 1, 3).TotalMass == 120, 'the square reaches cell 4')
        assert(brain.GridReclaim:MaximumInRadius(10, 10, 2).TotalMass == 0, 'nothing that far away')
        assert(brain.GridReclaim:MaximumInRadius(10, 10, 2).ReclaimCount == 0)
        local conditions = import('/lua/editor/MiscBuildConditions.lua')
        assert(conditions.ReclaimAvailableInGrid(brain, 'MAIN') == true,
               'the base at (100,100) sees 120 mass within three rings')
        snap2.reclaim.cells[4][4].mass = 5
        assert(conditions.ReclaimAvailableInGrid(brain, 'MAIN') == false,
               'under ten mass there is nothing worth reclaiming')
        snap2.reclaim = nil
        assert(conditions.ReclaimAvailableInGrid(brain, 'MAIN') == false,
               'no snapshot grid means no reclaim, not an error')

        -- The reclaim decision's target: the richest cell's centre and half-size, or nil.
        snap2.reclaim = { cellCount = 16, cellSize = 32,
                          cells = { [4] = { [4] = { mass = 120, energy = 0, count = 2 } },
                                    [6] = { [2] = { mass = 300, energy = 0, count = 1 } } } }
        local rx, rz, rr, rmass = __rm_faf_reclaimTarget(brain, 3)
        assert(rx == 6 * 32 - 16 and rz == 2 * 32 - 16, 'the richer cell (6,2) wins within three rings')
        assert(rr == 16 and rmass == 300)
        snap2.reclaim.cells[6][2].mass = 5
        local px = __rm_faf_reclaimTarget(brain, 3)
        assert(px == 4 * 32 - 16, 'a cell under ten mass is skipped for the next richest')
        snap2.reclaim.cells[4][4].mass = 5
        assert(__rm_faf_reclaimTarget(brain, 3) == nil, 'nothing worth reclaiming means no target')
        snap2.reclaim = nil

        -- C-071, retail 0x005968b6: the AI trend is income minus usage, per tick.
        -- C-163's separate UI statistic multiplies by ten; this API must not.
        snap.massRequested = 0.4
        snap.massUsage = 0.1
        __rm_faf.brains[0].snap = snap
        assert(GetEconomyRequested(__rm_faf.brains[0], 'MASS') == 0.4)
        assert(__rm_faf.brains[0]:GetEconomyUsage('MASS') == 0.1)
        assert(math.abs(__rm_faf.brains[0]:GetEconomyTrend('MASS') - 0.1) < 0.000001)
    )");
    INFO(ai.lastError());
    REQUIRE(ok);
}

TEST_CASE("FAF factory upgrades reserve production until the current product finishes",
          "[faf][ai][factory-upgrade]") {
    const auto root = corpusRoot();
    if (root.empty()) SKIP("no vendored corpus; run `make ai`");
    FafAi ai(root);
    REQUIRE(ai.ready());
    REQUIRE(installFafDriver(ai));
    importAiEntryPoints(ai);
    const bool ok = ai.eval(R"(
        __rm_faf_boot(0, { faction = 1, startX = 100, startZ = 100,
            sizeX = 512, sizeZ = 512, armies = 2, base = 'NormalMain', markers = {} })
        local brain = __rm_faf.brains[0]
        -- Keep real FAF conditions/templates, isolating the two competing decisions.
        local builders = {}
        for _, item in ipairs(brain.builders) do
            if item.spec.BuilderName == 'T1AirFactoryUpgrade Slow'
                or item.spec.BuilderName == 'T1 Air Bomber' then
                table.insert(builders, item)
            end
        end
        assert(#builders == 2)
        brain.builders = builders
        __rm_faf_type('UEA0103', { 'MOBILE', 'AIR', 'BOMBER', 'BUILTBYTIER1FACTORY' })
        local factory = setmetatable({ h = __rm_faf_handle(55, 1), bp = 'UEB0102',
            x = 100, z = 100, idle = true, building = true,
            __cats = { STRUCTURE = true, FACTORY = true, AIR = true, TECH1 = true } },
            __rm_faf.unitMeta)
        local land = setmetatable({ h = __rm_faf_handle(56, 1), bp = 'UEB0101',
            x = 120, z = 100, idle = true,
            __cats = { STRUCTURE = true, FACTORY = true, LAND = true, TECH1 = true } },
            __rm_faf.unitMeta)
        local snap = { units = { factory, land }, occupied = {}, underway = {},
            mass = 970, energy = 10000, massStorage = 1000, energyStorage = 10000,
            massIncome = 6, energyIncome = 42.5, massRequested = 1.2, energyRequested = 35,
            massUsage = 1.2, energyUsage = 35, structuresUnderway = 0, mobileUnderway = 1 }
        local function decide()
            brain.condCache = {} -- Make changes visible without waiting for the cache TTL.
            return __rm_faf_decide(0, snap)
        end
        -- This scenario starts with an established economy, after FAF's monitor
        -- has collected its thirty one-second samples.
        brain.builders={}
        for tick=0,290,10 do snap.tick=tick; decide() end
        brain.builders=builders
        snap.tick=300
        assert(#decide() == 0, 'busy factory must finish its bomber before upgrading')
        -- The brief economy opportunity disappears before the bomber completes.
        snap.energyIncome = 30
        factory.building = false
        local decisions = decide()
        assert(#decisions == 1 and decisions[1].kind == 'upgrade'
            and decisions[1].builder == factory.h, 'retain the eligible upgrade across passes')
        factory.upgrading = true
        assert(#decide() == 0, 'do not duplicate an upgrade already in flight')

        factory.upgrading = false
        snap.energyIncome = 42.5
        decisions = decide()
        assert(#decisions == 1 and decisions[1].kind == 'upgrade',
            'free factory must upgrade instead of training a bomber in the same pass')

        factory.building = true
        assert(#decide() == 0)
        snap.energyIncome = 30
        snap.energyRequested = 20
        -- The pending reservation survives even after the rolling income gate
        -- loses the original opportunity while the product is still building.
        for tick=310,600,10 do snap.tick=tick; assert(#decide()==0) end
        -- A recycled slot is a different unit: never inherit the dead factory's reservation.
        factory.h = __rm_faf_handle(55, 2)
        factory.building = false
        decisions = decide()
        assert(#decisions == 1 and decisions[1].kind == 'train',
            'discard reservations for dead factories and resume ordinary production')
        assert(not __rm_faf.missing.pendingFactoryUpgrade,
            'an empty adapter reservation is not a missing engine API')
        local report=table.concat(__rm_faf_builder_conditions(),'\n')
        assert(report:find('T1AirFactoryUpgrade Slow',1,true))
        assert(report:find('GreaterThanEconIncomeOverTime',1,true),
            'report the observed first failing gate, not an inferred blocker')
        __rm_faf_decide(0,snap)
        assert(table.concat(__rm_faf_builder_conditions(),'\n')==report,
            'cached answers must not count as new condition evaluations')
    )");
    INFO(ai.lastError());
    REQUIRE(ok);
}

TEST_CASE("FAF tech loads speed upgrades and experimental construction", "[faf][ai-personality]") {
    const auto root = corpusRoot();
    if (root.empty()) SKIP("no vendored corpus; run `make ai`");
    FafAi ai(root);
    REQUIRE(ai.ready());
    REQUIRE(installFafDriver(ai));
    importAiEntryPoints(ai);
    const bool ok = ai.eval(R"(
        __rm_faf_boot(0, { faction = 1, startX = 100, startZ = 100,
            sizeX = 512, sizeZ = 512, armies = 2, base = 'TechMain', markers = {} })
        local brain = __rm_faf.brains[0]
        local names = {}
        for _, item in ipairs(brain.builders) do names[item.spec.BuilderName] = true end
        assert(names['T1 Land Factory Upgrade Speed'])
        assert(names['T2 Land Factory Upgrade Speed'])
        assert(names['T3 Land Exp1 Engineer 1'])
        assert(not names['T1 Land Factory Upgrade Slow'])
        assert(brain.BuilderManagers.MAIN.BaseSettings.EngineerCount.Tech3 == 25)
    )");
    INFO(ai.lastError());
    REQUIRE(ok);
}

TEST_CASE("FAF area counts distinguish own allies and visible enemies", "[faf][ai][area-query]") {
    const auto root = corpusRoot();
    if (root.empty()) SKIP("no vendored corpus; run `make ai`");
    FafAi ai(root);
    REQUIRE(installFafDriver(ai));
    importAiEntryPoints(ai);
    const bool ok = ai.eval(R"(
        __rm_faf_boot(0, { faction = 1, startX = 0, startZ = 0,
            sizeX = 2048, sizeZ = 2048, armies = 2, base = 'NormalMain', markers = {} })
        local brain = __rm_faf.brains[0]
        local function unit(x, kind)
            return { x = x, z = 0, __cats = { [kind] = true } }
        end
        -- Public query radii are ogrids; snapshot positions are elmos (8 per ogrid).
        brain.snap.units = { unit(40, 'MOBILE') }
        brain.snap.allies = { unit(80, 'MOBILE'), unit(96, 'STRUCTURE') }
        brain.snap.enemies = { unit(160, 'MOBILE'), unit(161, 'MOBILE') }
        assert(brain:GetNumUnitsAroundPoint(categories.MOBILE, {0,0,0}, 20, 'Own') == 1)
        assert(brain:GetNumUnitsAroundPoint(categories.MOBILE, {0,0,0}, 20, 'Ally') == 2)
        assert(brain:GetNumUnitsAroundPoint(categories.MOBILE, {0,0,0}, 20, 'Enemy') == 1)
        assert(brain:GetNumUnitsAroundPoint(categories.STRUCTURE, {0,0,0}, 20, 'Enemy') == 0)
        local ok = pcall(brain.GetUnitsAroundPoint, brain, categories.ALLUNITS, {0,0,0}, 20, 'bogus')
        assert(not ok, 'unknown alliances must not silently become own units')
    )");
    INFO(ai.lastError());
    REQUIRE(ok);
}

TEST_CASE("FAF naval marker filtering distinguishes the brain from allied armies",
          "[faf][naval-ai][area-query]") {
    const auto root = corpusRoot();
    if (root.empty()) SKIP("no vendored corpus; run `make ai`");
    FafAi ai(root);
    REQUIRE(installFafDriver(ai));
    importAiEntryPoints(ai);
    const bool ok = ai.eval(R"(
        __rm_faf_army(0, 0, 0, 1, false, 0)
        __rm_faf_army(1, 4000, 4000, 1, false, 1)
        __rm_faf_boot(0, { faction=1, startX=0, startZ=0,
            sizeX=4096, sizeZ=4096, armies=2, base='TechMain', markers={
                {name='Near Navy', type='Naval Area', x=800, y=0, z=0},
                {name='Far Navy', type='Naval Area', x=1600, y=0, z=0},
            } })
        local brain=__rm_faf.brains[0]
        local utilities=import('/lua/AI/aiutilities.lua')
        brain.snap.units={{x=800,z=0,__cats={STRUCTURE=true}}}
        -- Source aiutilities.lua:GetAlliesThreat excludes the querying brain by identity.
        local function selected()
            -- AIFindNavalAreaNeedsEngineer compares positions directly, unlike the public
            -- engine query below: convert the authored search radius at its call boundary.
            local position,name=utilities.AIFindNavalAreaNeedsEngineer(brain,'MAIN',250*8)
            return name
        end
        assert(selected()=='Near Navy', 'own army view must not masquerade as another ally')
        assert(ArmyBrains[1]==brain)
        __rm_faf_army(0, 0, 0, 1, false, 0)
        assert(ArmyBrains[1]==brain, 'snapshot refresh must preserve the own brain identity')
        brain.snap.units={}
        __rm_faf_army(1, 4000, 4000, 1, false, 0)
        brain.snap.allies={{x=1039,z=0,__cats={STRUCTURE=true}}}
        assert(selected()=='Far Navy', 'allied structure inside 30 ogrids excludes near site')
        brain.snap.allies[1].x=1041
        assert(selected()=='Near Navy', 'structure outside 30 ogrids does not exclude site')
        ArmyBrains[2].BuilderManagers['Near Navy']={}
        assert(selected()=='Far Navy', 'an allied marker reservation excludes the site')
    )");
    INFO(ai.lastError());
    REQUIRE(ok);
}

TEST_CASE("FAF threat queries use authored map cells and separate air from anti-air",
          "[faf][ai][threat-query]") {
    const auto root = corpusRoot();
    if (root.empty()) SKIP("no vendored corpus; run `make ai`");
    FafAi ai(root);
    REQUIRE(installFafDriver(ai));
    importAiEntryPoints(ai);
    const bool ok = ai.eval(R"(
        __rm_faf_boot(0, { faction = 1, startX = 0, startZ = 0,
            sizeX = 4096, sizeZ = 4096, armies = 2, base = 'NormalMain', markers = {} })
        local brain = __rm_faf.brains[0]
        assert(brain.IMAPConfig.IMAPSize == 32 and brain.IMAPConfig.Rings == 2)
        __rm_faf_type('AA', {'LAND', 'MOBILE'}, {s=2,a=10})
        __rm_faf_type('FIGHTER', {'AIR', 'MOBILE'}, {a=15})
        __rm_faf_type('TANK', {'LAND', 'MOBILE'}, {s=20})
        brain.snap.enemies = {
            {bp='AA', x=250,z=0,__cats=__rm_faf.cats.AA},
            {bp='FIGHTER', x=260,z=0,__cats=__rm_faf.cats.FIGHTER},
            {bp='TANK', x=800,z=0,__cats=__rm_faf.cats.TANK},
        }
        assert(brain:GetThreatAtPosition({0,0,0},0,true,'AntiAir') == 10)
        assert(brain:GetThreatAtPosition({0,0,0},1,true,'AntiAir') == 25)
        assert(brain:GetThreatAtPosition({0,0,0},1,true,'Air') == 15)
        assert(brain:GetThreatAtPosition({0,0,0},1,true,'AntiSurface') == 2)
        assert(brain:GetThreatAtPosition({0,0,0},3,true,'AntiSurface') == 22)
        assert(import('/lua/editor/ThreatBuildConditions.lua').EnemyThreatGreaterThanValueAtBase(
            brain, 'MAIN', 20, 'AntiAir'))
    )");
    INFO(ai.lastError());
    REQUIRE(ok);
}

TEST_CASE("FAF construction selects a free engineer instead of counting global work",
          "[faf][ai][construction]") {
    const auto root = corpusRoot();
    if (root.empty()) SKIP("no vendored corpus; run `make ai`");
    FafAi ai(root);
    REQUIRE(installFafDriver(ai));
    importAiEntryPoints(ai);
    const bool ok = ai.eval(R"(
        __rm_faf_boot(0,{faction=1,startX=0,startZ=0,sizeX=4096,sizeZ=4096,
            armies=2,base='NormalMain',markers={}})
        local brain=__rm_faf.brains[0]
        brain.builders={{kind='EngineerBuilder',spec={BuilderName='available engineer',
            Priority=1000,InstanceCount=1,BuilderConditions={},
            BuilderData={Construction={BuildStructures={'T1EnergyProduction'}}}}}}
        __rm_faf_type('ENGINEER',{'MOBILE','ENGINEER','TECH1'})
        __rm_faf_type('COMMANDER',{'MOBILE','ENGINEER','COMMAND','TECH1'})
        local free={h=1,bp='ENGINEER',idle=true,__cats=__rm_faf.cats.ENGINEER}
        local busy={h=2,bp='ENGINEER',idle=true,building=true,__cats=__rm_faf.cats.ENGINEER}
        local commander={h=3,bp='COMMANDER',idle=true,building=true,__cats=__rm_faf.cats.COMMANDER}
        local snap={units={free,busy,commander},occupied={},underway={},
            massIncome=10,energyIncome=100,massRequested=1,energyRequested=1,
            massUsage=1,energyUsage=1,
            structuresUnderway=2,mobileUnderway=0}
        local orders=__rm_faf_decide(0,snap)
        assert(#orders==1 and orders[1].builder==free.h,
            'construction ownership, not an offset into the pool, determines availability')
        free.building=true
        assert(#__rm_faf_decide(0,snap)==0, 'busy builders retain their construction orders')
        __rm_faf_type('T3ENGINEER',{'MOBILE','ENGINEER','TECH3'})
        __rm_faf_type('UEB1301',{'STRUCTURE','ENERGYPRODUCTION','TECH3','BUILTBYTIER3ENGINEER'})
        __rm_faf_type('UEB1101',{'STRUCTURE','ENERGYPRODUCTION','TECH1',
            'BUILTBYTIER1ENGINEER','BUILTBYTIER3ENGINEER'})
        free.building=false; free.bp='T3ENGINEER'; free.__cats=__rm_faf.cats.T3ENGINEER
        busy.building=false
        local function builder(name,structure)
            return {kind='EngineerBuilder',spec={BuilderName=name,Priority=1000,
                InstanceCount=1,BuilderConditions={},
                BuilderData={Construction={BuildStructures={structure}}}}}
        end
        brain.builders={builder('first T3','T3EnergyProduction'),
            builder('second T3','T3EnergyProduction'),builder('T1 fallback','T1EnergyProduction')}
        snap.structuresUnderway=0
        orders=__rm_faf_decide(0,snap)
        assert(#orders==2)
        assert(orders[1].builder==free.h and orders[1].structure=='T3EnergyProduction')
        assert(orders[2].builder==busy.h and orders[2].structure=='T1EnergyProduction',
            'each selected engineer must use its own technology permissions')
        __rm_faf_type('UEL0105',{'MOBILE','ENGINEER','TECH1'})
        __rm_faf_type('UEL0309',{'MOBILE','ENGINEER','TECH3'})
        busy.bp='UEL0105'; busy.__cats=__rm_faf.cats.UEL0105
        free.bp='UEL0309'; free.__cats=__rm_faf.cats.UEL0309
        snap.units={busy,free,commander} -- T1 appears before the available T3 engineer.
        brain.builders={builder('template T3','T3EnergyProduction'),
            builder('duplicate T3','T3EnergyProduction'),builder('template T1','T1EnergyProduction')}
        brain.builders[1].spec.PlatoonTemplate='T3BuildEngineer'
        brain.builders[2].spec.PlatoonTemplate='T3BuildEngineer'
        brain.builders[3].spec.PlatoonTemplate='T1BuildEngineer'
        orders=__rm_faf_decide(0,snap)
        assert(#orders==2, 'faction-squad blueprint IDs select actual available engineers')
        assert(orders[1].builder==free.h and orders[1].structure=='T3EnergyProduction')
        assert(orders[2].builder==busy.h and orders[2].structure=='T1EnergyProduction',
            'choosing a later pool member reserves it without consuming the earlier T1')

    )");
    INFO(ai.lastError());
    REQUIRE(ok);
}

TEST_CASE("FAF scouts retain exploration assignments and rotate interest areas",
          "[faf][ai][scouting]") {
    const auto root = corpusRoot();
    if (root.empty()) SKIP("no vendored corpus; run `make ai`");
    FafAi ai(root);
    REQUIRE(installFafDriver(ai));
    importAiEntryPoints(ai);
    const bool ok = ai.eval(R"(
        __rm_faf_boot(0, { faction=1, startX=0,startZ=0, sizeX=4096,sizeZ=4096,
            armies=2,base='NormalMain',markers={}, scoutSites={
                {x=1000,z=1000,high=true}, {x=2000,z=2000,high=false}} })
        local brain=__rm_faf.brains[0]
        local builders={}
        for _,item in ipairs(brain.builders) do
            if item.spec.BuilderName=='T1 Land Scout Form' then table.insert(builders,item) end
        end
        assert(#builders==1)
        brain.builders=builders
        __rm_faf_type('SCOUT',{'MOBILE','LAND','SCOUT','TECH1'})
        local scout={h=__rm_faf_handle(1,1),bp='SCOUT',x=0,z=0,idle=true,
            __cats=__rm_faf.cats.SCOUT,vision=100}
        local snap={tick=0,units={scout},occupied={},underway={},enemies={},
            mass=500,energy=5000,massStorage=500,energyStorage=5000,
            massIncome=10,energyIncome=100,massRequested=1,energyRequested=1,
            massUsage=1,energyUsage=1,structuresUnderway=0,mobileUnderway=0}
        -- The native path binding is tested separately; inspect the planner's destinations.
        __rm_faf_scout_route=function(h,x,z) return {{x,z}} end
        local orders=__rm_faf_decide(0,snap)
        assert(#orders==1 and orders[1].kind=='scout' and orders[1].x==1000)
        scout.idle=false; snap.tick=10
        assert(#__rm_faf_decide(0,snap)==0, 'keep the existing route while moving')
        scout.idle=true; scout.x=1000; scout.z=1000; snap.tick=20
        orders=__rm_faf_decide(0,snap)
        assert(#orders==1 and orders[1].x==2000, 'follow a high-priority visit with a low visit')
        scout.h=__rm_faf_handle(1,2); snap.tick=30
        orders=__rm_faf_decide(0,snap)
        assert(#orders==1, 'a dead scout must release its formation slot')
        __rm_faf_type('AIRSCOUT',{'MOBILE','AIR','SCOUT','TECH1'})
        scout.bp='AIRSCOUT'; scout.__cats=__rm_faf.cats.AIRSCOUT; scout.h=__rm_faf_handle(2,1)
        snap.tick=40
        orders=__rm_faf_decide(0,snap)
        assert(#orders==1 and orders[1].kind=='scout')
        assert(orders[1].x~=1000 or orders[1].z~=1000, 'air scouts use a vision-offset flyby')
        assert(orders[1].x>=40 and orders[1].x<=4056)
        assert(orders[1].z>=40 and orders[1].z<=4056)
        scout.bp='SCOUT'; scout.__cats=__rm_faf.cats.SCOUT
        brain.scoutAssignments={}; snap.tick=45
        __rm_faf_scout_route=function() return {} end
        orders=__rm_faf_decide(0,snap)
        assert(#orders==1 and #orders[1].route==1,
            'ScoutingAI still issues its final destination when safe path search fails')
        scout.scoutingBusy=true; snap.tick=46
        assert(#__rm_faf_decide(0,snap)==0, 'a pending native move retains the assignment')
        scout.scoutingBusy=false
        brain.builders[1].spec.Priority=0
        brain.scoutAssignments={}; snap.tick=50
        assert(#__rm_faf_decide(0,snap)==0, 'priority zero disables a builder')
    )");
    INFO(ai.lastError());
    REQUIRE(ok);
}

TEST_CASE("native scout routes respect terrain and observed threat", "[faf][ai][scouting]") {
    const auto root = corpusRoot();
    const char* home = std::getenv("HOME");
    const auto contentRoot = home ? std::filesystem::path{home} / "projects/llm/input/faf"
                                  : std::filesystem::path{};
    if (root.empty() || !std::filesystem::exists(contentRoot / "units/UEL0101/UEL0101_unit.bp")) {
        SKIP("requires the vendored FAF corpus and extracted retail unit blueprints");
    }
    rm::vfs::Vfs content;
    content.mountDirectory(contentRoot);
    auto scene = std::make_unique<rm::app::UnitScene>();
    scene->armies = {{.index = 0, .alliance = 0}, {.index = 1, .alliance = 1}};
    scene->economies.resize(2);
    auto scout = rm::unitbp::loadFile(contentRoot / "units/UEL0101/UEL0101_unit.bp");
    REQUIRE(scout);
    scene->definitions.push_back(*scout);
    const auto type = scene->catalog.add(&scene->definitions.back(), rm::sim::TickRate{});
    const auto id = scene->store.spawn({
        .type = type,
        .transform = {.x = rm::sim::Fx::fromInt(32), .z = rm::sim::Fx::fromInt(256)},
        .motion = {.armyIndex = 0},
        .health = {.current = rm::sim::magFromFloat(100), .maximum = rm::sim::magFromFloat(100)},
    });
    rm::HeightField field{.squaresX = 512, .squaresZ = 512};
    field.raw.resize(field.sampleCount());
    const std::array<rm::mapinfo::StartPosition, 4> starts{{
        {.x = 32, .z = 256}, {.x = 4000, .z = 256},
        {.x = 64, .z = 256}, {.x = 3900, .z = 256},
    }};
    const rm::ai::World world{.scene = *scene, .content = content, .field = field,
                              .starts = starts, .markers = {}};
    FafAi ai(root);
    REQUIRE(installFafDriver(ai));
    importAiEntryPoints(ai);
    rm::ai::FafOpponent opponent(ai, 0);
    opponent.observe(world, {});
    opponent.advance(0);
    REQUIRE(ai.eval(R"(
        local sites=__rm_faf.brains[0].scoutSites
        assert(#sites==2, 'vacant starts near allied territory do not need scouting')
        assert(sites[1].x==4000 and sites[1].high)
        assert(sites[2].x==3900 and not sites[2].high)
    )"));
    const auto decisions = opponent.drain();
    REQUIRE_FALSE(decisions.empty());
    for (std::size_t i = 0; i < decisions.size(); ++i) {
        CHECK(decisions[i].kind == rm::ai::Decision::Kind::Move);
        CHECK(decisions[i].unit == id);
        CHECK(decisions[i].queued == (i > 0));
    }
    CHECK(decisions.back().toX == rm::sim::Fx::fromInt(4000));
    CHECK(decisions.back().toZ == rm::sim::Fx::fromInt(256));
    REQUIRE(ai.eval("scoutHandle=__rm_faf_handle(" + std::to_string(id.index) + ","
                    + std::to_string(id.generation) + ")"));
    SECTION("dry ground reaches the requested destination") {
        REQUIRE(ai.eval(R"(
            local route=__rm_faf_scout_route(scoutHandle,480,256,64,0)
            assert(#route>0)
            assert(route[#route][1]==480 and route[#route][2]==256)
        )"));
    }
    SECTION("ground scouts cannot cross deep water but flying scouts can") {
        scene->waterLevelElmos = 100;
        REQUIRE(ai.eval("assert(#__rm_faf_scout_route(scoutHandle,480,256,64,0)==0)"));
        scene->store.motion()[id.index].canFly = true;
        REQUIRE(ai.eval(R"(
            local route=__rm_faf_scout_route(scoutHandle,480,256,64,0)
            assert(#route==1 and route[1][1]==480 and route[1][2]==256)
        )"));
    }
    SECTION("a hostile threat cell is avoided only when observed") {
        // Isolate the authored 400 AntiSurface cutoff with a single strong contact.
        auto enemy = *scout;
        enemy.surfaceThreat = 401;
        scene->definitions.push_back(enemy);
        const auto enemyType = scene->catalog.add(&scene->definitions.back(), rm::sim::TickRate{});
        (void)scene->store.spawn({
            .type = enemyType,
            .transform = {.x = rm::sim::Fx::fromInt(256), .z = rm::sim::Fx::fromInt(256)},
            .motion = {.armyIndex = 1},
            .health = {.current = rm::sim::magFromFloat(100), .maximum = rm::sim::magFromFloat(100)},
        });
        // Both opponents share one VM. Registering army 1 last must not make
        // army 0's scout use army 1's alliances when evaluating a route.
        rm::ai::FafOpponent otherOpponent(ai, 1);
        otherOpponent.observe(world, {});
        otherOpponent.advance(0);
        // Inactive intel is the simulation's explicit all-visible mode.
        REQUIRE(ai.eval(R"(
            local route=__rm_faf_scout_route(scoutHandle,480,256,64,0)
            assert(#route>0)
            for _,p in ipairs(route) do
                assert(not (p[1]>=256 and p[1]<320 and p[2]>=256 and p[2]<320))
            end
        )"));
        scene->intel.configure(2, rm::sim::Fx::fromInt(512), rm::sim::Fx::fromInt(512),
                               rm::sim::VisionStyle::ForgedAlliance);
        REQUIRE(ai.eval(R"(
            local route=__rm_faf_scout_route(scoutHandle,480,256,64,0)
            assert(#route>0)
            -- Native path cells span eight heightmap squares (64 elmos).
            for _,p in ipairs(route) do assert(p[2]==288 or p[2]==256) end
        )"));
    }
}

TEST_CASE("all FAF strategies load and scored selection is repeatable", "[faf][ai-personality]") {
    const auto root = corpusRoot();
    if (root.empty()) SKIP("no vendored corpus; run `make ai`");
    FafAi ai(root);
    REQUIRE(ai.ready());
    REQUIRE(installFafDriver(ai));
    importAiEntryPoints(ai);
    const bool ok = ai.eval(R"(
        local originalRandom, originalMapSize = Random, GetMapSize
        local function boot(base, size, army)
            __rm_faf_boot(army, { faction = 1, startX = 100, startZ = 100,
                sizeX = size, sizeZ = size, armies = 2, base = base,
                waterRatio = 0, markers = {} })
            local brain = __rm_faf.brains[army]
            assert(#brain.builders > 0, base .. ' has no builders')
            assert(Random == originalRandom and GetMapSize == originalMapSize,
                'selection must not replace gameplay globals')
            return brain.baseTemplate
        end
        for _, base in ipairs({ 'NormalMain', 'ChallengeMain', 'TechMain', 'RushMainLand',
            'RushMainAir', 'RushMainNaval', 'RushMainBalanced', 'TurtleMain' }) do
            assert(boot(base, 2048, 0) == base)
        end
        -- 2048 elmos = 256 FAF map units: real FAF strongly favors land rush here.
        assert(boot('adaptive', 2048, 0) == 'RushMainLand')
        local first = boot('random', 2048, 0)
        assert(first == boot('random', 2048, 0), 'same setup must select the same base')
        local large = boot('adaptive', 8192, 1)
        assert(large == boot('adaptive', 8192, 1))
    )");
    INFO(ai.lastError());
    REQUIRE(ok);
}

TEST_CASE("a viable water map exposes all surface-naval tiers without duplicate builders", "[faf][ai][naval-ai]") {
    const std::filesystem::path root = corpusRoot();
    if (root.empty()) {
        SKIP("no vendored corpus; run `make ai`");
    }
    FafAi ai(root);
    REQUIRE(ai.ready());
    REQUIRE(installFafDriver(ai));
    importAiEntryPoints(ai);

    const bool ok = ai.eval(R"(
        __rm_faf_boot(0, { faction = 1, startX = 100, startZ = 100,
                           sizeX = 512, sizeZ = 512, armies = 2,
                           base = 'NormalMain', markers = {}, hasNavalSite = true,
                           navalX = 3000, navalZ = 3000 })
        local names = {}
        for _, item in ipairs(__rm_faf.brains[0].builders) do
            assert(not names[item.spec.BuilderName], 'duplicate builder: ' .. item.spec.BuilderName)
            names[item.spec.BuilderName] = true
        end
        assert(names['T1 Naval Factory Builder'], 'no naval factory builder')
        assert(names['T2 Naval Factory Builder'], 'no T2 engineer naval builder')
        assert(names['T3 Naval Factory Builder'], 'no T3 engineer naval builder')
        assert(names['T1 Sea Frigate - init'], 'no T1 frigate builder')
        assert(names['Frequent Sea Attack T1'], 'no surface fleet former')
        assert(names['Frequent Sea Attack T2'], 'no T2 fleet former')
        assert(names['Frequent Sea Attack T3'], 'no T3 fleet former')
        assert(names['T1 Sea Factory Upgrade Slow'], 'no T2 naval upgrade')
        assert(names['T2 Sea Factory Upgrade Slow'], 'no T3 naval upgrade')
        assert(__rm_faf.brains[0].BuilderManagers.MAIN.BaseSettings.FactoryCount.Sea == 1)
        local brain = __rm_faf.brains[0]
        local position = brain:PBMGetLocationCoords('NAVAL')
        assert(position[1] == 3000 and position[3] == 3000, 'fleet base must be at the shipyard')
        __rm_faf_type('SHIP', {'MOBILE', 'NAVAL'}, {s=200})
        brain.snap.units = {{bp='SHIP', x=3000, z=3000, __cats=__rm_faf.cats.SHIP}}
        assert(import('/lua/AI/AIBuilders/AISeaAttackBuilders.lua').SeaAttackCondition(brain, 'NAVAL', 100),
            'ships far from the land base must contribute to fleet formation')
    )");
    INFO(ai.lastError());
    REQUIRE(ok);
}

TEST_CASE("FAF pool counts and fleet threat convert authored radii to elmos", "[faf][naval-ai][radius]") {
    const auto root=corpusRoot();
    if (root.empty()) SKIP("requires FAF corpus");
    FafAi ai(root);
    REQUIRE(installFafDriver(ai));
    importAiEntryPoints(ai);
    const bool ok=ai.eval(R"(
        __rm_faf_boot(0,{faction=1,startX=100,startZ=100,sizeX=4096,sizeZ=4096,
            armies=2,base='NormalMain',markers={}})
        local brain=__rm_faf.brains[0]
        __rm_faf_type('SHIP',{'MOBILE','NAVAL'},{s=20})
        brain.snap.units={}
        for _,x in ipairs({1699,1700,1701}) do
            brain.snap.units[#brain.snap.units+1]={bp='SHIP',x=x,z=100,__cats=__rm_faf.cats.SHIP}
        end
        local pool=brain:GetPlatoonUniquelyNamed('ArmyPool')
        local category=categories.MOBILE*categories.NAVAL
        local manager=brain.BuilderManagers.MAIN.EngineerManager
        assert(manager.Radius==100,'MAIN uses the radius passed by the authored brain setup')
        manager.Radius=200 -- authored ogrids; observed coordinates are elmos
        assert(pool:GetNumCategoryUnits(category,manager.Location,manager.Radius)==2)
        assert(pool:GetPlatoonThreat('Surface',category,manager.Location,manager.Radius)==40)
        assert(pool:GetNumCategoryUnits(category)==3)
        assert(pool:GetPlatoonThreat('Surface',category)==60)
        local sea=import('/lua/AI/AIBuilders/AISeaAttackBuilders.lua')
        assert(sea.SeaAttackCondition(brain,'MAIN',39))
        assert(not sea.SeaAttackCondition(brain,'MAIN',40))
    )");
    INFO(ai.lastError());
    REQUIRE(ok);
}

TEST_CASE("import hands back the module's environment, which is how FAF exports",
          "[faf][ai]") {
    const std::filesystem::path root = corpusRoot();
    if (root.empty()) {
        SKIP("no vendored corpus; run `make ai`");
    }
    FafAi ai(root);
    REQUIRE(ai.ready());

    // BuilderManager.lua never returns a value; it declares `BuilderManager` at file scope.
    // Moho's import returns the module environment, so the symbol must be reachable — this
    // is the semantic whose absence took the whole manager layer down with Class(nil).
    REQUIRE(ai.eval(R"(
        local m = import('/lua/sim/BuilderManager.lua')
        assert(type(m.BuilderManager) == 'table', 'BuilderManager not exported')
    )"));
}

TEST_CASE("the sanity report names every distinct condition failure", "[faf][ai]") {
    std::vector<std::string> errors;
    std::string expected = "  CONDITION ERRORS (each one fails closed):\n";
    for (int i = 1; i <= 10; ++i) {
        errors.push_back("condition failure " + std::to_string(i));
        expected += "    " + errors.back() + '\n';
    }

    CHECK(rm::ai::formatFafConditionErrorReport(errors) == expected);
    CHECK(rm::ai::formatFafConditionErrorReport({}).empty());
}

TEST_CASE("an exhausted instruction budget is one error, not a cascade", "[faf][ai]") {
    const std::filesystem::path root = corpusRoot();
    if (root.empty()) {
        SKIP("no vendored corpus; run `make ai`");
    }
    FafAi ai(root);
    REQUIRE(ai.ready());

    // The corpus wraps condition calls in pcall. A runaway condition is caught there, and
    // the work after it must run on a fresh budget: the loop below is a hundred times the
    // hook interval, and used to die after the first thousand instructions.
    REQUIRE(ai.eval(R"(
        local ok, err = pcall(function() while true do end end)
        assert(not ok and string.find(err, 'instruction budget'), tostring(err))
        local sum = 0
        for i = 1, 100000 do sum = sum + 1 end
        assert(sum == 100000)
    )"));
    // Not tested, because no hook can promise it: a chunk that swallows the error in pcall
    // forever cannot be stopped from inside the VM. The refill cap only bounds how much
    // budget such a chunk is GIVEN; the corpus is not adversarial and never does this.
}

TEST_CASE("caught watchdog overruns cannot reset the per-evaluation ceiling", "[faf][ai][watchdog]") {
    const auto root = corpusRoot();
    if (root.empty()) SKIP("no vendored corpus; run `make ai`");
    FafAi ai(root);
    REQUIRE_FALSE(ai.eval(R"(
        for attempt = 1, 6 do
            local ok = pcall(function()
                local n = 0
                for i = 1, 50000000 do n = n + i end
            end)
            assert(not ok)
            local n = 0
            for i = 1, 2000 do n = n + i end
        end
    )"));
    REQUIRE(ai.lastError().find("instruction budget") != std::string::npos);
    REQUIRE(ai.eval("local n=0; for i=1,10000 do n=n+1 end; assert(n==10000)"));
}

TEST_CASE("FAF repeated category queries fit the decision budget for a large army",
          "[faf][ai][category-budget]") {
    const auto root = corpusRoot();
    if (root.empty()) SKIP("no vendored corpus; run `make ai`");
    FafAi ai(root);
    REQUIRE(installFafDriver(ai));
    REQUIRE(ai.eval(R"(
        __rm_faf_type('UEL0201', {'MOBILE','LAND','TECH1','DIRECTFIRE'})
        armyUnits = {}
        for i=1,600 do armyUnits[i] = {bp='UEL0201', __cats=__rm_faf.cats.UEL0201} end
    )"));
    // Equivalent expressions are recreated by many builders each pass. Their answers
    // depend on blueprint categories, not the number of identical tanks in the army.
    const bool ok = ai.eval(R"(
        for query=1,400 do
            local category = (categories.MOBILE * categories.LAND * categories.TECH1
                * categories.DIRECTFIRE) - (categories.ENGINEER + categories.SCOUT)
            assert(EntityCategoryCount(category, armyUnits) == 600)
        end
    )");
    INFO(ai.lastError());
    REQUIRE(ok);
}

TEST_CASE("FAF manager assistance reserves helpers and releases the completed work", "[faf][assist]") {
    const auto root=corpusRoot();
    if (root.empty()) SKIP("requires FAF corpus");
    FafAi ai(root);
    REQUIRE(installFafDriver(ai));
    importAiEntryPoints(ai);
    const bool ok=ai.eval(R"(
        __rm_faf_boot(0,{faction=1,startX=0,startZ=0,sizeX=4096,sizeZ=4096,
            armies=2,base='TechMain',markers={}})
        local brain=__rm_faf.brains[0]
        local assist={kind='EngineerBuilder',spec={BuilderName='test assistance',Priority=1000,
            InstanceCount=1,PlatoonTemplate='T3EngineerAssist',BuilderConditions={},
            BuilderData={Assist={AssistLocation='MAIN',AssisteeType='Engineer',
                BeingBuiltCategories={'EXPERIMENTAL'},AssistUntilFinished=true,AssistRange=80}}}}
        brain.builders={assist}
        local helper={h=1,x=0,z=0,idle=true,__cats={ENGINEER=true,TECH3=true}}
        local builder={h=2,x=400,z=0,idle=true,building=true,guardCount=0,
            __cats={ENGINEER=true,TECH3=true}}
        local work={builder=2,bp='EXPERIMENTAL',command=10,remaining=1,x=400,z=0,
            __cats={EXPERIMENTAL=true,MOBILE=true,LAND=true}}
        local snap={tick=0,units={helper,builder},underway={work},occupied={},
            massIncome=10,energyIncome=100,massRequested=1,energyRequested=1,massUsage=1,energyUsage=1}
        brain.snap=snap
        local manager=brain.BuilderManagers.MAIN.EngineerManager
        assert(#manager:GetEngineersWantingAssistance(categories.EXPERIMENTAL,categories.TECH3)==1)
        local orders=__rm_faf_decide(0,snap)
        assert(#orders==1 and orders[1].kind=='guard' and orders[1].builder==1 and orders[1].target==2,
            'a helper 50 ogrids away must receive a native guard order; count='..#orders
                ..' plan='..tostring(PlatoonTemplates.T3EngineerAssist and PlatoonTemplates.T3EngineerAssist.Plan))
        local second={h=3,x=0,z=0,idle=true,__cats={ENGINEER=true,TECH3=true}}
        snap.units={helper,builder,second}; snap.tick=10
        assert(#__rm_faf_decide(0,snap)==0,'the assigned helper retains its slot and InstanceCount')
        -- A new order at the same site is different work, even with the same blueprint.
        work.command=11; snap.tick=25
        orders=__rm_faf_decide(0,snap)
        assert(orders[1] and orders[1].kind=='stop' and orders[1].builder==1)
        brain.builders={}
        snap.units={builder,second}; snap.tick=40
        __rm_faf_decide(0,snap)
        assert(brain.assists[1]==nil,'dead helpers leave no generation-stale reservation')
        builder.desiresAssist=false
        assert(#manager:GetEngineersWantingAssistance(categories.EXPERIMENTAL,categories.TECH3)==0)
    )");
    INFO(ai.lastError());
    REQUIRE(ok);
}

TEST_CASE("FAF assistance honors target priorities limits and authored seconds", "[faf][assist]") {
    const auto root=corpusRoot();
    if (root.empty()) SKIP("requires FAF corpus");
    FafAi ai(root);
    REQUIRE(installFafDriver(ai));
    importAiEntryPoints(ai);
    // platoon.lua EconAssistBody / ManagerEngineerAssistAI: category order,
    // strict range and guard caps, then ten ticks plus Time seconds of assistance.
    const bool ok=ai.eval(R"(
        __rm_faf_boot(0,{faction=1,startX=0,startZ=0,sizeX=4096,sizeZ=4096,
            armies=2,base='TechMain',markers={},fafTickScale=2})
        local brain=__rm_faf.brains[0]
        local data={AssistLocation='MAIN',AssisteeType='Engineer',Time=3,
            BeingBuiltCategories={'EXPERIMENTAL','ENERGYPRODUCTION'}}
        local item={kind='EngineerBuilder',spec={BuilderName='timed assistance',Priority=1000,
            InstanceCount=1,PlatoonTemplate='T3EngineerAssist',BuilderConditions={},
            BuilderData={Assist=data}}}
        local helper={h=1,x=0,z=0,idle=true,__cats={ENGINEER=true,TECH3=true}}
        local near={h=2,x=80,z=0,building=true,guardCount=2,__cats={ENGINEER=true,TECH3=true}}
        local far={h=3,x=400,z=0,building=true,guardCount=0,__cats={ENGINEER=true,TECH3=true}}
        local work={builder=2,bp='experimental',command=1,remaining=1,x=80,z=0,
            __cats={EXPERIMENTAL=true}}
        local second={builder=3,bp='experimental',command=2,remaining=1,x=400,z=0,
            __cats={EXPERIMENTAL=true}}
        local snap={tick=0,units={helper,near,far},underway={work,second},occupied={},
            massIncome=10,energyIncome=100,massRequested=1,energyRequested=1,massUsage=1,energyUsage=1}
        local function choose()
            brain.assists={}; brain.builders={item}; helper.__taken=nil
            return __rm_faf_decide(0,snap)
        end
        item.spec.BuilderData.NumAssistees=3
        assert(choose()[1].target==3,'fewest guards wins over proximity')
        assert(brain.assistLimits[1]==3,'every engineer platoon assignment applies NumAssistees')
        item.spec.BuilderData.NumAssistees=nil
        data.AssistClosestUnit=true
        assert(choose()[1].target==2,'closest selection is explicitly authored')
        assert(brain.assistLimits[1]==nil,'a new assignment clears an omitted NumAssistees limit')
        near.guardCount=20
        assert(choose()[1].target==3,'twenty guards excludes even the closest target')
        near.guardCount=2; brain.assistLimits[2]=2
        assert(choose()[1].target==3,'NumAssistees caps a builder below twenty')
        brain.assistLimits={}; near.x=640; far.x=800
        second.__cats={ENERGYPRODUCTION=true}; far.x=40
        assert(#choose()==0,'first nonempty category at the strict range boundary blocks fallback')
        near.x=80; second.__cats={EXPERIMENTAL=true}; far.x=400
        assert(choose()[1].target==2)
        brain.builders={}
        snap.underway={}; snap.tick=79
        assert(#__rm_faf_decide(0,snap)==0,'timed assistance waits even if construction ends')
        snap.tick=80
        local orders=__rm_faf_decide(0,snap)
        assert(#orders==1 and orders[1].kind=='stop' and orders[1].builder==1,
            'ten FAF ticks plus three seconds becomes eighty native ticks at scale two')
        assert(brain.assists[1]==nil)
    )");
    INFO(ai.lastError());
    REQUIRE(ok);
}

TEST_CASE("FAF assistance uses registered manager membership rather than distance", "[faf][membership]") {
    const auto root=corpusRoot();
    if (root.empty()) SKIP("requires FAF corpus");
    FafAi ai(root);
    REQUIRE(installFafDriver(ai));
    importAiEntryPoints(ai);
    const bool ok=ai.eval(R"(
        __rm_faf_boot(0,{faction=1,startX=0,startZ=0,sizeX=4096,sizeZ=4096,
            armies=2,base='TechMain',markers={},hasNavalSite=true,navalX=400,navalZ=0})
        local brain=__rm_faf.brains[0]
        local main=brain.BuilderManagers.MAIN.EngineerManager
        local naval=brain.BuilderManagers.NAVAL.EngineerManager
        local first={h=1,x=0,z=0,__cats={ENGINEER=true,TECH3=true}}
        local second={h=2,x=1,z=0,__cats={ENGINEER=true,TECH3=true}}
        brain.snap={units={first,second},underway={
            {builder=1,__cats={EXPERIMENTAL=true}}, {builder=2,__cats={EXPERIMENTAL=true}}}}
        main:AddUnit(first)
        naval:AddUnit(second)
        local function targets(manager)
            return manager:GetEngineersWantingAssistance(categories.EXPERIMENTAL,categories.TECH3)
        end
        assert(#targets(main)==1 and targets(main)[1].h==1)
        assert(#targets(naval)==1 and targets(naval)[1].h==2,
            'a nearby engineer remains registered with its own base')
        second.x=3000
        assert(#targets(naval)==1,'movement alone cannot transfer ownership')
        main:AddUnit(second)
        assert(#targets(naval)==0 and #targets(main)==2,'explicit registration transfers membership')
        main:RemoveUnit(second)
        assert(#targets(main)==1,'unmanaged engineers are not implicitly MAIN members')
        naval:AddUnit(second)
        first.building=true; second.idle=true; second.x=1
        brain.snap.underway={brain.snap.underway[1]}
        brain.snap.occupied={}
        brain.snap.massIncome=10; brain.snap.energyIncome=100
        brain.snap.massUsage=1; brain.snap.energyUsage=1
        brain.snap.massRequested=1; brain.snap.energyRequested=1
        brain.builders={{kind='EngineerBuilder',spec={BuilderName='MAIN assistance',Priority=1000,
            PlatoonTemplate='T3EngineerAssist',BuilderConditions={},
            BuilderData={Assist={AssistLocation='MAIN',AssisteeType='Engineer',
                BeingBuiltCategories={'EXPERIMENTAL'}}}}}}
        assert(#__rm_faf_decide(0,brain.snap)==0,'MAIN cannot recruit a NAVAL helper')
        main:AddUnit(second)
        local orders=__rm_faf_decide(0,brain.snap)
        assert(#orders==1 and orders[1].kind=='guard' and orders[1].builder==2)
        brain.unitLocations[first.h]='NAVAL'
        brain.builders={}; brain.snap.units={second}
        __rm_faf_decide(0,brain.snap)
        assert(brain.unitLocations[first.h]=='NAVAL','unfinished work retains its dead founder provenance')
    )");
    INFO(ai.lastError());
    REQUIRE(ok);
}

TEST_CASE("FAF manager counts use membership and the correct producer category", "[faf][membership-count]") {
    const auto root=corpusRoot();
    if (root.empty()) SKIP("requires FAF corpus");
    FafAi ai(root);
    REQUIRE(installFafDriver(ai));
    importAiEntryPoints(ai);
    const bool ok=ai.eval(R"(
        __rm_faf_boot(0,{faction=1,startX=0,startZ=0,sizeX=4096,sizeZ=4096,
            armies=2,base='TechMain',markers={},hasNavalSite=true,navalX=400,navalZ=0})
        local brain=__rm_faf.brains[0]
        local main,naval=brain.BuilderManagers.MAIN,brain.BuilderManagers.NAVAL
        local eng={h=1,__cats={ENGINEER=true,TECH2=true},desiresAssist=false}
        local other={h=2,__cats={ENGINEER=true,TECH2=true}}
        local fac={h=3,__cats={FACTORY=true,STRUCTURE=true,TECH2=true},upgrading=true}
        local yard={h=4,__cats={FACTORY=true,STRUCTURE=true,TECH2=true,NAVAL=true}}
        local tank={h=5,__cats={MOBILE=true,LAND=true,TECH2=true}}
        brain.snap={units={eng,other,fac,yard,tank},underway={
            {builder=1,__cats={TECH3=true}}, {builder=1,__cats={TECH3=true}},
            {builder=2,__cats={TECH3=true}}, {builder=3,__cats={TECH3=true}}}}
        main.EngineerManager:AddUnit(eng)
        naval.EngineerManager:AddUnit(other)
        main.FactoryManager:AddFactory(fac)
        naval.FactoryManager:AddFactory(yard)
        assert(main.EngineerManager:GetNumCategoryUnits('Engineers',categories.TECH2)==1,
            'the other base engineer cannot consume MAIN capacity')
        assert(naval.EngineerManager:GetNumCategoryUnits('Engineers',categories.TECH2)==1)
        assert(main.FactoryManager:GetNumFactories()==1)
        assert(naval.FactoryManager:GetNumFactories()==1)
        assert(main.FactoryManager:GetNumCategoryFactories(categories.TECH2)==1,
            'engineers and tanks are not factories')
        assert(main.EngineerManager:GetNumCategoryBeingBuilt(categories.TECH3,categories.TECH2)==1,
            'count producers once and do not apply assistance willingness')
        assert(main.EngineerManager:GetNumCategoryBeingBuilt(categories.TECH3,categories.TECH3)==0)
        assert(main.FactoryManager:GetNumCategoryBeingBuilt(categories.TECH3,categories.TECH2)==1,
            'factory upgrades count in the factory manager, not the engineer manager')
        assert(#main.FactoryManager:GetFactoriesWantingAssistance(categories.TECH3,categories.TECH2)==1)
        fac.desiresAssist=false
        assert(#main.FactoryManager:GetFactoriesWantingAssistance(categories.TECH3,categories.TECH2)==0)
        assert(main.FactoryManager:GetNumCategoryBeingBuilt(categories.TECH3,categories.TECH2)==1)
        assert(naval.FactoryManager:GetNumCategoryBeingBuilt(categories.TECH3,categories.ALLUNITS)==0)
        eng.upgrading=true
        assert(main.EngineerManager:GetNumCategoryBeingBuilt(categories.TECH3,categories.ALLUNITS)==0)
        main.EngineerManager:AddUnit(other)
        assert(main.EngineerManager:GetNumCategoryUnits('Engineers',categories.TECH2)==2)
        assert(naval.EngineerManager:GetNumCategoryUnits('Engineers',categories.TECH2)==0,
            'registration invalidates counts within the same snapshot')
        main.EngineerManager:RemoveUnit(other)
        assert(main.EngineerManager:GetNumCategoryUnits('Engineers',categories.TECH2)==1)
    )");
    INFO(ai.lastError());
    REQUIRE(ok);
}

TEST_CASE("FAF assistance queries fit the budget with a large army", "[faf][assist-budget]") {
    const auto root=corpusRoot();
    if (root.empty()) SKIP("requires FAF corpus");
    FafAi ai(root);
    REQUIRE(installFafDriver(ai));
    importAiEntryPoints(ai);
    const bool ok=ai.eval(R"(
        __rm_faf_boot(0,{faction=1,startX=0,startZ=0,sizeX=4096,sizeZ=4096,
            armies=2,base='TechMain',markers={}})
        local brain=__rm_faf.brains[0]
        local units,work={},{}
        for i=1,1200 do units[i]={h=i,__cats={MOBILE=true,LAND=true}} end
        for i=1,80 do
            units[#units+1]={h=1200+i,__cats={ENGINEER=true,TECH3=true}}
            work[i]={builder=1200+i,__cats={EXPERIMENTAL=true}}
        end
        brain.snap={units=units,underway=work}
        local manager=brain.BuilderManagers.MAIN.EngineerManager
        for i=1,400 do
            assert(#manager:GetEngineersWantingAssistance(categories.EXPERIMENTAL,categories.TECH3)==80)
        end
    )");
    INFO(ai.lastError());
    REQUIRE(ok);
}

TEST_CASE("FAF completion notifications inherit the founder's current manager", "[faf][membership]") {
    const auto root=corpusRoot();
    if (root.empty()) SKIP("requires FAF corpus");
    FafAi ai(root);
    REQUIRE(installFafDriver(ai));
    importAiEntryPoints(ai);
    rm::app::UnitScene scene;
    scene.armies=rm::sim::freeForAll(2);
    scene.economies.resize(2);
    rm::HeightField field{.squaresX=64,.squaresZ=64};
    field.raw.resize(field.sampleCount());
    rm::vfs::Vfs content;
    const std::array<rm::mapinfo::StartPosition,2> starts{{{.x=0,.z=0},{.x=400,.z=400}}};
    const rm::ai::World world{.scene=scene,.content=content,.field=field,.starts=starts,.markers={}};
    rm::ai::FafOpponent opponent(ai,0,"TechMain");
    opponent.observe(world,{});
    opponent.advance(0);
    REQUIRE(ai.eval(R"(
        local brain=__rm_faf.brains[0]
        brain.unitLocations[4294967296]='NAVAL'
    )"));
    // Events carry generation-qualified handles even if an upgrade already replaced
    // its founder. A later birth in the same stream can inherit from that replacement.
    const rm::sim::UnitId founder{0,1},factory{1,1},engineer{2,1};
    const std::array events{
        rm::sim::Event{.kind=rm::sim::EventKind::UnitFinished,.unit=factory,
            .instigator=founder,.builder=founder,.army=0},
        rm::sim::Event{.kind=rm::sim::EventKind::UnitFinished,.unit=engineer,
            .builder=factory,.army=0}};
    opponent.observe(world,events);
    REQUIRE(ai.eval(R"(
        local locations=__rm_faf.brains[0].unitLocations
        assert(locations[4294967297]=='NAVAL')
        assert(locations[4294967298]=='NAVAL')
        assert(locations[8589934593]==nil,'a recycled index is a different member')
    )"));
}

TEST_CASE("FAF air and naval factories can train mobile construction units", "[faf][factory-engineer]") {
    const auto root=corpusRoot();
    if (root.empty()) SKIP("requires FAF corpus");
    FafAi ai(root);
    REQUIRE(installFafDriver(ai));
    importAiEntryPoints(ai);
    const bool ok=ai.eval(R"(
        __rm_faf_boot(0,{faction=1,startX=0,startZ=0,sizeX=4096,sizeZ=4096,
            armies=2,base='TechMain',markers={}})
        local brain=__rm_faf.brains[0]
        local engineer
        for _,item in ipairs(brain.builders) do
            if item.spec.BuilderName=='T3 Engineer Disband - Init' then engineer=item end
        end
        assert(engineer)
        brain.builders={engineer}
        -- UEL0309 and all twelve T3 factory blueprints: MOBILE CONSTRUCTION is
        -- buildable independently of a factory's LAND/AIR/NAVAL combat domain.
        __rm_faf_type('UEL0309',{'MOBILE','LAND','CONSTRUCTION','ENGINEER','TECH3','BUILTBYTIER3FACTORY'})
        for _,domain in ipairs({'LAND','AIR','NAVAL'}) do
            local factory={h=1,bp='factory',idle=true,x=0,z=0,
                __cats={STRUCTURE=true,FACTORY=true,TECH3=true,[domain]=true}}
            local snap={units={factory},occupied={},underway={},
                massIncome=10,energyIncome=100,massRequested=1,energyRequested=1,
                massUsage=1,energyUsage=1}
            local orders=__rm_faf_decide(0,snap)
            assert(#orders==1 and orders[1].kind=='train' and orders[1].bp=='uel0309',domain)
            factory.__cats.TECH3=nil; factory.__cats.TECH2=true; factory.__taken=nil
            assert(#__rm_faf_decide(0,snap)==0,'cross-domain construction still requires its factory tier')
        end
    )");
    INFO(ai.lastError());
    REQUIRE(ok);
}

TEST_CASE("FAF engineer caps count their consumption group, not all units of the tier", "[faf][engineer-cap]") {
    const auto root=corpusRoot();
    if (root.empty()) SKIP("requires FAF corpus");
    FafAi ai(root);
    REQUIRE(installFafDriver(ai));
    importAiEntryPoints(ai);
    const bool ok=ai.eval(R"(
        __rm_faf_boot(0,{faction=1,startX=0,startZ=0,sizeX=4096,sizeZ=4096,
            armies=2,base='TechMain',markers={}})
        local brain=__rm_faf.brains[0]
        local units={}
        for i=1,9 do units[#units+1]={__cats={ENGINEER=true,TECH2=true}} end
        for i=1,20 do units[#units+1]={__cats={LAND=true,DIRECTFIRE=true,TECH2=true}} end
        units[#units+1]={__cats={ENGINEER=true,ENGINEERSTATION=true,TECH2=true}}
        brain.snap={units=units}
        local manager=brain.BuilderManagers.MAIN.EngineerManager
        local conditions=import('/lua/editor/UnitCountBuildConditions.lua')
        assert(manager:GetNumCategoryUnits('Engineers',categories.TECH2)==9,
            'tanks and engineer stations cannot consume the mobile engineer cap')
        assert(manager:GetNumCategoryUnits('EngineerStations',categories.TECH2)==1)
        assert(manager:GetNumCategoryUnits('Unknown',categories.TECH2)==0)
        assert(conditions.EngineerCapCheck(brain,'MAIN','Tech2'))
        units[#units+1]={__cats={ENGINEER=true,TECH2=true}}
        brain.snap={units=units}
        assert(not conditions.EngineerCapCheck(brain,'MAIN','Tech2'),
            'TechMain caps T2 engineers at ten')
    )");
    INFO(ai.lastError());
    REQUIRE(ok);
}

TEST_CASE("FAF economy over time retains thirty one-second samples", "[faf][economy]") {
    const auto root=corpusRoot();
    if (root.empty()) SKIP("requires FAF corpus");
    FafAi ai(root);
    REQUIRE(installFafDriver(ai));
    importAiEntryPoints(ai);
    const bool ok=ai.eval(R"(
        __rm_faf_boot(0,{faction=1,startX=0,startZ=0,sizeX=4096,sizeZ=4096,
            armies=2,base='TechMain',markers={}})
        local brain=__rm_faf.brains[0]
        brain.builders={}
        local snap={tick=0,units={},occupied={},underway={},
            massIncome=3,energyIncome=90,massRequested=2,energyRequested=60,
            massUsage=2,energyUsage=60}
        __rm_faf_decide(0,snap)
        local eco=brain.EconomyOverTimeCurrent
        assert(math.abs(eco.EnergyIncome-3)<1e-9,
            'EconomyMonitor starts with thirty zero slots, not one instantaneous sample')
        assert(math.abs(eco.MassIncome-0.1)<1e-9)
        assert(math.abs(eco.EnergyTrendOverTime-1)<1e-9)
        assert(math.abs(eco.EnergyEfficiencyOverTime-1.5)<1e-9)
        snap.tick=1
        __rm_faf_decide(0,snap)
        assert(math.abs(eco.EnergyIncome-3)<1e-9, 'sample once per second')
        for tick=10,290,10 do snap.tick=tick; __rm_faf_decide(0,snap) end
        assert(math.abs(eco.EnergyIncome-90)<1e-9)
        assert(math.abs(eco.EnergyTrendOverTime-30)<1e-9)
        local conditions=import('/lua/editor/EconomyBuildConditions.lua')
        assert(conditions.LessThanEnergyTrendOverTime(brain,45),
            'T2 power gate uses 45 per FAF tick, not 45 per second')
        snap.energyRequested=180; snap.energyUsage=90; snap.tick=300
        __rm_faf_decide(0,snap)
        assert(math.abs(eco.EnergyRequested-64)<1e-9)
        assert(math.abs(eco.EnergyEfficiencyOverTime-90/64)<1e-9)
        assert(math.abs(eco.EnergyTrendOverTime-29)<1e-9)
        assert(brain:GetEconomyRequested('ENERGY')==180,
            'combined efficiency still checks the instantaneous demand separately')
        for tick=310,590,10 do snap.tick=tick; __rm_faf_decide(0,snap) end
        assert(math.abs(eco.EnergyEfficiencyOverTime-0.5)<1e-9)
        assert(math.abs(eco.EnergyTrendOverTime)<1e-9)
    )");
    INFO(ai.lastError());
    REQUIRE(ok);
}

TEST_CASE("FAF adaptive opening keeps its commander through authored power and mass phases", "[faf][opening]") {
    const auto root=corpusRoot();
    if (root.empty()) SKIP("requires FAF corpus");
    FafAi ai(root);
    REQUIRE(installFafDriver(ai));
    importAiEntryPoints(ai);
    const bool ok=ai.eval(R"(
        __rm_faf_boot(0,{faction=1,startX=200,startZ=200,sizeX=4096,sizeZ=4096,
            armies=2,base='TechMain',markers={}})
        local brain=__rm_faf.brains[0]
        local initial
        for _,item in ipairs(brain.builders) do
            if item.spec.BuilderName=='CDR Initial Default' then initial=item end
        end
        assert(initial)
        brain.builders={initial}
        __rm_faf_type('UEL0001',{'COMMAND','ENGINEER','MOBILE','TECH1'})
        local acu=setmetatable({h=1,x=200,z=200,bp='UEL0001',idle=true,
            __cats=__rm_faf.cats.UEL0001},__rm_faf.unitMeta)
        local snap={tick=0,units={acu},occupied={},underway={},mass=500,
            massIncome=1,energyIncome=1,massRequested=1,energyRequested=1,
            massUsage=1,energyUsage=1}
        __rm_faf_opening_survey=function(h)
            assert(h==acu.h)
            return {close={{240,200},{200,240},{160,200},{200,160}},distant={}}
        end
        local orders=__rm_faf_decide(0,snap)
        assert(#orders==2 and orders[1].structure=='T1LandFactory')
        assert(orders[2].structure=='T1Resource' and orders[2].site[1]==240)
        assert(not orders[1].queued and orders[2].queued)
        acu.queueBusy=true
        snap.tick=10
        assert(#__rm_faf_decide(0,snap)==0, 'queued work owns the commander')
        acu.queueBusy=false
        snap.tick=20
        orders=__rm_faf_decide(0,snap)
        local expected={'T1EnergyProduction','T1EnergyProduction','T1Resource',
            'T1Resource','T1EnergyProduction','T1Resource'}
        assert(#orders==#expected)
        for i,name in ipairs(expected) do
            assert(orders[i].structure==name)
            assert(orders[i].builder==acu.h)
            assert(orders[i].queued==(i>1))
        end
        snap.tick=30
        orders=__rm_faf_decide(0,snap)
        assert(#orders==4 and orders[4].structure=='T1LandFactory')
        for i=1,3 do assert(orders[i].structure=='T1EnergyProduction') end
        snap.tick=40
        assert(#__rm_faf_decide(0,snap)==0)
        snap.tick=50
        assert(#__rm_faf_decide(0,snap)==0, 'BuildOnce cannot restart a completed opening')
    )");
    INFO(ai.lastError());
    REQUIRE(ok);
}

TEST_CASE("FAF hydro builder distances use authored ogrids", "[faf][opening]") {
    const auto root=corpusRoot();
    if (root.empty()) SKIP("requires FAF corpus");
    FafAi ai(root);
    REQUIRE(installFafDriver(ai));
    importAiEntryPoints(ai);
    const bool ok=ai.eval(R"(
        __rm_faf_boot(0,{faction=1,startX=200,startZ=200,sizeX=4096,sizeZ=4096,
            armies=2,base='TechMain',markers={{name='hydro',type='Hydrocarbon',x=480,y=0,z=200}}})
        local brain=__rm_faf.brains[0]
        local hydro
        for _,item in ipairs(brain.builders) do
            if item.spec.BuilderName=='T1 Hydrocarbon Engineer Single' then hydro=item end
        end
        assert(hydro)
        brain.builders={hydro}
        __rm_faf_type('UEL0105',{'ENGINEER','MOBILE','TECH1'})
        local u=setmetatable({h=1,x=200,z=200,bp='UEL0105',idle=true,
            __cats=__rm_faf.cats.UEL0105},__rm_faf.unitMeta)
        local snap={tick=0,units={u},occupied={},underway={},enemies={},
            massIncome=1,energyIncome=1,massRequested=1,energyRequested=1,
            massUsage=1,energyUsage=1}
        local orders=__rm_faf_decide(0,snap)
        assert(#orders==1 and orders[1].structure=='T1HydroCarbon',
            '280 elmos is inside the authored 160-ogrid hydro search radius')
        brain.condCache={}
        brain.BuilderManagers.MAIN.EngineerManager.GetLocationCoords=function() return {2000,0,200} end
        assert(#__rm_faf_decide(0,snap)==0, '1520 elmos is outside 160 ogrids')
    )");
    INFO(ai.lastError());
    REQUIRE(ok);
}

TEST_CASE("FAF hydro opening waits for assistance and expands after completion", "[faf][opening]") {
    const auto root=corpusRoot();
    if (root.empty()) SKIP("requires FAF corpus");
    FafAi ai(root);
    REQUIRE(installFafDriver(ai));
    importAiEntryPoints(ai);
    const bool ok=ai.eval(R"(
        local function start(base)
            __rm_faf_boot(0,{faction=1,startX=200,startZ=200,sizeX=4096,sizeZ=4096,
                armies=2,base=base,markers={}})
            local brain=__rm_faf.brains[0]
            for _,item in ipairs(brain.builders) do
                if item.spec.BuilderName=='CDR Initial Default' then brain.builders={item}; break end
            end
            __rm_faf_type('UEL0001',{'COMMAND','ENGINEER','MOBILE','TECH1'})
            __rm_faf_type('ENGINEER',{'ENGINEER','MOBILE','TECH1'})
            __rm_faf_type('HYDRO',{'HYDROCARBON','STRUCTURE','TECH1'})
            local acu=setmetatable({h=1,x=200,z=200,bp='UEL0001',idle=true,
                __cats=__rm_faf.cats.UEL0001},__rm_faf.unitMeta)
            local snap={tick=0,units={acu},occupied={},underway={},mass=500,
                massIncome=1,energyIncome=1,massRequested=1,energyRequested=1,
                massUsage=1,energyUsage=1}
            __rm_faf_opening_survey=function()
                return {close={{240,200},{200,240},{160,200}},distant={},hydro={248,200}}
            end
            local orders=__rm_faf_decide(0,snap)
            assert(orders[1].structure==(base=='RushMainAir' and 'T1AirFactory' or 'T1LandFactory'))
            snap.tick=10
            orders=__rm_faf_decide(0,snap)
            assert(#orders==3 and orders[1].structure=='T1EnergyProduction')
            assert(orders[2].structure=='T1Resource' and orders[3].structure=='T1Resource')
            return brain,snap,acu
        end
        for _,base in ipairs({'TechMain','RushMainAir'}) do
            local brain,snap,acu=start(base)
            local engineer={h=2,x=248,z=200,guardCount=0,__cats=__rm_faf.cats.ENGINEER}
            table.insert(snap.units,engineer)
            snap.underway={{builder=2,x=248,z=200,__cats=__rm_faf.cats.HYDRO}}
            snap.tick=20
            local orders=__rm_faf_decide(0,snap)
            assert(#orders==2 and orders[1].kind=='stop' and orders[2].kind=='guard')
            assert(orders[2].target==2)
            acu.queueBusy=true
            snap.tick=40
            assert(#__rm_faf_decide(0,snap)==0)
            snap.underway={}
            table.insert(snap.units,{h=3,x=248,z=200,__cats=__rm_faf.cats.HYDRO})
            snap.tick=50
            orders=__rm_faf_decide(0,snap)
            assert(orders[1].kind=='stop')
            if base=='RushMainAir' then
                assert(#orders==2 and orders[2].structure=='T1AirFactory')
            else
                assert(#orders==3 and orders[2].structure=='T1LandFactory'
                    and orders[3].structure=='T1AirFactory')
            end
            assert(orders[2].anchor[1]==248 and orders[2].queued)
        end
        local brain,snap=start('TechMain')
        snap.tick=20
        assert(__rm_faf_decide(0,snap)[1].kind=='stop')
        for tick=35,170,15 do
            snap.tick=tick
            assert(#__rm_faf_decide(0,snap)==0 and brain.opening,
                'without an assistee the commander waits eleven authored 15-tick intervals')
        end
        snap.tick=185
        assert(#__rm_faf_decide(0,snap)==0 and not brain.opening)
    )");
    INFO(ai.lastError());
    REQUIRE(ok);
}

TEST_CASE("FAF structure selection skips terrain refused by its engineer", "[faf][placement]") {
    const auto root=corpusRoot();
    const char* home=std::getenv("HOME");
    const auto input=home ? std::filesystem::path{home}/"projects/llm/input/faf" : std::filesystem::path{};
    if (root.empty() || !std::filesystem::exists(input/"units/UAL0208/UAL0208_unit.bp")) SKIP("requires retail content");
    const auto engineer=rm::unitbp::loadFile(input/"units/UAL0208/UAL0208_unit.bp");
    const auto generator=rm::unitbp::loadFile(input/"units/UAB1201/UAB1201_unit.bp");
    REQUIRE(engineer); REQUIRE(generator);
    rm::app::UnitScene scene;
    scene.armies=rm::sim::freeForAll(2);
    scene.economies.resize(2);
    scene.players=rm::sim::onePlayerPerArmy(2,0);
    scene.definitions.push_back(*engineer);
    const auto type=scene.catalog.add(&scene.definitions.back());
    scene.moveDefForType.push_back(rm::data::moveDefFor(*engineer));
    scene.definitions.push_back(*generator);
    const auto product=scene.catalog.add(&scene.definitions.back());
    scene.moveDefForType.push_back(rm::data::moveDefFor(*generator));
    (void)scene.store.spawn({.type=type,
        .transform={.x=rm::sim::Fx::fromInt(200),.z=rm::sim::Fx::fromInt(200)},
        .motion={.armyIndex=0},.health={.current=engineer->health,.maximum=engineer->health}});
    rm::HeightField field{.squaresX=128,.squaresZ=128,.heightScale=1};
    field.raw.resize(field.sampleCount(),200);
    const auto first=rm::sim::structureSite({rm::sim::Fx::fromInt(200),{},rm::sim::Fx::fromInt(200)},{},{},0);
    const auto snapped=scene.terrain(field).buildSite(*generator,first[0],first[2]);
    const auto x=static_cast<int>(rm::sim::fxToFloat(snapped[0]))/rm::kSquareSize;
    const auto z=static_cast<int>(rm::sim::fxToFloat(snapped[1]))/rm::kSquareSize;
    field.raw[static_cast<std::size_t>(z*(field.squaresX+1)+x)]=0;
    rm::app::PassabilitySet passability(field,false,0);
    const auto& grid=passability.gridForBuild(scene,product,type);
    const auto radius=rm::sim::fxFromFloat(generator->collisionRadiusElmos);
    REQUIRE_FALSE(rm::sim::sitePlaceable(grid,snapped[0],snapped[1],radius));
    rm::vfs::Vfs content;
    content.mountDirectory(input);
    const std::array<rm::mapinfo::StartPosition,2> starts{{{.x=200,.z=200},{.x=800,.z=800}}};
    const rm::ai::World world{.scene=scene,.content=content,.field=field,.starts=starts,.markers={}};
    FafAi ai(root);
    REQUIRE(installFafDriver(ai));
    importAiEntryPoints(ai);
    rm::ai::FafOpponent opponent(ai,0,"TechMain");
    opponent.observe(world,{});
    opponent.advance(0);
    REQUIRE(ai.eval(R"(
        __rm_faf_decide=function(army,snap)
            return {{kind='build',bp='uab1201',structure='T2EnergyProduction',builder=snap.units[1].h}}
        end
    )"));
    opponent.advance(1);
    REQUIRE(opponent.drain().size()==1);
    const auto site=opponent.drain()[0].site;
    CHECK(rm::sim::sitePlaceable(grid,site[0],site[2],radius));
    const auto mex=rm::unitbp::loadFile(input/"units/UAB1103/UAB1103_unit.bp");
    REQUIRE(mex);
    scene.definitions.push_back(*mex);
    const auto mexType=scene.catalog.add(&scene.definitions.back());
    (void)scene.store.spawn({.type=mexType,
        .transform={.x=rm::sim::Fx::fromInt(320),.z=rm::sim::Fx::fromInt(320)},
        .motion={.radiusElmos=rm::sim::fxFromFloat(mex->collisionRadiusElmos),.armyIndex=1},
        .health={.current=mex->health,.maximum=mex->health}});
    const std::array<rm::scenario::Marker,2> markers{{
        {.name="occupied",.type="Mass",.position={320,200,320}},
        {.name="free",.type="Mass",.position={400,200,320}}}};
    scene.resourceDeposits={{.x=rm::sim::Fx::fromInt(320),.z=rm::sim::Fx::fromInt(320)},
                            {.x=rm::sim::Fx::fromInt(400),.z=rm::sim::Fx::fromInt(320)}};
    const rm::ai::World deposits{.scene=scene,.content=content,.field=field,.starts=starts,.markers=markers};
    opponent.observe(deposits,{});
    REQUIRE(ai.eval(R"(
        __rm_faf_decide=function(army,snap)
            return {{kind='build',bp='uab1103',structure='T1Resource',builder=snap.units[1].h}}
        end
    )"));
    opponent.advance(2);
    REQUIRE(opponent.drain().size()==1);
    CHECK(opponent.drain()[0].site[0]==rm::sim::Fx::fromInt(400));
    scene.resourceDeposits.pop_back();
    opponent.advance(3);
    CHECK(opponent.drain().empty()); // A marker alone cannot authorize native extraction.
}

TEST_CASE("FAF opening surveys native resources and preserves selected queued sites", "[faf][opening][construction]") {
    const auto root = corpusRoot();
    const char* home = std::getenv("HOME");
    const auto input = home ? std::filesystem::path{home}/"projects/llm/input/faf" : std::filesystem::path{};
    if (root.empty() || !std::filesystem::exists(input/"units/UEL0001/UEL0001_unit.bp")) SKIP("requires FAF and retail content");
    auto def = rm::unitbp::loadFile(input/"units/UEL0001/UEL0001_unit.bp");
    REQUIRE(def);
    rm::app::UnitScene scene;
    scene.armies = rm::sim::freeForAll(2);
    scene.economies.resize(2);
    scene.players = rm::sim::onePlayerPerArmy(2,0);
    scene.definitions.push_back(*def);
    const auto type = scene.catalog.add(&scene.definitions.back());
    const auto unit = scene.store.spawn({.type=type,
        .transform={.x=rm::sim::Fx::fromInt(200),.z=rm::sim::Fx::fromInt(200)},
        .motion={.armyIndex=0},.health={.current=def->health,.maximum=def->health}});
    rm::HeightField field{.squaresX=128,.squaresZ=128,.heightScale=1};
    field.raw.resize(field.sampleCount());
    rm::vfs::Vfs content;
    content.mountDirectory(input);
    const std::array<rm::mapinfo::StartPosition,2> starts{{{.x=200,.z=200},{.x=800,.z=800}}};
    const std::array<rm::scenario::Marker,5> markers{{
        {.name="close",.type="Mass",.position={240,0,200}},
        {.name="distant",.type="Mass",.position={360,0,200}},
        {.name="outside",.type="Mass",.position={400,0,200}},
        {.name="hydro",.type="Hydrocarbon",.position={480,0,200}},
        {.name="outside hydro",.type="Hydrocarbon",.position={800,0,200}}}};
    for (const auto& marker : markers) {
        scene.resourceDeposits.push_back({
            .kind=marker.isType("Mass") ? rm::unitdef::BuildRestriction::MassDeposit
                                        : rm::unitdef::BuildRestriction::HydrocarbonDeposit,
            .x=rm::sim::fxFromFloat(marker.position[0]),.z=rm::sim::fxFromFloat(marker.position[2])});
    }
    const rm::ai::World world{.scene=scene,.content=content,.field=field,.starts=starts,.markers=markers};
    FafAi ai(root);
    REQUIRE(installFafDriver(ai));
    importAiEntryPoints(ai);
    rm::ai::FafOpponent opponent(ai,0,"TechMain");
    opponent.observe(world,{});
    opponent.advance(0);
    const bool ok=ai.eval(R"(
        local u=__rm_faf.brains[0].snap.units[1]
        local sites=__rm_faf_opening_survey(u.h)
        assert(#sites.close==1 and sites.close[1][1]==240)
        assert(#sites.distant==1 and sites.distant[1][1]==360,
            'authored squared distance 484 is in ogrids, not elmos')
        assert(sites.hydro[1]==480 and not sites.inWater)
        assert(not u.queueBusy)
        __rm_faf_decide=function(army,snap)
            return {{kind='build',bp='ueb1103',structure='T1Resource',builder=u.h,
                site=sites.distant[1]},
                {kind='build',bp='ueb1103',structure='T1Resource',builder=u.h,
                site=sites.close[1],queued=true}}
        end
    )");
    INFO(ai.lastError());
    REQUIRE(ok);
    opponent.advance(1);
    REQUIRE(opponent.drain().size()==2);
    CHECK(opponent.drain()[0].builder==unit);
    CHECK(opponent.drain()[0].site[0]==rm::sim::Fx::fromInt(360));
    CHECK(opponent.drain()[1].site[0]==rm::sim::Fx::fromInt(240));
    CHECK_FALSE(opponent.drain()[0].queued);
    CHECK(opponent.drain()[1].queued);
    const auto generator=rm::unitbp::loadFile(input/"units/UEB1101/UEB1101_unit.bp");
    const auto factory=rm::unitbp::loadFile(input/"units/UEB0101/UEB0101_unit.bp");
    REQUIRE(generator);
    REQUIRE(factory);
    const auto firstSite=rm::sim::structureSite({rm::sim::Fx::fromInt(200),{},rm::sim::Fx::fromInt(200)},
        world.centreX,world.centreZ,0);
    scene.definitions.push_back(*factory);
    const auto factoryType=scene.catalog.add(&scene.definitions.back());
    const auto factoryX=firstSite[0]+rm::sim::Fx::fromInt(8);
    (void)scene.store.spawn({.type=factoryType,.transform={.x=factoryX,.z=firstSite[2]},
        .motion={.radiusElmos=rm::sim::fxFromFloat(factory->collisionRadiusElmos),.armyIndex=1},
        .health={.current=factory->health,.maximum=factory->health}});
    REQUIRE(ai.eval(R"(
        __rm_faf_decide=function(army,snap)
            return {{kind='build',bp='ueb1101',structure='T1EnergyProduction',builder=snap.units[1].h}}
        end
    )"));
    opponent.advance(2);
    REQUIRE(opponent.drain().size()==1);
    const auto selected=opponent.drain()[0].site;
    CHECK(rm::sim::groundDistanceElmos(selected,{factoryX,{},firstSite[2]})
        >=rm::sim::fxFromFloat(factory->collisionRadiusElmos+generator->collisionRadiusElmos));
    scene.definitions.push_back(*generator);
    const auto generatorType=scene.catalog.add(&scene.definitions.back());
    (void)scene.store.orders()[unit.index].give(rm::sim::Command{
        .kind=rm::sim::CommandKind::Build,.unit=unit,.targetX=selected[0],.targetZ=selected[2],
        .buildType=generatorType},true);
    opponent.advance(3);
    REQUIRE(opponent.drain().size()==1);
    CHECK(rm::sim::groundDistanceElmos(opponent.drain()[0].site,selected)
        >=rm::sim::fxFromFloat(2*generator->collisionRadiusElmos));
    // A pending build has no active task yet, but must reserve its engineer.
    (void)scene.store.orders()[unit.index].give(rm::sim::Command{
        .kind=rm::sim::CommandKind::Build,.unit=unit,.buildType=type},true);
    REQUIRE(ai.eval(R"(
        __rm_faf_decide=function(army,snap)
            __rm_faf.brains[army].snap=snap
            assert(snap.units[1].queueBusy and snap.units[1].building)
            pendingBuildSeen=true
            return {}
        end
    )"));
    opponent.advance(2);
    REQUIRE(ai.eval("assert(pendingBuildSeen)"));
    REQUIRE(opponent.drain().empty());
    const rm::ai::World noMarkers{.scene=scene,.content=content,.field=field,.starts=starts,.markers={}};
    rm::ai::FafOpponent otherObservation(ai,0,"TechMain");
    otherObservation.observe(noMarkers,{});
    otherObservation.advance(3);
    REQUIRE(ai.eval("assert(#__rm_faf_opening_survey(__rm_faf.brains[0].snap.units[1].h).close==0)"));
    opponent.advance(4);
    REQUIRE(ai.eval("assert(#__rm_faf_opening_survey(__rm_faf.brains[0].snap.units[1].h).close==1)"));
    scene.hasWater=true;
    scene.waterLevelElmos=10;
    REQUIRE(ai.eval(R"(
        local h=__rm_faf.brains[0].snap.units[1].h
        assert(__rm_faf_opening_survey(h).inWater)
    )"));
    scene.hasWater=false;
    scene.waterLevelElmos=0;
    // Exceed the native amphibious depth limit across the full map. Amphibious's
    // current 60-degree slope conversion permits every finite terrain slope, so a
    // ridge is not an impassable fixture for this motion class.
    field.baseHeight=-2.0e6f;
    field.heightScale=1.0e6f;
    std::fill(field.raw.begin(),field.raw.end(),2);
    for (int z=0; z<field.verticesZ(); ++z)
        field.raw[static_cast<std::size_t>(z*field.verticesX()+40)]=0;
    const bool blocked=ai.eval(R"(
        local sites=__rm_faf_opening_survey(__rm_faf.brains[0].snap.units[1].h)
        assert(#sites.close==1 and #sites.distant==0 and not sites.hydro,
            'blocked survey: close='..#sites.close..', distant='..#sites.distant..', hydro='..tostring(sites.hydro))
    )");
    INFO(ai.lastError());
    REQUIRE(blocked);
}

TEST_CASE("FAF enhancement plans read installed slots and emit validated native sequences", "[faf][enhancement]") {
    const auto root = corpusRoot();
    const char* home = std::getenv("HOME");
    const auto input = home ? std::filesystem::path{home}/"projects/llm/input/faf" : std::filesystem::path{};
    if (root.empty() || !std::filesystem::exists(input/"units/UAL0001/UAL0001_unit.bp")) SKIP("requires FAF and retail content");
    auto def = rm::unitbp::loadFile(input/"units/UAL0001/UAL0001_unit.bp");
    REQUIRE(def);
    rm::app::UnitScene scene;
    scene.armies = rm::sim::freeForAll(2);
    scene.armies[0].faction = rm::sim::Faction::Aeon;
    scene.economies.resize(2);
    scene.players = rm::sim::onePlayerPerArmy(2,0);
    scene.definitions.push_back(*def);
    const auto type = scene.catalog.add(&scene.definitions.back());
    const auto unit = scene.store.spawn({.type=type,.motion={.armyIndex=0},
        .health={.current=def->health,.maximum=def->health}});
    auto engineerDef = rm::unitbp::loadFile(input/"units/UAL0105/UAL0105_unit.bp");
    REQUIRE(engineerDef);
    scene.definitions.push_back(*engineerDef);
    const auto engineerType = scene.catalog.add(&scene.definitions.back());
    (void)scene.store.spawn({.type=engineerType,.motion={.armyIndex=0},
        .health={.current=engineerDef->health,.maximum=engineerDef->health}});
    rm::HeightField field{.squaresX=64,.squaresZ=64};
    field.raw.resize(field.sampleCount());
    rm::vfs::Vfs content;
    content.mountDirectory(input);
    const std::array<rm::mapinfo::StartPosition,2> starts{{{.x=0,.z=0},{.x=400,.z=400}}};
    const rm::ai::World world{.scene=scene,.content=content,.field=field,.starts=starts,.markers={}};
    FafAi ai(root);
    REQUIRE(installFafDriver(ai));
    importAiEntryPoints(ai);
    rm::ai::FafOpponent opponent(ai,0,"TechMain");
    opponent.observe(world,{});
    opponent.advance(0);
    REQUIRE(ai.eval(R"(
        local brain=__rm_faf.brains[0]
        local function builder(name, sequence)
            return {kind='EngineerBuilder',spec={BuilderName=name,Priority=1000,
                PlatoonTemplate='CommanderEnhance',BuilderConditions={},
                BuilderData={Enhancement=sequence}}}
        end
        brain.opening=false -- This test replaces the opening with enhancement-only builders.
        brain.builders={builder('invalid prerequisite',{'T3Engineering'}),
            builder('unsupported effect',{'Shield'}),builder('engineering',{'AdvancedEngineering','T3Engineering'})}
        local u=brain.snap.units[1]
        assert(not u:HasEnhancement('AdvancedEngineering'))
        assert(not u:IsUnitState('Enhancing'))
    )"));
    opponent.advance(1);
    REQUIRE(opponent.drain().size() == 2);
    CHECK_FALSE(opponent.drain()[0].queued);
    CHECK(opponent.drain()[1].queued);
    CHECK(opponent.drain()[1].enhancement == "T3Engineering");
    CHECK(opponent.drain()[0].kind == rm::ai::Decision::Kind::Enhance);
    CHECK(opponent.drain()[0].enhancement == "AdvancedEngineering");
    CHECK(opponent.drain()[0].unit == unit);
    REQUIRE(rm::sim::installEnhancement(scene.store,scene.catalog,unit,"AdvancedEngineering"));
    opponent.observe(world,{});
    opponent.advance(2);
    REQUIRE(ai.eval(R"(
        local u=__rm_faf.brains[0].snap.units[1]
        assert(u:HasEnhancement('AdvancedEngineering'))
        assert(not u:HasEnhancement('T3Engineering'))
    )"));
    REQUIRE(opponent.drain().size() == 1);
    CHECK(opponent.drain()[0].enhancement == "T3Engineering");
    REQUIRE(ai.eval(R"(
        __rm_faf.brains[0].builders={{kind='EngineerBuilder',spec={
            BuilderName='enhanced commander T2 power',Priority=1000,
            PlatoonTemplate='CommanderBuilder',BuilderConditions={},
            BuilderData={Construction={BuildStructures={'T2EnergyProduction'}}}}}}
    )"));
    opponent.advance(3);
    REQUIRE(opponent.drain().size() == 1);
    CHECK(opponent.drain()[0].kind == rm::ai::Decision::Kind::StartConstruction);
    CHECK(opponent.drain()[0].blueprint.find("UAB1201") != std::string::npos);
    rm::sim::EnhancementTasks tasks(scene.store,scene.catalog,scene.enhancementWork);
    const std::string next = "T3Engineering";
    REQUIRE(rm::sim::applyCommand(rm::sim::CommandIssue{.source=0,
        .id=scene.store.allocateCommandId(0).value(),.player=0,
        .kind=rm::sim::CommandKind::Script,.units={unit},.scriptTask="EnhanceTask",
        .scriptData={next.begin(),next.end()}},scene.store,scene.catalog,scene.players,
        scene.armies,rm::sim::Terrain{field},
        [](rm::sim::UnitId) { return static_cast<const rm::sim::PassabilityGrid*>(nullptr); },
        rm::sim::TickRate{},nullptr,nullptr,nullptr,nullptr,&tasks));
    opponent.advance(4);
    CHECK(opponent.drain().empty());
    REQUIRE(ai.eval(R"(
        local u=__rm_faf.brains[0].snap.units[1]
        assert(not u:IsUnitState('Enhancing'))
        assert(not u.idle, 'pending enhancement commands reserve the stationary commander')
    )"));
    const auto grid = rm::sim::buildPassability(field,0,60,0);
    const std::vector<const rm::sim::PassabilityGrid*> grids{&grid};
    (void)rm::sim::advanceOrders(scene.store,scene.catalog,rm::sim::Terrain{field},grids,
        rm::sim::TickRate{},nullptr,nullptr,nullptr,nullptr,nullptr,scene.armies,nullptr,nullptr,&tasks);
    opponent.advance(5);
    REQUIRE(ai.eval(R"(
        local u=__rm_faf.brains[0].snap.units[1]
        assert(u:IsUnitState('Enhancing') and u:IsUnitState('Upgrading'))
    )"));
    scene.store.orders()[unit.index].clear();

}
