// The scripted opponent's plan, as data.
//
// §7 P3.3's stated test is here: "one build order drives UEF and Cybran from data alone". The
// plan names ROLES, the roster answers per faction, and neither the plan nor the engine mentions
// a blueprint id — which is what the four deleted `constexpr` paths made impossible.
#include <catch2/catch_test_macros.hpp>

#include "core/data/Opening.hpp"
#include "core/data/Roster.hpp"
#include "core/lua/LuaTable.hpp"
#include "core/unit/UnitBlueprint.hpp"

#include <algorithm>
#include <cstdlib>
#include <array>
#include <filesystem>
#include <string>
#include <vector>

using rm::data::Opening;
using rm::data::Roster;
using rm::sim::Faction;
using rm::unitdef::Role;
using rm::unitdef::UnitDef;

namespace {

[[nodiscard]] std::filesystem::path unitRoot() {
    if (const char* home = std::getenv("HOME")) {
        return std::filesystem::path{home} / "projects/llm/input/faf/units";
    }
    return {};
}

/// The shipped `data/opening.lua`, wherever CTest runs from. CWD covers the repo-root
/// case; otherwise resolve against this file's own directory (tests/ → root).
[[nodiscard]] std::filesystem::path openingFile() {
    const std::filesystem::path fromCwd = "data/opening.lua";
    if (std::filesystem::exists(fromCwd)) return fromCwd;
    const std::filesystem::path fromSource =
        std::filesystem::absolute(std::filesystem::path{__FILE__}).parent_path().parent_path()
        / "data/opening.lua";
    return fromSource;
}

[[nodiscard]] std::optional<Opening> parse(std::string_view source) {
    const auto table = rm::lua::parseTable(source);
    if (!table) {
        return std::nullopt;
    }
    return rm::data::parseOpening(*table);
}

} // namespace

TEST_CASE("an opening parses roles, tiers and requirements") {
    const auto opening = parse(R"({
        structures = {
            { role = 'extractor', tech = 1 },
            { role = 'energy' },
            { role = 'factory', tech = 1, requires = { 'LAND' } },
        },
        wave = {
            unit = { role = 'raider', tech = 2, requires = { 'LAND', 'TANK' },
                     fallback = { 'LAND' } },
            size = 12,
        },
    })");

    REQUIRE(opening.has_value());
    REQUIRE(opening->structures.size() == 3);

    CHECK(opening->structures[0].role == Role::Extractor);
    CHECK(opening->structures[0].tech == 1);
    CHECK(opening->structures[0].requires_.empty());

    // No `tech` means 0, which the roster reads as "the cheapest this faction fields" — the
    // useful default, since an opening should not have to know which tiers exist.
    CHECK(opening->structures[1].role == Role::Energy);
    CHECK(opening->structures[1].tech == 0);

    REQUIRE(opening->structures[2].requires_.size() == 1);
    CHECK(opening->structures[2].requires_[0] == "LAND");

    CHECK(opening->waveUnit.role == Role::Raider);
    CHECK(opening->waveUnit.tech == 2);
    REQUIRE(opening->waveUnit.requires_.size() == 2);
    REQUIRE(opening->waveUnit.fallback.size() == 1);
    CHECK(opening->waveUnit.fallback[0] == "LAND");
    CHECK(opening->waveSize == 12);
}

TEST_CASE("an unknown role is refused, not skipped") {
    // A newer data file on an older binary must fail loudly. Skipping the step it does not
    // understand would open with three quarters of a plan and look like a balance problem.
    CHECK_FALSE(parse(R"({ structures = { { role = 'battlecruiser' } } })").has_value());

    // And so is a wave of nothing, which would never launch.
    CHECK_FALSE(parse(R"({
        structures = { { role = 'extractor' } },
        wave = { size = 0 },
    })").has_value());

    // An opening with no structures describes nothing.
    CHECK_FALSE(parse(R"({ wave = { size = 5 } })").has_value());
}

TEST_CASE("the built-in opening and the shipped file say the same thing") {
    // `defaultOpening()` exists so a build with no `data/` directory still plays. If it drifted
    // from the file, two installs of the same commit would play different matches — and only
    // one of them would match the golden log.
    const Opening builtIn = rm::data::defaultOpening();
    REQUIRE(builtIn.valid());

    const std::filesystem::path file = openingFile();
    if (!std::filesystem::exists(file)) {
        SKIP("no data/opening.lua beside the repo or CWD");
    }
    const auto fromFile = rm::data::loadOpening(file);
    REQUIRE(fromFile.has_value());

    REQUIRE(fromFile->structures.size() == builtIn.structures.size());
    for (std::size_t i = 0; i < builtIn.structures.size(); ++i) {
        INFO("step " << i);
        CHECK(fromFile->structures[i].role == builtIn.structures[i].role);
        CHECK(fromFile->structures[i].tech == builtIn.structures[i].tech);
        CHECK(fromFile->structures[i].requires_ == builtIn.structures[i].requires_);
    }
    CHECK(fromFile->waveUnit.role == builtIn.waveUnit.role);
    CHECK(fromFile->waveUnit.requires_ == builtIn.waveUnit.requires_);
    CHECK(fromFile->waveUnit.fallback == builtIn.waveUnit.fallback);
    CHECK(fromFile->waveSize == builtIn.waveSize);
}

