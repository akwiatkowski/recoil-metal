// The build tree: what each builder can build, resolved from category expressions.
//
// `07-ai-and-gamesetup.md §4.3` calls this "a hard blocker for any skirmish, not just for the
// AI", because Supreme Commander ships no build lists — it ships expressions, and the fact
// "engineers build mexes" exists nowhere until every expression has been evaluated against
// every unit.
//
// TWO KINDS OF CASE. The hand-written ones pin the SEMANTICS: space means AND, the list means
// OR, and an empty expression builds nothing rather than everything. The corpus one is §7
// P3.2's stated test — a T1 engineer's set holds its own faction's mex, power plant and land
// factory, and nothing of another faction's — and it is the only case that can catch a
// misparsed real expression.
#include <catch2/catch_test_macros.hpp>

#include "core/unit/BuildTree.hpp"
#include "core/unit/Role.hpp"
#include "core/unit/UnitBlueprint.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

using rm::unitdef::BuildTree;
using rm::unitdef::CategoryExpression;
using rm::unitdef::matchesExpression;
using rm::unitdef::UnitDef;

namespace {

[[nodiscard]] UnitDef unitWith(std::string name, std::vector<std::string> tags) {
    UnitDef def;
    def.name = std::move(name);
    std::sort(tags.begin(), tags.end());
    def.categories = std::move(tags);
    return def;
}

/// A builder whose expression is written the way a blueprint writes it.
[[nodiscard]] UnitDef builderWith(std::string name, std::vector<std::string> expressions) {
    UnitDef def = unitWith(std::move(name), {"STRUCTURE"});
    for (const std::string& text : expressions) {
        def.buildableCategory.push_back(rm::unitdef::parseCategoryTerm(text));
    }
    return def;
}

[[nodiscard]] std::filesystem::path unitRoot() {
    if (const char* home = std::getenv("HOME")) {
        return std::filesystem::path{home} / "projects/llm/input/faf/units";
    }
    return {};
}

} // namespace

// --- The semantics ----------------------------------------------------------------------

TEST_CASE("a term splits on whitespace, and drops nothing that matters") {
    const auto term = rm::unitdef::parseCategoryTerm("BUILTBYTIER1ENGINEER UEF");
    REQUIRE(term.size() == 2);
    CHECK(term[0] == "BUILTBYTIER1ENGINEER");
    CHECK(term[1] == "UEF");

    // Any whitespace, not just the single space the corpus happens to use — a tab in a
    // hand-written data file would otherwise fail in a way that reads as a missing unit.
    const auto tabbed = rm::unitdef::parseCategoryTerm("  A\tB \n C ");
    REQUIRE(tabbed.size() == 3);
    CHECK(tabbed[0] == "A");
    CHECK(tabbed[2] == "C");

    CHECK(rm::unitdef::parseCategoryTerm("").empty());
    CHECK(rm::unitdef::parseCategoryTerm("   ").empty());
}

TEST_CASE("a space means AND") {
    const CategoryExpression expression{{"BUILTBYTIER1ENGINEER", "UEF"}};

    CHECK(matchesExpression(expression,
                            unitWith("mex", {"BUILTBYTIER1ENGINEER", "UEF", "STRUCTURE"})));

    // Both tags, or neither counts. The same tier from another faction is not buildable, which
    // is the whole reason the faction tag is in the expression.
    CHECK_FALSE(matchesExpression(
        expression, unitWith("cybran mex", {"BUILTBYTIER1ENGINEER", "CYBRAN", "STRUCTURE"})));
    CHECK_FALSE(matchesExpression(expression, unitWith("uef t2", {"BUILTBYTIER2ENGINEER",
                                                                 "UEF", "STRUCTURE"})));
}

TEST_CASE("ALLUNITS is the universal category even when absent from authored tags") {
    const auto generator = unitWith("UEB1101", {"STRUCTURE", "ENERGYPRODUCTION"});
    CHECK(matchesExpression(CategoryExpression{{"ALLUNITS"}}, generator));
    CHECK(matchesExpression(CategoryExpression{{"ALLUNITS", "STRUCTURE"}}, generator));
    CHECK_FALSE(matchesExpression(CategoryExpression{{"ALLUNITS", "MOBILE"}}, generator));
    CHECK_FALSE(matchesExpression(CategoryExpression{{"UNKNOWN"}}, generator));
    CHECK_FALSE(matchesExpression(CategoryExpression{{}}, generator));
}

