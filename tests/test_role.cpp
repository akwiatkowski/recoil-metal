// What a unit is for, inferred from what its blueprint declares.
//
// TWO KINDS OF CASE, and the second is the one that matters. The synthetic cases pin the
// PRECEDENCE — a commander is a commander before it is a builder, a mex with a gun is still a
// mex — which is the whole design and is invisible in a corpus test because no real unit
// isolates it. The corpus cases check the classifier against the actual archive, which is the
// only thing that can catch a category this engine spelled wrong.
#include <catch2/catch_test_macros.hpp>

#include "core/unit/Role.hpp"
#include "core/unit/UnitBlueprint.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

using rm::unitdef::Role;
using rm::unitdef::roleOf;
using rm::unitdef::UnitDef;

namespace {

/// A def with nothing but the tags a test wants to reason about.
[[nodiscard]] UnitDef withCategories(std::vector<std::string> tags) {
    UnitDef def;
    def.name = "synthetic";
    std::sort(tags.begin(), tags.end());
    def.categories = std::move(tags);
    return def;
}

/// The extracted corpus, the same place every other real-blueprint test reads from. Assets are
/// never committed (AGENT.md rule 3), so these SKIP when it has not been extracted.
[[nodiscard]] std::filesystem::path unitRoot() {
    if (const char* home = std::getenv("HOME")) {
        return std::filesystem::path{home} / "projects/llm/input/faf/units";
    }
    return {};
}

/// Every `<ID>_unit.bp` under it.
[[nodiscard]] std::vector<std::filesystem::path> blueprints() {
    std::vector<std::filesystem::path> found;
    const std::filesystem::path root = unitRoot();
    if (root.empty() || !std::filesystem::exists(root)) {
        return found;
    }
    for (const auto& entry : std::filesystem::recursive_directory_iterator{root}) {
        if (entry.is_regular_file()
            && entry.path().filename().string().ends_with("_unit.bp")) {
            found.push_back(entry.path());
        }
    }
    // Sorted, so a tally over the corpus is the same every run.
    std::sort(found.begin(), found.end());
    return found;
}

} // namespace

// --- The precedence, which is the design ------------------------------------------------

TEST_CASE("a commander is a commander before it is anything else") {
    // The real ACU's categories: it builds, it makes mass AND energy, it carries a gun. Every
    // one of those would classify it as something else if the order were wrong, and losing it
    // ends the match — so nothing it also does competes.
    const UnitDef acu = withCategories({"COMMAND", "CONSTRUCTION", "ENGINEER", "ECONOMIC",
                                        "ENERGYPRODUCTION", "MASSPRODUCTION", "MOBILE", "LAND"});
    CHECK(roleOf(acu) == Role::Commander);
}

TEST_CASE("an extractor with a gun is still an extractor") {
    // The economic kinds are tested before the military ones. A mass extractor that declares
    // DIRECTFIRE would otherwise come out a raider, and a build order asking for "an
    // extractor" would never find one.
    const UnitDef mex = withCategories({"MASSEXTRACTION", "STRUCTURE", "TECH1", "DIRECTFIRE"});
    CHECK(roleOf(mex) == Role::Extractor);
}