TEST_CASE("a missing or unparseable file is nothing, so the caller can fall back") {
    CHECK_FALSE(rm::data::loadOpening("/nonexistent/opening.lua").has_value());
}

TEST_CASE("one build order drives all four factions from data alone") {
    // §7 P3.3'S STATED TEST, widened from two factions to the full four when AI-vs-AI
    // skirmishes started seating everyone. The plan is read from the shipped file, and every
    // step resolves for each faction to that faction's OWN blueprints — with no id named
    // anywhere in the plan, the roster, or this test's expectations except as the answer.
    const std::filesystem::path root = unitRoot();
    if (root.empty() || !std::filesystem::exists(root)) {
        SKIP("no extracted unit corpus at " + root.string());
    }
    const std::filesystem::path file = openingFile();
    if (!std::filesystem::exists(file)) {
        SKIP("no data/opening.lua beside the repo or CWD");
    }

    const auto opening = rm::data::loadOpening(file);
    REQUIRE(opening.has_value());

    std::vector<UnitDef> defs;
    std::vector<std::string> ids;
    for (const auto& entry : std::filesystem::recursive_directory_iterator{root}) {
        if (entry.is_regular_file()
            && entry.path().filename().string().ends_with("_unit.bp")) {
            if (auto def = rm::unitbp::loadFile(entry.path())) {
                ids.push_back(def->name);
                defs.push_back(std::move(*def));
            }
        }
    }
    REQUIRE(defs.size() > 500);
    const Roster roster = Roster::build(defs, ids);

    const auto resolve = [&roster, &opening](Faction faction) {
        std::vector<std::string> plan;
        // Through `resolveStep`, the same function the engine uses. Writing the `tech > 0`
        // branch out here instead is what this test did first, and it got it wrong — which is
        // why the branch now lives in one place.
        for (const rm::data::OpeningStep& step : opening->structures) {
            const auto entry = rm::data::resolveStep(roster, faction, step.role, step.tech,
                                                     step.requires_, step.fallback);
            plan.push_back(entry ? entry->id : std::string{"-"});
        }
        const auto wave = rm::data::resolveStep(
            roster, faction, opening->waveUnit.role, opening->waveUnit.tech,
            opening->waveUnit.requires_, opening->waveUnit.fallback);
        plan.push_back(wave ? wave->id : std::string{"-"});
        return plan;
    };

    const std::array<Faction, 4> factions{Faction::Uef, Faction::Aeon, Faction::Cybran,
                                          Faction::Seraphim};
    std::array<std::vector<std::string>, 4> plans;
    for (std::size_t f = 0; f < factions.size(); ++f) {
        plans[f] = resolve(factions[f]);
        REQUIRE(plans[f].size() == opening->structures.size() + 1);
    }

    // EVERY step resolves for every faction. A single "-" means the plan asks for something
    // a faction does not field, which is the failure the four hardcoded paths hid by never
    // asking — and exactly what an AI-vs-AI seat of that faction would trip over at spawn.
    for (std::size_t i = 0; i < plans[0].size(); ++i) {
        for (std::size_t f = 0; f < factions.size(); ++f) {
            INFO("step " << i << ", faction " << f << ": " << plans[f][i]);
            CHECK(plans[f][i] != "-");

            // And every pair differs, which is the point — one plan, four rosters. A match
            // between any two would mean the roster is ignoring the faction.
            for (std::size_t g = f + 1; g < factions.size(); ++g) {
                CHECK(plans[f][i] != plans[g][i]);
            }
        }
    }

    // The UEF resolution is the ids the deleted constants named, which is what makes this a
    // refactor of the opening rather than a change to it. `plans` is in the factions array's
    // own order: UEF, Aeon, Cybran, Seraphim.
    const std::vector<std::string>& uef = plans[0];
    const std::vector<std::string>& cybran = plans[2];
    CHECK(uef[0] == "UEB1103");  // extractor
    CHECK(uef[1] == "UEB1101");  // power generator
    CHECK(uef[3] == "UEB0101");  // land factory
    // THE WAVE UNIT DIFFERS BY WHAT THE FACTION HAS, which is the `fallback` mechanism doing
    // the one job it exists for. UEF fields a T1 tank and gets it — the same unit the deleted
    // constant named, so the match stays comparable. CYBRAN FIELDS NO T1 TANK: its T1 army is
    // bots and the tank line starts at T2, a faction design decision in Supreme Commander
    // rather than a gap in the data. It falls back to its bot.
    CHECK(uef[4] == "UEL0201");
    CHECK(cybran[4] == "URL0106");
}
