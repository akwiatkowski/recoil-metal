// Who owns what, and who may shoot whom.
//
// All pure, so all tested here. What these rules get wrong is not visible on screen:
// a mis-set alliance looks like units declining to fire for no reason, and a
// commander id that resolves to nothing looks like a map with no spawns.
#include <catch2/catch_test_macros.hpp>

#include "core/sim/Army.hpp"

#include <string>

using rm::sim::Army;
using rm::sim::Faction;

TEST_CASE("a faction is read from the name a blueprint states") {
    // All 568 shipped units state `General.FactionName`, and across the corpus it
    // takes exactly these four values — 217 UEF, 154 Cybran, 107 Aeon, 90 Seraphim.
    CHECK(rm::sim::factionFromName("UEF") == Faction::Uef);
    CHECK(rm::sim::factionFromName("Aeon") == Faction::Aeon);
    CHECK(rm::sim::factionFromName("Cybran") == Faction::Cybran);
    CHECK(rm::sim::factionFromName("Seraphim") == Faction::Seraphim);

    // Case-insensitively, because the corpus spells two of them differently from each
    // other and a mod has no reason to agree with either.
    CHECK(rm::sim::factionFromName("uef") == Faction::Uef);
    CHECK(rm::sim::factionFromName("SERAPHIM") == Faction::Seraphim);

    // A name from outside the set is nullopt rather than a default. Defaulting an
    // unknown faction to UEF would give a modded unit the wrong commander and the
    // wrong colour, and look like a content bug rather than a reader one.
    CHECK_FALSE(rm::sim::factionFromName("Nomads").has_value());
    CHECK_FALSE(rm::sim::factionFromName("").has_value());
}

TEST_CASE("a faction names its own commander") {
    CHECK(rm::sim::commanderBlueprintId(Faction::Uef) == "UEL0001");
    CHECK(rm::sim::commanderBlueprintId(Faction::Aeon) == "UAL0001");
    CHECK(rm::sim::commanderBlueprintId(Faction::Cybran) == "URL0001");
    CHECK(rm::sim::commanderBlueprintId(Faction::Seraphim) == "XSL0001");

    // And the path it lives at, which is the form the VFS wants.
    CHECK(rm::sim::commanderBlueprintPath(Faction::Uef) == "/units/UEL0001/UEL0001_unit.bp");
}

TEST_CASE("every faction round-trips through its name") {
    // The two directions must agree, or a log line names one faction and the
    // blueprint loaded is another's.
    for (const Faction faction :
         {Faction::Uef, Faction::Aeon, Faction::Cybran, Faction::Seraphim}) {
        CHECK(rm::sim::factionFromName(rm::sim::factionName(faction)) == faction);
    }
}

TEST_CASE("a free-for-all gives every start position its own side") {
    const std::vector<Army> armies = rm::sim::freeForAll(8);
    REQUIRE(armies.size() == 8);

    for (std::size_t i = 0; i < armies.size(); ++i) {
        CHECK(armies[i].index == static_cast<int>(i));
        CHECK(armies[i].team == static_cast<int>(i));  // nobody allied with anybody
        CHECK_FALSE(armies[i].defeated);
    }

    // Factions deal round-robin rather than randomly, so the same map produces the
    // same match every run — the reason `--march` and `--screenshot` prove anything.
    CHECK(armies[0].faction == Faction::Uef);
    CHECK(armies[3].faction == Faction::Seraphim);
    CHECK(armies[4].faction == Faction::Uef);  // wraps

    // Colours come from the palette in order, and it wraps too: a map may declare
    // nine start positions (SCMP maps declare up to ARMY_9) against eight colours.
    CHECK(armies[0].colour != armies[1].colour);
    const std::vector<Army> nine = rm::sim::freeForAll(9);
    CHECK(nine[8].colour == nine[0].colour);
}

TEST_CASE("an army is allied with itself, and hostile to another team") {
    const std::vector<Army> armies = rm::sim::freeForAll(2);

    // Itself, which is not a pedantic case: "do not shoot allies" written without it
    // has every unit shoot itself.
    CHECK(rm::sim::allied(armies[0], armies[0]));
    CHECK_FALSE(rm::sim::hostile(armies[0], armies[0]));

    CHECK_FALSE(rm::sim::allied(armies[0], armies[1]));
    CHECK(rm::sim::hostile(armies[0], armies[1]));
}

TEST_CASE("armies sharing a team are allies") {
    std::vector<Army> armies = rm::sim::freeForAll(4);
    armies[1].team = armies[0].team;  // 2v2

    CHECK(rm::sim::allied(armies[0], armies[1]));
    CHECK_FALSE(rm::sim::hostile(armies[0], armies[1]));
    CHECK(rm::sim::hostile(armies[0], armies[2]));
}

TEST_CASE("a defeated army is nobody's target and nobody's threat") {
    std::vector<Army> armies = rm::sim::freeForAll(3);
    armies[1].defeated = true;

    // Both directions: a corpse army neither draws fire nor opens it. Without this a
    // surviving force keeps shooting at a side that has already lost.
    CHECK_FALSE(rm::sim::hostile(armies[0], armies[1]));
    CHECK_FALSE(rm::sim::hostile(armies[1], armies[0]));

    // ...but it is still not an ALLY, which is a different question and would let a
    // defeated army's units be treated as friendly.
    CHECK_FALSE(rm::sim::allied(armies[0], armies[1]));

    CHECK(rm::sim::survivorCount(armies) == 2);
}

TEST_CASE("survivors are counted, and nobody left is a draw rather than an error") {
    std::vector<Army> armies = rm::sim::freeForAll(2);
    CHECK(rm::sim::survivorCount(armies) == 2);

    armies[0].defeated = true;
    CHECK(rm::sim::survivorCount(armies) == 1);  // the match is over

    // Both dying in one tick is possible — two commanders inside one blast — and is a
    // draw. Reporting it as an error would turn a legitimate outcome into a crash.
    armies[1].defeated = true;
    CHECK(rm::sim::survivorCount(armies) == 0);
}

TEST_CASE("no armies at all is an empty list, not a crash") {
    CHECK(rm::sim::freeForAll(0).empty());
    CHECK(rm::sim::survivorCount({}) == 0);
}