TEST_CASE("a category outranks a figure, and a figure is the last resort") {
    // THIS CASE ENCODES A BUG THAT WAS MADE AND FIXED. The first classifier accepted either
    // the category or a non-zero production/storage FIGURE at the same precedence, reasoning
    // that 07 §4.4 derives `extractsmetal` from both. True of the capability, wrong for the
    // role: in the shipped corpus a land factory carries `StorageEnergy` and a T1 engineer
    // carries both, so "has any storage" classified UEB0101 and UEL0105 as `storage`. Only the
    // corpus test could see it.

    // A tag is authoritative.
    CHECK(roleOf(withCategories({"MASSEXTRACTION", "STRUCTURE"})) == Role::Extractor);

    // A figure alone still works — for content that declares no role category at all.
    UnitDef byFigure = withCategories({"STRUCTURE", "TECH1"});
    byFigure.producesMassPerSecond = 2.0f;
    CHECK(roleOf(byFigure) == Role::Extractor);

    UnitDef energyByFigure = withCategories({"STRUCTURE"});
    energyByFigure.producesEnergyPerSecond = 20.0f;
    CHECK(roleOf(energyByFigure) == Role::Energy);

    // But a figure NEVER outvotes a tag. A factory with a buffer is a factory; an engineer
    // with one is a builder. These two are the exact units the bug misclassified.
    UnitDef factory = withCategories({"FACTORY", "CONSTRUCTION", "STRUCTURE", "TECH1"});
    factory.storageEnergy = rm::sim::Mag::fromInt(105);
    CHECK(roleOf(factory) == Role::Factory);

    UnitDef engineer = withCategories({"CONSTRUCTION", "ENGINEER", "MOBILE", "TECH1"});
    engineer.storageMass = rm::sim::Mag::fromInt(10);
    engineer.storageEnergy = rm::sim::Mag::fromInt(50);
    CHECK(roleOf(engineer) == Role::Builder);
}

TEST_CASE("a factory is not a builder, and the distinction is the point") {
    // A build order queues units at a factory and structures with an engineer. Collapsing the
    // two loses the only thing it needs to tell them apart.
    const UnitDef factory =
        withCategories({"FACTORY", "CONSTRUCTION", "STRUCTURE", "TECH1", "LAND"});
    CHECK(roleOf(factory) == Role::Factory);

    const UnitDef engineer =
        withCategories({"CONSTRUCTION", "ENGINEER", "MOBILE", "LAND", "TECH1"});
    CHECK(roleOf(engineer) == Role::Builder);
}

TEST_CASE("a build rate alone makes a builder") {
    // `isBuilder()` is the third way in, for content that states a rate and no category.
    UnitDef def = withCategories({"MOBILE", "LAND"});
    def.buildRate = 10.0f;
    CHECK(roleOf(def) == Role::Builder);

    // A repair arm is secondary to an explicitly authored combat role (URL0107, Mantis).
    def.categories = {"DIRECTFIRE", "LAND", "MOBILE", "TECH1"};
    CHECK(roleOf(def) == Role::Raider);
}

TEST_CASE("an armed structure is a defence, whatever it is armed with") {
    // What a build order wants to know is "does this hold ground", and immobile-and-armed is
    // what that means. Tested before the weapon classes so a point-defence turret does not
    // come out as an assault unit.
    CHECK(roleOf(withCategories({"STRUCTURE", "DEFENSE", "DIRECTFIRE", "TECH1"}))
          == Role::Defence);
    CHECK(roleOf(withCategories({"STRUCTURE", "DEFENSE", "ANTIAIR", "TECH2"}))
          == Role::Defence);
    CHECK(roleOf(withCategories({"STRUCTURE", "ARTILLERY", "TECH3"})) == Role::Defence);

    // MOBILE overrides it: a mobile artillery piece is artillery, not a defence.
    CHECK(roleOf(withCategories({"STRUCTURE", "MOBILE", "ARTILLERY", "TECH2"}))
          == Role::Artillery);
}

TEST_CASE("an aircraft is classified by flying first") {
    CHECK(roleOf(withCategories({"AIR", "MOBILE", "DIRECTFIRE", "TECH1"})) == Role::Air);
    CHECK(roleOf(withCategories({"AIR", "MOBILE", "BOMBER", "TECH1"})) == Role::Bomber);
    CHECK(roleOf(withCategories({"AIR", "MOBILE", "ANTIAIR", "TECH1"})) == Role::AntiAir);
}

