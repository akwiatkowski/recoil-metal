// Which blueprint is "the T1 extractor" for a faction.
//
// This replaced four `constexpr std::string_view` paths in `core/sim/BuildOrder.hpp`, all UEF —
// which is why the scripted opponent could only play one faction. What is tested here is that
// the lookup is unambiguous (the tie-break is part of the contract, not an accident of file
// order) and that it answers for all four factions from the real corpus.
#include <catch2/catch_test_macros.hpp>

#include "core/data/Roster.hpp"
#include "core/unit/UnitBlueprint.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

using rm::data::Roster;
using rm::sim::Faction;
using rm::unitdef::Role;
using rm::unitdef::UnitDef;

namespace {

[[nodiscard]] UnitDef unitWith(std::string name, std::vector<std::string> tags, int costMass) {
    UnitDef def;
    def.name = std::move(name);
    std::sort(tags.begin(), tags.end());
    def.categories = std::move(tags);
    def.buildCostMass = rm::sim::Mag::fromInt(costMass);
    return def;
}

[[nodiscard]] std::filesystem::path unitRoot() {
    if (const char* home = std::getenv("HOME")) {
        return std::filesystem::path{home} / "projects/llm/input/faf/units";
    }
    return {};
}

} // namespace

TEST_CASE("a faction's tag decides whose roster a unit joins") {
    CHECK(rm::data::factionOf(unitWith("a", {"UEF"}, 0)) == Faction::Uef);
    CHECK(rm::data::factionOf(unitWith("b", {"CYBRAN"}, 0)) == Faction::Cybran);
    CHECK(rm::data::factionOf(unitWith("c", {"AEON"}, 0)) == Faction::Aeon);
    CHECK(rm::data::factionOf(unitWith("d", {"SERAPHIM"}, 0)) == Faction::Seraphim);

    // Nothing for content with no faction tag. Skipped rather than guessed at: campaign
    // scenery in a faction's roster is something a build order could pick.
    CHECK_FALSE(rm::data::factionOf(unitWith("e", {"CIVILIAN", "STRUCTURE"}, 0)).has_value());
}

TEST_CASE("a tie goes to the cheapest, then to the lowest id") {
    // THE CONTRACT, and the reason it is one: a faction fields several T1 structures that make
    // energy, so most queries match more than one blueprint. Sorting on cost rather than on
    // file order is what makes "the T1 power generator" mean the small one rather than
    // whichever the directory walk reached first — and what makes it the same answer every run.
    const std::vector<UnitDef> units{
        unitWith("UEB1102", {"UEF", "ENERGYPRODUCTION", "TECH1", "STRUCTURE"}, 200),
        unitWith("UEB1101", {"UEF", "ENERGYPRODUCTION", "TECH1", "STRUCTURE"}, 75),
        unitWith("UEB1103", {"UEF", "ENERGYPRODUCTION", "TECH1", "STRUCTURE"}, 75),
    };
    const std::vector<std::string> ids{"UEB1102", "UEB1101", "UEB1103"};

    const Roster roster = Roster::build(units, ids);
    const auto pick = roster.pick(Faction::Uef, Role::Energy, 1);
    REQUIRE(pick.has_value());
    CHECK(pick->id == "UEB1101");  // cheapest; and of the two at 75, the lower id

    // Listing gives all three, cheapest first.
    const std::vector<rm::data::RosterEntry> all = roster.all(Faction::Uef, Role::Energy);
    REQUIRE(all.size() == 3);
    CHECK(all[0].id == "UEB1101");
    CHECK(all[2].id == "UEB1102");
}

TEST_CASE("requires narrows a role that is ambiguous on its own") {
    // The bug this exists for: `role = 'factory'` picked the AIR factory, because it is cheaper
    // than the land one, and an air factory builds no tanks.
    const std::vector<UnitDef> units{
        unitWith("UEB0102", {"UEF", "FACTORY", "TECH1", "STRUCTURE", "AIR"}, 90),
        unitWith("UEB0101", {"UEF", "FACTORY", "TECH1", "STRUCTURE", "LAND"}, 120),
    };
    const std::vector<std::string> ids{"UEB0102", "UEB0101"};
    const Roster roster = Roster::build(units, ids);

    // Unfiltered, the cheaper air factory wins — correct, and not what an opening wants.
    REQUIRE(roster.pick(Faction::Uef, Role::Factory, 1)->id == "UEB0102");

    const std::vector<std::string> land{"LAND"};
    REQUIRE(roster.pick(Faction::Uef, Role::Factory, 1, land)->id == "UEB0101");

    // And a requirement nothing satisfies is nothing, not a fallback to the wrong domain.
    const std::vector<std::string> naval{"NAVAL"};
    CHECK_FALSE(roster.pick(Faction::Uef, Role::Factory, 1, naval).has_value());
}