TEST_CASE("the list means OR") {
    // A real UEF T1 land factory states three expressions.
    const CategoryExpression expression{
        {"BUILTBYTIER1FACTORY", "UEF", "MOBILE", "CONSTRUCTION"},
        {"BUILTBYTIER1FACTORY", "UEF", "STRUCTURE", "LAND"},
        {"BUILTBYTIER1FACTORY", "UEF", "MOBILE", "LAND"},
    };

    CHECK(matchesExpression(expression, unitWith("engineer", {"BUILTBYTIER1FACTORY", "UEF",
                                                             "MOBILE", "CONSTRUCTION"})));
    CHECK(matchesExpression(expression,
                            unitWith("tank", {"BUILTBYTIER1FACTORY", "UEF", "MOBILE", "LAND"})));
    CHECK_FALSE(matchesExpression(
        expression, unitWith("air", {"BUILTBYTIER1FACTORY", "UEF", "MOBILE", "AIR"})));
}

TEST_CASE("an empty expression builds nothing, not everything") {
    // THE TRAP. `hasAllCategories({})` is true by definition — all zero tags are present — so
    // treating an empty expression or an empty term as a match would turn every wall in the
    // game into a build option off one malformed line.
    const UnitDef anything = unitWith("wall", {"STRUCTURE", "WALL"});

    CHECK_FALSE(matchesExpression(CategoryExpression{}, anything));
    CHECK_FALSE(matchesExpression(CategoryExpression{{}}, anything));

    // And an empty term alongside a real one is skipped rather than short-circuiting the
    // whole expression to true.
    const CategoryExpression mixed{{}, {"UEF", "STRUCTURE"}};
    CHECK_FALSE(matchesExpression(mixed, anything));
    CHECK(matchesExpression(mixed, unitWith("uef thing", {"UEF", "STRUCTURE"})));
}

TEST_CASE("materialising turns expressions into indices") {
    const std::vector<UnitDef> units{
        builderWith("engineer", {"BUILTBYTIER1ENGINEER UEF"}),           // 0
        unitWith("uef mex", {"BUILTBYTIER1ENGINEER", "UEF"}),            // 1
        unitWith("uef pgen", {"BUILTBYTIER1ENGINEER", "UEF"}),           // 2
        unitWith("cybran mex", {"BUILTBYTIER1ENGINEER", "CYBRAN"}),      // 3
        unitWith("wall", {"STRUCTURE", "WALL"}),                         // 4
    };

    const BuildTree tree = BuildTree::materialise(units);

    const std::span<const std::size_t> options = tree.optionsFor(0);
    REQUIRE(options.size() == 2);
    CHECK(options[0] == 1);
    CHECK(options[1] == 2);

    // Indices are into the caller's own span, ascending.
    CHECK(std::is_sorted(options.begin(), options.end()));

    // A non-builder has no options and declares no expression — two different facts, and the
    // tree tells them apart so a diagnosis can say which.
    CHECK(tree.optionsFor(4).empty());
    CHECK_FALSE(tree.declaresExpression(4));
    CHECK(tree.declaresExpression(0));

    CHECK(tree.builderCount() == 1);

    // Out of range is empty rather than a crash: this is indexed from a loop over a catalog
    // that may be shorter than the tree it was built from.
    CHECK(tree.optionsFor(999).empty());
    CHECK_FALSE(tree.declaresExpression(999));
}

TEST_CASE("a builder that matches nothing is distinguishable from a non-builder") {
    // The diagnosis this exists for: an expression that resolves to nothing means either the
    // expression is wrong or the unit set is incomplete, and both are bugs. A unit with no
    // expression is simply not a builder, which is not.
    const std::vector<UnitDef> units{
        builderWith("orphan", {"BUILTBYNOTHING UEF"}),
        unitWith("thing", {"UEF"}),
    };

    const BuildTree tree = BuildTree::materialise(units);
    CHECK(tree.optionsFor(0).empty());
    CHECK(tree.declaresExpression(0));  // it asked for something and got nothing
    CHECK(tree.builderCount() == 0);
}