TEST_CASE("direct fire splits into raider and assault by tech") {
    // A proxy, and the comment in `Role.cpp` says so: speed against the family median would be
    // better and needs the whole corpus. What is pinned here is that the proxy is applied.
    CHECK(roleOf(withCategories({"DIRECTFIRE", "MOBILE", "LAND", "TECH1"})) == Role::Raider);
    CHECK(roleOf(withCategories({"DIRECTFIRE", "MOBILE", "LAND", "TECH2"})) == Role::Assault);
    CHECK(roleOf(withCategories({"DIRECTFIRE", "MOBILE", "LAND", "TECH3"})) == Role::Assault);
}

TEST_CASE("an experimental is an experimental whatever it does") {
    CHECK(roleOf(withCategories({"EXPERIMENTAL", "MOBILE", "LAND", "DIRECTFIRE"}))
          == Role::Experimental);
    CHECK(roleOf(withCategories({"EXPERIMENTAL", "AIR", "MOBILE", "BOMBER"}))
          == Role::Experimental);
}

TEST_CASE("nothing recognised is unknown, not a guess") {
    // A wall is not an error, and 568 blueprints do not all classify.
    CHECK(roleOf(withCategories({"STRUCTURE", "WALL"})) == Role::Unknown);
    CHECK(roleOf(UnitDef{}) == Role::Unknown);
}

TEST_CASE("scenery is scenery even when it has a gun") {
    // A civilian role rather than `Unknown`, and the distinction earns itself twice: it
    // separates "we could not classify this" from "this is a house", and it keeps a campaign
    // map's defended village out of a build order. Before it existed, civilian buildings were
    // four fifths of the unclassified pile and made the tally look like a broken classifier.
    CHECK(roleOf(withCategories({"CIVILIAN", "STRUCTURE", "BENIGN"})) == Role::Civilian);
    CHECK(roleOf(withCategories({"CIVILIAN", "STRUCTURE", "DIRECTFIRE", "DEFENSE"}))
          == Role::Civilian);

    // And it beats the economic kinds too — a civilian power plant is scenery.
    UnitDef plant = withCategories({"CIVILIAN", "STRUCTURE", "ENERGYPRODUCTION"});
    plant.producesEnergyPerSecond = 20.0f;
    CHECK(roleOf(plant) == Role::Civilian);
}

TEST_CASE("the corpus's own tags for lobbing things are artillery") {
    // `INDIRECTFIRE` and `SILO` are what the blueprints actually say; testing only `ARTILLERY`
    // left eleven units unclassified.
    CHECK(roleOf(withCategories({"INDIRECTFIRE", "MOBILE", "LAND", "TECH2"}))
          == Role::Artillery);
    // UEL0103 carries this generic intel tag as well; it does not turn artillery into a scout.
    CHECK(roleOf(withCategories({"ARTILLERY", "INTELLIGENCE", "MOBILE", "LAND", "TECH1"}))
          == Role::Artillery);
    CHECK(roleOf(withCategories({"SILO", "MOBILE", "LAND", "TECH3"})) == Role::Artillery);
    CHECK(roleOf(withCategories({"SILO", "STRUCTURE", "TECH3"})) == Role::Defence);
}

TEST_CASE("tech is separate from role, and experimental reads as tier four") {
    // A T1 and a T3 tank are the same KIND of thing; a build order cares about both facts
    // independently. Experimental first, because an experimental declares no `TECHn` and
    // testing the tiers first would report zero for the biggest things in the game.
    CHECK(rm::unitdef::techOf(withCategories({"TECH1"})) == 1);
    CHECK(rm::unitdef::techOf(withCategories({"TECH2"})) == 2);
    CHECK(rm::unitdef::techOf(withCategories({"TECH3"})) == 3);
    CHECK(rm::unitdef::techOf(withCategories({"EXPERIMENTAL"})) == 4);
    CHECK(rm::unitdef::techOf(withCategories({"EXPERIMENTAL", "TECH3"})) == 4);
    CHECK(rm::unitdef::techOf(withCategories({"STRUCTURE"})) == 0);
}