TEST_CASE("a path is derived from an id, so the two cannot disagree") {
    rm::data::RosterEntry entry;
    entry.id = "UEB1103";
    CHECK(entry.path() == "/units/UEB1103/UEB1103_unit.bp");
}

TEST_CASE("nothing of a role is nothing, not an error") {
    const std::vector<UnitDef> units{unitWith("UEB1103", {"UEF", "MASSEXTRACTION"}, 36)};
    const std::vector<std::string> ids{"UEB1103"};
    const Roster roster = Roster::build(units, ids);

    // No faction has a T1 experimental, and a caller that needs one must say what it does
    // without it rather than being handed something wrong.
    CHECK_FALSE(roster.pick(Faction::Uef, Role::Experimental, 1).has_value());
    CHECK_FALSE(roster.pickCheapest(Faction::Uef, Role::Experimental).has_value());
    CHECK_FALSE(roster.pick(Faction::Cybran, Role::Extractor, 1).has_value());
}

TEST_CASE("the real corpus answers for all four factions") {
    // The measurement, and the point of the whole file: before this, three of the four factions
    // had no answer at all because the paths were UEF literals.
    const std::filesystem::path root = unitRoot();
    if (root.empty() || !std::filesystem::exists(root)) {
        SKIP("no extracted unit corpus at " + root.string());
    }

    std::vector<UnitDef> defs;
    std::vector<std::string> ids;
    for (const auto& entry : std::filesystem::recursive_directory_iterator{root}) {
        if (!entry.is_regular_file()
            || !entry.path().filename().string().ends_with("_unit.bp")) {
            continue;
        }
        if (auto def = rm::unitbp::loadFile(entry.path())) {
            ids.push_back(def->name);
            defs.push_back(std::move(*def));
        }
    }
    REQUIRE(defs.size() > 500);

    const Roster roster = Roster::build(defs, ids);
    // Scenery has no faction, so the roster is shorter than the corpus — by design.
    CHECK(roster.size() > 400);
    CHECK(roster.size() < defs.size());

    const std::vector<std::string> land{"LAND"};
    for (const Faction faction :
         {Faction::Uef, Faction::Cybran, Faction::Aeon, Faction::Seraphim}) {
        INFO("faction " << rm::sim::factionName(faction));

        // The four the deleted constants named, now answered per faction.
        const auto mex = roster.pick(faction, Role::Extractor, 1);
        const auto energy = roster.pick(faction, Role::Energy, 1);
        const auto factory = roster.pick(faction, Role::Factory, 1, land);
        const auto raider = roster.pickCheapest(faction, Role::Raider, land);

        REQUIRE(mex.has_value());
        REQUIRE(energy.has_value());
        REQUIRE(factory.has_value());
        REQUIRE(raider.has_value());

        // Each is that faction's own, which is the whole claim.
        CHECK(mex->faction == faction);
        CHECK(energy->faction == faction);
        CHECK(factory->faction == faction);
        CHECK(raider->faction == faction);

        // And the factory is a LAND factory, not the cheaper air one.
        CHECK(std::binary_search(factory->categories.begin(), factory->categories.end(),
                                 std::string{"LAND"}));
    }

    // UEF's answers are the ids the deleted constants named — the one place this can be
    // checked against what the engine used to hardcode.
    CHECK(roster.pick(Faction::Uef, Role::Extractor, 1)->id == "UEB1103");
    CHECK(roster.pick(Faction::Uef, Role::Energy, 1)->id == "UEB1101");
    CHECK(roster.pick(Faction::Uef, Role::Factory, 1, land)->id == "UEB0101");
}
