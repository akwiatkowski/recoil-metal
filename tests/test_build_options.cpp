// What the selection can build: the query behind the build panel.
//
// WHY THIS FILE EXISTS SEPARATELY FROM `test_build_panel.cpp`. That file tests the panel's
// ARITHMETIC — where a cell is, which one a click is over — over a `BuildOption` list handed
// to it. This one tests where that list comes from, which is the half a screenshot cannot
// check: a panel showing three plausible-looking blueprint ids looks exactly as correct as one
// showing the right three, and the wrong ones are the other faction's, or the tier the engine
// cannot build yet, or the ones a dead unit could have built.
//
// The scene is assembled by hand rather than loaded from the corpus. `gatherBuildOptions` reads
// six things — the store, the catalog, the roster, the armies, the economies and `playerArmy` —
// and none of them needs a map, a model or a texture. A corpus-backed test here would be a
// content test wearing a logic test's name, and it would go red when somebody edited a
// blueprint.

#include <catch2/catch_test_macros.hpp>

#include "app/Interface.hpp"
#include "app/Scene.hpp"
#include "app/SceneBuild.hpp"

#include <algorithm>
#include <string>
#include <vector>

using rm::app::gatherBuildOptions;
using rm::app::UnitScene;

namespace {

/// A definition with the categories that decide its role, tier and faction.
///
/// CATEGORIES ARE SORTED HERE because `UnitDef::hasCategory` is a binary search over them and
/// `Roster` copies the vector as-is. An unsorted list does not fail loudly — it silently
/// answers "no" for tags that are present, which classifies a commander as `Unknown` and
/// empties the panel for reasons no assertion in this file would name.
[[nodiscard]] rm::unitdef::UnitDef aDef(std::string name, std::vector<std::string> categories,
                                        float massCost = 100.0f, float buildRate = 0.0f) {
    rm::unitdef::UnitDef def;
    def.name = std::move(name);
    def.buildCostMass = rm::sim::magFromFloat(massCost);
    def.buildRate = buildRate;
    std::sort(categories.begin(), categories.end());
    def.categories = std::move(categories);
    return def;
}

/// A scene with one army, an economy, and a corpus of structures for two factions.
///
/// The corpus is deliberately small and deliberately mixed: two factions, two tiers, and one
/// unit of a role the panel must not offer. Every assertion below is about which slice of it
/// comes back.
struct Fixture {
    UnitScene scene;

    /// The definitions the roster is built from. Separate from `scene.definitions` because
    /// `Roster::build` wants a contiguous span and the scene's is a deque — the same split the
    /// app makes, for the same reason.
    std::vector<rm::unitdef::UnitDef> corpus;
    std::vector<std::string> ids;

    Fixture() {
        add(aDef("UEB1103", {"UEF", "TECH1", "STRUCTURE", "MASSEXTRACTION"}, 36.0f));
        add(aDef("UEB1101", {"UEF", "TECH1", "STRUCTURE", "ENERGYPRODUCTION"}, 75.0f));
        add(aDef("UEB0101", {"UEF", "TECH1", "STRUCTURE", "FACTORY"}, 240.0f));
        // Tier two: in the corpus, and never in the panel — see the note in `gatherBuildOptions`.
        add(aDef("UEB1201", {"UEF", "TECH2", "STRUCTURE", "MASSEXTRACTION"}, 900.0f));
        // A mobile unit of no buildable role: it must not reach a structure menu.
        add(aDef("UEL0201", {"UEF", "TECH1", "LAND", "TANK", "DIRECTFIRE"}, 52.0f));
        // The other faction's, to prove the faction comes from the builder's own army.
        add(aDef("URB1103", {"CYBRAN", "TECH1", "STRUCTURE", "MASSEXTRACTION"}, 36.0f));

        scene.roster = rm::data::Roster::build(corpus, ids);

        scene.armies.push_back(rm::sim::Army{.index = 0, .faction = rm::sim::Faction::Uef});
        scene.armies.push_back(rm::sim::Army{.index = 1, .faction = rm::sim::Faction::Cybran});
        scene.economies.resize(2);
        scene.playerArmy = 0;
        setStoredMass(10000.0f);
    }