TEST_CASE("a factory may build another factory") {
    // No self-exclusion, and it would be wrong to add one: `BUILTBYTIER1FACTORY UEF STRUCTURE
    // LAND` legitimately includes another factory, and an engineer that builds factories that
    // build engineers is the normal shape of the game.
    std::vector<UnitDef> units{
        builderWith("factory", {"BUILTBYTIER1FACTORY UEF"}),
    };
    units[0].categories = {"BUILTBYTIER1FACTORY", "STRUCTURE", "UEF"};
    std::sort(units[0].categories.begin(), units[0].categories.end());

    const BuildTree tree = BuildTree::materialise(units);
    REQUIRE(tree.optionsFor(0).size() == 1);
    CHECK(tree.optionsFor(0)[0] == 0);
}

TEST_CASE("a lower-case single token is a blueprint id, not a category") {
    // THE SECOND FORM, which `07 §4.3` does not describe. `UEB1103`, the T1 mass extractor,
    // states `BuildableCategory = { 'ueb1202' }` — the id of the T2 mex it upgrades into. 34 of
    // the 105 declaring units use this, which is every upgrade chain in the game.
    CHECK(rm::unitdef::isIdReference({"ueb1202"}));
    CHECK_FALSE(rm::unitdef::isIdReference({"UEF"}));                    // one token, upper
    CHECK_FALSE(rm::unitdef::isIdReference({"CYBRANMOBILEMISSILE"}));    // a real one-tag term
    CHECK_FALSE(rm::unitdef::isIdReference({"ueb1202", "UEF"}));         // two tokens

    // And it resolves by id, case-insensitively — the reference is lower case and the def
    // carries the id as its directory spells it.
    const std::vector<UnitDef> units{
        builderWith("UEB1103", {"ueb1202"}),
        unitWith("UEB1202", {"UEF", "STRUCTURE", "MASSEXTRACTION"}),
        unitWith("URB1202", {"CYBRAN", "STRUCTURE", "MASSEXTRACTION"}),
    };
    const BuildTree tree = BuildTree::materialise(units);
    REQUIRE(tree.optionsFor(0).size() == 1);
    CHECK(tree.optionsFor(0)[0] == 1);  // the UEF T2 mex, not the Cybran one
}

// --- The corpus, which is §7 P3.2's stated test -----------------------------------------

TEST_CASE("a real T1 engineer builds its own faction's economy and nothing of another's") {
    // §7 P3.2's stated corpus test, and the only case here that can catch a real expression
    // being misparsed — a synthetic one is written the same way it is read.
    const std::filesystem::path root = unitRoot();
    if (root.empty() || !std::filesystem::exists(root)) {
        SKIP("no extracted unit corpus at " + root.string());
    }

    // The whole corpus, because the answer depends on all of it: that is the point of §4.3.
    std::vector<UnitDef> units;
    std::vector<std::string> ids;
    for (const auto& entry : std::filesystem::recursive_directory_iterator{root}) {
        if (!entry.is_regular_file()
            || !entry.path().filename().string().ends_with("_unit.bp")) {
            continue;
        }
        if (auto def = rm::unitbp::loadFile(entry.path())) {
            ids.push_back(entry.path().parent_path().filename().string());
            units.push_back(std::move(*def));
        }
    }
    REQUIRE(units.size() > 500);

    const BuildTree tree = BuildTree::materialise(units);

    const auto indexOf = [&ids](std::string_view id) -> std::size_t {
        const auto found = std::find(ids.begin(), ids.end(), id);
        return found == ids.end() ? ids.size() : static_cast<std::size_t>(found - ids.begin());
    };

    const std::size_t engineer = indexOf("UEL0105");  // UEF T1 engineer
    REQUIRE(engineer < units.size());
    REQUIRE(tree.declaresExpression(engineer));

    const std::span<const std::size_t> options = tree.optionsFor(engineer);
    REQUIRE_FALSE(options.empty());

    const auto canBuild = [&](std::string_view id) {
        const std::size_t want = indexOf(id);
        return want < units.size()
               && std::find(options.begin(), options.end(), want) != options.end();
    };

    // Its own faction's mex, power plant and land factory — the three §7 P3.2 names.
    CHECK(canBuild("UEB1103"));  // UEF T1 mass extractor
    CHECK(canBuild("UEB1101"));  // UEF T1 power generator
    CHECK(canBuild("UEB0101"));  // UEF T1 land factory
    CHECK(canBuild("UEB3101"));  // UEF T1 radar: fog ships with the tool that answers it

    // And nothing of another faction's. This is the assertion that catches an expression
    // parsed as OR when it means AND: drop the faction tag and a UEF engineer builds the whole
    // game.
    CHECK_FALSE(canBuild("URB1103"));  // Cybran mex
    CHECK_FALSE(canBuild("UAB1103"));  // Aeon mex
    CHECK_FALSE(canBuild("XSB1103"));  // Seraphim mex
    CHECK_FALSE(canBuild("URB3101"));  // Cybran radar

    // Every option is UEF, checked over the whole set rather than three spot cases.
    for (const std::size_t option : options) {
        INFO("engineer can build " << ids[option]);
        REQUIRE(units[option].hasCategory("UEF"));
    }
}

