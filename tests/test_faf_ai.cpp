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