    void add(rm::unitdef::UnitDef def) {
        ids.push_back(def.name);
        corpus.push_back(std::move(def));
    }

    void setStoredMass(float mass) {
        scene.economies[0].stored.mass = rm::sim::magFromFloat(mass);
    }

    /// Registers a type in the scene's own catalog and spawns one of it for an army.
    ///
    /// The def goes in the scene's `deque` rather than the corpus vector: the catalog holds
    /// POINTERS, and a vector reallocating would dangle every type registered before it.
    rm::sim::UnitId spawn(rm::unitdef::UnitDef def, int army) {
        scene.definitions.push_back(std::move(def));
        const rm::UnitTypeIndex type = scene.catalog.add(&scene.definitions.back(), rate);

        rm::sim::MoveState motion{};
        motion.armyIndex = army;
        return scene.store.spawn({
            .type = type,
            .transform = {},
            .motion = motion,
            .health = rm::sim::Health{.current = rm::sim::magFromFloat(100.0f),
                                      .maximum = rm::sim::magFromFloat(100.0f)},
        });
    }

    /// A T1 engineer: `ENGINEER`, and a build rate, which is what `isBuilder()` reads.
    rm::sim::UnitId spawnEngineer(int army = 0) {
        return spawn(aDef("UEL0105", {"UEF", "TECH1", "LAND", "ENGINEER"}, 52.0f, 5.0f), army);
    }

    rm::sim::UnitId spawnCommander(int army = 0) {
        return spawn(aDef("UEL0001", {"UEF", "COMMAND", "LAND"}, 0.0f, 10.0f), army);
    }

    rm::sim::UnitId spawnTank(int army = 0) {
        return spawn(aDef("UEL0201", {"UEF", "TECH1", "LAND", "TANK", "DIRECTFIRE"}, 52.0f), army);
    }

    /// Runs the query and returns the ids it offered, in panel order.
    [[nodiscard]] std::vector<std::string> optionsFor(
        std::vector<rm::sim::UnitId> selection) {
        std::vector<rm::ui::BuildOption> out;
        gatherBuildOptions(scene, selection, rm::ui::neutralTheme(), out, who);
        std::vector<std::string> got;
        got.reserve(out.size());
        for (const rm::ui::BuildOption& option : out) {
            got.push_back(option.id);
        }
        last = std::move(out);
        return got;
    }

    [[nodiscard]] bool offers(const std::vector<std::string>& got, std::string_view id) const {
        return std::find(got.begin(), got.end(), id) != got.end();
    }

    rm::sim::TickRate rate{};
    rm::app::BuildSelection who;
    std::vector<rm::ui::BuildOption> last;
};

} // namespace

TEST_CASE("a selected engineer is offered its faction's tier-one structures", "[ui][build]") {
    // THE CRITERION THIS PANEL EXISTS FOR. An engineer is the unit a player selects when they
    // want to build something, and until this returned a list the engine knew what it could
    // build and had no way to say so.
    Fixture fixture;
    const auto got = fixture.optionsFor({fixture.spawnEngineer()});

    REQUIRE_FALSE(got.empty());
    CHECK(fixture.who.name == "UEL0105");
    CHECK(fixture.offers(got, "UEB1103"));
    CHECK(fixture.offers(got, "UEB1101"));
    CHECK(fixture.offers(got, "UEB0101"));
}

TEST_CASE("a commander is offered the same structures as an engineer", "[ui][build]") {
    // Both are `isBuilder()` and both build structures; the panel must not treat one as a
    // special case. A commander-only menu is how this shipped once and it made the engineer —
    // the unit a player actually builds FOR construction — the one thing with no menu.
    Fixture engineerScene;
    Fixture commanderScene;

    const auto byEngineer = engineerScene.optionsFor({engineerScene.spawnEngineer()});
    const auto byCommander = commanderScene.optionsFor({commanderScene.spawnCommander()});

    CHECK(commanderScene.who.name == "UEL0001");
    CHECK(byEngineer == byCommander);
}