TEST_CASE("every role has a name, and it round-trips") {
    // A data file names a role as a string, so the two directions must agree — a mismatch
    // would make a build order silently ask for `unknown`.
    for (int i = 0; i <= static_cast<int>(Role::Experimental); ++i) {
        const auto role = static_cast<Role>(i);
        const std::string_view name = rm::unitdef::roleName(role);
        REQUIRE_FALSE(name.empty());
        const auto back = rm::unitdef::roleFromName(name);
        REQUIRE(back.has_value());
        REQUIRE(*back == role);
    }

    // And a name this engine does not know is nothing, which is what lets a newer data file
    // fail loudly on an older binary rather than classifying as unknown.
    CHECK_FALSE(rm::unitdef::roleFromName("battlecruiser").has_value());
    CHECK_FALSE(rm::unitdef::roleFromName("").has_value());
}

TEST_CASE("categories are sorted, so membership is a binary search") {
    const UnitDef def = withCategories({"ZULU", "ALPHA", "MIKE"});
    CHECK(std::is_sorted(def.categories.begin(), def.categories.end()));
    CHECK(def.hasCategory("ALPHA"));
    CHECK(def.hasCategory("MIKE"));
    CHECK(def.hasCategory("ZULU"));
    CHECK_FALSE(def.hasCategory("BRAVO"));

    // Case-sensitive on purpose: the corpus is consistently upper case, and folding case
    // would hide a typo in a data file rather than failing on it.
    CHECK_FALSE(def.hasCategory("alpha"));
}

TEST_CASE("all-of is what one BuildableCategory term needs") {
    const UnitDef def = withCategories({"BUILTBYTIER1ENGINEER", "CYBRAN", "STRUCTURE"});

    const std::vector<std::string_view> both{"BUILTBYTIER1ENGINEER", "CYBRAN"};
    CHECK(def.hasAllCategories(both));

    const std::vector<std::string_view> wrongFaction{"BUILTBYTIER1ENGINEER", "UEF"};
    CHECK_FALSE(def.hasAllCategories(wrongFaction));

    // An empty term matches everything. Deliberate rather than an oversight — the parser is
    // what must refuse an expression that reduced to nothing.
    CHECK(def.hasAllCategories({}));
}

// --- The corpus, which is the only thing that can catch a misspelt category -------------

TEST_CASE("the shipped blueprints classify as what they are") {
    // The four ids §7 P3.1 names, plus the commander. Read from the retail archive, so this
    // skips without it — and when it runs it is the only case here that can catch a category
    // spelled wrong in `Role.cpp`, because a synthetic def spells them the same way.
    const std::filesystem::path root = unitRoot();
    if (root.empty() || !std::filesystem::exists(root)) {
        SKIP("no extracted unit corpus at " + root.string());
    }

    struct Expected {
        std::string_view id;
        Role role;
        int tech;
    };
    // UEF, because that is the faction the current build order hardcodes — the point of P3.3
    // is that these ids stop being named in C++, and the point of this test is that a role is
    // enough to find them.
    const std::vector<Expected> cases{
        {"UEL0001", Role::Commander, 0},
        {"UEB1103", Role::Extractor, 1},
        {"UEB1101", Role::Energy, 1},
        {"UEB0101", Role::Factory, 1},
        {"UEL0105", Role::Builder, 1},
        {"UEL0101", Role::Scout, 1},
        {"UEL0103", Role::Artillery, 1},
        {"UEL0201", Role::Raider, 1},
    };

    for (const Expected& expected : cases) {
        const std::filesystem::path path =
            root / expected.id / (std::string{expected.id} + "_unit.bp");
        if (!std::filesystem::exists(path)) {
            SKIP("no " + path.string());
        }
        const auto def = rm::unitbp::loadFile(path);
        REQUIRE(def.has_value());
        INFO("blueprint " << expected.id << " classified as "
                          << rm::unitdef::roleName(roleOf(*def)));
        CHECK(roleOf(*def) == expected.role);
        CHECK_FALSE(def->categories.empty());
        if (expected.tech > 0) {
            CHECK(rm::unitdef::techOf(*def) == expected.tech);
        }
    }
}

