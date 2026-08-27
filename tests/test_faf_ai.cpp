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
        __rm_faf_type('UEL0001', { 'COMMAND', 'MOBILE', 'LAND' })
        local commander = { bp = 'UEL0001', h = 1, x = 100, z = 100, idle = true,
                            healthPercent = 0.75,
                            __cats = __rm_faf.cats.UEL0001 }
        setmetatable(commander, __rm_faf.unitMeta)
        assert(commander:GetHealthPercent() == 0.75,
               'unit proxy does not expose its hull health')
        local snap = { units = { commander }, occupied = {}, underway = {},
                       mass = 400, energy = 1500, massStorage = 650, energyStorage = 4000,
                       massIncome = 0.2, energyIncome = 10, massUsage = 0, energyUsage = 0,
                       structuresUnderway = 0, mobileUnderway = 0 }
        local decisions = __rm_faf_decide(0, snap)
        assert(#decisions >= 1, 'the corpus decided nothing')
        assert(decisions[1].kind == 'build',
               'expected a build, got ' .. tostring(decisions[1].kind))
        assert(type(decisions[1].bp) == 'string' and #decisions[1].bp > 0)
    )");
    INFO(ai.lastError());
    REQUIRE(ok);
}

TEST_CASE("a viable water map exposes only the T1 surface-naval FAF slice", "[faf][ai]") {
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
                           base = 'NormalMain', markers = {}, hasNavalSite = true })
        local names = {}
        for _, item in ipairs(__rm_faf.brains[0].builders) do
            names[item.spec.BuilderName] = true
        end
        assert(names['T1 Naval Factory Builder'], 'no naval factory builder')
        assert(not names['T2 Naval Factory Builder'], 'T2 yard leaked into T1 slice')
        assert(not names['T3 Naval Factory Builder'], 'T3 yard leaked into T1 slice')
        assert(names['T1 Sea Frigate - init'], 'no T1 frigate builder')
        assert(names['Frequent Sea Attack T1'], 'no surface fleet former')
        assert(not names['Frequent Sea Attack T2'], 'T2 fleet former leaked into T1 slice')
        assert(not names['T1 Sea Factory Upgrade Slow'], 'sea upgrade leaked into T1 slice')
        assert(__rm_faf.brains[0].BuilderManagers.MAIN.BaseSettings.FactoryCount.Sea == 1)
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