TEST_CASE("a tier-two structure is in the corpus and never in the panel", "[ui][build]") {
    // Listing it would offer a player something no order can satisfy: the higher tiers need an
    // upgraded engineer this engine does not model. A menu entry that cannot be clicked is
    // worse than a short menu.
    Fixture fixture;
    const auto got = fixture.optionsFor({fixture.spawnEngineer()});

    CHECK(fixture.offers(got, "UEB1103"));
    CHECK_FALSE(fixture.offers(got, "UEB1201"));
}

TEST_CASE("only structures are offered, never mobile units", "[ui][build]") {
    // A tank is in the roster, is tier one, and is the builder's own faction — the three
    // things the query filters on. What keeps it out is its ROLE, and nothing else would.
    Fixture fixture;
    const auto got = fixture.optionsFor({fixture.spawnEngineer()});

    CHECK_FALSE(fixture.offers(got, "UEL0201"));
}

TEST_CASE("the faction comes from the builder's own army", "[ui][build]") {
    // Not from `playerArmy`, and not from a default. Selecting a Cybran engineer must offer
    // Cybran structures even while the viewing player is UEF — which is what an observer or a
    // replay is, and it is also the case a hardcoded faction passes by accident.
    Fixture fixture;
    const auto got = fixture.optionsFor({fixture.spawnEngineer(1)});

    CHECK(fixture.offers(got, "URB1103"));
    CHECK_FALSE(fixture.offers(got, "UEB1103"));
}

TEST_CASE("a non-builder in the selection is skipped, not fatal", "[ui][build]") {
    // Box-selecting a group picks up tanks alongside the engineer, and that is the common
    // case rather than the corner one: a panel that empties when you select MORE is behaviour
    // a player learns to work around instead of using.
    Fixture fixture;
    const rm::sim::UnitId tank = fixture.spawnTank();
    const rm::sim::UnitId engineer = fixture.spawnEngineer();

    const auto got = fixture.optionsFor({tank, engineer});

    CHECK(fixture.who.name == "UEL0105");
    CHECK(fixture.offers(got, "UEB1103"));
}

TEST_CASE("selecting only non-builders offers nothing", "[ui][build]") {
    // Which is what makes the panel ABSENT rather than empty — `buildPanelLayout` returns an
    // empty layout for a count of zero, and the caller draws no frame at all.
    Fixture fixture;
    const auto got = fixture.optionsFor({fixture.spawnTank()});

    CHECK(got.empty());
    CHECK(fixture.who.name.empty());
}

TEST_CASE("a dead builder offers nothing, and a live one behind it answers", "[ui][build]") {
    // A selection outlives the units in it: a unit dies between the click that selected it and
    // the frame that draws the panel. Reading a dead unit's type is reading a tombstone, and
    // the store keeps its last values, so this fails silently rather than crashing — the panel
    // would keep offering a dead engineer's menu.
    Fixture fixture;
    const rm::sim::UnitId dead = fixture.spawnEngineer();
    const rm::sim::UnitId alive = fixture.spawnCommander();
    fixture.scene.store.kill(dead);

    const auto got = fixture.optionsFor({dead, alive});

    CHECK(fixture.who.name == "UEL0001");
    CHECK_FALSE(got.empty());
}

TEST_CASE("an option the army cannot pay for is offered, and marked", "[ui][build]") {
    // DIMMED RATHER THAN HIDDEN. "Not yet" is the information a player deciding what to build
    // next needs, and hiding it would make the grid reflow as the economy moves — the one
    // thing a learned-by-position layout must never do.
    Fixture fixture;
    fixture.setStoredMass(50.0f);  // the extractor at 36 is affordable, the factory at 240 is not
    const auto got = fixture.optionsFor({fixture.spawnEngineer()});

    REQUIRE(got.size() == fixture.last.size());
    bool sawAffordable = false;
    bool sawUnaffordable = false;
    for (const rm::ui::BuildOption& option : fixture.last) {
        if (option.id == "UEB1103") {
            sawAffordable = option.affordable;
        }
        if (option.id == "UEB0101") {
            sawUnaffordable = !option.affordable;
        }
    }
    CHECK(sawAffordable);
    CHECK(sawUnaffordable);
}