TEST_CASE("the corpus's builders all resolve to something") {
    // The measurement that says the evaluator is worth anything. 105 of the 568 shipped units
    // declare an expression; before this existed, EVERY ONE of them had zero build options,
    // which is what `07 §4.3` means by a hard blocker.
    const std::filesystem::path root = unitRoot();
    if (root.empty() || !std::filesystem::exists(root)) {
        SKIP("no extracted unit corpus at " + root.string());
    }

    std::vector<UnitDef> units;
    for (const auto& entry : std::filesystem::recursive_directory_iterator{root}) {
        if (entry.is_regular_file()
            && entry.path().filename().string().ends_with("_unit.bp")) {
            if (auto def = rm::unitbp::loadFile(entry.path())) {
                units.push_back(std::move(*def));
            }
        }
    }
    REQUIRE(units.size() > 500);

    const BuildTree tree = BuildTree::materialise(units);

    std::size_t declaring = 0;
    std::size_t resolving = 0;
    std::vector<std::string> unresolved;
    for (std::size_t i = 0; i < units.size(); ++i) {
        if (tree.declaresExpression(i)) {
            ++declaring;
            if (tree.optionsFor(i).empty()) {
                unresolved.push_back(units[i].name);
            } else {
                ++resolving;
            }
        }
    }

    INFO(declaring << " units declare an expression, " << resolving << " resolve to something");
    CHECK(declaring > 100);  // 105, measured

    // 104 OF 105 RESOLVE, AND THE ONE THAT DOES NOT IS A FACT ABOUT THE RETAIL DATA.
    //
    // This assertion is why the id form was found: it read 71 of 105 on the first attempt, and
    // the 34 that failed turned out to be every upgrade chain in the game. After that it read
    // 104, and the last one is URL0111, the Cybran Mobile Missile Launcher: it declares
    // `BuildableCategory = { 'CYBRANMOBILEMISSILE' }` and NOTHING in the shipped corpus carries
    // that category — a dangling reference in the retail files, not a gap here.
    //
    // Named rather than tolerated as a count, so that a data set which fixes it, or which
    // breaks a different one, fails here and says which.
    REQUIRE(unresolved.size() == 1);
    CHECK(unresolved.front() == "URL0111");

    // And a commander's deferred upgrade additions are RECORDED, which `07 §4.3` asks for
    // explicitly — parsed, kept, not applied.
    const auto commander =
        std::find_if(units.begin(), units.end(), [](const UnitDef& def) {
            return rm::unitdef::roleOf(def) == rm::unitdef::Role::Commander;
        });
    REQUIRE(commander != units.end());
    CHECK_FALSE(commander->buildableCategoryAdds.empty());
}
