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