TEST_CASE("an empty selection clears the previous frame's options", "[ui][build]") {
    // The output vector is reused every frame, so "nothing selected" has to CLEAR it rather
    // than leave it alone. Left alone, the panel keeps drawing the last builder's menu after
    // the player has deselected — and it looks like a live panel, not a stale one.
    Fixture fixture;
    const auto first = fixture.optionsFor({fixture.spawnEngineer()});
    REQUIRE_FALSE(first.empty());

    const auto second = fixture.optionsFor({});
    CHECK(second.empty());
    CHECK(fixture.who.name.empty());
}

// --- Adoption ---------------------------------------------------------------
//
// `--units` spawns before the armies exist, so its crowd carries `kNoArmy`. Harmless in a
// march, where `playerArmy` is `kNoArmy` too and the selection filter is skipped — and fatal in
// a skirmish, where it makes every one of those units unselectable and therefore unable to show
// any of the interface. It is also the only route an ENGINEER has into a match today, which is
// why these live next to the build options rather than off in a scene-building file.

TEST_CASE("a unit nobody owns is adopted by the seated player", "[ui][build][scene]") {
    Fixture fixture;
    const rm::sim::UnitId orphan = fixture.spawnEngineer(rm::sim::kNoArmy);

    // Before: unselectable, and so the panel it could have filled never appears.
    CHECK(fixture.optionsFor({orphan}).empty());

    CHECK(rm::app::adoptOwnerlessUnits(fixture.scene) == 1);
    CHECK(fixture.scene.armyOf(orphan.index) == 0);

    const auto got = fixture.optionsFor({orphan});
    CHECK_FALSE(got.empty());
    CHECK(fixture.who.name == "UEL0105");
}

TEST_CASE("adoption leaves a unit that already has an army alone", "[ui][build][scene]") {
    // The skirmish's own spawns all carry an army by the time this runs, and taking one off its
    // owner would hand the player the enemy's commander — which is exactly the sort of thing
    // "adopt everything" would do if it did not check.
    Fixture fixture;
    const rm::sim::UnitId theirs = fixture.spawnCommander(1);
    const rm::sim::UnitId orphan = fixture.spawnEngineer(rm::sim::kNoArmy);

    CHECK(rm::app::adoptOwnerlessUnits(fixture.scene) == 1);
    CHECK(fixture.scene.armyOf(theirs.index) == 1);
    CHECK(fixture.scene.armyOf(orphan.index) == 0);
}

TEST_CASE("an observer adopts nothing", "[ui][build][scene]") {
    // `--observer` seats nobody: `playerArmy` stays `kNoArmy` and nothing is selectable. Handing
    // the crowd to "no army" would be a no-op written as an assignment, and handing it to army
    // zero would quietly seat a player in a mode whose whole point is that nobody is seated.
    Fixture fixture;
    fixture.scene.playerArmy = rm::sim::kNoArmy;
    const rm::sim::UnitId orphan = fixture.spawnEngineer(rm::sim::kNoArmy);

    CHECK(rm::app::adoptOwnerlessUnits(fixture.scene) == 0);
    CHECK(fixture.scene.armyOf(orphan.index) == rm::sim::kNoArmy);
}

TEST_CASE("adopting twice changes nothing the second time", "[ui][build][scene]") {
    // It runs once per scene today, and a count that kept climbing would be the first sign that
    // it had quietly become per-frame.
    Fixture fixture;
    (void)fixture.spawnEngineer(rm::sim::kNoArmy);

    CHECK(rm::app::adoptOwnerlessUnits(fixture.scene) == 1);
    CHECK(rm::app::adoptOwnerlessUnits(fixture.scene) == 0);
}

TEST_CASE("the panel reports the builder it is showing, not just its name", "[ui][build]") {
    // THE HANDLE, because a placement is ORDERED FROM this unit and the header only NAMES it.
    // Re-deriving "the first builder in the selection" at the click site would be a second copy
    // of the rule, and a second copy drifts — the panel headed by one unit and the build order
    // issued by another, which nothing notices until two builders are selected at once.
    Fixture fixture;
    const rm::sim::UnitId tank = fixture.spawnTank();
    const rm::sim::UnitId engineer = fixture.spawnEngineer();

    const auto got = fixture.optionsFor({tank, engineer});
    REQUIRE_FALSE(got.empty());

    CHECK(fixture.who.builder == engineer);
    CHECK(fixture.who.builder != tank);
    CHECK(fixture.who.any());
}