TEST_CASE("the whole corpus classifies, and mostly not as unknown") {
    // The measurement that says whether the classifier is worth anything. A per-role tally
    // over every shipped blueprint: if `unknown` dominated, the vocabulary or the precedence
    // would be wrong, and no individual case would show it.
    const std::vector<std::filesystem::path> paths = blueprints();
    if (paths.empty()) {
        SKIP("no extracted unit corpus at " + unitRoot().string());
    }

    std::size_t total = 0;
    std::size_t unknown = 0;
    std::size_t tagged = 0;
    for (const std::filesystem::path& path : paths) {
        const auto def = rm::unitbp::loadFile(path);
        if (!def) {
            continue;
        }
        ++total;
        tagged += def->categories.empty() ? 0u : 1u;
        unknown += roleOf(*def) == Role::Unknown ? 1u : 0u;
    }

    REQUIRE(total > 500);  // the corpus is 568 units; a much smaller number means a bad mount

    // Every unit declares categories. 07 §4.2 says the blueprints "carry everything needed",
    // and this is that claim checked rather than trusted.
    CHECK(tagged == total);

    // MEASURED, not guessed: 28 of 568 at the time of writing, so the bar is a tenth. It was
    // 180 before `Civilian` existed and before `INDIRECTFIRE`/`SILO` were handled, which is
    // what this tally is for — no individual case shows a systematically missing category.
    INFO("unknown: " << unknown << " of " << total);
    CHECK(unknown * 10 < total);
}

// --- The combat predicate, for box-select preference and the idle-combat hotkey ----------

TEST_CASE("a mobile unit with an automatic weapon is mobile combat") {
    UnitDef tank = withCategories({"DIRECTFIRE", "LAND", "MOBILE", "TECH1"});
    tank.speedElmosPerSecond = 30.0f;
    rm::unitdef::Weapon gun;
    gun.maxRange = rm::sim::fxFromFloat(240.0f);
    gun.rateOfFire = 1.0f;
    gun.damage = rm::sim::Mag::fromInt(50);
    tank.weapons = {gun};
    CHECK(rm::unitdef::isMobileCombat(tank));
}

TEST_CASE("unarmed mobility and immobile guns are both not mobile combat") {
    // The field engineer: moves, builds, holds no weapon — a box that caught it together
    // with tanks prefers the tanks.
    UnitDef engineer = withCategories({"CONSTRUCTION", "ENGINEER", "LAND", "MOBILE", "TECH1"});
    engineer.speedElmosPerSecond = 20.0f;
    engineer.buildRate = 10.0f;
    CHECK_FALSE(rm::unitdef::isMobileCombat(engineer));

    // Point defence: armed, and stays exactly where the box found it.
    UnitDef pd = withCategories({"DEFENSE", "DIRECTFIRE", "STRUCTURE", "TECH1"});
    rm::unitdef::Weapon gun;
    gun.maxRange = rm::sim::fxFromFloat(400.0f);
    gun.rateOfFire = 2.0f;
    gun.damage = rm::sim::Mag::fromInt(100);
    pd.weapons = {gun};
    CHECK_FALSE(rm::unitdef::isMobileCombat(pd));

    // A death weapon alone does not make a combat unit either.
    UnitDef mine = withCategories({"LAND", "MOBILE", "TECH1"});
    mine.speedElmosPerSecond = 20.0f;
    rm::unitdef::Weapon death;
    death.role = rm::unitdef::WeaponRole::Death;
    death.maxRange = rm::sim::fxFromFloat(10.0f);
    death.rateOfFire = 1.0f;
    death.damage = rm::sim::Mag::fromInt(500);
    mine.weapons = {death};
    CHECK_FALSE(rm::unitdef::isMobileCombat(mine));
}