TEST_CASE("nothing selected reports no builder at all", "[ui][build]") {
    Fixture fixture;
    (void)fixture.optionsFor({fixture.spawnTank()});

    CHECK_FALSE(fixture.who.any());
    CHECK_FALSE(fixture.scene.store.alive(fixture.who.builder));
}

TEST_CASE("a factory offers what its BuildableCategory names, and nothing else", "[ui][build]") {
    Fixture fixture;

    // The factory's expression, as retail states one: category terms, ANY of which admits.
    rm::unitdef::UnitDef factory =
        aDef("UEB0101", {"UEF", "TECH1", "STRUCTURE", "FACTORY"}, 240.0f, 20.0f);
    factory.buildableCategory.push_back(
        rm::unitdef::parseCategoryTerm("BUILTBYTIER1FACTORY UEF MOBILE"));

    // Corpus additions: a tank the term admits, an engineer it admits, an air unit of the
    // right faction it does NOT (no BUILTBYTIER1FACTORY), and the enemy's tank.
    fixture.add(aDef("UEL0202", {"BUILTBYTIER1FACTORY", "MOBILE", "TANK", "TECH1", "UEF"}, 52.0f));
    fixture.add(aDef("UEL0106", {"BUILTBYTIER1FACTORY", "ENGINEER", "MOBILE", "TECH1", "UEF"}, 36.0f));
    fixture.add(aDef("UEA0101", {"AIR", "MOBILE", "TECH1", "UEF"}, 80.0f));
    fixture.add(aDef("URL0202", {"BUILTBYTIER1FACTORY", "CYBRAN", "MOBILE", "TANK", "TECH1"}, 52.0f));
    fixture.scene.roster = rm::data::Roster::build(fixture.corpus, fixture.ids);

    const rm::sim::UnitId site = fixture.spawn(factory, 0);
    const auto got = fixture.optionsFor({site});

    CHECK(fixture.offers(got, "UEL0202"));
    CHECK(fixture.offers(got, "UEL0106"));
    CHECK_FALSE(fixture.offers(got, "UEA0101"));  // the expression does not admit it
    CHECK_FALSE(fixture.offers(got, "URL0202"));  // the enemy's, whatever the tags say
    CHECK_FALSE(fixture.offers(got, "UEB1103"));  // structures are the other tray's

    // The header names the factory, so the panel is attributable.
    CHECK(fixture.who.role == "factory");
    CHECK(fixture.who.builder == site);
}

TEST_CASE("a factory stating no BuildableCategory offers nothing, not everything",
          "[ui][build]") {
    Fixture fixture;
    const rm::unitdef::UnitDef factory =
        aDef("UEB0101", {"UEF", "TECH1", "STRUCTURE", "FACTORY"}, 240.0f, 20.0f);
    const rm::sim::UnitId site = fixture.spawn(factory, 0);
    CHECK(fixture.optionsFor({site}).empty());
}

TEST_CASE("an id reference in the expression admits exactly that unit", "[ui][build]") {
    Fixture fixture;
    rm::unitdef::UnitDef factory =
        aDef("UEB0102", {"UEF", "TECH1", "STRUCTURE", "FACTORY"}, 240.0f, 20.0f);
    factory.buildableCategory.push_back(rm::unitdef::parseCategoryTerm("uel0202"));

    fixture.add(aDef("UEL0202", {"MOBILE", "TANK", "TECH1", "UEF"}, 52.0f));
    fixture.add(aDef("UEL0203", {"MOBILE", "TANK", "TECH1", "UEF"}, 52.0f));
    fixture.scene.roster = rm::data::Roster::build(fixture.corpus, fixture.ids);

    const auto got = fixture.optionsFor({fixture.spawn(factory, 0)});
    CHECK(fixture.offers(got, "UEL0202"));
    CHECK_FALSE(fixture.offers(got, "UEL0203"));
}
